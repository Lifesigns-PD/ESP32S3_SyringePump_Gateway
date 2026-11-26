/*
 * Agilia serial export protocol (Fresenius Kabi, CC2777 rev.6) - decoder
 *
 * Pure C++ (no Arduino dependency) so it can be unit-tested on a PC.
 *  - AgiliaParser : byte stream -> validated frames (CRC8 + CRC16)
 *  - agiliaDecodeStatus()    : frame ID 0x80  (Device status, Injectomat / Volumat)
 *  - agiliaStatusToJson()    : decoded status -> JSON
 *  - agiliaAdvancedToJson()  : frame ID 0xDF  (Advanced infusion data) -> JSON
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string>

// ---------------------------------------------------------------- CRC
uint8_t  agiliaCrc8(const uint8_t* d, size_t n);    // reflected poly 0x8C, init 0
uint16_t agiliaCrc16(const uint8_t* d, size_t n);   // CRC-16/ARC (0xA001), init 0

// ---------------------------------------------------------------- frames
struct AgiliaFrame {
  uint8_t type;           // 0x60 spontaneous (direct), 0x40 standard, 0x61..0x68 / 0x41..0x48 = Link+ slot
  uint8_t no;
  uint8_t ack;
  uint8_t sz;             // size of payload (ID + DATA)
  uint8_t payload[255];   // payload[0] = frame ID
  bool    crc16Disabled;  // CRC16 field was 0xFFFF
  uint8_t slot() const {
    uint8_t lo = type & 0x0F, hi = type & 0xF0;
    return ((hi == 0x40 || hi == 0x60) && lo >= 1 && lo <= 8) ? lo : 0;
  }
};

struct AgiliaStats {
  uint32_t bytes, framesOk, crc8Err, crc16Err;
};

class AgiliaParser {
public:
  AgiliaParser();
  void push(uint8_t b, uint32_t nowMs);
  bool next(AgiliaFrame& out, uint32_t nowMs);   // call repeatedly until false
  AgiliaStats stats;
private:
  uint8_t  buf_[300];
  size_t   len_;
  uint32_t lastMs_;
  void drop(size_t n);
};

// ---------------------------------------------------------------- decoded status (0x80)
struct AgiliaInfusion {
  bool present;                       // base fields decoded
  uint32_t flowRate;                  // 1/1000 units
  char     drug[48];
  uint16_t flowUnit;
  uint32_t volume;                    // 1/1000 ml
  uint16_t pressure;                  // mmHg
  uint32_t dilution;                  // 1/1000
  uint16_t dilUnit;
  uint16_t dilVol;                    // ml
  uint32_t leftVolume;                // 1/1000 ml
  uint8_t  syringeSize;               // ml
  char     syringe[48];
  bool     hasUserVolume;  uint32_t userVolume;
  bool     hasUserMass;    uint32_t userMass;  uint16_t userMassUnit;
  bool     hasVolPri;      uint32_t volPri;
  bool     hasVolSec;      uint32_t volSec;
};

struct AgiliaPatient {
  bool present;
  uint32_t created;
  char last[64], first[64], code[64];
  uint32_t weightG;
  uint16_t ageMonths;
  uint8_t  gender;
  bool     hasHeightBsa;
  uint16_t height, bsa;
};

struct AgiliaTci {
  bool present;
  uint32_t plasma, effect, target;
  uint16_t unit;
  uint8_t  mode;
};

struct AgiliaStatus {
  uint8_t  deviceType;                // 0x42 Injectomat, 0x44 Volumat
  char     version[5];
  char     serial[17];
  uint32_t alarm;
  uint8_t  error;
  uint8_t  silalCnt;
  uint32_t pumpTime;                  // seconds since 1970 (pump internal clock)
  bool     hasOper;   uint8_t  operMode, status;
  bool     hasStep;   uint8_t  step;
  bool     hasStatusWord; uint16_t statusWord;
  bool     hasMode;   uint8_t  mode;
  AgiliaInfusion inf;
  AgiliaPatient  pat;
  AgiliaTci      tci;
  bool     hasSubDevice; uint8_t subDevice;
  bool     truncated;
  uint32_t changeKey;                 // hash of the "interesting" fields (for send-on-change)
};

// Returns false if the frame is not a decodable 0x80 Injectomat/Volumat status frame.
bool agiliaDecodeStatus(const AgiliaFrame& f, AgiliaStatus& out, bool parsePatient);

// ---------------------------------------------------------------- JSON
struct AgiliaJsonContext {
  const char* gatewayId;
  const char* mac;
  const char* fw;
  uint32_t    uptimeMs;
  const char* receivedAtUtc;          // "" if time not synced
  bool        includePatient;
  const char* iface;                  // "ethernet" / "wifi" / "" (network the JSON leaves the gateway on)
  const char* ip;                     // gateway IP address on that network ("" if unknown)
};

void agiliaStatusToJson(const AgiliaFrame& f, const AgiliaStatus& s, const AgiliaJsonContext& c, std::string& out);
bool agiliaAdvancedToJson(const AgiliaFrame& f, const AgiliaJsonContext& c, std::string& out);   // ID 0xDF
void agiliaRawToJson(const AgiliaFrame& f, const AgiliaJsonContext& c, std::string& out);        // any other frame

std::string agiliaSummary(const AgiliaStatus& s);              // one human readable line
const char* agiliaDeviceName(uint8_t deviceType);              // "Injectomat Agilia", "Volumat Agilia", ...
const char* agiliaDeviceClass(uint8_t deviceType);             // "syringe_pump", "infusion_pump", "other"
std::string agiliaJsonEscape(const char* s);
void        agiliaUnitToString(uint16_t u, char* out, size_t n);
