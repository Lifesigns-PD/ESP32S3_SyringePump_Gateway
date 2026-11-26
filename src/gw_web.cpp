#include "gw_web.h"
#include <string.h>
#include "esp_log.h"
#include "webutil.h"

GwWebServer server;

static const char* statusLine(int code) {
  switch (code) {
    case 200: return "200 OK";
    case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";
    case 403: return "403 Forbidden";
    case 404: return "404 Not Found";
    case 409: return "409 Conflict";
    case 413: return "413 Payload Too Large";
    case 500: return "500 Internal Server Error";
    default:  return "200 OK";
  }
}

bool GwWebServer::begin(uint16_t port) {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = port;
  cfg.stack_size = 10240;                    // handlers build big strings and call into NVS / flash code
  cfg.max_uri_handlers = 48;
  cfg.max_open_sockets = 6;
  cfg.lru_purge_enable = true;               // a browser that left a connection open never blocks new ones
  cfg.recv_wait_timeout = 10;
  cfg.send_wait_timeout = 10;
  if (httpd_start(&srv_, &cfg) != ESP_OK) { srv_ = nullptr; return false; }
  for (size_t i = 0; i < routes_.size(); i++) {
    httpd_uri_t u;
    memset(&u, 0, sizeof(u));
    u.uri = routes_[i]->uri.c_str();
    u.method = routes_[i]->method == WEB_POST ? HTTP_POST : HTTP_GET;
    u.handler = &GwWebServer::dispatch;
    u.user_ctx = routes_[i];
    httpd_register_uri_handler(srv_, &u);
  }
  return true;
}

void GwWebServer::on(const char* uri, int method, WebHandler handler, WebHandler uploadHandler) {
  routes_.push_back(new Route{String(uri), method, handler, uploadHandler});
}

String GwWebServer::arg(const char* name) const {
  auto it = args_.find(name);
  return it == args_.end() ? String("") : String(it->second);
}

bool GwWebServer::hasArg(const char* name) const { return args_.find(name) != args_.end(); }

void GwWebServer::sendHeader(const char* name, const String& value) {
  if (strcasecmp(name, "Connection") == 0) return;        // the HTTP server manages the connection itself
  headers_.push_back(std::make_pair(String(name), value));
}

void GwWebServer::send(int code, const char* contentType, const String& body) {
  if (!req_ || sent_) return;
  sent_ = true;
  httpd_resp_set_status(req_, statusLine(code));
  httpd_resp_set_type(req_, contentType);
  for (size_t i = 0; i < headers_.size(); i++) httpd_resp_set_hdr(req_, headers_[i].first.c_str(), headers_[i].second.c_str());
  httpd_resp_send(req_, body.c_str(), (ssize_t)body.length());
}

bool GwWebServer::authenticate(const char* user, const char* pass) {
  if (!req_) return false;
  const size_t l = httpd_req_get_hdr_value_len(req_, "Authorization");
  if (l == 0 || l > 300) return false;
  std::string h(l + 1, '\0');
  if (httpd_req_get_hdr_value_str(req_, "Authorization", &h[0], l + 1) != ESP_OK) return false;
  h.resize(l);
  return basicAuthMatches(h, user, pass);
}

void GwWebServer::requestAuthentication() {
  sendHeader("WWW-Authenticate", "Basic realm=\"Agilia gateway\"");
  send(401, "text/plain", "Login required");
}

// ---------------------------------------------------------------- reading the request
static int recvSome(httpd_req_t* req, char* buf, size_t len) {
  int timeouts = 0;
  for (;;) {
    int n = httpd_req_recv(req, buf, len);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) { if (++timeouts > 3) return -1; continue; }
    return n;
  }
}

bool GwWebServer::readForm() {                           // small urlencoded body -> args
  size_t remaining = req_->content_len;
  std::string body;
  static char buf[1024];
  while (remaining > 0) {
    int n = recvSome(req_, buf, remaining < sizeof(buf) ? remaining : sizeof(buf));
    if (n <= 0) return false;
    if (body.size() < 8192) body.append(buf, (size_t)n);  // a form is never bigger; the rest is read and thrown away
    remaining -= (size_t)n;
  }
  parseUrlEncoded(body, args_);
  return true;
}

