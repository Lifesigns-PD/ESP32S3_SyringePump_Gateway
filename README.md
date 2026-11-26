# Injectomat Agilia gateway: ESP-IDF version (PlatformIO)

The same firmware as `UART_Router_Agilia_VSCode_v3.7_uart0` (1.2.4), now **native ESP-IDF 4.4.7, no Arduino**.
Same pins, same web pages and login, same JSON to your server, same settings, same functions and flow. Version number 1.3.0.

ESP32-S3-WROOM-1U (N4 / N8 / N8R2 / N16), VS Code with the PlatformIO extension.

## 1. Build and upload

1. VS Code: **File -> Open Folder** -> this folder. PlatformIO shows the environments in the status bar / PlatformIO tab.
2. Choose the environment that matches your module's flash (default `wroom1u_8mb`, the Minew N8R2):

| Environment | Flash | OTA slots |
|---|---|---|
| `wroom1u_4mb` | 4 MB | 2 x 1.25 MB |
| `wroom1u_8mb` | 8 MB | 2 x 3.2 MB |
| `wroom1u_16mb` | 16 MB | 2 x 6.4 MB |

3. **Build**, **Upload** (by cable: USB-UART adapter on the TTL header, BOOT + RESET), **Monitor** (115200). The first build downloads the ESP-IDF toolchain (a few GB, several minutes).
4. Later updates need no cable: web page `http://<gateway>/ota` -> upload `.pio/build/<environment>/firmware.bin`.

**Coming from the Arduino firmware.** The partition tables have the same layout as the Arduino default tables, so you can simply upload this `firmware.bin` on the **/ota** page of a gateway that runs the old firmware.
Saved settings (WiFi list, server, LAN pins, device, pump pins...) are kept: they live in the same flash area (NVS namespace `cfg`) with the same keys. They also survive a cable upload; they are lost only by "Erase Flash".

## 2. Nothing changed for you

`src/config.h` is the same file: pump UART (GPIO44 RXD0, 115200 8N1), LAN presets (Minew: SCLK 41, MISO 40, MOSI 39, CS 42, RST 37), network modes, WiFi defaults and setup hotspot, server, login `admin / admin123`,
intervals, device identification and server-managed update. Decoder (`agilia.cpp`), update rules (`fleet_logic.cpp`), pin rules (`eth_pins.cpp`) and power-cycle reset (`recovery_logic.cpp`) are **byte-identical** to the Arduino version, and so is `web_pages.h` (apart from one include line).
The 5 quick power cycles and the BOOT-button reset still restore the network settings.

## 3. What was replaced (Arduino -> ESP-IDF)

| Before (Arduino) | Now (ESP-IDF) | Where |
|---|---|---|
| `WebServer` | `esp_http_server`, thin layer with the same handler style (`arg`, `send`, `authenticate`, streamed upload) | `gw_web.*`, `webutil.h` |
| `HTTPClient`, `WiFiClientSecure` | `esp_http_client` (https without certificate check, as before) | `http_util.*`, `http_sender.cpp`, `fleet_ota.cpp`, `diag.cpp` |
| `Preferences` | NVS, same namespace, key names and value types | `gw_compat.*` (`Prefs`) |
| `Update` | `esp_ota_*` (begin / write / end / set boot partition) | `ota_update.cpp`, `fleet_ota.cpp` |
| `WiFi`, `WiFiMulti` | `esp_wifi`: known networks strongest first, retry 5 s / 60 s, setup hotspot, async scan | `wifi_manager.cpp` |
| `ESPmDNS`, `configTime` | `mdns`, `sntp` | `net_manager.cpp` |
| `HardwareSerial` (pump) | UART driver: UART1 pump line, UART2 pin finder | `pump_reader.cpp`, `pump_scan.cpp` |
| `Serial` | `Console` (printf), switched off while the pump uses GPIO43/44; the TXD0 pad is released | `gw_compat.*` |
| `setup()` / `loop()` | `app_main()` + `loopTask` with the same body and 1 ms rhythm | `main.cpp` |
| Arduino `String`, `IPAddress` | `String` / `IPAddress` of this project (std::string based) | `gw_string.h` |
| `tcpipInit()` (LAN-only DNS) | `esp_netif_init()`, standard lwIP DNS | `eth_lan.cpp` |

