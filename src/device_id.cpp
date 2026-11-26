#include "device_id.h"
#include "config.h"
#include "settings.h"
#include "fleet_logic.h"

static SemaphoreHandle_t mux;
static String   curModel, curSerial, curVersion;       // last auto-detected (live or remembered)
static uint32_t lastSeenMs = 0;                         // 0 = nothing seen since boot
static bool     curInfusing = false;

void deviceIdBegin() {
  mux = xSemaphoreCreateMutex();
  curModel  = settingsNoteGet("dv_model");
  curSerial = settingsNoteGet("dv_serial");
  curVersion = settingsNoteGet("dv_ver");
  if (curModel.length()) Console.printf("Device (last seen): %s SN=%s\n", curModel.c_str(), curSerial.c_str());
}

void deviceIdSeen(const char* model, const char* serial, const char* version, bool infusing) {
  if (!mux) return;
  String m = model ? model : "", s = serial ? serial : "", v = version ? version : "";
  bool changed = false;
  xSemaphoreTake(mux, portMAX_DELAY);
  changed = (m != curModel) || (s != curSerial);
  curModel = m; curSerial = s; curVersion = v;
  curInfusing = infusing;
  lastSeenMs = millis() ? millis() : 1;
  xSemaphoreGive(mux);
  if (changed) {                                                         // flash is written only when the device changes
    settingsNoteSet("dv_model", m);
    settingsNoteSet("dv_serial", s);
    settingsNoteSet("dv_ver", v);
    Console.printf("Connected device: %s SN=%s (%s)\n", m.c_str(), s.c_str(), fleetClassFromModel(m.c_str()));
  }
}

void deviceIdGet(DeviceIdentity& o) {
  DeviceCfg dc;
  settingsGetDevice(dc);
  o = DeviceIdentity();
  o.known = false; o.live = false; o.manual = false; o.ageS = 0; o.infusing = false;
  if (dc.manual) {
    o.known = true; o.live = true; o.manual = true;
    o.cls = dc.cls; o.model = dc.model; o.serial = dc.serial;
    return;
  }
  if (!mux) return;
  xSemaphoreTake(mux, portMAX_DELAY);
  o.model = curModel; o.serial = curSerial; o.version = curVersion;
  o.infusing = curInfusing && lastSeenMs != 0;
  if (lastSeenMs != 0) {
    uint32_t age = millis() - lastSeenMs;
    o.ageS = age / 1000;
    o.live = age < (uint32_t)FLEET_LIVE_WINDOW_MS;
  }
  xSemaphoreGive(mux);
  o.known = o.model.length() > 0;
  o.cls = o.known ? String(fleetClassFromModel(o.model.c_str())) : String("");
}

const char* deviceClassText(const String& cls) {
  if (cls == "syringe_pump")    return "syringe pump";
  if (cls == "infusion_pump")   return "infusion pump";
  if (cls == "patient_monitor") return "patient monitor";
  if (cls == "other")           return "other device";
  return "unknown";
}
