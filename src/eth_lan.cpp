#include "eth_lan.h"
#include "config.h"
#include "settings.h"
#include "eth_pins.h"

#if ETH_ENABLED

#include <esp_eth.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_system.h>
#include <esp_mac.h>
#include <driver/gpio.h>
#include <driver/spi_master.h>

static esp_eth_handle_t  ethHandle = nullptr;
static esp_netif_t*      ethNetif  = nullptr;
static NetCfg            cfg;
static String            initError;
static String            probeText = "not checked yet";

static volatile bool     sStarted = false, sLink = false, sIp = false, sFallback = false, sRestartDhcp = false;
static volatile uint32_t sIpAddr = 0, sMask = 0, sGw = 0;      // lwIP order == what IPAddress(uint32_t) expects
static volatile uint32_t sLinkUpMs = 0;

// ---------------------------------------------------------------- events (run in the system event task)
static void onEthEvent(void*, esp_event_base_t base, int32_t id, void* data) {
  if (base == ETH_EVENT) {
    switch (id) {
      case ETHERNET_EVENT_CONNECTED:
        sLink = true;
        sLinkUpMs = millis();
        Console.println("LAN: cable connected (link up)");
        break;
      case ETHERNET_EVENT_DISCONNECTED:
        sLink = false;
        sIp = false;
        sRestartDhcp = true;
        Console.println("LAN: cable disconnected");
        break;
      case ETHERNET_EVENT_START:
        Console.println("LAN: W5500 started");
        break;
      case ETHERNET_EVENT_STOP:
        sLink = false;
        sIp = false;
        break;
      default: break;
    }
  } else if (base == IP_EVENT) {
    if (id == IP_EVENT_ETH_GOT_IP) {
      const ip_event_got_ip_t* e = (const ip_event_got_ip_t*)data;
      sIpAddr = e->ip_info.ip.addr;
      sMask   = e->ip_info.netmask.addr;
      sGw     = e->ip_info.gw.addr;
      sIp     = true;
      Console.printf("LAN: IP %s  mask %s  gateway %s%s\n", IPAddress(sIpAddr).toString().c_str(),
                    IPAddress(sMask).toString().c_str(), IPAddress(sGw).toString().c_str(),
                    sFallback ? "  (static address, DHCP did not answer)" : "");
    } else if (id == IP_EVENT_ETH_LOST_IP) {
      sIp = false;
    }
  }
}

// ---------------------------------------------------------------- static address
static bool applyStaticAddress() {
  IPAddress ip, mask, gw, dns;
  if (!ip.fromString(cfg.ip) || !mask.fromString(cfg.mask)) {
    Console.println("LAN: static address is not valid - check Settings -> Network interface");
    return false;
  }
  gw.fromString(cfg.gw);
  dns.fromString(cfg.dns);

  esp_err_t e = esp_netif_dhcpc_stop(ethNetif);
  if (e != ESP_OK && e != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
    Console.printf("LAN: could not stop DHCP client (err %d)\n", (int)e);
  }
  esp_netif_ip_info_t info;
  info.ip.addr      = (uint32_t)ip;
  info.netmask.addr = (uint32_t)mask;
  info.gw.addr      = (uint32_t)gw;
  e = esp_netif_set_ip_info(ethNetif, &info);
  if (e != ESP_OK) { Console.printf("LAN: could not set static address (err %d)\n", (int)e); return false; }

  if (dns != IPAddress(0, 0, 0, 0)) {
    esp_netif_dns_info_t d;
    d.ip.type = ESP_IPADDR_TYPE_V4;
    d.ip.u_addr.ip4.addr = (uint32_t)dns;
    esp_netif_set_dns_info(ethNetif, ESP_NETIF_DNS_MAIN, &d);
  }
  sIpAddr = info.ip.addr;
  sMask   = info.netmask.addr;
  sGw     = info.gw.addr;
  return true;
}

