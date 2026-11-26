#include "hl7_parse.h"
#include <stdio.h>

static std::vector<std::string> splitStr(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  for (;;) {
    size_t p = s.find(sep, start);
    if (p == std::string::npos) { out.push_back(s.substr(start)); break; }
    out.push_back(s.substr(start, p - start));
    start = p + 1;
  }
  return out;
}

// field N (1-based HL7 numbering, MSH has the special -1 shift handled by the caller) component C (1-based)
static std::string comp(const std::vector<std::string>& f, size_t idx, size_t c, char csep) {
  if (idx >= f.size()) return "";
  if (c == 0) return f[idx];
  std::vector<std::string> p = splitStr(f[idx], csep);
  return (c - 1 < p.size()) ? p[c - 1] : "";
}

// undo \F\ \S\ \T\ \R\ \E\ escapes.  enc = encoding characters from MSH-2: [0] component, [1] repeat, [2] escape, [3] sub-component
static std::string unescape(const std::string& s, const char* enc) {
  const char esc = enc[2];
  if (s.find(esc) == std::string::npos) return s;
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    size_t e = (s[i] == esc) ? s.find(esc, i + 1) : std::string::npos;
    if (e != std::string::npos) {
      std::string code = s.substr(i + 1, e - i - 1);
      if      (code == "F") o += '|';
      else if (code == "S") o += enc[0];
      else if (code == "T") o += enc[3];
      else if (code == "R") o += enc[1];
      else if (code == "E") o += esc;
      i = e;                                   // unknown codes (e.g. \.br\) are dropped
    } else o += s[i];
  }
  return o;
}

Hl7Msg hl7Parse(const std::string& raw, bool keepPatient) {
  Hl7Msg m;
  // normalise segment separators to '\r'
  std::string t;
  t.reserve(raw.size());
  for (size_t i = 0; i < raw.size(); i++) {
    char c = raw[i];
    if (c == '\n') { if (t.empty() || t.back() != '\r') t += '\r'; }
    else t += c;
  }
  std::vector<std::string> segs = splitStr(t, '\r');
  if (segs.empty() || segs[0].compare(0, 3, "MSH") != 0 || segs[0].size() < 8) { m.error = "no MSH segment"; return m; }

  const char fsep = segs[0][3];                       // normally '|'
  char csep = '^';
  char enc[5] = {'^', '~', '\\', '&', 0};
  {
    size_t e = segs[0].find(fsep, 4);
    std::string encs = segs[0].substr(4, e == std::string::npos ? std::string::npos : e - 4);
    for (size_t i = 0; i < encs.size() && i < 4; i++) enc[i] = encs[i];
    csep = enc[0];
  }

  // MSH is special: MSH-1 is the field separator itself, so split index == field number - 1
  std::vector<std::string> h = splitStr(segs[0], fsep);
  m.sendingApp      = comp(h, 2, 1, csep);
  m.sendingFacility = comp(h, 3, 1, csep);
  m.msgTime         = comp(h, 6, 0, csep);
  m.msgType         = comp(h, 8, 0, csep);
  m.controlId       = comp(h, 9, 0, csep);
  m.version         = comp(h, 11, 1, csep);
  if (m.msgType.compare(0, 3, "ORU") != 0) { m.error = "unsupported message type '" + m.msgType + "' (expected ORU^R01)"; return m; }

  for (size_t s = 1; s < segs.size(); s++) {
    const std::string& seg = segs[s];
    if (seg.size() < 3) continue;
    std::vector<std::string> f = splitStr(seg, fsep);     // f[0] = segment name, f[n] = field n
    if (f[0] == "PID" && keepPatient) {
      m.patientId   = comp(f, 3, 1, csep);
      std::string fam = comp(f, 5, 1, csep), giv = comp(f, 5, 2, csep);
      m.patientName = giv.empty() ? fam : giv + " " + fam;
    } else if (f[0] == "PV1") {
      m.bed = f.size() > 3 ? f[3] : "";
      std::string pt = comp(f, 3, 3, csep);              // PV1-3 = point of care ^ room ^ bed
      std::string rm = comp(f, 3, 2, csep);
      if (!pt.empty() || !rm.empty()) m.bed = (rm.empty() ? "" : rm + "/") + pt;
    } else if (f[0] == "OBX") {
      Hl7Obs o;
      o.valueType = comp(f, 2, 0, csep);
      o.code      = comp(f, 3, 1, csep);
      o.name      = unescape(comp(f, 3, 2, csep), enc);
      o.system    = comp(f, 3, 3, csep);
      o.subId     = comp(f, 4, 0, csep);
      // OBX-5 of type SN (structured numeric) looks like  ^120  or  >^5 : take the number part
      o.value     = unescape(comp(f, 5, 0, csep), enc);
      if (o.valueType == "SN") {
        std::vector<std::string> p = splitStr(o.value, csep);
        if (p.size() >= 2 && !p[1].empty()) o.value = p[1];
      }
      o.unit      = comp(f, 6, 1, csep);
      o.range     = comp(f, 7, 0, csep);
      o.flag      = comp(f, 8, 0, csep);
      o.status    = comp(f, 11, 0, csep);
      o.time      = comp(f, 14, 0, csep);
      m.obs.push_back(o);
    }
  }
  if (m.obs.empty()) { m.error = "no OBX segments"; return m; }
  m.valid = true;
  return m;
}

