// Unit test for src/eth_pins.cpp.  g++ -std=c++11 -Wall -Wextra -I../src pins_test.cpp ../src/eth_pins.cpp -o pins_test && ./pins_test
#include "eth_pins.h"
#include <stdio.h>
static int fails = 0, total = 0;
#define CHECK(c) do { total++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
int main() {
  std::string e;
  const bool QSPI = false, OCT = true, UART_CON = false, USB_CON = true;
  // ---- which pins exist on the module
  CHECK(ethPinUsable(0, QSPI, UART_CON) && ethPinUsable(21, QSPI, UART_CON) && ethPinUsable(35, QSPI, UART_CON) && ethPinUsable(48, QSPI, UART_CON));
  CHECK(!ethPinUsable(-1, QSPI, UART_CON) && !ethPinUsable(22, QSPI, UART_CON) && !ethPinUsable(26, QSPI, UART_CON) && !ethPinUsable(32, QSPI, UART_CON));
  CHECK(!ethPinUsable(33, QSPI, UART_CON) && !ethPinUsable(34, QSPI, UART_CON) && !ethPinUsable(49, QSPI, UART_CON));   // not on the module pads
  CHECK(!ethPinUsable(43, QSPI, UART_CON) && !ethPinUsable(44, QSPI, UART_CON) && ethPinUsable(19, QSPI, UART_CON));     // UART0 console
  CHECK(ethPinUsable(43, QSPI, USB_CON) && !ethPinUsable(19, QSPI, USB_CON) && !ethPinUsable(20, QSPI, USB_CON));       // console on native USB
  CHECK(ethPinUsable(36, QSPI, UART_CON) && !ethPinUsable(36, OCT, UART_CON) && ethPinUsable(38, OCT, UART_CON));       // octal PSRAM takes 35-37
  // ---- whole pin sets
  CHECK(ethPinsValid(12, 13, 11, 10, 4, 5, 20, 17, QSPI, UART_CON, e));       // default: FSPI pins
  CHECK(ethPinsValid(12, 13, 11, 10, 4, 5, 20, 17, OCT, UART_CON, e));        // default also fine on N16R8
  CHECK(ethPinsValid(13, 12, 11, 14, 10, 9, 20, 17, QSPI, UART_CON, e));      // Waveshare ESP32-S3-ETH preset
  CHECK(ethPinsValid(10, 11, 12, 9, 13, 14, 20, 17, QSPI, UART_CON, e));      // LilyGO T-ETH-Lite-S3 preset
  CHECK(ethPinsValid(12, 13, 11, 10, 4, -1, 20, 17, QSPI, UART_CON, e));      // reset pin not wired
  CHECK(!ethPinsValid(12, 13, 11, 10, 4, 5, 20, 12, QSPI, UART_CON, e) && e.find("RX") != std::string::npos);   // clash with the pump RX pin
  CHECK(!ethPinsValid(12, 12, 11, 10, 4, 5, 20, 17, QSPI, UART_CON, e) && e.find("same pin") != std::string::npos);
  CHECK(!ethPinsValid(12, 13, 11, 10, 4, 4, 20, 17, QSPI, UART_CON, e));      // INT and RST the same
  CHECK(ethPinsValid(12, 13, 11, 10, -1, 5, 20, 17, QSPI, UART_CON, e));      // INT not wired is allowed (the firmware polls)
  CHECK(ethPinsValid(41, 40, 39, 42, -1, 37, 14, 17, QSPI, UART_CON, e));     // Minew gateway pins from the reference firmware
  CHECK(!ethPinsValid(41, 40, 39, 42, -1, 37, 14, 17, OCT, UART_CON, e) && e.find("RST") != std::string::npos && e.find("octal PSRAM") != std::string::npos);  // GPIO37 clashes with octal PSRAM (N16R8)
  CHECK(!ethPinsValid(41, 40, 39, 42, 34, 37, 14, 17, QSPI, UART_CON, e));    // 34 is not on the module, so it cannot be a user INT pin
  CHECK(!ethPinsValid(12, 13, 11, 28, 4, 5, 20, 17, QSPI, UART_CON, e) && e.find("CS") != std::string::npos && e.find("not available") != std::string::npos);
  CHECK(!ethPinsValid(12, 13, 11, 10, 43, 5, 20, 17, QSPI, UART_CON, e) && e.find("UART0") != std::string::npos);
  CHECK(!ethPinsValid(12, 13, 11, 10, 36, 5, 20, 17, OCT, UART_CON, e) && e.find("octal PSRAM") != std::string::npos);
  CHECK(ethPinsValid(12, 13, 11, 10, 36, 5, 20, 17, QSPI, UART_CON, e));
  CHECK(!ethPinsValid(12, 13, 11, 10, 4, 5, 0, 17, QSPI, UART_CON, e) && !ethPinsValid(12, 13, 11, 10, 4, 5, 41, 17, QSPI, UART_CON, e));
  CHECK(ethPinsValid(12, 13, 11, 10, 4, 5, 1, 17, QSPI, UART_CON, e) && ethPinsValid(12, 13, 11, 10, 4, 5, 40, 17, QSPI, UART_CON, e));
  // ---- pump UART pins
  const int lanMinew[6] = {41, 40, 39, 42, -1, 37};
  CHECK(pumpCfgValid(17, -1, 115200, lanMinew, 6, QSPI, e));                       // the old default
  CHECK(pumpCfgValid(44, -1, 115200, lanMinew, 6, QSPI, e));                       // TTL header RX (UART0)
  CHECK(pumpCfgValid(43, -1, 115200, lanMinew, 6, QSPI, e));                       // TTL header TX pad used as RX
  CHECK(pumpCfgValid(44, 43, 115200, lanMinew, 6, QSPI, e));
  CHECK(!pumpCfgValid(44, 44, 115200, lanMinew, 6, QSPI, e));                      // RX = TX
  CHECK(!pumpCfgValid(30, -1, 115200, lanMinew, 6, QSPI, e) && e.find("RX") != std::string::npos);   // flash pin
  CHECK(!pumpCfgValid(33, -1, 115200, lanMinew, 6, QSPI, e));                      // not on the module
  CHECK(!pumpCfgValid(17, 28, 115200, lanMinew, 6, QSPI, e) && e.find("TX") != std::string::npos);
  CHECK(!pumpCfgValid(41, -1, 115200, lanMinew, 6, QSPI, e) && e.find("LAN chip") != std::string::npos);   // clash with the LAN SCLK
  CHECK(!pumpCfgValid(17, 40, 115200, lanMinew, 6, QSPI, e) && e.find("LAN chip") != std::string::npos);   // TX clashes with LAN MISO
  CHECK(pumpCfgValid(41, -1, 115200, lanMinew, 0, QSPI, e));                       // LAN not used (WiFi only): no clash check
  CHECK(!pumpCfgValid(36, -1, 115200, lanMinew, 6, OCT, e) && pumpCfgValid(36, -1, 115200, lanMinew, 6, QSPI, e));   // octal PSRAM takes 35-37
  CHECK(!pumpCfgValid(17, -1, 12345, lanMinew, 6, QSPI, e) && e.find("baud") != std::string::npos);
  CHECK(pumpBaudValid(115200) && pumpBaudValid(9600) && !pumpBaudValid(0) && !pumpBaudValid(-115200));
  // ---- UART0 pins / console
  CHECK(pumpCfgValid(44, -1, 115200, lanMinew, 6, QSPI, e));                       // the exact line of the module: RXD0 = GPIO44 (pad 36)
  CHECK(pumpUsesUart0Pins(44, -1) && pumpUsesUart0Pins(43, -1) && pumpUsesUart0Pins(17, 43) && !pumpUsesUart0Pins(17, -1) && !pumpUsesUart0Pins(18, 21));
  CHECK(pumpConsoleCanStay(44, -1));                                                // console TX (GPIO43) stays free
  CHECK(!pumpConsoleCanStay(44, 43) && !pumpConsoleCanStay(43, -1) && !pumpConsoleCanStay(17, -1) && !pumpConsoleCanStay(44, 44));
  // ---- MAC address parsing
  unsigned char m[6];
  CHECK(ethMacParse("DE:AD:BE:EF:FE:ED", m, e) && m[0] == 0xDE && m[1] == 0xAD && m[2] == 0xBE && m[3] == 0xEF && m[4] == 0xFE && m[5] == 0xED);   // the reference MAC
  CHECK(ethMacParse("de:ad:be:ef:fe:ed", m, e) && m[5] == 0xED);                          // lower case
  CHECK(ethMacParse("02:00:00:12:34:56", m, e) && m[0] == 0x02 && m[5] == 0x56);
  CHECK(!ethMacParse("DE:AD:BE:EF:FE", m, e));                                              // too short
  CHECK(!ethMacParse("DE:AD:BE:EF:FE:ED:00", m, e));                                        // too long
  CHECK(!ethMacParse("DE-AD-BE-EF-FE-ED", m, e));                                           // wrong separator
  CHECK(!ethMacParse("DE:AD:BE:EF:FE:EG", m, e));                                           // not hex
  CHECK(!ethMacParse("01:00:5E:00:00:01", m, e) && e.find("multicast") != std::string::npos);   // group address
  CHECK(!ethMacParse("00:00:00:00:00:00", m, e));
  { unsigned char keep[6] = {1,2,3,4,5,6}; ethMacParse("zz", keep, e); CHECK(keep[0] == 1 && keep[5] == 6); }   // output untouched on error
  printf("%d/%d checks passed\n", total - fails, total);
  return fails ? 1 : 0;
}