// ---------------------------------------------------------------- helper task: DHCP timeout -> static address
static void ethTask(void*) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(500));
    if (!sStarted) continue;

    const bool staticNow = (cfg.ipMode == ETHIP_STATIC) || sFallback;
    if (staticNow) sIp = sLink;                      // a static address is usable as soon as the cable is plugged in

    if (cfg.ipMode == ETHIP_DHCP_THEN_STATIC) {
      if (sRestartDhcp && sFallback) {               // cable was unplugged while on the fallback address: try DHCP again
        sRestartDhcp = false;
        sFallback = false;
        esp_netif_dhcpc_start(ethNetif);
        Console.println("LAN: cable re-plugged -> asking DHCP again");
      } else if (sRestartDhcp) {
        sRestartDhcp = false;
      }
      if (sLink && !sIp && !sFallback && (millis() - sLinkUpMs) >= (uint32_t)cfg.dhcpWaitS * 1000UL) {
        Console.printf("LAN: no DHCP answer for %u s -> using static address %s  (NOT an address from your network: choose \"DHCP\" only on the Settings page if your network has DHCP; some networks only accept known MAC addresses - see \"LAN MAC address\")\n", (unsigned)cfg.dhcpWaitS, cfg.ip.c_str());
        if (applyStaticAddress()) {
          sFallback = true;
          sIp = true;
          Console.printf("LAN: IP %s  mask %s  gateway %s  (static address, DHCP did not answer)\n",
                        IPAddress(sIpAddr).toString().c_str(), IPAddress(sMask).toString().c_str(), IPAddress(sGw).toString().c_str());
        }
      }
    }
  }
}


// ---------------------------------------------------------------- MAC address
// Automatic = unique, derived from this chip. Fixed = what the Settings page says (the reference firmware used the same
// fixed DE:AD:BE:EF:FE:ED in every unit, which is only fine when ONE gateway is on the network, or the network allows only known MACs).
static void currentMac(const NetCfg& c, uint8_t* m) {
  std::string e;
  if (c.macMode && ethMacParse(c.mac.c_str(), m, e)) return;
  esp_read_mac(m, ESP_MAC_ETH);
}

// ---------------------------------------------------------------- no INT wire: wake the driver ourselves
// The ESP-IDF W5500 driver reads received packets only when the INT pin falls (or when its task is notified, which the INT
// interrupt does). Boards without an INT wire (like the Minew gateway) get the same notification from this small task.
static void ethPollTask(void*) {
  TaskHandle_t rx = nullptr;
  for (;;) {
    if (!rx) rx = xTaskGetHandle("w5500_tsk");           // the driver's receive task
    if (rx) xTaskNotifyGive(rx);
    vTaskDelay(pdMS_TO_TICKS(4));
  }
}

// ---------------------------------------------------------------- is a W5500 answering on these pins?
// W5500 SPI frame: 16 bit address, 8 bit control byte (block select, read/write, mode), data. The chip version register
// (address 0x0039, common register block) always reads 0x04.
static bool readW5500Version(spi_device_handle_t spi, uint8_t& v) {
  spi_transaction_t t = {};
  t.flags  = SPI_TRANS_USE_RXDATA;
  t.cmd    = 0x0039;          // address phase (16 bit)
  t.addr   = 0x00;            // control byte: common registers, read, variable length
  t.length = 8;
  if (spi_device_polling_transmit(spi, &t) != ESP_OK) return false;
  v = t.rx_data[0];
  return true;
}

static void hardResetW5500(int rstPin) {
  if (rstPin < 0) return;
  gpio_reset_pin((gpio_num_t)rstPin);
  gpio_set_direction((gpio_num_t)rstPin, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)rstPin, 0);
  vTaskDelay(pdMS_TO_TICKS(100));                        // same timing as the working reference firmware: 100 ms low ...
  gpio_set_level((gpio_num_t)rstPin, 1);
  vTaskDelay(pdMS_TO_TICKS(300));                        // ... then give the chip time to start (reference waits 500 ms)
}

static String versionVerdict(uint8_t v, bool read) {
  char b[200];
  if (!read) return "SPI transfer failed (pins or bus problem)";
  if (v == 0x04) return "W5500 answered (chip version 0x04): the SPI wiring and pins are correct";
  if (v == 0xFF) return "NO ANSWER (read 0xFF, data line stayed high): wrong SCLK/MISO/MOSI/CS pin, no 3.3 V on the W5500, RST held low, or a different Ethernet chip";
  if (v == 0x00) return "NO ANSWER (read 0x00, data line stayed low): MISO wrong or shorted to ground, wrong pins, or a different Ethernet chip";
  snprintf(b, sizeof(b), "unexpected answer 0x%02X (a W5500 reads 0x04): wrong chip, wrong pins, or noisy wiring - try a lower SPI clock", v);
  return String(b);
}

