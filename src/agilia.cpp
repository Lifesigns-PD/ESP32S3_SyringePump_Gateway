#include "agilia.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

// =====================================================================
// CRC (verified against the example frame in the protocol document:
//  header 40 00 00 0B -> CRC8 = C0 ; payload 70 01 08 08 00 13 88 4E 20 FF FF -> CRC16 = 8C68)
// =====================================================================
uint8_t agiliaCrc8(const uint8_t* d, size_t n) {
  uint8_t c = 0;
  while (n--) {
    c ^= *d++;
    for (int i = 0; i < 8; i++) c = (c & 1) ? (uint8_t)((c >> 1) ^ 0x8C) : (uint8_t)(c >> 1);
  }
  return c;
}

uint16_t agiliaCrc16(const uint8_t* d, size_t n) {
  uint16_t c = 0;
  while (n--) {
    c ^= *d++;
    for (int i = 0; i < 8; i++) c = (c & 1) ? (uint16_t)((c >> 1) ^ 0xA001) : (uint16_t)(c >> 1);
  }
  return c;
}

// =====================================================================
// Frame parser:  ESC TYPE NO ACK SZ CRC8 [ID DATA...] [CRC16 big endian]
// =====================================================================
AgiliaParser::AgiliaParser() : len_(0), lastMs_(0) {
  memset(&stats, 0, sizeof(stats));
}

void AgiliaParser::drop(size_t n) {
  if (n >= len_) { len_ = 0; return; }
  memmove(buf_, buf_ + n, len_ - n);
  len_ -= n;
}

void AgiliaParser::push(uint8_t b, uint32_t nowMs) {
  stats.bytes++;
  if (len_ >= sizeof(buf_)) drop(1);
  buf_[len_++] = b;
  lastMs_ = nowMs;
}

bool AgiliaParser::next(AgiliaFrame& out, uint32_t nowMs) {
  for (;;) {
    size_t i = 0;
    while (i < len_ && buf_[i] != 0x1B) i++;       // find start of frame
    if (i) drop(i);
    if (len_ < 6) return false;

    if (agiliaCrc8(buf_ + 1, 4) != buf_[5]) {       // bad header -> resync
      stats.crc8Err++;
      drop(1);
      continue;
    }
    size_t sz = buf_[4];
    size_t total = 6 + (sz ? sz + 2 : 0);
    if (len_ < total) {
      if ((uint32_t)(nowMs - lastMs_) > 1000) { drop(1); continue; }   // stalled partial frame
      return false;
    }

    bool crcOff = false;
    if (sz) {
      uint16_t rx = (uint16_t)((buf_[6 + sz] << 8) | buf_[6 + sz + 1]);
      uint16_t calc = agiliaCrc16(buf_ + 6, sz);
      if (rx == 0xFFFF && calc != 0xFFFF) crcOff = true;           // CRC16 deactivated
      else if (rx != calc) { stats.crc16Err++; drop(1); continue; }
    }

    out.type = buf_[1];
    out.no   = buf_[2];
    out.ack  = buf_[3];
    out.sz   = (uint8_t)sz;
    out.crc16Disabled = crcOff;
    if (sz) memcpy(out.payload, buf_ + 6, sz);
    drop(total);
    stats.framesOk++;
    return true;
  }
}

// =====================================================================
// Helpers
// =====================================================================
class Reader {
public:
  Reader(const uint8_t* d, size_t n) : p_(d), n_(n), pos_(0) {}
  size_t remaining() const { return n_ - pos_; }
  const uint8_t* cur() const { return p_ + pos_; }
  bool skip(size_t k) { if (remaining() < k) return false; pos_ += k; return true; }
  bool u8(uint8_t& v)   { if (remaining() < 1) return false; v = p_[pos_++]; return true; }
  bool u16(uint16_t& v) { if (remaining() < 2) return false; v = (uint16_t)((p_[pos_] << 8) | p_[pos_ + 1]); pos_ += 2; return true; }
  bool u32(uint32_t& v) {
    if (remaining() < 4) return false;
    v = ((uint32_t)p_[pos_] << 24) | ((uint32_t)p_[pos_ + 1] << 16) | ((uint32_t)p_[pos_ + 2] << 8) | p_[pos_ + 3];
    pos_ += 4; return true;
  }
  bool s32(int32_t& v)  { uint32_t t; if (!u32(t)) return false; v = (int32_t)t; return true; }
  bool str(char* out, size_t outSize, size_t fieldLen, bool allowEscape);
  bool vstr(char* out, size_t outSize, size_t maxLen);
private:
  const uint8_t* p_; size_t n_; size_t pos_;
};

