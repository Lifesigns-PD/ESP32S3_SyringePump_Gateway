// PC test of src/dev_proto.cpp
//   g++ -std=c++11 -Isrc tools/dev_proto_test.cpp src/dev_proto.cpp -o dev_proto_test
//   ./dev_proto_test                                   built-in cases
//   python tools/device_sim.py --dry-run | ./dev_proto_test --stdin     parse the simulator's real output (inf and pca lines)
#include "dev_proto.h"
#include <stdio.h>
#include <string.h>
#include <iostream>

static int fails = 0, total = 0;
#define CHECK(c) do { total++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static std::string mk(const std::string& body) {
  char cs[8];
  snprintf(cs, sizeof(cs), "%02X", devChecksum(body.data(), body.size()));
  return "$" + body + "*" + cs + "\r\n";
}

static DevJsonCtx ctx() {
  DevJsonCtx c;
  c.gatewayId = "mydevice"; c.mac = "AA:BB"; c.fw = "1.3.3"; c.iface = "ethernet"; c.ip = "1.2.3.4";
  c.transport = "tcp"; c.receivedAtUtc = ""; c.uptimeMs = 5; c.seq = 1;
  return c;
}

static bool balanced(const std::string& s) {      // cheap JSON sanity: braces / quotes balance
  int d = 0; bool q = false;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\\' && q) { i++; continue; }
    if (s[i] == '"') q = !q;
    else if (!q && s[i] == '{') d++;
    else if (!q && s[i] == '}') d--;
    if (d < 0) return false;
  }
  return d == 0 && !q;
}

int main(int argc, char** argv) {
  DevMsg m; std::string err;
  const std::string inf = mk("INF,INF-0001,RUNNING,NONE,25,100,12.5,118,Heparin");
  CHECK(devParseLine(DEV_INFUSION, inf, m, err));
  std::string js = devToJson(m, ctx());
  CHECK(balanced(js));
  CHECK(js.find("\"flow_rate_ml_h\":25") != std::string::npos);
  CHECK(js.find("\"infused_volume_ml\":12.5") != std::string::npos);
  CHECK(js.find("\"alarm_present\":false") != std::string::npos);
  CHECK(js.find("\"serial\":\"INF-0001\"") != std::string::npos);
  CHECK(js.find("\"source\":\"infusion_pump\"") != std::string::npos);
  CHECK(devChangeKey(m) == "RUNNING|NONE");

  const std::string pca = mk("PCA,PCA-0001,ALARM,HOURLY_LIMIT,Morphine,1,1,6,0.5,10,3.25,4,2");
  CHECK(devParseLine(DEV_PCA, pca, m, err));
  js = devToJson(m, ctx());
  CHECK(balanced(js));
  CHECK(js.find("\"alarm_present\":true") != std::string::npos);
  CHECK(js.find("\"total_given_mg\":3.25") != std::string::npos);
  CHECK(js.find("\"pca\":{") != std::string::npos);

  // rejects
  CHECK(!devParseLine(DEV_PCA, inf, m, err));                                   // wrong device tag
  CHECK(!devParseLine(DEV_INFUSION, "$INF,a,b*00\r\n", m, err));                // bad checksum
  CHECK(!devParseLine(DEV_INFUSION, "garbage", m, err));
  CHECK(!devParseLine(DEV_INFUSION, mk("INF,S,RUNNING,NONE,25,100,12.5,118"), m, err));             // too few fields
  CHECK(!devParseLine(DEV_INFUSION, mk("INF,S,RUNNING,NONE,abc,100,12.5,118,X"), m, err));          // not a number
  CHECK(!devParseLine(DEV_INFUSION, mk("INF,S,RUNNING,NONE,007,100,12.5,118,X"), m, err));          // leading zeros
  CHECK(!devParseLine(DEV_INFUSION, mk("INF,S,RUNNING,NONE,5.,100,12.5,118,X"), m, err));           // trailing dot
  CHECK(!devParseLine(DEV_INFUSION, mk("INF,S,FLYING,NONE,25,100,12.5,118,X"), m, err));            // unknown state
  CHECK(!devParseLine(DEV_INFUSION, mk("INF,,RUNNING,NONE,25,100,12.5,118,X"), m, err));            // empty serial
  CHECK(!devParseLine(DEV_INFUSION, mk("INF,S,RUNNING,,25,100,12.5,118,X"), m, err));               // empty alarm
  CHECK(devParseLine(DEV_INFUSION, mk("INF,S,IDLE,NONE,0,0,0,0,"), m, err));                        // empty drug is allowed
  CHECK(devParseLine(DEV_INFUSION, mk("INF,S,IDLE,NONE,-1.5,0,0,0,Q\"x"), m, err));                 // quote is escaped in JSON
  CHECK(balanced(devToJson(m, ctx())));
  CHECK(devParseLine(DEV_INFUSION, "noise" + inf, m, err));                                          // junk before $ is skipped
  // LF only line ending
  std::string lf = inf; lf.erase(lf.size() - 2, 2); lf += "\n";
  CHECK(devParseLine(DEV_INFUSION, lf, m, err));

  if (argc > 1 && !strcmp(argv[1], "--stdin")) {
    std::string l; int ok = 0, bad = 0;
    while (std::getline(std::cin, l)) {
      const size_t b = l.find('$');
      if (b == std::string::npos) continue;
      l = l.substr(b);
      const DevType t = l.compare(0, 4, "$INF") == 0 ? DEV_INFUSION : DEV_PCA;
      if (devParseLine(t, l, m, err)) { ok++; std::string j = devToJson(m, ctx()); if (!balanced(j)) bad++; if (ok <= 2 || ok % 40 == 0) puts(j.c_str()); }
      else { bad++; printf("REJECTED: %s (%s)\n", l.c_str(), err.c_str()); }
    }
    printf("stdin: %d valid, %d bad\n", ok, bad);
    if (bad) fails++;
  }
  printf("%d / %d checks passed\n", total - fails, total);
  return fails ? 1 : 0;
}
