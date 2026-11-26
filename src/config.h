/*
 * config.h  -  ALL compile-time settings of the project in ONE place.
 *
 * Things you change here need a rebuild + upload.
 * Things that can be changed at run time from the web page (WiFi networks, server IP/port/URL,
 * header key) are stored in flash by settings.cpp and do NOT need a rebuild.
 */
#pragma once
#include "gw_compat.h"
#define FW_VERSION           "1.3.6"

// =====================================================================
// 1. PUMP UART  (Agilia serial export protocol: fixed 115200 baud, 8N1)
// =====================================================================
// The gateway's UART line = module UART0 (ESP32-S3-WROOM-1 datasheet):
//     pad 36  RXD0  U0RXD  GPIO44   <-  the PUMP'S TX goes here
//     pad 37  TXD0  U0TXD  GPIO43   <-  not used: this firmware only listens (left high-impedance so it can never fight the pump)
// UART0 is also the debug console, so the console is switched OFF while the pump uses it (see PUMP_CONSOLE_ON).
#define PUMP_RX_PIN          44           // ESP32-S3 GPIO44 = RXD0 (module pad 36)  <-  pump TX
#define PUMP_TX_PIN          -1           // not used (receive only); -1 = no TX pin assigned
#define PUMP_BAUD            115200 
// Defaults only: RX/TX pin, baud rate and "find the line automatically" are changed on the web page (Settings -> Pump serial input).
#define PUMP_CONSOLE_ON      0            // 1 = keep the serial console (TXD0, GPIO43) running while the pump uses GPIO44. ONLY when nothing but your USB-UART adapter's RX is on TXD0: the debug text would otherwise go into the pump
#define PUMP_AUTO_FIND       1            // 1 = if no valid pump frame arrives within PUMP_AUTO_FIND_DELAY_S after start-up, listen on the likely pins once and use the one where the pump is found
#define PUMP_AUTO_FIND_DELAY_S  30
// (the pump line is always 8 data bits, no parity, 1 stop bit)
#define UART_RX_BUF_SIZE     4096

// =====================================================================
// 2. WEB PAGE LOGIN   (protects dashboard, settings and OTA)
// =====================================================================
#define WEB_USER             "admin"
#define WEB_PASS             "admin123"   // CHANGE THIS
#define HOSTNAME             "mydevice"   // => http://mydevice.local

// =====================================================================
// 3. WIFI
//    Networks added on the web page (Settings) are saved in flash and tried first.
//    The built-in list below is the fallback (set USE_DEFAULT_WIFI 0 to disable it).
// =====================================================================
struct WifiCred { const char* ssid; const char* pass; };
#define WIFI_ENABLED         1            // 0 = NO network at all (no WiFi, no LAN, no web page, no upload), only UART decode on the console (for fault finding)
#define USE_DEFAULT_WIFI     1
static const WifiCred DEFAULT_WIFI_LIST[] = {
  { "LS_ABCDE", "copernicus" },
  { "LSPL20",   "LspL@#20"   },
};

// Setup hotspot: if no WiFi network can be joined for AP_START_DELAY_MS, the ESP32 starts its own
// hotspot so you can open http://192.168.4.1 and type a new SSID / password.
#define AP_FALLBACK_ENABLE   1
#define AP_SSID              "Agilia-Router-Setup"
#define AP_PASS              "agilia1234"   // 8..63 characters
#define AP_START_DELAY_MS    30000

