#include "eth_pins.h"
#include <ctype.h>
#include <stdlib.h>

bool ethPinUsable(int g, bool octalPsram, bool consoleOnUsb) {
  const bool onModule = (g >= 0 && g <= 21) || (g >= 35 && g <= 48);
  if (!onModule) return false;
  if (octalPsram && g >= 35 && g <= 37) return false;
  if (consoleOnUsb ? (g == 19 || g == 20) : (g == 43 || g == 44)) return false;
  return true;
}

static std::string why(int g, bool octalPsram, bool consoleOnUsb) {
  if (consoleOnUsb ? (g == 19 || g == 20) : (g == 43 || g == 44))
    return consoleOnUsb ? "is native USB (the debug console)" : "is UART0 (TXD0/RXD0 = the debug console and programming port)";
  if (octalPsram && g >= 35 && g <= 37) return "is used by the octal PSRAM of this module (N16R8 / N8R8)";
  return "is not available on the ESP32-S3-WROOM-1(U) module (usable: GPIO0-21 and 35-48)";
}

bool ethPinsValid(int sclk, int miso, int mosi, int cs, int intr, int rst, int mhz, int pumpRx,
                  bool octalPsram, bool consoleOnUsb, std::string& err) {
  struct P { const char* name; int pin; bool optional; };
  const P p[6] = { {"SCLK", sclk, false}, {"MISO", miso, false}, {"MOSI", mosi, false}, {"CS", cs, false}, {"INT", intr, true}, {"RST", rst, true} };
  for (int i = 0; i < 6; i++) {
    if (p[i].optional && p[i].pin == -1) continue;
    if (!ethPinUsable(p[i].pin, octalPsram, consoleOnUsb)) {
      err = std::string(p[i].name) + " pin " + std::to_string(p[i].pin) + " " + why(p[i].pin, octalPsram, consoleOnUsb);
      return false;
    }
    if (p[i].pin == pumpRx) { err = std::string(p[i].name) + " pin " + std::to_string(p[i].pin) + " is the pump UART RX pin (PUMP_RX_PIN)"; return false; }
    for (int k = 0; k < i; k++) {
      if (p[k].pin == p[i].pin) { err = std::string(p[k].name) + " and " + p[i].name + " use the same pin " + std::to_string(p[i].pin); return false; }
    }
  }
  if (mhz < 1 || mhz > 40) { err = "SPI clock must be 1-40 MHz"; return false; }
  return true;
}

bool ethMacParse(const std::string& t, unsigned char* out, std::string& err) {
  if (t.size() != 17) { err = "MAC address must look like DE:AD:BE:EF:FE:ED"; return false; }
  unsigned char m[6];
  for (int i = 0; i < 6; i++) {
    const char a = t[i * 3], b = t[i * 3 + 1];
    if (!isxdigit((unsigned char)a) || !isxdigit((unsigned char)b) || (i < 5 && t[i * 3 + 2] != ':')) { err = "MAC address must look like DE:AD:BE:EF:FE:ED"; return false; }
    char h[3] = {a, b, 0};
    m[i] = (unsigned char)strtoul(h, nullptr, 16);
  }
  if (m[0] & 1) { err = "MAC address is a group (multicast) address: the first byte must be even"; return false; }
  if ((m[0] | m[1] | m[2] | m[3] | m[4] | m[5]) == 0) { err = "MAC address 00:00:00:00:00:00 is not usable"; return false; }
  for (int i = 0; i < 6; i++) out[i] = m[i];
  return true;
}

// ---------------------------------------------------------------- pump UART
bool pumpPinUsable(int g, bool octalPsram) {
  const bool onModule = (g >= 0 && g <= 21) || (g >= 35 && g <= 48);
  if (!onModule) return false;
  if (octalPsram && g >= 35 && g <= 37) return false;
  return true;                                   // 43/44 are allowed here on purpose (the TTL header of a gateway)
}

bool pumpBaudValid(long b) {
  static const long ok[] = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600};
  for (unsigned i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) if (ok[i] == b) return true;
  return false;
}

bool pumpCfgValid(int rx, int tx, long baud, const int* lan, int nLan, bool octalPsram, std::string& err) {
  if (!pumpPinUsable(rx, octalPsram)) { err = "Pump RX pin " + std::to_string(rx) + " cannot be used (ESP32-S3-WROOM-1: GPIO0-21 and 35-48)"; return false; }
  if (tx != -1 && !pumpPinUsable(tx, octalPsram)) { err = "Pump TX pin " + std::to_string(tx) + " cannot be used (leave it empty: this firmware only listens)"; return false; }
  if (tx == rx) { err = "Pump RX and TX cannot be the same pin"; return false; }
  if (!pumpBaudValid(baud)) { err = "Unsupported baud rate (use 1200 ... 921600; the Agilia pump uses 115200)"; return false; }
  for (int i = 0; i < nLan; i++) {
    if (lan[i] < 0) continue;
    if (lan[i] == rx) { err = "Pump RX pin " + std::to_string(rx) + " is already used by the LAN chip"; return false; }
    if (tx >= 0 && lan[i] == tx) { err = "Pump TX pin " + std::to_string(tx) + " is already used by the LAN chip"; return false; }
  }
  return true;
}

bool pumpConsoleCanStay(int rx, int tx) { return rx == 44 && tx != 43 && tx != 44; }
bool pumpUsesUart0Pins(int rx, int tx) { return rx == 43 || rx == 44 || tx == 43 || tx == 44; }
