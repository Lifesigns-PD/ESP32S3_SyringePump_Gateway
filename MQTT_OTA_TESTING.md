# MQTT OTA - how it works and how to test it

## How it works
```
 your PC / server                         gateway (ESP32-S3, WiFi or LAN) 
 ----------------                         --------------------------------
 MQTT broker  <---- subscribe ---------  agilia-gw/<gw-id>/ota/cmd , agilia-gw/all/ota/cmd           
 publish cmd  ----- {"cmd":"update"..} -> checks: version, sha256, size, target (model/serial), not infusing
                                          downloads firmware.bin from "url" over HTTP(S)  (not through MQTT)
 status topic <---- progress / result ---  agilia-gw/<gw-id>/ota/status
                                          verifies SHA-256 + size -> activates new slot -> reboots
                                          after reboot: publishes "ok" (version confirmed) or "bad"
```
* Works on WiFi **and** LAN: it uses the normal network stack, starts when the first interface gets an IP, and
  reconnects by itself if the active interface changes. Nothing in WiFi / LAN / pump / JSON push was changed. 
* Settings are in [src/config.h](src/config.h) section 8 (`MQTT_OTA_*`). Change the broker there, rebuild, upload once by cable.
* Shares the update lock with the web `/update` upload and the server-managed update: only one runs at a time.
* Retained commands are ignored (a stored command can never re-run after the reboot).

## Command / status format
Command to `agilia-gw/<gw-id>/ota/cmd` (or `agilia-gw/all/ota/cmd`):
```json
{"cmd":"update","id":"job-42","version":"1.3.1","url":"http://192.168.50.1:8080/firmware.bin",
 "sha256":"<64 hex>","size":1076748,"force":false,"class":"","model":"","serial":""}
```
`version`, `url`, `sha256`, `size` are required. `class`/`model`/`serial` are optional wildcard patterns checked against the pump
connected right now. `force:true` allows same/older version and install during an infusion. `{"cmd":"status"}` asks for the "online" info.

Status messages on `agilia-gw/<gw-id>/ota/status`: `received, downloading (progress %), installed, rejected, failed, busy`,
and after the reboot `ok` or `bad`. Gateway presence: `agilia-gw/<gw-id>/status` (retained; `offline` is its last will).

## 0. Prepare (once)
1. Install **Mosquitto** (broker) on the PC, allow it on the network: in `mosquitto.conf`
   `listener 1883` and `allow_anonymous true` (test only), start it. Allow TCP 1883 and 8080 in the Windows firewall.
2. `pip install paho-mqtt`
3. LAN direct-cable test: PC adapter = `192.168.50.1/255.255.255.0` (default in config.h, so the gateway reaches the broker at `mqtt://192.168.50.1:1883`).
   WiFi test: put the PC on the same WiFi and set `MQTT_OTA_BROKER_URI` to the PC's IP.
4. Build and flash **version A** by cable: `pio run -t upload`. Open the serial console (115200) and note the line
   `MQTT OTA: client gw-xxxxxxxxxxxx -> mqtt://...` - that is the `<gw-id>`.

## 1. Connectivity (LAN, then repeat on WiFi)
| # | Step | Expected |
|---|------|----------|
| 1.1 | Console after boot | `MQTT OTA: connected to broker via ethernet` (or `wifi`) |
| 1.2 | `mosquitto_sub -h 192.168.50.1 -t "agilia-gw/#" -v` | retained `agilia-gw/<id>/status {"state":"online",...,"iface":"ethernet"}` |
| 1.3 | `python tools/mqtt_ota_push.py --broker 192.168.50.1 --gw <id> --status` | the online JSON comes back (fw, iface, ip, model, serial) |
| 1.4 | Pull the power of the gateway | broker shows `{"state":"offline"}` (last will) after ~1.5 x keepalive |
| 1.5 | Switch to WiFi in Settings -> Network interface, save | after reboot console shows `connected ... via wifi`, JSON POST to the server still works |
| 1.6 | Auto mode: unplug the LAN cable while connected | MQTT reconnects through WiFi (`disconnected (will reconnect)` then `connected ... via wifi`) |