// CP1252-ish -> UTF-8. A leading byte < 0x20 (non-zero) is the codepage escape code and is skipped.
static void decodeText(const uint8_t* s, size_t n, bool allowEscape, char* out, size_t outSize) {
  size_t o = 0;
  size_t i = 0;
  if (allowEscape && n > 0 && s[0] != 0 && s[0] < 0x20) i = 1;
  for (; i < n && s[i] != 0; i++) {
    uint8_t c = s[i];
    if (c < 0x20) continue;
    if (c < 0x80) { if (o + 1 < outSize) out[o++] = (char)c; }
    else if (c >= 0xA0) {
      if (o + 2 < outSize) { out[o++] = (char)(0xC0 | (c >> 6)); out[o++] = (char)(0x80 | (c & 0x3F)); }
    } else { if (o + 1 < outSize) out[o++] = '?'; }
  }
  while (o > 0 && out[o - 1] == ' ') o--;     // trim trailing spaces
  out[o] = 0;
}

bool Reader::str(char* out, size_t outSize, size_t fieldLen, bool allowEscape) {
  if (remaining() < fieldLen) return false;
  decodeText(cur(), fieldLen, allowEscape, out, outSize);
  pos_ += fieldLen;
  return true;
}

bool Reader::vstr(char* out, size_t outSize, size_t maxLen) {
  size_t k = 0;
  while (k < maxLen && k < remaining() && cur()[k] != 0) k++;
  decodeText(cur(), k, true, out, outSize);
  size_t used = k;
  if (k < maxLen && k < remaining()) used++;      // consume terminating NUL
  return skip(used);
}

static uint32_t fnv(uint32_t h, const void* d, size_t n) {
  const uint8_t* p = (const uint8_t*)d;
  while (n--) { h ^= *p++; h *= 16777619u; }
  return h;
}

// ---------------------------------------------------------------- JSON writer
std::string agiliaJsonEscape(const char* s) {
  std::string o;
  for (; s && *s; s++) {
    unsigned char c = (unsigned char)*s;
    switch (c) {
      case '"':  o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n";  break;
      case '\r': o += "\\r";  break;
      case '\t': o += "\\t";  break;
      default:
        if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += (char)c;
    }
  }
  return o;
}

namespace {
struct Json {
  std::string s;
  bool first[12];
  int d;
  Json() : d(0) { s.reserve(1800); first[0] = true; }
  void sep() { if (!first[d]) s += ','; first[d] = false; }
  void key(const char* k) { sep(); s += '"'; s += k; s += "\":"; }
  void begin(const char* k, char open) { if (k) key(k); else sep(); s += open; d++; first[d] = true; }
  void end(char close) { s += close; d--; }
  void obj(const char* k = 0) { begin(k, '{'); }
  void endObj() { end('}'); }
  void arr(const char* k) { begin(k, '['); }
  void endArr() { end(']'); }
  void strVal(const char* v) { s += '"'; s += agiliaJsonEscape(v); s += '"'; }
  void str(const char* k, const char* v) { key(k); strVal(v); }
  void item(const char* v) { sep(); strVal(v); }
  void u32(const char* k, uint32_t v) { key(k); char b[16]; snprintf(b, sizeof(b), "%lu", (unsigned long)v); s += b; }
  void i32(const char* k, int32_t v)  { key(k); char b[16]; snprintf(b, sizeof(b), "%ld", (long)v); s += b; }
  void milli(const char* k, uint32_t v) { key(k); char b[20]; snprintf(b, sizeof(b), "%lu.%03lu", (unsigned long)(v / 1000), (unsigned long)(v % 1000)); s += b; }
  void milliS(const char* k, int32_t v) {
    key(k); char b[24];
    uint32_t a = v < 0 ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
    snprintf(b, sizeof(b), "%s%lu.%03lu", v < 0 ? "-" : "", (unsigned long)(a / 1000), (unsigned long)(a % 1000));
    s += b;
  }
  void boolean(const char* k, bool v) { key(k); s += v ? "true" : "false"; }
  void null(const char* k) { key(k); s += "null"; }
  void hex(const char* k, uint32_t v, int w) { key(k); char b[16]; snprintf(b, sizeof(b), "\"0x%0*lX\"", w, (unsigned long)v); s += b; }
};
}  // namespace

