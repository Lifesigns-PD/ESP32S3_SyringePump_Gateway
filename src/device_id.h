/*
 * device_id.h / device_id.cpp  -  "what is this gateway connected to?"
 *
 * One ESP32 gateway hardware is used with different hospital devices. Firmware updates are targeted by
 * device model / serial number, so the gateway must know what is on its UART:
 *
 *   Auto-detect : taken from the Agilia status frames (0x80): model, serial, software version.
 *                 Injectomat Agilia -> class "syringe_pump", Volumat Agilia -> class "infusion_pump".
 *   Manual      : typed in on the Settings page for devices the gateway cannot decode yet (patient monitor ...).
 *
 * The last auto-detected device is also remembered in flash, but it only counts as "live" while frames keep arriving
 * (FLEET_LIVE_WINDOW_MS), so a rule is never applied for a device that is no longer connected.
 */
#pragma once
#include "gw_compat.h"

struct DeviceIdentity {
  bool   known;        // model is known (manual, live, or remembered)
  bool   live;         // manual, or a frame was seen within FLEET_LIVE_WINDOW_MS
  bool   manual;       // typed in by hand
  String cls;          // syringe_pump | infusion_pump | patient_monitor | other
  String model;
  String serial;
  String version;      // device software version (auto-detect only)
  uint32_t ageS;       // seconds since the last frame (auto-detect only)
  bool   infusing;     // last frame said "infusion in progress"
};

void deviceIdBegin();                                           // load the remembered device from flash
void deviceIdSeen(const char* model, const char* serial, const char* version, bool infusing);   // call for every decoded status frame
void deviceIdGet(DeviceIdentity& out);
const char* deviceClassText(const String& cls);                 // "syringe pump", "infusion pump", ...
