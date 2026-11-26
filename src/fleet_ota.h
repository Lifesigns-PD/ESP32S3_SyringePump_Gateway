/*
 * fleet_ota.h / fleet_ota.cpp  -  server-managed firmware update, chosen by device model / serial number
 *
 * Every FleetCfg.intervalMin minutes (and on "Check now") the gateway asks the update server:
 *
 *   GET <base><path>?gw=<id>&mac=<mac>&fw=<running version>&iface=<ethernet|wifi>
 *                   &class=<syringe_pump|infusion_pump|patient_monitor|other>&model=<..>&serial=<..>&last=<last result>
 *
 * The server holds rules ("all Volumat gateways run 1.3.0", "serial 018090/22083105X runs 1.3.1-test") and answers
 *
 *   {"update":true,"version":"1.3.0","url":"/firmware/gateway-1.3.0.bin","sha256":"<64 hex>","size":1048576,"rule":"..."}
 *   {"update":false,"reason":"..."}
 *
 * The gateway downloads the file, checks the SHA-256 and size, and only then switches to the new firmware.
 * Mode 0 = off (default), 1 = check only (install with the button), 2 = install automatically.
 * Nothing happens unless a device frame was seen recently (FLEET_REQUIRE_LIVE_DEVICE), so the rule is always
 * applied to the device that is connected NOW.
 */
#pragma once
#include "gw_compat.h"

void   fleetBegin();
void   fleetCheckNow();        // ask the task to check at once
bool   fleetInstallNow();      // ask the task to install the offered version (false if nothing is offered)
String fleetStatusJson();      // for the web page
