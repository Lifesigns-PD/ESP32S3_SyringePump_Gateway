/*
 * gw_string.h  -  small String and IPAddress classes with the semantics the gateway code was written against
 *
 * These are part of THIS project (plain C++, no Arduino library): String is a std::string with the helper methods the firmware uses
 * (toInt, trim, substring, startsWith ...), IPAddress is a 4-byte address. Keeping them lets the ESP-IDF port keep the original logic
 * line by line. Pure C++ so they are unit-tested on a PC (tools/gwstring_test.cpp).
 */
#pragma once
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

class String : public std::string {
 public:
  String() {}
  String(const char* s) : std::string(s ? s : "") {}
  String(const std::string& s) : std::string(s) {}
  String(char c) : std::string(1, c) {}
  String(int v) : std::string(num("%d", v)) {}
  String(unsigned v) : std::string(num("%u", v)) {}
  String(long v) : std::string(num("%ld", v)) {}
  String(unsigned long v) : std::string(num("%lu", v)) {}
  String(long long v) : std::string(num("%lld", v)) {}
  String(unsigned long long v) : std::string(num("%llu", v)) {}
  String(double v, unsigned char decimals = 2) { char f[8]; snprintf(f, sizeof(f), "%%.%uf", (unsigned)decimals); char b[40]; snprintf(b, sizeof(b), f, v); assign(b); }

  // ---- Arduino-style helpers
  long toInt() const { return atol(c_str()); }
  float toFloat() const { return (float)atof(c_str()); }
  bool reserve(unsigned n) { std::string::reserve(n); return true; }
  bool isEmpty() const { return empty(); }
  bool equals(const String& o) const { return compare(o) == 0; }
  bool equalsIgnoreCase(const String& o) const {
    if (size() != o.size()) return false;
    for (size_t i = 0; i < size(); i++) if (tolower((unsigned char)(*this)[i]) != tolower((unsigned char)o[i])) return false;
    return true;
  }
  bool startsWith(const String& p) const { return size() >= p.size() && compare(0, p.size(), p) == 0; }
  bool endsWith(const String& p) const { return size() >= p.size() && compare(size() - p.size(), p.size(), p) == 0; }
  int indexOf(char c, unsigned from = 0) const { size_t p = find(c, from); return p == npos ? -1 : (int)p; }
  int indexOf(const String& s, unsigned from = 0) const { size_t p = find(s, from); return p == npos ? -1 : (int)p; }
  int lastIndexOf(char c) const { size_t p = rfind(c); return p == npos ? -1 : (int)p; }
  int lastIndexOf(const String& s) const { size_t p = rfind(s); return p == npos ? -1 : (int)p; }
  String substring(unsigned from) const { return from >= size() ? String() : String(substr(from)); }
  String substring(unsigned from, unsigned to) const {
    if (from > to) { unsigned t = from; from = to; to = t; }
    if (from >= size()) return String();
    if (to > size()) to = (unsigned)size();
    return String(substr(from, to - from));
  }
  void remove(unsigned index) { if (index < size()) erase(index); }
  void remove(unsigned index, unsigned count) { if (index < size()) erase(index, count); }
  void toUpperCase() { for (size_t i = 0; i < size(); i++) (*this)[i] = (char)toupper((unsigned char)(*this)[i]); }
  void toLowerCase() { for (size_t i = 0; i < size(); i++) (*this)[i] = (char)tolower((unsigned char)(*this)[i]); }
  void trim() {
    size_t b = 0, e = size();
    while (b < e && isspace((unsigned char)(*this)[b])) b++;
    while (e > b && isspace((unsigned char)(*this)[e - 1])) e--;
    assign(substr(b, e - b));
  }
  void replace(const String& from, const String& to) {
    if (from.empty()) return;
    size_t p = 0;
    while ((p = find(from, p)) != npos) { std::string::replace(p, from.size(), to); p += to.size(); }
  }

  // += with numbers appends the number as text (like Arduino), += char / text as usual
  String& operator+=(const String& s) { append(s); return *this; }
  String& operator+=(const char* s) { if (s) append(s); return *this; }
  String& operator+=(char c) { push_back(c); return *this; }
  String& operator+=(int v) { append(num("%d", v)); return *this; }
  String& operator+=(unsigned v) { append(num("%u", v)); return *this; }
  String& operator+=(long v) { append(num("%ld", v)); return *this; }
  String& operator+=(unsigned long v) { append(num("%lu", v)); return *this; }

 private:
  template <typename T> static std::string num(const char* fmt, T v) { char b[32]; snprintf(b, sizeof(b), fmt, v); return std::string(b); }
};

inline String operator+(const String& a, const String& b) { String r(a); r.append(b); return r; }
inline String operator+(const String& a, const char* b) { String r(a); if (b) r.append(b); return r; }
inline String operator+(const char* a, const String& b) { String r(a); r.append(b); return r; }
inline String operator+(const String& a, char b) { String r(a); r.push_back(b); return r; }
inline String operator+(char a, const String& b) { String r(a); r.append(b); return r; }

// ---------------------------------------------------------------- IPAddress
// Stored like lwIP does (first octet in the lowest byte), so a value from lwIP can be used directly.
class IPAddress {
 public:
  IPAddress() : a_(0) {}
  IPAddress(uint8_t o1, uint8_t o2, uint8_t o3, uint8_t o4) : a_((uint32_t)o1 | ((uint32_t)o2 << 8) | ((uint32_t)o3 << 16) | ((uint32_t)o4 << 24)) {}
  IPAddress(uint32_t addr) : a_(addr) {}
  bool fromString(const char* s) {
    if (!s) return false;
    unsigned o[4];
    int n = 0, val = 0, digits = 0;
    for (const char* p = s;; p++) {
      if (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); if (++digits > 3 || val > 255) return false; }
      else if (*p == '.' || *p == 0) {
        if (digits == 0 || n > 3) return false;
        o[n++] = (unsigned)val; val = 0; digits = 0;
        if (*p == 0) break;
      } else return false;
    }
    if (n != 4) return false;
    a_ = o[0] | (o[1] << 8) | (o[2] << 16) | (o[3] << 24);
    return true;
  }
  bool fromString(const String& s) { return fromString(s.c_str()); }
  String toString() const { char b[20]; snprintf(b, sizeof(b), "%u.%u.%u.%u", (unsigned)(*this)[0], (unsigned)(*this)[1], (unsigned)(*this)[2], (unsigned)(*this)[3]); return String(b); }
  operator uint32_t() const { return a_; }
  uint8_t operator[](int i) const { return (uint8_t)((a_ >> (8 * i)) & 0xFF); }
  bool operator==(const IPAddress& o) const { return a_ == o.a_; }
  bool operator!=(const IPAddress& o) const { return a_ != o.a_; }
 private:
  uint32_t a_;
};