// Probe one pin set on SPI3 (the LAN driver itself uses SPI2). Never call while the LAN driver is running.
static bool probePins(int sclk, int miso, int mosi, int cs, int rst, int mhz, String& text) {
  spi_bus_config_t bus = {};
  bus.miso_io_num = miso; bus.mosi_io_num = mosi; bus.sclk_io_num = sclk;
  bus.quadwp_io_num = -1; bus.quadhd_io_num = -1;
  if (spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) { text = "could not use these pins for SPI"; return false; }
  spi_device_interface_config_t dev = {};
  dev.command_bits = 16; dev.address_bits = 8; dev.mode = 0;
  dev.clock_speed_hz = mhz * 1000 * 1000; dev.spics_io_num = cs; dev.queue_size = 1;
  spi_device_handle_t h = nullptr;
  if (spi_bus_add_device(SPI3_HOST, &dev, &h) != ESP_OK) { spi_bus_free(SPI3_HOST); text = "could not add the SPI device"; return false; }
  hardResetW5500(rst);
  uint8_t v = 0;
  bool read = false, ok = false;
  for (int i = 0; i < 3 && !ok; i++) { vTaskDelay(pdMS_TO_TICKS(i ? 40 : 100)); read = readW5500Version(h, v); ok = read && v == 0x04; }
  text = versionVerdict(v, read);
  spi_bus_remove_device(h);
  spi_bus_free(SPI3_HOST);
  return ok;
}

String ethScanPresetsJson() {
  if (sStarted) return "{\"allowed\":false,\"message\":\"The LAN chip is already running - nothing to scan.\",\"results\":[]}";
  struct Set { const char* name; int sclk, miso, mosi, cs, intr, rst; };
  NetCfg c;
  settingsGetNet(c);
  const Set sets[5] = {
    {"Saved pins (what is set on this page)", c.pSclk, c.pMiso, c.pMosi, c.pCs, c.pInt, c.pRst},
    {"Minew standalone gateway (from the reference firmware, no INT)", 41, 40, 39, 42, -1, 37},
    {"ESP32-S3 module + W5500, FSPI pins", 12, 13, 11, 10, 4, 5},
    {"Waveshare ESP32-S3-ETH", 13, 12, 11, 14, 10, 9},
    {"LilyGO T-ETH-Lite ESP32-S3", 10, 11, 12, 9, 13, 14},
  };
  String j = "{\"allowed\":true,\"message\":\"\",\"results\":[";
  j = "{\"allowed\":true,\"message\":\"\",\"results\":[";
  for (int i = 0; i < 5; i++) {
    String text;
    const bool ok = probePins(sets[i].sclk, sets[i].miso, sets[i].mosi, sets[i].cs, sets[i].rst, 10, text);
    if (i) j += ",";
    j += "{\"name\":\"" + String(sets[i].name) + "\",\"pins\":[" + String(sets[i].sclk) + "," + String(sets[i].miso) + "," + String(sets[i].mosi) + "," +
         String(sets[i].cs) + "," + String(sets[i].intr) + "," + String(sets[i].rst) + "],\"ok\":" + String(ok ? "true" : "false") + ",\"text\":\"" + text + "\"}";
    if (ok) Console.printf("LAN scan: W5500 found with preset '%s'\n", sets[i].name);
  }
  j += "]}";
  return j;
}

