/*
 * ota_update.h / ota_update.cpp  -  firmware update by browser upload of firmware.bin (route "/update")
 *
 * The page is PAGE_OTA in web_pages.h. Settings in flash (WiFi, LAN, server, device) are NOT erased by an update.
 *
 * Target guard: /update?tcls=...&tmodel=...&tserial=...   (all optional, '*' and '?' wildcards)
 *   The gateway compares them with the device it is connected to RIGHT NOW (device_id.cpp) and REFUSES the image
 *   (HTTP 409) if it does not match. This is what makes "update only the Volumat gateways" or
 *   "update only serial numbers starting with 018090/" safe, also when a script pushes to many gateways (tools/ota_push.py).
 *
 * Only one update at a time (web upload and server-managed update share the lock below).
 */
#pragma once
#include "gw_compat.h"

void otaHandleUpload();   // called for every chunk of the uploaded file
void otaHandleDone();     // called when the upload is finished -> answer + reboot

bool otaBusyTry();        // true = you now own the update lock
void otaBusyClear();
bool otaBusy();