// ---------------------------------------------------------------- unit decoding (§3.2.1)
void agiliaUnitToString(uint16_t u, char* out, size_t n) {
  if (u == 0) { if (n) out[0] = 0; return; }
  static const char* const prefix[8] = {"p", "n", "\xC2\xB5", "m", "", "k", "M", "G"};
  static const char* const unit[8]   = {"g", "l", "U", "mol", "cal", "?", "?", "?"};
  static const char* const timeS[4]  = {"", "/min", "/h", "/24h"};
  static const char* const dilS[4]   = {"", "/ml", "/l", "/X ml"};
  std::string r;
  r += prefix[(u >> 9) & 7];
  r += unit[(u >> 6) & 7];
  if ((u >> 12) & 1) r += "/m\xC2\xB2";
  if ((u >> 5) & 1)  r += "/kg";
  r += timeS[(u >> 3) & 3];
  r += dilS[(u >> 1) & 3];
  if (u & 1) r += "/ml plasma";
  snprintf(out, n, "%s", r.c_str());
}

// ---------------------------------------------------------------- name tables
// Injectomat alarm bits (bits 0-15 alarms, 16-22 pre-alarms, 23-31 information)
static const char* const kAlarmBit[32] = {
  "end_of_limit_volume", "end_of_volume_time", "end_of_infusion", "battery", "occlusion",
  "syringe_clamp", "flange", "plunger_head", "disengagement", 0, "force_sensor", 0, 0, 0, 0, 0,
  "end_of_limit_volume", "end_of_volume_time", "end_of_infusion", "battery", "occlusion", 0, 0,
  "start_infusion_delayed_at_end_of_pause", "flow_rate_low", "flow_rate_high", "syringe_empty",
  "power_disconnection", "pressure_rise", "pressure_drop", "keyboard_locked", "night_mode"
};

struct CodeName { uint8_t code; const char* name; };

static const CodeName kStepInj[] = {
  {0x00, "other"}, {0x01, "syringe_selection"}, {0x02, "wait_for_prime"},
  {0x03, "flow_rate_selection"}, {0x83, "flow_rate_selection_infusing"},
  {0x04, "drug_selection"}, {0x05, "volume_time_programming"},
  {0x06, "loading_dose_selection"}, {0x07, "waiting_loading_dose_launch"},
  {0x08, "programmed_bolus_selection"}, {0x88, "programmed_bolus_selection_infusing"},
  {0x09, "waiting_programmed_bolus_launch"}, {0x89, "waiting_programmed_bolus_launch_infusing"},
  {0x0A, "simple_bolus_rate_selection"}, {0x8A, "simple_bolus_rate_selection_infusing"},
  {0x0D, "waiting_before_launch"}, {0x0F, "menu"}, {0x8F, "menu_infusing"},
  {0x10, "infusion"}, {0x11, "simple_bolus_since_stop"}, {0x91, "simple_bolus_since_infusion"},
  {0x13, "prime"}, {0x20, "stop"}, {0x21, "occlusion"}, {0x22, "kvo_infusion"},
  {0x23, "error"}, {0x24, "pause"}, {0x30, "booting"}, {0xFF, "unknown"}
};

static const CodeName kModeInj[] = {
  {0, "unknown_standby"}, {1, "basic_infusion"}, {2, "volume_time"}, {3, "simple_bolus"},
  {4, "prime"}, {5, "manual_bolus"}, {6, "testing_function"}, {7, "loading_dose"},
  {8, "programmed_bolus"}, {9, "tci"}
};

template <size_t N>
static const char* lookup(const CodeName (&t)[N], uint8_t code) {
  for (size_t i = 0; i < N; i++) if (t[i].code == code) return t[i].name;
  return "unknown";
}

static const char* deviceName(uint8_t d) {
  switch (d) {
    case 0x42: return "Injectomat Agilia";
    case 0x44: return "Volumat Agilia";
    case 0x02: case 0x12: return "Link+";
    default: return "unknown";
  }
}

static const char* operName(uint8_t m) {
  switch (m) { case 1: return "idle"; case 2: return "on"; case 0: return "other"; case 0xFE: return "other"; default: return "unknown"; }
}

