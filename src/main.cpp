/*
 * Injectomat Agilia -> ESP32-S3 : protocol decoder + JSON uploader + WiFi / LAN + OTA
 *
 * WIRING (receive only):
 *   Pump TX  -> ESP32-S3 RX pin (Settings -> Pump serial input; default PUMP_RX_PIN in config.h)
 *   Pump GND -> ESP32-S3 GND
 *   3.3 V logic only! If the pump cable is RS-232 (+/-V) use a MAX3232 level shifter.
 *   Pump serial parameters are fixed by the protocol: 115200 baud, 8N1.
 *
 * PROJECT MAP  (where to change what)
 * ---------------------------------------------------------------------------------------
 *  config.h         all compile-time settings: pins, login, default WiFi, default server, send timing, debug
 *  settings.*       settings saved in flash from the web page: WiFi networks, server IP/port/path/header
 *  net_manager.*    chooses WiFi, LAN cable or both (Settings -> Network interface); mDNS + NTP for the LAN
 *  eth_lan.*        LAN cable through a W5500 SPI chip: DHCP / static / DHCP-then-static
 *  wifi_manager.*   WiFi connect (saved + built-in networks), setup hotspot fallback, mDNS, NTP, scan
 *  device_id.*      what the gateway is connected to: syringe pump / infusion pump (detected) or typed in (monitor ...)
 *  fleet_logic.*    pure C++: wildcard match, version compare, URL / JSON helpers (unit-tested on a PC)
 *  fleet_ota.*      server-managed firmware update chosen by device class / model / serial number
 *  pump_reader.*    UART -> frames -> decode -> JSON, latest JSON + frame log, "when to send" decision
 *  agilia.*         Agilia protocol: CRC, frame parser, 0x80 / 0xDF decoder, JSON builder (no Arduino code)
 *  http_sender.*    queue + task that POSTs the JSON to the server
 *  web_pages.h      all HTML / CSS / JavaScript pages (Dashboard, Settings, Firmware update)
 *  web_server.*     web routes: pages and /api/... handlers, login
 *  ota_update.*     firmware upload handlers (OTA) with the model / serial number guard
 *  utils.*          small string helpers
 *
 * DATA FLOW
 *   pump TX -> UART -> pump_reader (agilia parser/decoder) -> JSON -> http_sender -> WiFi or LAN -> your server
 *                                         \-> latest JSON / log -> web_server -> browser
 *                                         \-> device_id (model, serial) -> fleet_ota / OTA guard
 * ---------------------------------------------------------------------------------------
 * First upload by cable, afterwards use http://mydevice.local/ota (or the LAN address)
 */

#include "gw_compat.h"
#include "config.h"
#include "settings.h"
#include "pump_reader.h"
#include "http_sender.h"
#include "wifi_manager.h"
#include "net_manager.h"
#include "device_id.h"
#include "fleet_ota.h"
#include "mqtt_ota.h"
#include "diag.h"
#include "pump_scan.h"
#include "hl7_monitor.h"
#include "multi_dev.h"
#include "web_server.h"
#include "esp_chip_info.h"
#include "esp_event.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs_flash.h"

static const char* resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "POWERON (power was applied / supply dropped / EN pin pulled low)";
    case ESP_RST_EXT:       return "EXT (external reset pin)";
    case ESP_RST_SW:        return "SW (esp_restart, e.g. after OTA or WiFi save)";
    case ESP_RST_PANIC:     return "PANIC (program crash - see Guru Meditation text above)";
    case ESP_RST_INT_WDT:   return "INT_WDT (interrupt watchdog)";
    case ESP_RST_TASK_WDT:  return "TASK_WDT (task watchdog)";
    case ESP_RST_WDT:       return "WDT (other watchdog)";
    case ESP_RST_BROWNOUT:  return "BROWNOUT (supply voltage too low - weak USB cable / port / power)";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    default:                return "other / unknown";
  }
}

static uint32_t flashSizeMB() {
  uint32_t sz = 0;
  if (esp_flash_get_size(nullptr, &sz) != ESP_OK) return 0;
  return sz / (1024 * 1024);
}