// =====================================================================
// 2b. THE ESP32-S3 MODULE  (ESP32-S3-WROOM-1U on your own board - not a DevKit)
//     Flash size is chosen by the PlatformIO ENVIRONMENT (platformio.ini: wroom1u_4mb / wroom1u_8mb / wroom1u_16mb),
//     not here. Read the letters on the module label:  ESP32-S3-WROOM-1U-N8R2 = 8 MB flash (N8), 2 MB quad PSRAM (R2).
//       N4 = 4 MB, N8 = 8 MB, N16 = 16 MB flash.   R2 = quad PSRAM,   R8 = OCTAL PSRAM.
//     This firmware does not use PSRAM, but an octal PSRAM module (R8) takes GPIO35-37 for itself.
// =====================================================================
#define MODULE_PSRAM_OCTAL   0            // 1 = module name ends in R8 (N16R8 / N8R8): GPIO35-37 are not usable
// The console (debug output + programming) is UART0 = TXD0 GPIO43 / RXD0 GPIO44 -> connect a 3.3 V USB-UART adapter there.
// To use the native USB pins GPIO19/20 for it instead, set CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y in sdkconfig.defaults and CONSOLE_ON_USB to 1 below.
// ESP-IDF: the console (printf / log output) is UART0 (CONFIG_ESP_CONSOLE_UART_DEFAULT in sdkconfig.defaults).
#define CONSOLE_ON_USB       0
#ifndef BUILD_FLASH_MB
  #define BUILD_FLASH_MB     0            // set by the platformio.ini environment (4 / 8 / 16); 0 = unknown
#endif

// =====================================================================
// 3b. NETWORK INTERFACE  (WiFi and/or LAN cable)
//     Which interface is used is chosen on the web page (Settings -> Network interface)
//     and saved in flash. NET_DEFAULT_MODE is only used until you save something there.
//       0 = WiFi only (as in firmware 1.1.x)
//       1 = LAN cable (Ethernet) only - the WiFi radio is not started at all (no radio, no hotspot)
//       2 = LAN first, WiFi as backup (hotspot is not opened while the LAN is working)
//
//     The ESP32-S3 has NO built-in Ethernet MAC. A LAN port needs an SPI Ethernet chip: the W5500
//     (a WIZnet W5500 wired to the module on your own board, or boards such as Waveshare ESP32-S3-ETH / LilyGO T-ETH-Lite-S3).
// =====================================================================
#define NET_DEFAULT_MODE     1            // this build: LAN only. A fresh device AND a device after any network reset start like this.
#define ETH_ENABLED          1            // 0 = LAN code is left out completely (smaller build, WiFi only)

// Default pin set of the W5500 (WIZnet W5500 on SPI). These are only the DEFAULTS: the pins can be changed on the
// web page (Settings -> Network interface -> LAN chip pins) without a rebuild. CHECK THEM AGAINST YOUR WIRING.
//   4 = Minew standalone gateway (pins from the working reference firmware, no INT)
//   3 = ESP32-S3-WROOM-1(U) module + WIZnet W5500, FSPI pins GPIO10-13 + INT 4 / RST 5 (wiring table in the README)
//   1 = Waveshare ESP32-S3-ETH        2 = LilyGO T-ETH-Lite ESP32-S3        0 = your own wiring (edit the pins below)
#define ETH_BOARD            4
#if   ETH_BOARD == 4
  // Minew standalone BLE gateway (ESP32-S3-WROOM-1 N8R2): taken from the working LAN reference firmware
  // (esp32s3_ble_gateway_standalone: ETH_SCK 41, ETH_MISO 40, ETH_MOSI 39, ETH_CS 42, ETH_RST 37).
  // That firmware never uses the W5500 INT pin, so there is none: this firmware polls the chip instead.
  // GPIO39-42 are the JTAG pins of the S3; they work as normal GPIO here (JTAG debugging is not available).
  // GPIO37 is only usable on modules WITHOUT octal PSRAM (N8R2 = quad PSRAM = fine; N16R8 / N8R8 = not fine).
  #define ETH_PIN_SCLK       41
  #define ETH_PIN_MISO       40
  #define ETH_PIN_MOSI       39
  #define ETH_PIN_CS         42
  #define ETH_PIN_INT        -1           // -1 = INT not wired: the firmware wakes the driver every 4 ms instead
  #define ETH_PIN_RST        37
