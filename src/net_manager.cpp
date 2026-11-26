#include "net_manager.h"
#include "config.h"
#include "settings.h"
#include "eth_lan.h"
#include "wifi_manager.h"
#include "recovery_logic.h"
#include <time.h>
#include "driver/gpio.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "mdns.h"

static uint8_t sMode = NET_WIFI;

uint8_t netMode() { return sMode; }

bool netIsUp() {
  switch (sMode) {
    case NET_ETH:  return ethHasIp();
    case NET_AUTO: return ethHasIp() || wifiIsConnected();
    default:       return wifiIsConnected();
  }
}

const char* netActiveIface() {
  if (sMode != NET_WIFI && ethHasIp()) return "ethernet";
  if (sMode != NET_ETH && wifiIsConnected()) return "wifi";
  return "none";
}

String netActiveIp() {
  if (sMode != NET_WIFI && ethHasIp()) return ethIpString();
  if (sMode != NET_ETH && wifiIsConnected()) return wifiIp().toString();
  return "0.0.0.0";
}

String netActiveMask() {
  if (sMode != NET_WIFI && ethHasIp()) return ethMaskString();
  if (sMode != NET_ETH && wifiIsConnected()) return wifiMask().toString();
  return "0.0.0.0";
}

String netActiveGateway() {
  if (sMode != NET_WIFI && ethHasIp()) return ethGatewayString();
  if (sMode != NET_ETH && wifiIsConnected()) return wifiGateway().toString();
  return "0.0.0.0";
}

String netActiveDns() {
  if (sMode != NET_WIFI && ethHasIp()) return ethDnsString();
  if (sMode != NET_ETH && wifiIsConnected()) return wifiDns().toString();
  return "0.0.0.0";
}

String netMacString() {
  if (sMode == NET_ETH || (sMode == NET_AUTO && ethHasIp())) return ethMacString();
  return wifiMacString();
}

// In LAN modes this task does what wifi_manager does for WiFi: NTP time and the mDNS name.
// (wifi_manager keeps doing it by itself in WiFi-only mode, unchanged.)
void netNtpStart() {
  if (esp_sntp_enabled()) esp_sntp_stop();
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, NTP_SERVER_1);
  esp_sntp_setservername(1, NTP_SERVER_2);
  esp_sntp_init();
}

static bool mdnsRunning = false;

bool netMdnsStart() {
  if (mdnsRunning) return true;
  if (mdns_init() != ESP_OK) return false;
  mdns_hostname_set(HOSTNAME);
  mdns_instance_name_set(HOSTNAME);
  mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
  mdnsRunning = true;
  return true;
}

void netMdnsStop() {
  if (!mdnsRunning) return;
  mdns_free();
  mdnsRunning = false;
}

static void netServicesTask(void*) {
  bool ntpOn = false, mdnsOn = false;
  uint8_t mdnsTries = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    if (!netIsUp()) continue;
    if (!ntpOn) { netNtpStart(); ntpOn = true; }
    if (!mdnsOn && mdnsTries < 10) {
      mdnsTries++;
      if (netMdnsStart()) {
        mdnsOn = true;
        Console.printf("Web page: http://%s.local  (or http://%s)\n", HOSTNAME, netActiveIp().c_str());
      }
    }
  }
}

void netBegin() {
  NetCfg c;
  settingsGetNet(c);
  sMode = c.mode;
#if !ETH_ENABLED
  if (sMode != NET_WIFI) { Console.println("Network: this build has no LAN support (ETH_ENABLED 0) -> using WiFi"); sMode = NET_WIFI; }
#endif
  Console.printf("Network: %s\n", sMode == NET_WIFI ? "WiFi only" : sMode == NET_ETH ? "LAN cable only (WiFi radio stays off)" : "LAN first, WiFi as backup");

  if (sMode != NET_WIFI) ethBegin();
  if (sMode != NET_ETH)  wifiManagerBegin();
  if (sMode != NET_WIFI) xTaskCreatePinnedToCore(netServicesTask, "netSvc", 4096, nullptr, 1, nullptr, 0);
}

// ---- reset by quick power cycles (no tools needed) ----
static bool powerCycleCounted = false;      // a non-zero counter was saved this boot and must be cleared after the window

void netPowerCycleCheck() {
#if NET_POWERCYCLE_COUNT > 0
  const bool powerOn = (esp_reset_reason() == ESP_RST_POWERON);      // power applied or EN pin; software restarts do not count
  bool doReset = false;
  const int prev = settingsNoteGet("pcyc").toInt();
  const int now = recoveryNextCount(prev, powerOn, NET_POWERCYCLE_COUNT, doReset);
  if (doReset) {
    settingsNoteSet("pcyc", "0");
    settingsResetNet();
    Console.printf("\n*** Network settings reset to defaults: power was cycled %d times in a row ***\n", NET_POWERCYCLE_COUNT);
    Console.println("*** Defaults: LAN only, DHCP first, then static 192.168.50.2 / 255.255.255.0. Restarting. Leave the power ON now. ***");
    delay(300);
    gwRestart();
  }
  if (now > 0) {
    settingsNoteSet("pcyc", String(now));
    powerCycleCounted = true;
    Console.printf("Quick power cycle %d of %d (cut the power again within %d s to continue the reset, or just wait)\n", now, NET_POWERCYCLE_COUNT, NET_POWERCYCLE_WINDOW_MS / 1000);
  } else if (prev != 0) {
    settingsNoteSet("pcyc", "0");
  }
#endif
}

// Lock-out protection: a wrong static LAN address can make the web page unreachable. Hold the BOOT button
// (NET_RESET_PIN) for NET_RESET_HOLD_MS while the firmware runs -> network settings return to the config.h defaults.
void netResetButtonPoll() {
#if NET_POWERCYCLE_COUNT > 0
  if (powerCycleCounted && millis() > (uint32_t)NET_POWERCYCLE_WINDOW_MS) {        // survived the window: it was a normal start
    settingsNoteSet("pcyc", "0");
    powerCycleCounted = false;
  }
#endif
#if NET_RESET_PIN >= 0
  static bool inited = false;
  static uint32_t downSince = 0;
  if (!inited) { gpio_reset_pin((gpio_num_t)NET_RESET_PIN); gpio_set_direction((gpio_num_t)NET_RESET_PIN, GPIO_MODE_INPUT); gpio_set_pull_mode((gpio_num_t)NET_RESET_PIN, GPIO_PULLUP_ONLY); inited = true; }
  if (gpio_get_level((gpio_num_t)NET_RESET_PIN) == 0) {
    if (downSince == 0) downSince = millis() ? millis() : 1;
    else if (millis() - downSince >= NET_RESET_HOLD_MS) {
      settingsResetNet();
      Console.println("Network settings reset to defaults (button held) -> rebooting");
      delay(300);
      gwRestart();
    }
  } else {
    downSince = 0;
  }
#endif
}
