/*
 * webutil.h  -  pure C++ helpers of the web server layer (unit-tested on a PC: tools/webutil_test.cpp)
 *
 *   urlDecode / parseUrlEncoded   query strings and form bodies
 *   base64Decode / basicAuthMatches  "Authorization: Basic ..." check
 *   MultipartParser               streaming parser for the firmware upload (multipart/form-data), any chunking
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string>
#include <map>
#include <vector>

inline int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

inline std::string urlDecode(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '+') o += ' ';
    else if (s[i] == '%' && i + 2 < s.size() + 0 && hexVal(s[i + 1]) >= 0 && hexVal(s[i + 2]) >= 0) { o += (char)(hexVal(s[i + 1]) * 16 + hexVal(s[i + 2])); i += 2; }
    else o += s[i];
  }
  return o;
}

// "a=1&b=x%20y&flag" -> map (later values win; a key without '=' gets an empty value)
inline void parseUrlEncoded(const std::string& s, std::map<std::string, std::string>& out) {
  size_t i = 0;
  while (i <= s.size()) {
    size_t amp = s.find('&', i);
    if (amp == std::string::npos) amp = s.size();
    if (amp > i) {
      std::string pair = s.substr(i, amp - i);
      size_t eq = pair.find('=');
      if (eq == std::string::npos) out[urlDecode(pair)] = "";
      else out[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
    }
    i = amp + 1;
  }
}

inline bool base64Decode(const std::string& in, std::string& out) {
  static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  out.clear();
  int val = 0, bits = -8;
  for (size_t i = 0; i < in.size(); i++) {
    char c = in[i];
    if (c == '=') break;
    const char* p = nullptr;
    for (const char* t = T; *t; t++) if (*t == c) { p = t; break; }
    if (!p) return false;
    val = (val << 6) | (int)(p - T);
    bits += 6;
    if (bits >= 0) { out += (char)((val >> bits) & 0xFF); bits -= 8; }
  }
  return true;
}

// header value is "Basic dXNlcjpwYXNz"
inline bool basicAuthMatches(const std::string& header, const char* user, const char* pass) {
  if (header.size() < 7 || header.compare(0, 6, "Basic ") != 0) return false;
  std::string dec;
  if (!base64Decode(header.substr(6), dec)) return false;
  return dec == std::string(user) + ":" + pass;
}

// ---------------------------------------------------------------- multipart/form-data, streaming
class MultipartSink {
 public:
  virtual ~MultipartSink() {}
  virtual void onField(const std::string& name, const std::string& value) = 0;
  virtual void onFileStart(const std::string& name, const std::string& filename) = 0;
  virtual bool onFileData(const uint8_t* data, size_t len) = 0;     // return false to stop the upload
  virtual void onFileEnd() = 0;
};

class MultipartParser {
 public:
  MultipartParser(const std::string& boundary, MultipartSink* sink) : sink_(sink), state_(PREAMBLE) {
    delim_ = "\r\n--" + boundary;
    fail_.assign(delim_.size(), 0);                    // KMP failure table: the delimiter may be cut anywhere by the chunking
    for (size_t i = 1, k = 0; i < delim_.size(); i++) {
      while (k > 0 && delim_[i] != delim_[k]) k = fail_[k - 1];
      if (delim_[i] == delim_[k]) k++;
      fail_[i] = k;
    }
    match_ = 2;                                       // the first boundary line is not preceded by CRLF: pretend it was
  }

  // returns false on a malformed body or when the sink stopped the upload
  bool feed(const uint8_t* d, size_t n) {
    for (size_t i = 0; i < n && state_ != DONE && state_ != FAILED; i++) step(d[i]);
    flush();
    return state_ != FAILED;
  }
  bool finished() const { return state_ == DONE; }
  bool failed() const { return state_ == FAILED; }

 private:
  enum State { PREAMBLE, AFTER_BOUNDARY, HEADERS, BODY, DONE, FAILED };

  void emit(const char* p, size_t n) {
    if (!isFile_) { if (value_.size() < 2048) value_.append(p, n); return; }
    out_.append(p, n);
    if (out_.size() >= 1024) flush();
  }
  void flush() {
    if (out_.empty()) return;
    if (!sink_->onFileData((const uint8_t*)out_.data(), out_.size())) state_ = FAILED;
    out_.clear();
  }
  void endPart() {
    if (isFile_) { flush(); if (state_ != FAILED) sink_->onFileEnd(); }
    else sink_->onField(name_, value_);
    value_.clear();
  }
  void step(uint8_t c) {
    switch (state_) {
      case PREAMBLE:                                  // skip until the first "--boundary"
      case BODY: {
        while (match_ > 0 && (char)c != delim_[match_]) {
          size_t k = fail_[match_ - 1];
          if (state_ == BODY) emit(delim_.data(), match_ - k);
          match_ = k;
        }
        if ((char)c == delim_[match_]) {
          if (++match_ == delim_.size()) { match_ = 0; if (state_ == BODY) endPart(); state_ = AFTER_BOUNDARY; tail_ = 0; }
        } else if (state_ == BODY) {
          char ch = (char)c;
          emit(&ch, 1);
        }
        break;
      }
      case AFTER_BOUNDARY:
        if (tail_ == 0) { if (c == '-') tail_ = 1; else if (c == '\r') tail_ = 2; else if (c == ' ' || c == '\t') {} else state_ = FAILED; }
        else if (tail_ == 1) { if (c == '-') state_ = DONE; else state_ = FAILED; }
        else { if (c == '\n') { state_ = HEADERS; hdr_.clear(); } else state_ = FAILED; }
        break;
      case HEADERS:
        hdr_ += (char)c;
        if (hdr_.size() > 4096) { state_ = FAILED; break; }
        if (hdr_.size() >= 4 && hdr_.compare(hdr_.size() - 4, 4, "\r\n\r\n") == 0) startPart();
        else if (hdr_.size() == 2 && hdr_ == "\r\n") startPart();           // part without headers
        break;
      default: break;
    }
  }
  static std::string param(const std::string& h, const char* key) {            // name="x" / filename="y"
    std::string k = std::string(key) + "=\"";
    size_t p = 0;
    while ((p = h.find(k, p)) != std::string::npos) {
      if (p == 0 || h[p - 1] == ' ' || h[p - 1] == ';') {                      // "name=" must not match inside "filename="
        size_t s = p + k.size(), e = h.find('"', s);
        return e == std::string::npos ? std::string() : h.substr(s, e - s);
      }
      p += k.size();
    }
    return std::string();
  }
  void startPart() {
    name_ = param(hdr_, "name");
    std::string fn = param(hdr_, "filename");
    isFile_ = hdr_.find("filename=\"") != std::string::npos;
    state_ = BODY;
    match_ = 0;
    value_.clear();
    out_.clear();
    if (isFile_) sink_->onFileStart(name_, fn);
  }

  MultipartSink* sink_;
  State state_;
  std::string delim_, hdr_, name_, value_, out_;
  std::vector<size_t> fail_;
  size_t match_ = 0;
  int tail_ = 0;
  bool isFile_ = false;
};
