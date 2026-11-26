/*
 * dev_proto.h / dev_proto.cpp  -  line protocol of the infusion pump and the PCA pump + JSON builder
 * Pure C++ (no ESP-IDF code): unit-tested on a PC with tools/dev_proto_test.cpp.
 *
 * ASSUMPTION: no protocol document exists for these two devices, so this is a simple NMEA-style text protocol defined by this project.
 * When the real devices arrive, replace devParseLine() with their real decoder; everything else stays.
 *
 *   $INF,serial,state,alarm,rate_ml_h,vtbi_ml,infused_ml,pressure_mmhg,drug*HH
 *   $PCA,serial,state,alarm,drug,conc_mg_ml,bolus_mg,lockout_min,basal_mg_h,hourly_limit_mg,total_given_mg,demands,delivered*HH
 *
 *   HH = two hex digits, XOR of every character between '$' and '*'.  Line ends with CR LF (or LF).
 *   state: IDLE RUNNING PAUSED ALARM COMPLETE      alarm: NONE OCCLUSION AIR_IN_LINE DOOR_OPEN LOW_BATTERY END_OF_INFUSION ...
 */
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

enum DevType { DEV_INFUSION = 0, DEV_PCA = 1 };

struct DevMsg {
  DevType                  type;
  std::vector<std::string> f;       // fields, in protocol order (after the $TAG)
};

struct DevJsonCtx {
  const char* gatewayId;
  const char* mac;
  const char* fw;
  const char* iface;
  const char* ip;
  const char* transport;            // "uart" or "tcp"
  const char* receivedAtUtc;        // "" if the clock is not set
  uint32_t    uptimeMs;
  uint32_t    seq;
};

uint8_t     devChecksum(const char* s, size_t n);
// true = message valid, m filled. false = rejected, err says why. 'expect' = the device this input belongs to.
bool        devParseLine(DevType expect, const std::string& line, DevMsg& m, std::string& err);
std::string devToJson(const DevMsg& m, const DevJsonCtx& c);
std::string devSummary(const DevMsg& m);                    // one log line, e.g. "RUNNING none rate=25 ml/h"
std::string devChangeKey(const DevMsg& m);                  // state + alarm: a change of it is uploaded at once
const char* devTypeName(DevType t);                         // "infusion_pump" / "pca_pump"