static void gwSetup() {
  // settings live in NVS: same partition and namespace as in the earlier firmware, so saved settings are kept
  esp_err_t ne = nvs_flash_init();
  if (ne == ESP_ERR_NVS_NO_FREE_PAGES || ne == ESP_ERR_NVS_NEW_VERSION_FOUND) { nvs_flash_erase(); nvs_flash_init(); }
  esp_netif_init();
  esp_event_loop_create_default();

  Console.println("\n=== Injectomat Agilia decoder + JSON uploader + OTA (ESP32-S3) ===");
  Console.println("FW " FW_VERSION);
  Console.printf("Reset reason: %s\n", resetReasonText());
  // Module report: handy when bringing up your own board. A build for a bigger flash than the chip has would not boot.
  esp_chip_info_t chip;
  esp_chip_info(&chip);
  Console.printf("Chip: %s rev %u, %u core(s), flash %u MB, PSRAM %u kB, free heap %u kB\n", chip.model == CHIP_ESP32S3 ? "ESP32-S3" : "ESP32 (other)", (unsigned)chip.revision,
                (unsigned)chip.cores, (unsigned)flashSizeMB(), (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024),
                (unsigned)(esp_get_free_heap_size() / 1024));
#if BUILD_FLASH_MB > 0
  if (flashSizeMB() > (uint32_t)BUILD_FLASH_MB)
    Console.printf("Note: this module has %u MB flash but this build uses %d MB. Choose the matching PlatformIO environment (wroom1u_8mb / wroom1u_16mb) for bigger OTA slots.\n",
                  (unsigned)flashSizeMB(), BUILD_FLASH_MB);
#endif
#if MODULE_PSRAM_OCTAL
  Console.println("Module setting: octal PSRAM (GPIO35-37 are reserved)");
#endif

  settingsBegin();        // load WiFi list + server + network + device config from flash
  Console.println("[boot] settings loaded tooo");
#if WIFI_ENABLED
  netPowerCycleCheck();   // 5 quick power cycles -> network settings back to the defaults (before the network starts)
#endif
  deviceIdBegin();        // remembered device (model / serial)
  pumpBegin();            // start pump UART
  Console.println("[boot] pump UART started");
  senderBegin();          // start HTTP sender task
  Console.println("[boot] sender task started");
#if WIFI_ENABLED
  netBegin();             // start WiFi and/or LAN cable, as saved on the Settings page
  Console.println("[boot] network started");
  fleetBegin();           // server-managed update task (stays idle while switched off)
  mqttOtaBegin();         // update commanded over MQTT (WiFi or LAN; idle until a network interface has an IP)
  diagBegin();            // connection test (runs only when started from the Settings page)
  hl7Begin();             // HL7 patient-monitor listener (TCP, MLLP); waits by itself until the LAN / WiFi has an IP
  devBegin();             // infusion pump + PCA pump inputs (UART and/or TCP simulator port)
  pumpScanBegin();        // finds the pump line once after start-up if no pump data arrives (can be switched off on the Settings page)
  webBegin();             // start web server
  Console.println("[boot] web server started");
#else
  Console.println("[boot] WIFI_ENABLED 0 -> network, web page and upload are OFF (UART decode only)");
#endif

  ServerCfg c;
  settingsGetServer(c);
  Console.printf("Server: %s\n", c.enabled() ? c.url().c_str() : "(sending disabled)");
}

static void gwLoop() {
  pumpPoll();             // read UART, decode frames
  webLoop();              // serve web pages
#if WIFI_ENABLED
  netResetButtonPoll();   // hold BOOT 5 s -> network settings back to defaults
#endif

  static uint32_t lastStat = 0;
  if (millis() - lastStat >= 15000) {
    lastStat = millis();
    const AgiliaStats& st = pumpStats();
    Console.printf("[stats] bytes=%lu frames=%lu crc8Err=%lu crc16Err=%lu | posts ok=%lu fail=%lu\n",
                  (unsigned long)st.bytes, (unsigned long)st.framesOk, (unsigned long)st.crc8Err,
                  (unsigned long)st.crc16Err, (unsigned long)senderOkCount(), (unsigned long)senderFailCount());
    Console.printf("[net] in use: %s %s\n", netActiveIface(), netActiveIp().c_str());
    if (st.bytes == 0) Console.printf("  -> no bytes from pump: check wiring (pump TX -> GPIO%d), GND, RS-232 level shifter; Settings -> Pump serial input -> Find the pump line\n", pumpRxPin());
    else if (st.framesOk == 0) Console.println("  -> bytes received but no valid frame: check baud 115200 / signal level / noise");
  }
  delay(1);
}

// The Arduino loop() as its own task, with the same body and the same 1 ms rhythm
static void loopTask(void*) {
  for (;;) gwLoop();
}

extern "C" void app_main(void) {
  gwSetup();
  xTaskCreatePinnedToCore(loopTask, "loopTask", 8192, nullptr, 1, nullptr, 1);
}
