#include "dev_proto.h"
#include <string.h>
#include <stdio.h>

namespace {

struct Fld { const char* group; const char* key; bool num; };

const Fld kInf[] = {
  {"device",   "serial",               false},
  {"status",   "state",                false},
  {"status",   "alarm",                false},
  {"infusion", "flow_rate_ml_h",       true},
  {"infusion", "vtbi_ml",              true},
  {"infusion", "infused_volume_ml",    true},
  {"infusion", "pressure_mmhg",        true},
  {"infusion", "drug_name",            false},
};
const Fld kPca[] = {
  {"device", "serial",               false},
  {"status", "state",                false},
  {"status", "alarm",                false},
  {"pca",    "drug_name",            false},
  {"pca",    "concentration_mg_ml",  true},
  {"pca",    "bolus_dose_mg",        true},
  {"pca",    "lockout_min",          true},
  {"pca",    "basal_rate_mg_h",      true},
  {"pca",    "hourly_limit_mg",      true},
  {"pca",    "total_given_mg",       true},
  {"pca",    "demands_total",        true},
  {"pca",    "boluses_delivered",    true},
};
const char* const kStates[] = {"IDLE", "RUNNING", "PAUSED", "ALARM", "COMPLETE"};

const Fld* spec(DevType t, size_t& n) {
  if (t == DEV_INFUSION) { n = sizeof(kInf) / sizeof(kInf[0]); return kInf; }
  n = sizeof(kPca) / sizeof(kPca[0]);
  return kPca;
}

bool validNum(const std::string& s) {
  if (s.empty() || s.size() > 12) return false;
  size_t i = 0;
  if (s[0] == '-') i = 1;
  const size_t ds = i;
  size_t digits = 0;
  bool dot = false;
  for (; i < s.size(); i++) {
    if (s[i] >= '0' && s[i] <= '9') digits++;
    else if (s[i] == '.' && !dot && digits > 0 && i + 1 < s.size()) dot = true;
    else return false;
  }
  if (digits == 0) return false;
  if (s[ds] == '0' && s.size() > ds + 1 && s[ds + 1] != '.') return false;   // "007" is not valid JSON
  return true;
}

bool validText(const std::string& s) {
  if (s.size() > 40) return false;
  for (size_t i = 0; i < s.size(); i++) {
    const unsigned char c = (unsigned char)s[i];
    if (c < 0x20 || c > 0x7E || c == '$' || c == '*') return false;
  }
  return true;
}

bool validState(const std::string& s) {
  for (size_t i = 0; i < sizeof(kStates) / sizeof(kStates[0]); i++) if (s == kStates[i]) return true;
  return false;
}

void esc(std::string& o, const std::string& s) {
  o += '"';
  for (size_t i = 0; i < s.size(); i++) {
    const char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else o += c;
  }
  o += '"';
}

}  // namespace

uint8_t devChecksum(const char* s, size_t n) {
  uint8_t c = 0;
  for (size_t i = 0; i < n; i++) c ^= (uint8_t)s[i];
  return c;
}

const char* devTypeName(DevType t) { return t == DEV_INFUSION ? "infusion_pump" : "pca_pump"; }

bool devParseLine(DevType expect, const std::string& rawLine, DevMsg& m, std::string& err) {
  std::string line = rawLine;
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) line.pop_back();
  const size_t dollar = line.find('$');
  if (dollar == std::string::npos) { err = "no $ start"; return false; }
  const size_t star = line.rfind('*');
  if (star == std::string::npos || star < dollar || line.size() != star + 3) { err = "no *HH checksum"; return false; }
  const std::string body = line.substr(dollar + 1, star - dollar - 1);
  unsigned want = 0;
  if (sscanf(line.c_str() + star + 1, "%2x", &want) != 1) { err = "bad checksum digits"; return false; }
  if (devChecksum(body.data(), body.size()) != (uint8_t)want) { err = "checksum mismatch"; return false; }

  std::vector<std::string> parts;
  size_t st = 0;
  for (;;) {
    const size_t c = body.find(',', st);
    if (c == std::string::npos) { parts.push_back(body.substr(st)); break; }
    parts.push_back(body.substr(st, c - st));
    st = c + 1;
  }
  const char* tag = expect == DEV_INFUSION ? "INF" : "PCA";
  if (parts[0] != tag) { err = std::string("wrong device tag (this input is ") + tag + ")"; return false; }
  size_t n;
  const Fld* sp = spec(expect, n);
  if (parts.size() != n + 1) { err = "expected " + std::to_string(n) + " fields, got " + std::to_string(parts.size() - 1); return false; }

  m.type = expect;
  m.f.assign(parts.begin() + 1, parts.end());
  for (size_t i = 0; i < n; i++) {
    const std::string& v = m.f[i];
    if (sp[i].num) {
      if (!validNum(v)) { err = std::string("field ") + sp[i].key + " is not a number"; return false; }
    } else if (!validText(v)) { err = std::string("field ") + sp[i].key + " has bad characters"; return false; }
  }
  if (m.f[0].empty()) { err = "empty serial"; return false; }
  if (!validState(m.f[1])) { err = "unknown state"; return false; }
  if (m.f[2].empty()) { err = "empty alarm field (use NONE)"; return false; }
  return true;
}

std::string devToJson(const DevMsg& m, const DevJsonCtx& c) {
  size_t n;
  const Fld* sp = spec(m.type, n);
  std::string o = "{";
  o += "\"source\":\""; o += devTypeName(m.type); o += "\"";
  o += ",\"transport\":"; esc(o, c.transport);
  o += ",\"gateway\":{\"id\":"; esc(o, c.gatewayId);
  o += ",\"mac\":"; esc(o, c.mac);
  o += ",\"fw\":"; esc(o, c.fw);
  o += ",\"iface\":"; esc(o, c.iface);
  o += ",\"ip\":"; esc(o, c.ip);
  o += ",\"uptime_ms\":" + std::to_string(c.uptimeMs);
  o += ",\"frame_seq\":" + std::to_string(c.seq);
  if (c.receivedAtUtc && c.receivedAtUtc[0]) { o += ",\"received_at\":"; esc(o, c.receivedAtUtc); }
  o += "}";

  const bool alarmOn = m.f[2] != "NONE";
  const char* cur = "";
  for (size_t i = 0; i < n; i++) {
    if (strcmp(cur, sp[i].group) != 0) {
      if (cur[0]) {
        if (strcmp(cur, "status") == 0) o += std::string(",\"alarm_present\":") + (alarmOn ? "true" : "false");
        o += "}";
      }
      cur = sp[i].group;
      o += ",\""; o += cur; o += "\":{";
      if (strcmp(cur, "device") == 0) { o += "\"class\":\""; o += devTypeName(m.type); o += "\","; }
    } else {
      o += ",";
    }
    o += "\""; o += sp[i].key; o += "\":";
    if (sp[i].num) o += m.f[i]; else esc(o, m.f[i]);
  }
  o += "}}";
  return o;
}

std::string devSummary(const DevMsg& m) {
  if (m.type == DEV_INFUSION)
    return m.f[1] + " alarm=" + m.f[2] + " rate=" + m.f[3] + " ml/h infused=" + m.f[5] + "/" + m.f[4] + " ml " + m.f[7];
  return m.f[1] + " alarm=" + m.f[2] + " given=" + m.f[9] + " mg demands=" + m.f[10] + " delivered=" + m.f[11] + " " + m.f[3];
}

std::string devChangeKey(const DevMsg& m) { return m.f[1] + "|" + m.f[2]; }
