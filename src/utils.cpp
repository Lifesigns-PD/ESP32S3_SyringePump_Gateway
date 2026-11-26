#include "utils.h"

String jsonEscape(const String& s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    unsigned char c = (unsigned char)s[i];
    switch (c) {
      case '"':  o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n";  break;
      case '\r': o += "\\r";  break;
      case '\t': o += "\\t";  break;
      default:
        if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += (char)c;
    }
  }
  return o;
}

String htmlEscape(const String& s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '&': o += "&amp;";  break;
      case '<': o += "&lt;";   break;
      case '>': o += "&gt;";   break;
      case '"': o += "&quot;"; break;
      default:  o += c;
    }
  }
  return o;
}

String httpCodeText(int code) {
  switch (code) {
    case 0:    return "nothing sent yet";
    case -1:   return "could not connect: refused, or no answer (wrong address, firewall, other network)";
    case -2:   return "could not send the request header";
    case -3:   return "could not send the data";
    case -4:   return "not connected";
    case -5:   return "connection lost";
    case -6:   return "no data stream";
    case -7:   return "the other end is not an HTTP server";
    case -8:   return "out of memory";
    case -9:   return "encoding not supported";
    case -10:  return "stream write error";
    case -11:  return "no answer in time (timeout)";
    case -100: return "no IP address on the WiFi / LAN interface in use";
    case -101: return "no server host set (Settings -> Server)";
    case -102: return "bad server address";
    case 400:  return "server says: bad request";
    case 401:  return "server says: login / header key wrong or missing";
    case 403:  return "server says: forbidden";
    case 404:  return "server says: path not found (check the Path)";
    case 413:  return "server says: message too large";
    case 500:  return "server says: internal error";
    case 502: case 503: case 504: return "server or gateway in between not available";
    default:   break;
  }
  if (code >= 200 && code < 300) return "OK";
  if (code >= 300 && code < 400) return "redirect (not followed)";
  return "HTTP status " + String(code);
}
