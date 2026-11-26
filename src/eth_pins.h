/*
 * eth_pins.h / eth_pins.cpp  -  is this a sane pin set for the W5500 on an ESP32-S3-WROOM-1 / WROOM-1U module?
 * Pure C++ (no Arduino code) so it is unit-tested on a PC: tools/pins_test.cpp
 *
 * Pins the module brings out (datasheet): GPIO0-21 and GPIO35-48.
 *   GPIO22-34   not available (22-25 do not exist, 26-32 are the flash / quad PSRAM, 33-34 are not on the module pads)
 *   GPIO35-37   used by the OCTAL PSRAM of ESP32-S3-WROOM-1(U)-N16R8 / -N8R8  -> not usable there (octalPsram = true)
 *   GPIO43/44   UART0 = the Serial debug console of this firmware (unless the console is on native USB)
 *   GPIO19/20   native USB (only reserved when the console is on native USB)
 * Strapping pins GPIO0, 3, 45, 46 are allowed but should be avoided (see the hint on the web page).
 */
#pragma once
#include <string>

bool ethPinUsable(int gpio, bool octalPsram, bool consoleOnUsb);

// sclk/miso/mosi/cs must be usable and all different. int and rst may be -1 (not connected; without INT the firmware polls the chip). The pump UART RX pin must not be used.
// mhz = SPI clock 1..40.
// "DE:AD:BE:EF:FE:ED" (any case) -> 6 bytes. Refuses wrong length/format, group (multicast) addresses and all-zero.
bool ethMacParse(const std::string& text, unsigned char* out6, std::string& err);

// ---- pump UART pins (the pump line may use the console pins GPIO43/44: the console is then switched off)
// rx = receives the pump's TX (required). tx = optional, -1 = none (this firmware only listens).
// lanPins = the W5500 pins in use (-1 entries are ignored); the pump pins must not collide with them.
bool pumpPinUsable(int gpio, bool octalPsram);
// The console (UART0) may keep running while the pump listens on RXD0 (GPIO44) alone: its TXD0 pin (GPIO43) stays free.
bool pumpConsoleCanStay(int rx, int tx);
// True when the pump line uses GPIO43 or GPIO44 (the UART0 pins).
bool pumpUsesUart0Pins(int rx, int tx);
bool pumpBaudValid(long baud);
bool pumpCfgValid(int rx, int tx, long baud, const int* lanPins, int nLan, bool octalPsram, std::string& err);

bool ethPinsValid(int sclk, int miso, int mosi, int cs, int intr, int rst, int mhz, int pumpRxPin,
                  bool octalPsram, bool consoleOnUsb, std::string& err);