// =====================================================================
// Device status frame 0x80
// =====================================================================
static void parseInfusion(Reader& b, AgiliaInfusion& i) {
  uint16_t extra;
  if (!b.u32(i.flowRate) || !b.str(i.drug, sizeof(i.drug), 20, true) || !b.u16(i.flowUnit)) return;
  if (((i.flowUnit >> 1) & 3) == 3) b.u16(extra);                 // "/X ml" -> extra WORD after unit
  if (!b.u32(i.volume) || !b.u16(i.pressure) || !b.u32(i.dilution) || !b.u16(i.dilUnit) ||
      !b.u16(i.dilVol) || !b.u32(i.leftVolume) || !b.u8(i.syringeSize) ||
      !b.str(i.syringe, sizeof(i.syringe), 20, true)) return;
  i.present = true;
  if (!b.u32(i.userVolume)) return;
  i.hasUserVolume = true;
  if (!b.u32(i.userMass) || !b.u16(i.userMassUnit)) return;
  if (((i.userMassUnit >> 1) & 3) == 3) b.u16(extra);
  i.hasUserMass = true;
  if (!b.u32(i.volPri)) return;
  i.hasVolPri = true;
  if (!b.u32(i.volSec)) return;
  i.hasVolSec = true;
}

static void parsePatient(Reader& b, AgiliaPatient& p) {
  if (!b.u32(p.created) || !b.vstr(p.last, sizeof(p.last), 30) || !b.vstr(p.first, sizeof(p.first), 30) ||
      !b.u32(p.weightG) || !b.u16(p.ageMonths) || !b.u8(p.gender) || !b.vstr(p.code, sizeof(p.code), 30)) return;
  p.present = true;
  if (b.u16(p.height) && b.u16(p.bsa)) p.hasHeightBsa = true;
}

bool agiliaDecodeStatus(const AgiliaFrame& f, AgiliaStatus& s, bool wantPatient) {
  memset(&s, 0, sizeof(s));
  if (f.sz < 2 || f.payload[0] != 0x80) return false;
  Reader r(f.payload + 1, f.sz - 1);

  uint8_t dev;
  if (!r.u8(dev)) return false;
  s.deviceType = dev;
  if (dev != 0x42 && dev != 0x44) return false;            // Link+ and others not decoded

  if (!r.str(s.version, sizeof(s.version), 4, false) || !r.str(s.serial, sizeof(s.serial), 16, false) ||
      !r.u32(s.alarm) || !r.u8(s.error) || !r.u8(s.silalCnt) || !r.u32(s.pumpTime)) return false;

  // The rest is shorter when the pump is in idle mode -> parse as far as data exists.
  do {
    if (!r.u8(s.operMode) || !r.u8(s.status)) break;
    s.hasOper = true;
    if (!r.u8(s.step)) break;
    s.hasStep = true;
    if (!r.u16(s.statusWord)) break;
    s.hasStatusWord = true;
    if (!r.u8(s.mode)) break;
    s.hasMode = true;
  } while (0);

  // Optional sub-blocks: [ZoneSize][SubId][data...], ZoneSize counts SubId + data
  while (s.hasMode && r.remaining() >= 2) {
    const uint8_t* base = r.cur();
    uint8_t size = base[0];
    if (size < 1 || r.remaining() < (size_t)size + 1) { s.truncated = true; break; }
    uint8_t sid = base[1];
    Reader b(base + 2, size - 1);
    r.skip((size_t)size + 1);

    if (sid == 0x01) {
      if (wantPatient) parsePatient(b, s.pat);
    } else if (sid == 0x02) {
      parseInfusion(b, s.inf);
    } else if (sid == 0x04) {
      if (b.u32(s.tci.plasma) && b.u32(s.tci.effect) && b.u32(s.tci.target) && b.u16(s.tci.unit) && b.u8(s.tci.mode))
        s.tci.present = true;
    } else if (sid == 0x05) {
      if (b.u8(s.subDevice)) s.hasSubDevice = true;
    }
  }

  uint32_t h = 2166136261u;
  h = fnv(h, &s.alarm, sizeof(s.alarm));
  h = fnv(h, &s.error, 1);
  h = fnv(h, &s.operMode, 1);
  h = fnv(h, &s.status, 1);
  h = fnv(h, &s.step, 1);
  h = fnv(h, &s.statusWord, 2);
  h = fnv(h, &s.mode, 1);
  h = fnv(h, &s.inf.flowRate, 4);
  h = fnv(h, s.inf.drug, strlen(s.inf.drug));
  h = fnv(h, s.inf.syringe, strlen(s.inf.syringe));
  h = fnv(h, &s.subDevice, 1);
  s.changeKey = h;
  return true;
}