namespace {
class UploadSink : public MultipartSink {
 public:
  UploadSink(GwWebServer* s, WebUpload* u, WebHandler h, std::map<std::string, std::string>* args) : s_(s), u_(u), h_(h), args_(args) {}
  void onField(const std::string& n, const std::string& v) override { (*args_)[n] = v; }
  void onFileStart(const std::string& n, const std::string& f) override {
    u_->status = UPLOAD_FILE_START; u_->name = String(n); u_->filename = String(f); u_->buf = nullptr; u_->currentSize = 0; u_->totalSize = 0;
    started_ = true; ended_ = false;
    if (h_) h_();
  }
  bool onFileData(const uint8_t* d, size_t n) override {
    u_->status = UPLOAD_FILE_WRITE; u_->buf = (uint8_t*)d; u_->currentSize = n; u_->totalSize += n;
    if (h_) h_();
    return true;
  }
  void onFileEnd() override {
    u_->status = UPLOAD_FILE_END; u_->buf = nullptr; u_->currentSize = 0; ended_ = true;
    if (h_) h_();
  }
  void abortIfOpen() {                                   // connection lost / malformed body in the middle of a file
    if (started_ && !ended_) { u_->status = UPLOAD_FILE_ABORTED; if (h_) h_(); ended_ = true; }
  }
 private:
  GwWebServer* s_; WebUpload* u_; WebHandler h_; std::map<std::string, std::string>* args_;
  bool started_ = false, ended_ = false;
};
}

void GwWebServer::readMultipart(Route* r, const std::string& ct) {
  std::string boundary;
  size_t p = ct.find("boundary=");
  if (p != std::string::npos) {
    boundary = ct.substr(p + 9);
    if (!boundary.empty() && boundary[0] == '"') { size_t e = boundary.find('"', 1); boundary = boundary.substr(1, e == std::string::npos ? std::string::npos : e - 1); }
    else { size_t e = boundary.find(';'); if (e != std::string::npos) boundary.resize(e); }
  }
  size_t remaining = req_->content_len;
  static char buf[2048];
  if (boundary.empty() || !r->uploadHandler) {           // nothing to do with a file: read it all and ignore it
    while (remaining > 0) { int n = recvSome(req_, buf, remaining < sizeof(buf) ? remaining : sizeof(buf)); if (n <= 0) break; remaining -= (size_t)n; }
    return;
  }
  UploadSink sink(this, &upload_, r->uploadHandler, &args_);
  MultipartParser parser(boundary, &sink);
  bool alive = true;
  while (remaining > 0 && alive) {
    int n = recvSome(req_, buf, remaining < sizeof(buf) ? remaining : sizeof(buf));
    if (n <= 0) { alive = false; break; }
    remaining -= (size_t)n;
    if (!parser.feed((const uint8_t*)buf, (size_t)n)) alive = false;
  }
  if (!parser.finished()) sink.abortIfOpen();
  while (remaining > 0 && alive) { int n = recvSome(req_, buf, remaining < sizeof(buf) ? remaining : sizeof(buf)); if (n <= 0) break; remaining -= (size_t)n; }
}

void GwWebServer::readBody(Route* r) {
  if (req_->content_len == 0) return;
  std::string ct;
  const size_t l = httpd_req_get_hdr_value_len(req_, "Content-Type");
  if (l > 0 && l < 300) {
    ct.assign(l + 1, '\0');
    if (httpd_req_get_hdr_value_str(req_, "Content-Type", &ct[0], l + 1) == ESP_OK) ct.resize(l); else ct.clear();
  }
  if (ct.find("multipart/form-data") != std::string::npos) readMultipart(r, ct);
  else readForm();
}

esp_err_t GwWebServer::dispatch(httpd_req_t* req) {
  Route* r = (Route*)req->user_ctx;
  GwWebServer& s = server;
  s.req_ = req;
  s.args_.clear();
  s.headers_.clear();
  s.sent_ = false;
  s.upload_ = WebUpload();

  const size_t ql = httpd_req_get_url_query_len(req);
  if (ql > 0 && ql < 1024) {
    std::string q(ql + 1, '\0');
    if (httpd_req_get_url_query_str(req, &q[0], q.size()) == ESP_OK) { q.resize(strlen(q.c_str())); parseUrlEncoded(q, s.args_); }
  }
  if (r->method == WEB_POST) s.readBody(r);

  r->handler();
  if (!s.sent_) s.send(500, "text/plain", "no answer");   // a handler that forgot to answer must not leave the browser waiting
  s.req_ = nullptr;
  return ESP_OK;
}
