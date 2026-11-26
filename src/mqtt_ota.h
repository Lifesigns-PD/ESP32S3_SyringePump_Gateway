/*
 * mqtt_ota.h / mqtt_ota.cpp  -  firmware update commanded over MQTT
 *
 * MQTT is the CONTROL channel (small JSON messages); the firmware image itself is fetched over HTTP(S) from the URL in
 * the command, checked with SHA-256 + size, and only then activated. (An image of ~1 MB is not pushed through MQTT
 * messages: it is slow, fragile and blocks the broker.)  The client uses the normal lwIP sockets, so it works on the
 * WiFi interface AND on the W5500 LAN interface, whichever net_manager has up. It starts when the first interface has an IP
 * and reconnects by itself (also when the active interface changes).
 *
 * Topics   (<p> = MQTT_OTA_TOPIC_PREFIX, <id> = "gw-" + the 12 hex digits of the chip's base MAC)
 *   <p>/<id>/ota/cmd     gateway SUBSCRIBES   command for this gateway
 *   <p>/all/ota/cmd      gateway SUBSCRIBES   command for every gateway (use "model"/"serial"/"class" patterns to target)
 *   <p>/<id>/ota/status  gateway PUBLISHES    progress / result of an update (QoS 1, not retained)
 *   <p>/<id>/status      gateway PUBLISHES    "online" JSON (retained) ; last will "offline" (retained)
 *
 * Command (flat JSON, retained messages are ignored so a stored command can never re-run after the reboot)
 *   {"cmd":"update","id":"job-42","version":"1.3.1","url":"http://host:8080/firmware/gateway-1.3.1.bin",
 *    "sha256":"<64 hex>","size":1048576,"force":false,"class":"","model":"Volumat*","serial":"018090*"}
 *   {"cmd":"status"}                      -> gateway publishes its "online" JSON again
 *   version, url, sha256, size are required. class / model / serial are optional wildcard patterns ('*' '?') compared
 *   with the pump connected RIGHT NOW (same guard as the web upload). "force":true allows the same / an older version and
 *   an install while the pump reports an infusion.
 *
 * Status JSON  {"id":"job-42","state":"...","progress":0-100,"fw":"1.3.0","msg":"..."}
 *   state = received | downloading | installed | rejected | failed | busy      (+ "ok" / "bad" after the first boot of the new image)
 *
 * Shares the update lock with the web upload and the server-managed update (ota_update.h), so only one runs at a time.
 */
#pragma once
#include "gw_compat.h"

void mqttOtaBegin();       // starts the task; stays idle until a network interface has an IP address
bool mqttOtaConnected();   // connected to the broker right now