// ---------------------------------------------------------------- start
void ethBegin() {
  settingsGetNet(cfg);
  initError = "";

  // TCP/IP stack and the default event loop (the WiFi start-up may have created them already: that is fine)
  esp_netif_init();
  esp_err_t er = esp_event_loop_create_default();
  if (er != ESP_OK && er != ESP_ERR_INVALID_STATE) { initError = "event loop"; Console.printf("LAN: event loop error %d\n", (int)er); return; }

  gpio_install_isr_service(0);                                   // needed for the W5500 interrupt pin (may already be installed)

  spi_bus_config_t bus = {};
  bus.miso_io_num   = cfg.pMiso;
  bus.mosi_io_num   = cfg.pMosi;
  bus.sclk_io_num   = cfg.pSclk;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  er = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
  if (er != ESP_OK && er != ESP_ERR_INVALID_STATE) { initError = "SPI bus"; Console.printf("LAN: SPI bus init failed (err %d) - check pins in config.h\n", (int)er); return; }

  spi_device_interface_config_t dev = {};
  dev.command_bits    = 16;                                      // W5500 SPI frame: 16 bit address phase + 8 bit control
  dev.address_bits    = 8;
  dev.mode            = 0;
  dev.clock_speed_hz  = (int)cfg.spiMhz * 1000 * 1000;
  dev.spics_io_num    = cfg.pCs;
  dev.queue_size      = 20;
  spi_device_handle_t spi = nullptr;
  er = spi_bus_add_device(SPI2_HOST, &dev, &spi);
  if (er != ESP_OK) { initError = "SPI device"; Console.printf("LAN: SPI device add failed (err %d)\n", (int)er); return; }

  // Is a W5500 really answering on these pins? (reads its version register; expected 0x04)
  {
    hardResetW5500(cfg.pRst);
    uint8_t v = 0;
    bool read = false, ok = false;
    for (int i = 0; i < 4 && !ok; i++) { vTaskDelay(pdMS_TO_TICKS(i ? 60 : 120)); read = readW5500Version(spi, v); ok = read && v == 0x04; }
    probeText = versionVerdict(v, read);
    Console.printf("LAN: %s\n", probeText.c_str());
    if (!ok) {
      initError = "W5500 not answering";
      Console.println("LAN: not started. Settings page -> Network interface -> \"Find the LAN chip\" tries the known pin sets; check the pins of your board.");
      spi_bus_remove_device(spi);
      spi_bus_free(SPI2_HOST);
      return;
    }
  }

  // ESP-IDF 5.x: the driver adds its own SPI device, so release the one used for the probe first
  spi_bus_remove_device(spi);
  spi = nullptr;

  eth_w5500_config_t w5500 = ETH_W5500_DEFAULT_CONFIG(SPI2_HOST, &dev);
  const bool polled = (cfg.pInt < 0);
  w5500.int_gpio_num = polled ? -1 : cfg.pInt;                     // -1 = no interrupt pin: the driver polls the chip instead
  w5500.poll_period_ms = polled ? 10 : 0;
  eth_mac_config_t macCfg = ETH_MAC_DEFAULT_CONFIG();
  macCfg.rx_task_stack_size = 4096;
  esp_eth_mac_t* mac = esp_eth_mac_new_w5500(&w5500, &macCfg);
  eth_phy_config_t phyCfg = ETH_PHY_DEFAULT_CONFIG();
  phyCfg.phy_addr = 1;
  phyCfg.reset_gpio_num = cfg.pRst;
  esp_eth_phy_t* phy = esp_eth_phy_new_w5500(&phyCfg);
  if (!mac || !phy) { initError = "W5500 driver"; Console.println("LAN: W5500 driver could not be created"); return; }

  esp_eth_config_t ecfg = ETH_DEFAULT_CONFIG(mac, phy);
  er = esp_eth_driver_install(&ecfg, &ethHandle);
  if (er != ESP_OK) {
    initError = "W5500 not found";
    Console.printf("LAN: W5500 not found (err %d). Check the chip is powered (3.3 V), the wiring, and the LAN chip pins on Settings -> Network interface.\n", (int)er);
    return;
  }

  uint8_t m[6];
  currentMac(cfg, m);                                            // the W5500 has no factory MAC: automatic (from the chip) or the fixed one
  esp_eth_ioctl(ethHandle, ETH_CMD_S_MAC_ADDR, m);
  Console.printf("LAN: MAC address %02X:%02X:%02X:%02X:%02X:%02X (%s)\n", m[0], m[1], m[2], m[3], m[4], m[5], cfg.macMode ? "fixed" : "automatic");

  // In "LAN first" mode the LAN gets a higher route priority than WiFi (WiFi STA = 100, LAN default = 50),
  // so lwIP chooses the cable whenever both have an address.
  static esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
  base.route_prio = (cfg.mode == NET_AUTO) ? 150 : 50;
  esp_netif_config_t ncfg = { &base, nullptr, ESP_NETIF_NETSTACK_DEFAULT_ETH };
  ethNetif = esp_netif_new(&ncfg);
  if (!ethNetif) { initError = "netif"; Console.println("LAN: could not create network interface"); return; }
  esp_netif_set_hostname(ethNetif, HOSTNAME);
  esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(ethHandle);
  esp_netif_attach(ethNetif, glue);

  esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &onEthEvent, nullptr);
  esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &onEthEvent, nullptr);
  esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, &onEthEvent, nullptr);

  if (cfg.ipMode == ETHIP_STATIC) applyStaticAddress();

  er = esp_eth_start(ethHandle);
  if (er != ESP_OK) { initError = "start"; Console.printf("LAN: start failed (err %d)\n", (int)er); return; }
  sStarted = true;
  if (polled) {
    xTaskCreatePinnedToCore(ethPollTask, "ethPoll", 2048, nullptr, 3, nullptr, 0);
    Console.println("LAN: INT pin not used -> the W5500 is polled every 4 ms (no INT wire needed)");
  }
  Console.printf("LAN: W5500 on SPI @%u MHz (SCLK=%d MISO=%d MOSI=%d CS=%d INT=%d RST=%d), address mode: %s\n",
                (unsigned)cfg.spiMhz, cfg.pSclk, cfg.pMiso, cfg.pMosi, cfg.pCs, cfg.pInt, cfg.pRst,
                cfg.ipMode == ETHIP_DHCP ? "DHCP" : cfg.ipMode == ETHIP_STATIC ? "static" : "DHCP, then static");
  xTaskCreatePinnedToCore(ethTask, "ethTask", 4096, nullptr, 1, nullptr, 0);
}