Two consequences of the new structure, both handled:
* The web server now runs in its **own task** (before it ran inside `loop()`). The shared pump data (latest JSON, log) is therefore locked, and the pump UART is only read by the loop task, also during an upload.
* IDF component messages are logged at WARN level (`sdkconfig.defaults`); the firmware's own messages are unchanged.

## 4. Settings of the SDK (`sdkconfig.defaults`)

240 MHz, QIO flash 80 MHz, `-Os`, FreeRTOS tick 1000 Hz, console UART0 115200, W5500 driver enabled, 16 sockets, HTTP header 1024 B, two NTP servers, and `ESP_TLS_SKIP_SERVER_CERT_VERIFY` (https to your server without certificate check, as `HTTPS_ROOT_CA nullptr` in `config.h` says).
Flash size and partition table come from `platformio.ini`. To change the SDK settings: `pio run -t menuconfig`.

## 5. What was verified, and what was not

Verified (on the PC, against the real ESP-IDF 4.4.7 headers and the ESP32-S3 SDK libraries):
* all 21 source files compile with ESP-IDF's own options `-Wall -Werror=all -std=gnu++11 -fno-exceptions`: no warnings;
* the project **links** against the ESP-IDF libraries (WiFi, LAN, HTTP server/client, mDNS, NVS, OTA, UART): every function resolves, **no Arduino symbol is in the image**;
* image size **1,046,000 bytes** (the Arduino build was 1,157,120 bytes against the same libraries); PlatformIO prints the exact figure for your configuration;
* PC unit tests: decoder 11/11, update rules 56/56, pin rules 52/52, power-cycle reset 13/13, String/IPAddress 27/27, web helpers 14/14 (URL decoding, Basic login, multipart upload with every chunk size from 1 byte, delimiter look-alikes in the data, malformed input);
* the partition tables with ESP-IDF's own `gen_esp32part.py`; every option of `sdkconfig.defaults` against the ESP-IDF 4.4.7 Kconfig.

**Not verified** (needs your PlatformIO build and your hardware): the PlatformIO/CMake build itself, and the behaviour on the gateway.
Please test in this order and tell me where something differs:
1. Build succeeds; boot log shows `[boot] ...` lines up to `web server started` (console is off only if the pump uses GPIO43/44).
2. LAN: DHCP address, `http://<ip>/` opens, login works, Settings -> Connection test shows the W5500 answer.
3. Pump data arrives: Dashboard shows frames; `/api/latest`.
4. The JSON reaches your server (Dashboard: last HTTP code 200).
5. Settings: save server / network / pump pins (reboot), WiFi scan, add a WiFi network.
6. Firmware upload on `/ota` (with and without `?tcls=&tmodel=&tserial=`), then server-managed update if you use it.
7. WiFi / hotspot behaviour if you use WiFi; 5 quick power cycles reset.

## 6. If the PlatformIO build stops

Copy me the **first** error line. Likely spots: a PlatformIO setting for flash size / partitions (`board_upload.flash_size`, `board_build.partitions` in `platformio.ini`), or the SDK option names in `sdkconfig.defaults` (checked against ESP-IDF 4.4.7, but PlatformIO's own ESP-IDF may differ slightly).

## 7. Files

`platformio.ini`, `CMakeLists.txt`, `src/CMakeLists.txt`, `sdkconfig.defaults`, `partitions_*.csv`, `src/` (firmware), `tools/` (PC tests and the update push tool `ota_push.py`).
PC tests: `g++ -std=c++11 -Isrc tools/fleet_test.cpp src/fleet_logic.cpp -o t && ./t` (same pattern for `pins_test`, `recovery_test`, `native_test` with `make_test_frames.py`, `gwstring_test`, `webutil_test`).