// =====================================================================
// JSON output
// =====================================================================
static void isoNoZone(uint32_t epoch, char* out, size_t n) {
  time_t t = (time_t)epoch;
  struct tm tmv;
  gmtime_r(&t, &tmv);
  strftime(out, n, "%Y-%m-%dT%H:%M:%S", &tmv);
}

static void writeHeader(Json& j, const AgiliaFrame& f, const AgiliaJsonContext& c) {
  j.u32("schema", 1);
  j.obj("gateway");
  j.str("id", c.gatewayId ? c.gatewayId : "");
  j.str("mac", c.mac ? c.mac : "");
  j.str("fw", c.fw ? c.fw : "");
  j.u32("uptime_ms", c.uptimeMs);
  if (c.receivedAtUtc && c.receivedAtUtc[0]) j.str("received_at", c.receivedAtUtc);
  if (c.iface && c.iface[0]) j.str("iface", c.iface);
  if (c.ip && c.ip[0]) j.str("ip", c.ip);
  j.endObj();
  j.obj("frame");
  j.hex("id", f.sz ? f.payload[0] : 0, 2);
  j.hex("type", f.type, 2);
  j.u32("no", f.no);
  j.u32("slot", f.slot());
  j.boolean("crc16_checked", !f.crc16Disabled);
  j.endObj();
}