#elif ETH_BOARD == 3
  #define ETH_PIN_SCLK       12           // = FSPI default pins of the ESP32-S3 (GPIO10-13): fastest SPI path
  #define ETH_PIN_MISO       13
  #define ETH_PIN_MOSI       11
  #define ETH_PIN_CS         10
  #define ETH_PIN_INT        4            // W5500 INT is required by the driver
  #define ETH_PIN_RST        5            // W5500 RSTn (-1 if not wired; then RSTn needs its own power-on reset / pull-up)
#elif ETH_BOARD == 1
  #define ETH_PIN_SCLK       13
  #define ETH_PIN_MISO       12
  #define ETH_PIN_MOSI       11
  #define ETH_PIN_CS         14
  #define ETH_PIN_INT        10
  #define ETH_PIN_RST        9
#elif ETH_BOARD == 2
  #define ETH_PIN_SCLK       10
  #define ETH_PIN_MISO       11
  #define ETH_PIN_MOSI       12
  #define ETH_PIN_CS         9
  #define ETH_PIN_INT        13
  #define ETH_PIN_RST        14
#else
  #define ETH_PIN_SCLK       12
  #define ETH_PIN_MISO       13
  #define ETH_PIN_MOSI       11
  #define ETH_PIN_CS         10
  #define ETH_PIN_INT        4
  #define ETH_PIN_RST        -1         // -1 = reset pin not connected
#endif
#define ETH_SPI_MHZ          14           // default SPI clock of the W5500 in MHz (the reference firmware's Ethernet library uses 14)
#define ETH_REF_MAC          "DE:AD:BE:EF:FE:ED"   // the fixed MAC the reference firmware uses; offered on the Settings page
#define ETH_DUMMY_INT_PIN    34           // when INT is not wired the driver still needs a pin: GPIO34 is not on the WROOM-1 pads (unconnected)

// LAN address defaults (until saved on the Settings page).
//   "DHCP then static" = ask a DHCP server first; if none answers within ETH_DHCP_TIMEOUT_MS use the static address.
//   That makes a DIRECT cable to a Windows PC work: set the PC's adapter to 192.168.50.1 / 255.255.255.0.
#define ETH_DEFAULT_IP_MODE  2            // 0 = DHCP, 1 = static, 2 = DHCP then static
#define ETH_DEFAULT_IP       "192.168.50.2"
#define ETH_DEFAULT_MASK     "255.255.255.0"
#define ETH_DEFAULT_GW       "192.168.50.1"
#define ETH_DEFAULT_DNS      "192.168.50.1"
#define ETH_DHCP_TIMEOUT_MS  30000      // default of the page field "Wait for DHCP" (office switches can take 30 s to open a port)

// NTP time servers. On a LAN with no internet they cannot be reached: "received_at" is then left out of the JSON
// (the gateway uptime is always there). Use your hospital NTP server here if you have one.
#define NTP_SERVER_1         "pool.ntp.org"
#define NTP_SERVER_2         "time.google.com"

// Lock-out protection: hold this pin LOW for NET_RESET_HOLD_MS while the firmware is running to put the network
// settings back to NET_DEFAULT_MODE + DHCP (use it if a wrong LAN address made the web page unreachable).
// GPIO0 = the BOOT button. On your own board with a WROOM-1U this only works if you fitted a push button from GPIO0 to GND
// (you need it anyway to enter download mode). No button on GPIO0? Use another pin with a button, or -1 = feature off.
#define NET_RESET_PIN        0
#define NET_RESET_HOLD_MS    5000

// Reset with NO button, NO serial cable and NO knowledge of the IP address: switch the gateway's power OFF and ON
// NET_POWERCYCLE_COUNT times in a row, each time cutting the power within NET_POWERCYCLE_WINDOW_MS of switching on.
// The last power-on resets the network settings (LAN-only, DHCP then 192.168.50.2) and the gateway restarts by itself.
// 0 = feature off. Use 3 or more. NOTE: a supply that keeps browning out and restarting counts as power cycles too.
#define NET_POWERCYCLE_COUNT      5
#define NET_POWERCYCLE_WINDOW_MS  8000

