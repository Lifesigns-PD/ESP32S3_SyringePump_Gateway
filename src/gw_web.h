/*
 * gw_web.h / gw_web.cpp  -  the web server of the gateway on ESP-IDF's esp_http_server
 *
 * Handlers are written exactly as before: server.arg("x"), server.send(code, type, body), server.authenticate(user, pass) ...
 * A request is read completely (query string, form body) before the handler runs. File uploads (multipart/form-data) are streamed
 * to the route's upload handler in pieces (UPLOAD_FILE_START / WRITE / END), then the route's normal handler answers.
 */
#pragma once
#include <map>
#include <string>
#include <utility>
#include <vector>
#include "esp_http_server.h"
#include "gw_compat.h"

#define WEB_GET  1
#define WEB_POST 2

enum { UPLOAD_FILE_START, UPLOAD_FILE_WRITE, UPLOAD_FILE_END, UPLOAD_FILE_ABORTED };

struct WebUpload {
  int      status = UPLOAD_FILE_START;
  String   filename;
  String   name;
  uint8_t* buf = nullptr;
  size_t   currentSize = 0;
  size_t   totalSize = 0;
};

typedef void (*WebHandler)();

class GwWebServer {
 public:
  bool begin(uint16_t port = 80);
  void on(const char* uri, int method, WebHandler handler, WebHandler uploadHandler = nullptr);

  String arg(const char* name) const;
  bool   hasArg(const char* name) const;
  void   send(int code, const char* contentType, const String& body);
  void   sendHeader(const char* name, const String& value);
  bool   authenticate(const char* user, const char* pass);
  void   requestAuthentication();
  WebUpload& upload() { return upload_; }

 private:
  struct Route { String uri; int method; WebHandler handler; WebHandler uploadHandler; };
  static esp_err_t dispatch(httpd_req_t* req);
  void readBody(Route* r);
  bool readForm();
  void readMultipart(Route* r, const std::string& contentType);

  httpd_handle_t srv_ = nullptr;
  httpd_req_t* req_ = nullptr;
  std::map<std::string, std::string> args_;
  std::vector<std::pair<String, String>> headers_;
  bool sent_ = false;
  WebUpload upload_;
  std::vector<Route*> routes_;
};

extern GwWebServer server;
