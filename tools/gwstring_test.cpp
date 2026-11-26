// Unit test of src/gw_string.h on a PC.   g++ -std=c++11 -Wall -Wextra -I../src gwstring_test.cpp -o gwstring_test && ./gwstring_test
#include "gw_string.h"
#include <stdio.h>
static int fails = 0, total = 0;
#define CHECK(c) do { total++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
int main() {
  // construction from numbers (Arduino semantics)
  CHECK(String(42) == "42" && String(-7) == "-7" && String(3000000000UL) == "3000000000" && String((unsigned)65) == "65");
  CHECK(String((uint8_t)5) == "5" && String((uint16_t)443) == "443" && String((int8_t)-1) == "-1");
  CHECK(String('x') == "x" && String("abc") == "abc" && String((const char*)nullptr) == "");
  CHECK(String(1.5, 1) == "1.5" && String(2.0, 3) == "2.000");
  // concatenation
  String s = "a"; s += "b"; s += 'c'; s += 12; s += String("d");
  CHECK(s == "abc12d");
  CHECK((String("x=") + String(5)) == "x=5" && ("p:" + String(80)) == "p:80" && (String("a") + 'b') == "ab" && (String("a") + "b" + String(1)) == "ab1");
  // searching and cutting
  String u = "http://host:8080/api";
  CHECK(u.startsWith("http://") && !u.startsWith("https") && u.endsWith("/api") && !u.endsWith("x"));
  CHECK(u.indexOf(':') == 4 && u.indexOf("host") == 7 && u.indexOf('#') == -1 && u.lastIndexOf('/') == 16 && u.indexOf(':', 5) == 11);
  CHECK(u.substring(7) == "host:8080/api" && u.substring(7, 11) == "host" && u.substring(11, 7) == "host" && u.substring(100) == "" && u.substring(0, 100) == u);
  String r = u; r.remove(11); CHECK(r == "http://host");
  r = u; r.remove(0, 7); CHECK(r == "host:8080/api");
  // toInt like atol: leading number, sign, text -> 0
  CHECK(String("123abc").toInt() == 123 && String("-5").toInt() == -5 && String("abc").toInt() == 0 && String("").toInt() == 0 && String("  7").toInt() == 7);
  // trim / case
  String t = "  hello \t\n"; t.trim(); CHECK(t == "hello");
  String e = "   "; e.trim(); CHECK(e == "" && e.length() == 0);
  String c = "aBc1"; c.toUpperCase(); CHECK(c == "ABC1"); c.toLowerCase(); CHECK(c == "abc1");
  CHECK(String("Ab").equalsIgnoreCase("aB") && !String("a").equalsIgnoreCase("ab"));
  String rep = "a-b-c"; rep.replace("-", "::"); CHECK(rep == "a::b::c");
  // std::string interop used by the code
  CHECK(String("abc").size() == 3 && String("abc").c_str()[1] == 'b' && String("abc").find('c') == 2 && String("").empty());
  std::string plain = String("xyz"); CHECK(plain == "xyz");
  String fromStd(std::string("q")); CHECK(fromStd == "q");
  // IPAddress
  IPAddress ip; CHECK(ip.fromString("192.168.50.2") && ip.toString() == "192.168.50.2" && ip[0] == 192 && ip[3] == 2);
  CHECK(!ip.fromString("192.168.50") && !ip.fromString("1.2.3.4.5") && !ip.fromString("256.1.1.1") && !ip.fromString("a.b.c.d") && !ip.fromString("") && !ip.fromString("1..2.3"));
  CHECK(IPAddress(10, 1, 2, 3).toString() == "10.1.2.3" && IPAddress(0, 0, 0, 0) == IPAddress() && IPAddress(1, 2, 3, 4) != IPAddress(1, 2, 3, 5));
  uint32_t raw = IPAddress(172, 16, 22, 201); CHECK(raw == (172u | (16u << 8) | (22u << 16) | (201u << 24)));   // same layout as lwIP's ip4_addr.addr
  CHECK(IPAddress((uint32_t)raw).toString() == "172.16.22.201");
  CHECK((((uint32_t)IPAddress(172, 16, 22, 201)) & ((uint32_t)IPAddress(255, 255, 254, 0))) == (((uint32_t)IPAddress(172, 16, 23, 5)) & ((uint32_t)IPAddress(255, 255, 254, 0))));
  printf("%d/%d checks passed\n", total - fails, total);
  return fails ? 1 : 0;
}
