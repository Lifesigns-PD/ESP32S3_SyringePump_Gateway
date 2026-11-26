/*
 * fleet_logic.h / fleet_logic.cpp  -  pure C++ helpers for "which gateway gets which firmware"
 *
 * No Arduino code in here, so it is unit-tested on a PC (tools/fleet_test.cpp).
 * Used by fleet_ota.cpp (server-managed update) and ota_update.cpp (guard on the manual upload).
 */
#pragma once
#include <stdint.h>
#include <string>

// Device classes used for targeting.
//   syringe_pump    Injectomat Agilia
//   infusion_pump   Volumat Agilia
//   patient_monitor typed in by hand (the gateway cannot decode monitors yet)
//   other           anything else
const char* fleetClassFromModel(const std::string& model);
bool        fleetClassValid(const std::string& cls);

// Wildcard match, case-insensitive. '*' = any characters, '?' = one character.
// An EMPTY pattern matches everything ("any").
bool fleetGlobMatch(const std::string& pattern, const std::string& value);

// Target = patterns for class / model / serial. All non-empty patterns must match.
struct FleetTarget {
  std::string cls, model, serial;
  bool any() const { return cls.empty() && model.empty() && serial.empty(); }
};
bool fleetTargetMatches(const FleetTarget& t, const std::string& cls, const std::string& model,
                        const std::string& serial, std::string& reason);

// "1.2.10" vs "1.2.9" -> -1 / 0 / +1. Text after the numbers ("-beta") is ignored.
int fleetVersionCompare(const std::string& a, const std::string& b);

// Percent-encoding for query strings (letters, digits and - _ . ~ stay as they are).
std::string fleetUrlEncode(const std::string& s);

// "http://host:8080" + "/firmware/x.bin"  ->  full URL. An absolute URL is returned unchanged.
std::string fleetResolveUrl(const std::string& base, const std::string& urlOrPath);

// Minimal reader for FLAT json objects such as {"update":true,"version":"1.2.0","size":123456}
bool fleetJsonString(const std::string& json, const char* key, std::string& out);
bool fleetJsonBool(const std::string& json, const char* key, bool& out);
bool fleetJsonUInt(const std::string& json, const char* key, uint32_t& out);

// 64 hex characters (SHA-256), compared case-insensitively.
bool fleetIsSha256Hex(const std::string& s);
bool fleetHexEqual(const std::string& a, const std::string& b);
