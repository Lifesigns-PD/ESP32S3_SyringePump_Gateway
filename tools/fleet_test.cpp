// Unit test for src/fleet_logic.cpp (runs on the PC, not part of the firmware).
//   g++ -std=c++11 -Wall -Wextra -I../src fleet_test.cpp ../src/fleet_logic.cpp -o fleet_test && ./fleet_test
#include "fleet_logic.h"
#include <stdio.h>
#include <string>

static int fails = 0, total = 0;
#define CHECK(cond) do { total++; if (!(cond)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

int main() {
  // ---- device class from model
  CHECK(std::string(fleetClassFromModel("Injectomat Agilia")) == "syringe_pump");
  CHECK(std::string(fleetClassFromModel("Volumat Agilia")) == "infusion_pump");
  CHECK(std::string(fleetClassFromModel("Philips IntelliVue Patient Monitor")) == "patient_monitor");
  CHECK(std::string(fleetClassFromModel("Link+")) == "other");
  CHECK(fleetClassValid("syringe_pump") && !fleetClassValid("toaster"));

  // ---- wildcard match
  CHECK(fleetGlobMatch("", "anything"));
  CHECK(fleetGlobMatch("Injectomat Agilia", "injectomat agilia"));
  CHECK(fleetGlobMatch("Injectomat*", "Injectomat Agilia"));
  CHECK(fleetGlobMatch("*Agilia", "Volumat Agilia"));
  CHECK(fleetGlobMatch("018090/*", "018090/22083105X"));
  CHECK(fleetGlobMatch("018090/2208310?X", "018090/22083105X"));
  CHECK(fleetGlobMatch("*", ""));
  CHECK(!fleetGlobMatch("018091/*", "018090/22083105X"));
  CHECK(!fleetGlobMatch("Volumat*", "Injectomat Agilia"));
  CHECK(!fleetGlobMatch("abc", "abcd"));
  CHECK(fleetGlobMatch("a*b*c", "aXXbYYc"));
  CHECK(!fleetGlobMatch("a*b*c", "aXXbYY"));

  // ---- target match
  {
    FleetTarget t; std::string why;
    CHECK(t.any());
    CHECK(fleetTargetMatches(t, "syringe_pump", "Injectomat Agilia", "SN1", why));
    t.model = "Volumat*";
    CHECK(!fleetTargetMatches(t, "syringe_pump", "Injectomat Agilia", "SN1", why));
    CHECK(why.find("model") != std::string::npos);
    t.model = "Injectomat*"; t.serial = "SN1";
    CHECK(fleetTargetMatches(t, "syringe_pump", "Injectomat Agilia", "SN1", why));
    CHECK(!fleetTargetMatches(t, "syringe_pump", "Injectomat Agilia", "SN2", why));
    CHECK(why.find("serial") != std::string::npos);
    t = FleetTarget(); t.cls = "infusion_pump";
    CHECK(!fleetTargetMatches(t, "syringe_pump", "x", "y", why));
    CHECK(why.find("class") != std::string::npos);
  }

  // ---- versions
  CHECK(fleetVersionCompare("1.2.0", "1.2.0") == 0);
  CHECK(fleetVersionCompare("1.2.10", "1.2.9") == 1);
  CHECK(fleetVersionCompare("1.2.9", "1.2.10") == -1);
  CHECK(fleetVersionCompare("1.3", "1.2.9") == 1);
  CHECK(fleetVersionCompare("2.0.0", "1.9.9") == 1);
  CHECK(fleetVersionCompare("v1.2.0", "1.2.0") == 0);
  CHECK(fleetVersionCompare("1.2.0-beta", "1.2.0") == 0);
  CHECK(fleetVersionCompare("1.1.0", "1.2.0") == -1);
  CHECK(fleetVersionCompare("", "") == 0);

  // ---- url
  CHECK(fleetUrlEncode("018090/22083105X") == "018090%2F22083105X");
  CHECK(fleetUrlEncode("Injectomat Agilia") == "Injectomat%20Agilia");
  CHECK(fleetUrlEncode("a-b_c.d~e") == "a-b_c.d~e");
  CHECK(fleetUrlEncode("a&b=c") == "a%26b%3Dc");
  CHECK(fleetResolveUrl("http://10.0.0.5:8080", "/firmware/x.bin") == "http://10.0.0.5:8080/firmware/x.bin");
  CHECK(fleetResolveUrl("http://10.0.0.5:8080/", "firmware/x.bin") == "http://10.0.0.5:8080/firmware/x.bin");
  CHECK(fleetResolveUrl("http://a", "https://b/c.bin") == "https://b/c.bin");

  // ---- flat JSON
  {
    std::string j = "{\"update\": true, \"version\":\"1.2.0\",\"url\":\"/firmware/a b.bin\",\"size\":1234567,"
                    "\"sha256\":\"ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789\",\"force\":false,"
                    "\"rule\":\"model=\\\"Injectomat*\\\"\"}";
    std::string s; bool b = false; uint32_t n = 0;
    CHECK(fleetJsonBool(j, "update", b) && b);
    CHECK(fleetJsonBool(j, "force", b) && !b);
    CHECK(fleetJsonString(j, "version", s) && s == "1.2.0");
    CHECK(fleetJsonString(j, "url", s) && s == "/firmware/a b.bin");
    CHECK(fleetJsonUInt(j, "size", n) && n == 1234567u);
    CHECK(fleetJsonString(j, "rule", s) && s == "model=\"Injectomat*\"");
    CHECK(!fleetJsonString(j, "missing", s));
    CHECK(!fleetJsonUInt(j, "version", n));             // not a number
    CHECK(fleetJsonString(j, "sha256", s) && fleetIsSha256Hex(s));
    // a VALUE that looks like a key must not confuse the reader
    std::string k = "{\"reason\":\"version\",\"version\":\"9.9.9\"}";
    CHECK(fleetJsonString(k, "version", s) && s == "9.9.9");
    CHECK(!fleetJsonString("{\"a\":\"unterminated", "a", s));
  }

  // ---- sha256 text
  CHECK(!fleetIsSha256Hex("abc"));
  CHECK(!fleetIsSha256Hex(std::string(64, 'g')));
  CHECK(fleetHexEqual("ABCDEF", "abcdef") && !fleetHexEqual("abc", "abd"));

  printf("%d/%d checks passed\n", total - fails, total);
  return fails ? 1 : 0;
}