void agiliaStatusToJson(const AgiliaFrame& f, const AgiliaStatus& s, const AgiliaJsonContext& c, std::string& out) {
  Json j;
  j.obj();
  writeHeader(j, f, c);

  const bool inj = (s.deviceType == 0x42);

  j.obj("device");
  j.str("model", deviceName(s.deviceType));
  j.str("class", agiliaDeviceClass(s.deviceType));
  j.hex("type_code", s.deviceType, 2);
  j.str("version", s.version);
  j.str("serial", s.serial);
  if (s.hasSubDevice) {
    j.hex("sub_device_code", s.subDevice, 2);
    j.str("sub_device", s.subDevice == 0x00 ? "standard" : s.subDevice == 0x20 ? "dose_rate" : s.subDevice == 0x40 ? "tiva" : "unknown");
  }
  j.endObj();

  j.obj("status");
  char tb[24];
  isoNoZone(s.pumpTime, tb, sizeof(tb));
  j.u32("pump_time_epoch", s.pumpTime);
  j.str("pump_time", tb);
  j.u32("error", s.error);
  j.u32("silence_alarm_count", s.silalCnt);
  if (s.hasOper) {
    j.u32("oper_mode", s.operMode);
    j.str("oper_mode_name", operName(s.operMode));
    j.hex("status_code", s.status, 2);
  }
  if (s.hasStep) {
    j.hex("step_code", s.step, 2);
    if (inj) j.str("step", lookup(kStepInj, s.step));
  }
  if (s.hasMode) {
    j.u32("infusion_mode_code", s.mode);
    if (inj) j.str("infusion_mode", lookup(kModeInj, s.mode));
  }
  if (s.hasStatusWord) {
    j.hex("status_word", s.statusWord, 4);
    j.boolean("beeping", s.statusWord & 0x01);
    j.boolean("alarm_present", s.statusWord & 0x02);
    j.boolean("prealarm_present", s.statusWord & 0x04);
    j.boolean("information_present", s.statusWord & 0x08);
    j.boolean("infusion_in_progress", s.statusWord & 0x10);
    j.boolean("mains_connected", s.statusWord & 0x20);
  }
  j.endObj();

  j.obj("alarms");
  j.hex("raw", s.alarm, 8);
  if (inj) {
    j.arr("alarms");
    for (int b = 0; b <= 15; b++) if ((s.alarm >> b) & 1 && kAlarmBit[b]) j.item(kAlarmBit[b]);
    j.endArr();
    j.arr("prealarms");
    for (int b = 16; b <= 22; b++) if ((s.alarm >> b) & 1 && kAlarmBit[b]) j.item(kAlarmBit[b]);
    j.endArr();
    j.arr("information");
    for (int b = 23; b <= 31; b++) if ((s.alarm >> b) & 1 && kAlarmBit[b]) j.item(kAlarmBit[b]);
    j.endArr();
  }
  j.endObj();

  if (s.inf.present) {
    const AgiliaInfusion& i = s.inf;
    char u[32];
    j.obj("infusion");
    j.milli("flow_rate", i.flowRate);
    agiliaUnitToString(i.flowUnit, u, sizeof(u));
    j.str("flow_rate_unit", u);
    j.str("drug_name", i.drug);
    j.milli("infused_volume_ml", i.volume);
    j.u32("pressure_mmhg", i.pressure);
    j.milli("dilution", i.dilution);
    agiliaUnitToString(i.dilUnit, u, sizeof(u));
    j.str("dilution_unit", u);
    j.u32("dilution_volume_ml", i.dilVol);
    j.milli("left_volume_ml", i.leftVolume);
    j.u32("syringe_size_ml", i.syringeSize);
    j.str("syringe_name", i.syringe);
    if (i.hasUserVolume) { if (i.userVolume == 0xFFFFFFFFu) j.null("user_volume_ml"); else j.milli("user_volume_ml", i.userVolume); }
    if (i.hasUserMass) {
      j.milli("user_mass", i.userMass);
      agiliaUnitToString(i.userMassUnit, u, sizeof(u));
      j.str("user_mass_unit", u);
    }
    if (i.hasVolPri) { if (i.volPri == 0xFFFFFFFFu) j.null("user_volume_primary_ml"); else j.milli("user_volume_primary_ml", i.volPri); }
    if (i.hasVolSec) { if (i.volSec == 0xFFFFFFFFu) j.null("user_volume_secondary_ml"); else j.milli("user_volume_secondary_ml", i.volSec); }
    j.endObj();
  }

  if (s.tci.present) {
    char u[32];
    agiliaUnitToString(s.tci.unit, u, sizeof(u));
    j.obj("tci");
    j.milli("plasma", s.tci.plasma);
    j.milli("effect", s.tci.effect);
    j.milli("target", s.tci.target);
    j.str("unit", u);
    j.str("mode", s.tci.mode == 1 ? "plasma" : s.tci.mode == 2 ? "effect" : "unknown");
    j.endObj();
  }

  if (c.includePatient && s.pat.present) {
    j.obj("patient");
    j.str("last_name", s.pat.last);
    j.str("first_name", s.pat.first);
    j.str("code", s.pat.code);
    j.u32("weight_g", s.pat.weightG);
    j.u32("age_months", s.pat.ageMonths);
    j.str("gender", s.pat.gender == 1 ? "male" : s.pat.gender == 2 ? "female" : "unknown");
    if (s.pat.hasHeightBsa) { j.u32("height_cm", s.pat.height); j.milli("bsa_m2", (uint32_t)s.pat.bsa); }
    j.endObj();
  }

  j.endObj();
  out = j.s;
}