## 2. Successful update (do it on LAN, then again on WiFi)
1. Change `FW_VERSION` in config.h to `"1.3.1"`, `pio run`. Copy `.pio/build/wroom1u_8mb/firmware.bin` to a folder, e.g. `C:\ota\firmware.bin`.
2. `cd C:\ota` then `python -m http.server 8080` (leave running).
3. Restore `FW_VERSION` to `"1.3.0"` and flash that by cable (gateway runs **1.3.0**, the served file is **1.3.1**).
4. `python tools/mqtt_ota_push.py --broker 192.168.50.1 --gw <id> --file C:\ota\firmware.bin --version 1.3.1 --url http://192.168.50.1:8080/firmware.bin`

Expected status sequence: `received` -> `downloading 0..100%` -> `installed` -> (gateway reboots) -> `online` -> `ok "update to 1.3.1 confirmed"`.
Console after reboot: `FW 1.3.1`. The pump JSON POSTs continue; WiFi / LAN / saved settings are unchanged (NVS is not erased).

## 3. Negative tests (each must leave the running firmware untouched and report a reason)
| # | Test | How | Expected state / message |
|---|------|-----|--------------------------|
| 3.1 | Bad checksum | add `--sha256 0000...0000` (64 zeros) | `failed` "SHA-256 mismatch ... NOT installed"; version unchanged, no reboot |
| 3.2 | Wrong size | add `--size 12345` | `failed` "file size ... differs from the command size" |
| 3.3 | Same / older version | `--version 1.3.0` (running) | `rejected` "not newer than the running ..." ; with `--force` it installs |
| 3.4 | Wrong target | `--model "Volumat*"` on an Injectomat | `rejected` "target mismatch" |
| 3.5 | Right target | `--model "Injectomat*"` on an Injectomat with live frames | accepted and installs |
| 3.6 | Unreachable URL | stop the http.server | `failed` "download failed, could not connect" |
| 3.7 | Download cut | stop http.server in the middle (use a bigger file / throttle) | `failed` "connection closed after ...", old firmware still boots |
| 3.8 | Power loss during download | unplug at ~50 % | gateway boots the old firmware (the new slot is only activated after verify) |
| 3.9 | Infusion running | send while the pump reports "infusion in progress" | `rejected` "infusion in progress (use force)" |
| 3.10 | Second command while busy | send two updates quickly | second gets `busy` (or `failed` "another firmware update is running") |
| 3.11 | Retained command | `mosquitto_pub -r -t agilia-gw/<id>/ota/cmd -m "{...}"` | console "retained command ignored"; nothing happens. Clear it with `mosquitto_pub -r -n -t agilia-gw/<id>/ota/cmd` |
| 3.12 | Broadcast | `--gw all` with 2 gateways | both react; with `--serial` pattern only the matching one installs |
| 3.13 | Image bigger than the slot | `--size` larger than the slot (4 MB build: 1.25 MB) | `rejected` "larger than the update partition" |

## 4. Regression (nothing else changed)
* Web pages, login, `/update` upload and the server-managed (fleet) OTA still work.
* JSON POST to the server continues during MQTT idle, and recovers after an MQTT update + reboot.
* Switching WiFi / LAN / auto on the Settings page behaves as before.
* Turn the feature off completely with `MQTT_OTA_ENABLED 0` (or an empty `MQTT_OTA_BROKER_URI`): the build is identical to before.

## 5. Production notes
* Use `mqtts://` on port 8883 with user/password and a broker CA (`MQTT_OTA_ROOT_CA`); with the default `nullptr` the broker
  certificate is not verified (same policy as
   `HTTPS_ROOT_CA` for the data upload). Serve firmware over https too.
* The SHA-256 in the command is the integrity check. Anyone who can publish to the cmd topic can push firmware, so restrict 
  publish rights on `agilia-gw/+/ota/cmd` and `agilia-gw/all/ota/cmd` with broker ACLs.
* There is no automatic rollback to the previous slot if the new firmware boots but is broken (the existing OTA has none either).
  `bad` is reported only when the new image reports a different version than commanded.