// =====================================================================
// 4. SERVER (default values, used until you save something on the Settings page)
// =====================================================================
#define DEFAULT_SERVER_SCHEME "https"       // "http" or "https"
#define DEFAULT_SERVER_HOST   "nexus.api.lifesigns.us"   // IP address or domain name ("" = sending disabled)
#define DEFAULT_SERVER_PORT   443           // 0 = default port of the scheme
#define DEFAULT_SERVER_PATH   "/gw/api/v1/public/external-device/niso/data"

// =====================================================================
// 5. UPLOAD BEHAVIOUR
// =====================================================================
#define SEND_ON_CHANGE       1          // send at once when alarm/step/mode/flow rate/drug/syringe change
#define SEND_MIN_GAP_MS      500        // minimum gap between two change-triggered sends
#define SEND_INTERVAL_MS     5000       // otherwise send the latest status at this period
#define SEND_ADVANCED        1          // also post 0xDF advanced-data frames (only present if requested)
#define SEND_PATIENT_DATA    0          // 1 = include patient name/ID (only MC/TIVA pumps) -> personal data!
#define HTTP_TIMEOUT_MS      8000
#define HTTPS_ROOT_CA        nullptr    // nullptr = do NOT verify the server certificate
#define QUEUE_LEN            8

// =====================================================================
// 5b. HL7 PATIENT MONITOR INPUT  (hl7_monitor.*, hl7_parse.*)   works on WiFi and on the LAN cable
//     The patient monitor (or tools/hl7_monitor_sim.py) connects to this gateway on TCP HL7_PORT and sends HL7 v2 ORU^R01
//     messages in MLLP framing. The gateway ACKs each one, shows it on http://<gateway>/monitor and uploads the JSON.
// =====================================================================
#define HL7_ENABLED            1
#define HL7_PORT               2575         // 2575 = the IANA-registered HL7 port; match what the monitor is set to send to
#define HL7_IDLE_TIMEOUT_S     120          // drop a connection that sends nothing for this long (the monitor reconnects)
#define HL7_MAX_MSG_BYTES      16384
#define HL7_SEND_INTERVAL_MS   5000         // upload to the server at most this often (the dashboard always shows every message)
#define HL7_KEEP_PATIENT       0            // 1 = keep PID name/ID in JSON -> personal data!

// =====================================================================
// 5c. INFUSION PUMP + PCA PUMP INPUTS  (multi_dev.*, dev_proto.*)   works on WiFi and on the LAN cable
//     Two more devices next to the Agilia syringe pump and the HL7 monitor (HL7 uses TCP only, no UART). Each one can be fed two ways, at the same time:
//       * its own UART line (real hardware, receive only)
//       * a TCP port (the PC simulator tools/device_sim.py connects here - no hardware needed)
//     UART map (ESP32-S3 has 3 UARTs):  UART0 = Agilia syringe pump (GPIO44 RX; the console is off while the pump uses these pins)
//                                       UART1 = infusion pump (GPIO17 TX / GPIO18 RX)
//                                       UART2 = PCA pump      (GPIO6 TX / GPIO7 RX)
//     If the console is kept ON (custom pump pins), the syringe pump falls back to UART1 and the infusion UART stays off (TCP still works).
//     The pump pin finder (Settings page) also needs a UART (UART2) and is therefore off while the PCA UART is enabled.
//     With nothing connected the UART lines just stay idle; the simulators use the TCP ports.
// =====================================================================
#define DEV_INF_ENABLED        1
#define DEV_INF_TCP_ENABLED    0            // 1 = also accept the PC simulator over TCP (0 = UART line only, no socket opened)
#define DEV_INF_TCP_PORT       2601         // PC simulator -> infusion pump input
#define DEV_INF_UART_ENABLED   1            // read the real UART1 line too
#define DEV_INF_UART_NUM       1            // UART_NUM_1
#define DEV_INF_TX_PIN         17
#define DEV_INF_RX_PIN         18
#define DEV_INF_BAUD           115200