std::string hl7BuildAck(const Hl7Msg& m, bool ok, const char* text) {
  std::string a = "MSH|^~\\&|AGILIA-GW|LIFESIGNS|" + m.sendingApp + "|" + m.sendingFacility + "||ACK^R01|ACK" + m.controlId +
                  "|P|" + (m.version.empty() ? "2.5" : m.version) + "\r";
  a += std::string("MSA|") + (ok ? "AA" : "AE") + "|" + m.controlId;
  if (text && *text) a += std::string("|") + text;
  a += "\r";
  return a;
}

std::string hl7JsonEscape(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
    else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
    else o += (char)c;
  }
  return o;
}

std::string hl7ToJson(const Hl7Msg& m, const char* gwId, unsigned long rxMs) {
  std::string j = "{\"type\":\"patient_monitor\",\"protocol\":\"HL7v2\",\"gateway\":\"" + hl7JsonEscape(gwId ? gwId : "") + "\"";
  j += ",\"uptime_ms\":" + std::to_string(rxMs);
  j += ",\"sender\":\"" + hl7JsonEscape(m.sendingApp) + "\",\"facility\":\"" + hl7JsonEscape(m.sendingFacility) + "\"";
  j += ",\"msg_type\":\"" + hl7JsonEscape(m.msgType) + "\",\"control_id\":\"" + hl7JsonEscape(m.controlId) + "\"";
  j += ",\"hl7_version\":\"" + hl7JsonEscape(m.version) + "\",\"msg_time\":\"" + hl7JsonEscape(m.msgTime) + "\"";
  j += ",\"bed\":\"" + hl7JsonEscape(m.bed) + "\"";
  if (!m.patientId.empty())   j += ",\"patient_id\":\"" + hl7JsonEscape(m.patientId) + "\"";
  if (!m.patientName.empty()) j += ",\"patient_name\":\"" + hl7JsonEscape(m.patientName) + "\"";
  j += ",\"observations\":[";
  for (size_t i = 0; i < m.obs.size(); i++) {
    const Hl7Obs& o = m.obs[i];
    if (i) j += ",";
    j += "{\"code\":\"" + hl7JsonEscape(o.code) + "\",\"name\":\"" + hl7JsonEscape(o.name) + "\"";
    if (!o.subId.empty()) j += ",\"sub_id\":\"" + hl7JsonEscape(o.subId) + "\"";
    bool numeric = (o.valueType == "NM" || o.valueType == "SN") && !o.value.empty();
    if (numeric) {                                  // only emit as a JSON number when it really is one
      for (size_t k = 0; k < o.value.size(); k++) { char c = o.value[k]; if (!((c >= '0' && c <= '9') || c == '.' || c == '-')) { numeric = false; break; } }
    }
    if (numeric) j += ",\"value\":" + o.value;
    else         j += ",\"value\":\"" + hl7JsonEscape(o.value) + "\"";
    j += ",\"unit\":\"" + hl7JsonEscape(o.unit) + "\",\"range\":\"" + hl7JsonEscape(o.range) + "\",\"flag\":\"" + hl7JsonEscape(o.flag) + "\"}";
  }
  j += "]}";
  return j;
}
