#include "fleet_logic.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- classes
const char* fleetClassFromModel(const std::string& model) {
  std::string m;
  for (size_t i = 0; i < model.size(); i++) m += (char)tolower((unsigned char)model[i]);
  if (m.find("injectomat") != std::string::npos) return "syringe_pump";
  if (m.find("volumat") != std::string::npos)    return "infusion_pump";
  if (m.find("monitor") != std::string::npos)    return "patient_monitor";
  return "other";
}

bool fleetClassValid(const std::string& c) {
  return c == "syringe_pump" || c == "infusion_pump" || c == "patient_monitor" || c == "other";
}

// ---------------------------------------------------------------- wildcard match
static bool globRec(const char* p, const char* v) {
  while (*p) {
    if (*p == '*') {
      while (*p == '*') p++;
      if (!*p) return true;
      for (; *v; v++) if (globRec(p, v)) return true;
      return globRec(p, v);            // v is at the terminating 0 here
    }
    if (!*v) return false;
    if (*p != '?' && tolower((unsigned char)*p) != tolower((unsigned char)*v)) return false;
    p++; v++;
  }
  return *v == 0;
}

bool fleetGlobMatch(const std::string& pattern, const std::string& value) {
  if (pattern.empty()) return true;
  return globRec(pattern.c_str(), value.c_str());
}

bool fleetTargetMatches(const FleetTarget& t, const std::string& cls, const std::string& model,
                        const std::string& serial, std::string& reason) {
  if (!fleetGlobMatch(t.cls, cls))       { reason = "device class \"" + cls + "\" does not match \"" + t.cls + "\""; return false; }
  if (!fleetGlobMatch(t.model, model))   { reason = "device model \"" + model + "\" does not match \"" + t.model + "\""; return false; }
  if (!fleetGlobMatch(t.serial, serial)) { reason = "device serial \"" + serial + "\" does not match \"" + t.serial + "\""; return false; }
  return true;
}

// ---------------------------------------------------------------- versions
static bool nextNumber(const char*& p, unsigned long& out) {
  while (*p && !isdigit((unsigned char)*p)) {
    if (*p == '-' || *p == '+' || *p == '_') return false;       // suffix starts: stop
    p++;
  }
  if (!*p) return false;
  out = strtoul(p, (char**)&p, 10);
  return true;
}

int fleetVersionCompare(const std::string& a, const std::string& b) {
  const char* pa = a.c_str();
  const char* pb = b.c_str();
  if (*pa == 'v' || *pa == 'V') pa++;
  if (*pb == 'v' || *pb == 'V') pb++;
  for (int i = 0; i < 6; i++) {
    unsigned long x = 0, y = 0;
    bool hx = nextNumber(pa, x), hy = nextNumber(pb, y);
    if (!hx && !hy) return 0;
    if (x != y) return x < y ? -1 : 1;
  }
  return 0;
}

// ---------------------------------------------------------------- URL helpers
std::string fleetUrlEncode(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    unsigned char c = (unsigned char)s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
  }
  return o;
}

std::string fleetResolveUrl(const std::string& base, const std::string& u) {
  if (u.compare(0, 7, "http://") == 0 || u.compare(0, 8, "https://") == 0) return u;
  std::string b = base;
  while (!b.empty() && b[b.size() - 1] == '/') b.erase(b.size() - 1);
  if (u.empty() || u[0] != '/') return b + "/" + u;
  return b + u;
}

// ---------------------------------------------------------------- flat JSON reader
// Finds  "key" : <value>  at any depth (our server replies are flat). Returns the position of the value or npos.
static size_t findValue(const std::string& j, const char* key) {
  const std::string k = std::string("\"") + key + "\"";
  size_t pos = 0;
  while ((pos = j.find(k, pos)) != std::string::npos) {
    size_t p = pos + k.size();
    while (p < j.size() && isspace((unsigned char)j[p])) p++;
    if (p < j.size() && j[p] == ':') {
      p++;
      while (p < j.size() && isspace((unsigned char)j[p])) p++;
      return p;
    }
    pos += k.size();                   // it was a value, not a key: keep looking
  }
  return std::string::npos;
}

bool fleetJsonString(const std::string& j, const char* key, std::string& out) {
  size_t p = findValue(j, key);
  if (p == std::string::npos || p >= j.size() || j[p] != '"') return false;
  out.clear();
  for (p++; p < j.size(); p++) {
    char c = j[p];
    if (c == '"') return true;
    if (c == '\\' && p + 1 < j.size()) {
      char n = j[++p];
      switch (n) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u': out += '?'; p += 4; break;       // \uXXXX not needed here
        default:  out += n;                         // \"  \\  \/
      }
    } else out += c;
  }
  return false;                        // unterminated
}

bool fleetJsonBool(const std::string& j, const char* key, bool& out) {
  size_t p = findValue(j, key);
  if (p == std::string::npos) return false;
  if (j.compare(p, 4, "true") == 0)  { out = true;  return true; }
  if (j.compare(p, 5, "false") == 0) { out = false; return true; }
  return false;
}

bool fleetJsonUInt(const std::string& j, const char* key, uint32_t& out) {
  size_t p = findValue(j, key);
  if (p == std::string::npos || p >= j.size() || !isdigit((unsigned char)j[p])) return false;
  out = (uint32_t)strtoul(j.c_str() + p, nullptr, 10);
  return true;
}

// ---------------------------------------------------------------- SHA-256 text
bool fleetIsSha256Hex(const std::string& s) {
  if (s.size() != 64) return false;
  for (size_t i = 0; i < 64; i++) if (!isxdigit((unsigned char)s[i])) return false;
  return true;
}

bool fleetHexEqual(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++) if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
  return true;
}