#define DEV_PCA_ENABLED        1
#define DEV_PCA_TCP_ENABLED    0            // 1 = also accept the PC simulator over TCP (0 = UART line only, no socket opened)
#define DEV_PCA_TCP_PORT       2602         // PC simulator -> PCA pump input
#define DEV_PCA_UART_ENABLED   1            // read the real UART2 line too
#define DEV_PCA_UART_NUM       2            // UART_NUM_2
#define DEV_PCA_TX_PIN         6
#define DEV_PCA_RX_PIN         7
#define DEV_PCA_BAUD           115200

#define DEV_IDLE_TIMEOUT_S     120          // drop a TCP connection that sends nothing for this long
#define DEV_SEND_INTERVAL_MS   5000         // upload to the server at most this often per device; a state / alarm change is sent at once
#define DEV_MAX_LINE           256

// =====================================================================
// 6. DEBUG / LOG
// =====================================================================
#define PRINT_JSON_SERIAL    1          // print every decoded JSON on USB serial
#define RAW_DUMP_SERIAL      0          // 1 = print raw received bytes in HEX (wiring / baud debugging)
#define LOG_LINES            30         // frames kept for the web log

// =====================================================================
// 7. CONNECTED DEVICE + FIRMWARE UPDATE BY MODEL / SERIAL NUMBER
//    The gateway learns what it is connected to from the pump's own frames
//    (Injectomat Agilia = syringe pump, Volumat Agilia = infusion pump).
//    Devices it cannot decode (patient monitor ...) are typed in on the Settings page.
//
//    Server-managed update ("fleet OTA"): the gateway asks YOUR server which firmware it should run, telling it
//    its device class / model / serial number. The server answers per rule (see test server README).
//    It is OFF until you switch it on at /ota. Mode on the web page: 0 off, 1 check only, 2 install automatically.
// =====================================================================
#define FLEET_DEFAULT_MODE         0
#define FLEET_DEFAULT_PATH         "/api/ota/check"
#define FLEET_DEFAULT_INTERVAL_MIN 10      // how often to ask the server
#define FLEET_FIRST_CHECK_MS       60000   // first check this long after boot
#define FLEET_REQUIRE_LIVE_DEVICE  1       // 1 = only check/update while a device frame was seen in the last FLEET_LIVE_WINDOW_MS
#define FLEET_LIVE_WINDOW_MS       600000  //     (so the gateway never applies a rule for a device that is no longer connected)
#define FLEET_AVOID_INFUSING       1       // 1 = automatic install waits while the pump reports "infusion in progress"
#define FLEET_MAX_IMAGE_BYTES      1310720 // refuse images bigger than this (one app slot of default.csv = 0x140000 = 1,310,720 bytes)

// =====================================================================
// 8. MQTT FIRMWARE UPDATE  (mqtt_ota.*)   works on WiFi and on the LAN cable
//    The broker carries the COMMAND ({"cmd":"update","version":..,"url":..,"sha256":..,"size":..}); the image is
//    downloaded from the url over HTTP(S), checked (SHA-256 + size) and then activated. See mqtt_ota.h for topics / format.
//    Topics: <PREFIX>/<gw-id>/ota/cmd (in), <PREFIX>/all/ota/cmd (in), <PREFIX>/<gw-id>/ota/status (out), <PREFIX>/<gw-id>/status (out).
// =====================================================================
#define MQTT_OTA_ENABLED       1
#define MQTT_OTA_BROKER_URI    "mqtt://172.16.22.201:1883"  // mqtt://host:1883 or mqtts://host:8883 ("" = MQTT update off)
#define MQTT_OTA_USER          ""                           // "" = no user name
#define MQTT_OTA_PASS          ""
#define MQTT_OTA_TOPIC_PREFIX  "agilia-gw"
#define MQTT_OTA_KEEPALIVE_S   60
#define MQTT_OTA_ROOT_CA       nullptr                      // nullptr = broker certificate of mqtts:// is NOT verified (like HTTPS_ROOT_CA)
