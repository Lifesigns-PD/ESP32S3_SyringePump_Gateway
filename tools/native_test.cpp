// Native test: reads hex frames from stdin, prints decoded JSON.
//   g++ -std=c++11 -Wall -Wextra -I../src native_test.cpp ../src/agilia.cpp -o native_test
//   python3 make_test_frames.py | ./native_test
#include "agilia.h"
#include <stdio.h>
#include <string.h>
#include <string>
#include <iostream>

int main() {
  AgiliaParser p;
  std::string line;
  AgiliaJsonContext ctx = {"mydevice", "AA:BB:CC:DD:EE:FF", "test", 1000, "2026-11-03T10:00:00Z", false, "ethernet", "192.168.50.2"};
  int n = 0;
  while (std::getline(std::cin, line)) {
    bool bad = line.compare(0, 4, "BAD ") == 0;
    if (bad) line = line.substr(4);
    for (size_t i = 0; i + 1 < line.size(); i += 2) p.push((uint8_t)strtol(line.substr(i, 2).c_str(), 0, 16), 0);
    AgiliaFrame f;
    bool got = false;
    while (p.next(f, 0)) {
      got = true;
      std::string json;
      if (f.payload[0] == 0x80) {
        AgiliaStatus st;
        if (agiliaDecodeStatus(f, st, false)) {
          agiliaStatusToJson(f, st, ctx, json);
          printf("#%d SUMMARY: %s\n", ++n, agiliaSummary(st).c_str());
        } else agiliaRawToJson(f, ctx, json);
      } else if (f.payload[0] == 0xDF) {
        agiliaAdvancedToJson(f, ctx, json);
        printf("#%d ADVANCED\n", ++n);
      } else agiliaRawToJson(f, ctx, json);
      printf("%s\n\n", json.c_str());
    }
    if (bad) printf("BAD frame -> accepted=%s (expected: no)\n\n", got ? "YES (ERROR)" : "no");
  }
  printf("stats: bytes=%u ok=%u crc8Err=%u crc16Err=%u\n", p.stats.bytes, p.stats.framesOk, p.stats.crc8Err, p.stats.crc16Err);
  return 0;
}
