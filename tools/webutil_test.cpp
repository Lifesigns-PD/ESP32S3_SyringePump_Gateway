// Unit test of src/webutil.h on a PC.  g++ -std=c++11 -Wall -Wextra -I../src webutil_test.cpp -o webutil_test && ./webutil_test
#include "webutil.h"
#include <stdio.h>
#include <stdlib.h>
static int fails = 0, total = 0;
#define CHECK(c) do { total++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

struct Sink : MultipartSink {
  std::map<std::string, std::string> fields; std::string fname, fieldName, data; int starts = 0, ends = 0; bool stopAfter = false; size_t maxData = (size_t)-1;
  void onField(const std::string& n, const std::string& v) override { fields[n] = v; }
  void onFileStart(const std::string& n, const std::string& f) override { starts++; fieldName = n; fname = f; }
  bool onFileData(const uint8_t* d, size_t n) override { data.append((const char*)d, n); return data.size() <= maxData; }
  void onFileEnd() override { ends++; }
};

static std::string build(const std::string& b, const std::string& file, bool preamble = false) {
  std::string s = preamble ? "garbage line\r\n" : "";
  s += "--" + b + "\r\nContent-Disposition: form-data; name=\"note\"\r\n\r\nhello\r\n";
  s += "--" + b + "\r\nContent-Disposition: form-data; name=\"firmware\"; filename=\"my fw.bin\"\r\nContent-Type: application/octet-stream\r\n\r\n";
  s += file + "\r\n--" + b + "--\r\n";
  return s;
}
static bool run(const std::string& body, const std::string& b, size_t chunk, Sink& k) {
  MultipartParser p(b, &k); bool ok = true;
  for (size_t i = 0; i < body.size(); i += chunk) ok = p.feed((const uint8_t*)body.data() + i, std::min(chunk, body.size() - i)) && ok;
  return ok && p.finished();
}

int main() {
  // ---- url decoding and forms
  CHECK(urlDecode("a%20b+c%2F%3D") == "a b c/=" && urlDecode("100%") == "100%" && urlDecode("%zz") == "%zz" && urlDecode("") == "");
  std::map<std::string, std::string> q; parseUrlEncoded("mode=1&ip=192.168.50.2&mac=DE%3AAD%3ABE&empty=&flag&sp=a+b", q);
  CHECK(q["mode"] == "1" && q["ip"] == "192.168.50.2" && q["mac"] == "DE:AD:BE" && q["empty"] == "" && q.count("flag") == 1 && q["sp"] == "a b" && q.size() == 6);
  std::map<std::string, std::string> q2; parseUrlEncoded("tserial=018090%2F22083105X&tmodel=Volumat%2A", q2);
  CHECK(q2["tserial"] == "018090/22083105X" && q2["tmodel"] == "Volumat*");
  // ---- base64 / basic auth ("admin:admin123" = YWRtaW46YWRtaW4xMjM=)
  std::string dec; CHECK(base64Decode("YWRtaW46YWRtaW4xMjM=", dec) && dec == "admin:admin123");
  CHECK(basicAuthMatches("Basic YWRtaW46YWRtaW4xMjM=", "admin", "admin123") && !basicAuthMatches("Basic YWRtaW46YWRtaW4xMjM=", "admin", "other"));
  CHECK(!basicAuthMatches("", "a", "b") && !basicAuthMatches("Bearer abc", "a", "b") && !basicAuthMatches("Basic !!!", "a", "b"));
  // ---- multipart, every chunk size from 1 byte up
  srand(7);
  std::string b = "----agilia0123456789abcdef";
  std::string bin; for (int i = 0; i < 6000; i++) bin += (char)(rand() & 0xFF); bin[0] = (char)0xE9;     // binary, starts like an ESP image
  // include look-alikes of the delimiter inside the data
  bin += "\r\n--"; bin += "\r\n--" + b.substr(0, 10); bin += "\r\n-"; bin += "\r"; bin += "\r\n--" + b.substr(0, b.size() - 1) + "X";
  std::string body = build(b, bin);
  size_t sizes[] = {1, 2, 3, 5, 7, 16, 63, 64, 100, 511, 1024, 1460, 4096, 100000};
  bool allOk = true;
  for (size_t c : sizes) {
    Sink k; bool ok = run(body, b, c, k);
    if (!(ok && k.data == bin && k.fname == "my fw.bin" && k.fieldName == "firmware" && k.starts == 1 && k.ends == 1 && k.fields["note"] == "hello")) { allOk = false; printf("  chunk size %zu failed (ok=%d data=%zu/%zu)\n", c, (int)ok, k.data.size(), bin.size()); }
  }
  CHECK(allOk);
  { Sink k; CHECK(run(build(b, bin, true), b, 37, k) && k.data == bin); }                                  // text before the first boundary is ignored
  { Sink k; CHECK(run(build(b, ""), b, 9, k) && k.data.empty() && k.starts == 1 && k.ends == 1); }          // empty file
  { Sink k; std::string big(300000, 'x'); for (size_t i = 0; i < big.size(); i += 97) big[i] = (char)i; CHECK(run(build(b, big), b, 1460, k) && k.data == big); }
  // ---- failure handling
  { Sink k; MultipartParser p(b, &k); std::string trunc = body.substr(0, body.size() - 20); p.feed((const uint8_t*)trunc.data(), trunc.size()); CHECK(!p.finished() && !p.failed()); }   // cut off: not finished
  { Sink k; k.maxData = 100; Sink& r = k; MultipartParser p(b, &r); CHECK(!p.feed((const uint8_t*)body.data(), body.size()) && p.failed()); }                                          // sink stops the upload
  { Sink k; MultipartParser p(b, &k); std::string bad = "--" + b + "XX"; CHECK(!p.feed((const uint8_t*)bad.data(), bad.size()) && p.failed()); }                                           // garbage after the boundary
  // a field called "xname" must not be taken as "name"
  { Sink k; std::string bb = "B"; std::string s = "--B\r\nContent-Disposition: form-data; xname=\"no\"; name=\"yes\"\r\n\r\nv\r\n--B--\r\n"; CHECK(run(s, bb, 5, k) && k.fields.count("yes") == 1); }
  printf("%d/%d checks passed\n", total - fails, total);
  return fails ? 1 : 0;
}