// =====================================================================
// Advanced infusion data 0xDF
// =====================================================================
bool agiliaAdvancedToJson(const AgiliaFrame& f, const AgiliaJsonContext& c, std::string& out) {
  if (f.sz < 6 || f.payload[0] != 0xDF) return false;
  Reader r(f.payload + 1, f.sz - 1);
  uint32_t sample;
  if (!r.u32(sample)) return false;

  Json j;
  j.obj();
  writeHeader(j, f, c);
  j.obj("advanced");
  j.u32("sample_time_10ms", sample);

  while (r.remaining() >= 1) {
    uint8_t size;
    r.u8(size);
    if (size == 0) break;                                   // end of data
    if (r.remaining() < size) break;
    Reader b(r.cur() + 1, size - 1);
    uint8_t id = r.cur()[0];
    r.skip(size);

    uint32_t a = 0, v = 0, d = 0, mass = 0;
    uint16_t mu = 0;
    int32_t sf = 0;
    char u[32];
    switch (id) {
      case 0x07:
        if (b.s32(sf)) j.milliS("flow_rate_ml_h", sf);
        break;
      case 0x0C:
        if (b.u32(v)) {
          j.obj("infused_volume");
          j.milli("volume_ml", v);
          if (b.u32(mass) && b.u16(mu)) { j.milli("mass", mass); agiliaUnitToString(mu, u, sizeof(u)); j.str("mass_unit", u); }
          j.endObj();
        }
        break;
      case 0x0E:
        if (b.u32(v) && b.u32(d)) {
          j.obj("manual_bolus");
          j.milli("bolus_ml", v);
          j.milli("total_ml", d);
          if (b.u32(mass) && b.u16(mu)) { j.milli("mass", mass); agiliaUnitToString(mu, u, sizeof(u)); j.str("mass_unit", u); }
          j.endObj();
        }
        break;
      case 0x10:
        if (b.u32(a) && b.u32(v)) {
          j.obj("simple_bolus");
          j.milli("flow_rate_ml_h", a);
          j.milli("volume_ml", v);
          if (b.u32(mass) && b.u16(mu)) { j.milli("mass", mass); agiliaUnitToString(mu, u, sizeof(u)); j.str("mass_unit", u); }
          j.endObj();
        }
        break;
      case 0x4D:    // loading dose: flow rate in nl/h
      case 0x4E:    // programmed bolus: flow rate in 1/1000 ml/h
        if (b.u32(a) && b.u32(v) && b.u32(d)) {
          j.obj(id == 0x4D ? "loading_dose" : "programmed_bolus");
          if (id == 0x4D) j.milli("flow_rate_ml_h", a / 1000u);   // nl/h -> 1/1000 ml/h
          else            j.milli("flow_rate_ml_h", a);
          j.milli("remaining_volume_ml", v);
          j.u32("remaining_duration_s", d);
          if (b.u32(mass) && b.u16(mu)) { j.milli("remaining_mass", mass); agiliaUnitToString(mu, u, sizeof(u)); j.str("mass_unit", u); }
          j.endObj();
        }
        break;
      default:
        break;
    }
  }
  j.endObj();
  j.endObj();
  out = j.s;
  return true;
}

void agiliaRawToJson(const AgiliaFrame& f, const AgiliaJsonContext& c, std::string& out) {
  Json j;
  j.obj();
  writeHeader(j, f, c);
  std::string hx;
  char b[4];
  for (uint8_t i = 0; i < f.sz; i++) { snprintf(b, sizeof(b), "%02X", f.payload[i]); hx += b; }
  j.str("raw_hex", hx.c_str());
  j.endObj();
  out = j.s;
}

const char* agiliaDeviceName(uint8_t d) { return deviceName(d); }

const char* agiliaDeviceClass(uint8_t d) {
  switch (d) {
    case 0x42: return "syringe_pump";       // Injectomat Agilia
    case 0x44: return "infusion_pump";      // Volumat Agilia
    default:   return "other";
  }
}

// =====================================================================
// One line summary for logs
// =====================================================================
std::string agiliaSummary(const AgiliaStatus& s) {
  std::string o = deviceName(s.deviceType);
  char b[96];
  snprintf(b, sizeof(b), " v%s SN=%s", s.version, s.serial);
  o += b;
  if (s.hasOper) { o += " op="; o += operName(s.operMode); }
  if (s.deviceType == 0x42) {
    if (s.hasStep) { o += " step="; o += lookup(kStepInj, s.step); }
    if (s.hasMode) { o += " mode="; o += lookup(kModeInj, s.mode); }
  }
  if (s.inf.present) {
    char u[32];
    agiliaUnitToString(s.inf.flowUnit, u, sizeof(u));
    snprintf(b, sizeof(b), " rate=%lu.%03lu %s vol=%lu.%03lu ml p=%ummHg",
             (unsigned long)(s.inf.flowRate / 1000), (unsigned long)(s.inf.flowRate % 1000), u,
             (unsigned long)(s.inf.volume / 1000), (unsigned long)(s.inf.volume % 1000), (unsigned)s.inf.pressure);
    o += b;
    if (s.inf.drug[0]) { o += " drug="; o += s.inf.drug; }
  }
  if (s.deviceType == 0x42 && s.alarm) {
    o += " alarms=";
    bool first = true;
    for (int i = 0; i < 32; i++) if ((s.alarm >> i) & 1 && kAlarmBit[i]) {
      if (!first) o += ',';
      first = false;
      o += (i >= 23 ? "i:" : i >= 16 ? "pre:" : "");
      o += kAlarmBit[i];
    }
  }
  return o;
}