bool ethStarted()             { return sStarted; }
bool ethLinkUp()              { return sStarted && sLink; }
bool ethHasIp()               { return sStarted && sLink && sIp; }
bool ethUsingStaticFallback() { return sFallback; }
String ethIpString()          { return ethHasIp() ? IPAddress(sIpAddr).toString() : String("0.0.0.0"); }
String ethGatewayString()     { return ethHasIp() ? IPAddress(sGw).toString() : String("0.0.0.0"); }
String ethMaskString()        { return ethHasIp() ? IPAddress(sMask).toString() : String("0.0.0.0"); }

String ethMacString() {
  uint8_t m[6];
  NetCfg c;
  settingsGetNet(c);
  currentMac(c, m);
  char b[20];
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  return String(b);
}

String ethDnsString() {
  if (!ethNetif) return "0.0.0.0";
  esp_netif_dns_info_t d;
  if (esp_netif_get_dns_info(ethNetif, ESP_NETIF_DNS_MAIN, &d) != ESP_OK) return "0.0.0.0";
  return IPAddress(d.ip.u_addr.ip4.addr).toString();
}

String ethProbeText() { return probeText; }

String ethStateText() {
  if (!sStarted) return initError.length() ? String("LAN failed to start: ") + initError + " - see the LAN chip line below" : String("LAN not started");
  if (!sLink)    return "no link (cable not plugged in or other end is off)";
  if (!sIp)      return String("link up, waiting for DHCP (") + String((millis() - sLinkUpMs) / 1000) + " s)";
  if (sFallback) return "connected, static address (DHCP did not answer)";
  return (cfg.ipMode == ETHIP_STATIC) ? "connected, static address" : "connected, address from DHCP";
}

#else   // ETH_ENABLED == 0 : LAN left out

void   ethBegin()             {}
bool   ethStarted()           { return false; }
bool   ethLinkUp()            { return false; }
bool   ethHasIp()             { return false; }
bool   ethUsingStaticFallback() { return false; }
String ethIpString()          { return "0.0.0.0"; }
String ethGatewayString()     { return "0.0.0.0"; }
String ethMaskString()        { return "0.0.0.0"; }
String ethMacString()         { return "00:00:00:00:00:00"; }
String ethStateText()         { return "LAN not built in (ETH_ENABLED 0)"; }
String ethDnsString()         { return "0.0.0.0"; }
String ethProbeText()         { return "LAN not built in"; }
String ethScanPresetsJson()   { return "{\"allowed\":false,\"message\":\"LAN not built in (ETH_ENABLED 0)\",\"results\":[]}"; }

#endif
