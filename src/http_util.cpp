#include "http_util.h"
#include "config.h"
#include "esp_http_client.h"

namespace {
struct Collect { String* out; size_t max; };

esp_err_t onHttpEvent(esp_http_client_event_t* e) {
  if (e->event_id == HTTP_EVENT_ON_DATA && e->user_data && e->data && e->data_len > 0) {
    Collect* c = (Collect*)e->user_data;
    if (c->out && c->out->length() < c->max) {
      size_t room = c->max - c->out->length();
      c->out->append((const char*)e->data, (size_t)e->data_len < room ? (size_t)e->data_len : room);
    }
  }
  return ESP_OK;
}
}

int gwHttpRequest(const String& url, bool post, const char* body, size_t bodyLen, const char* contentType,
                  const String& hdrName, const String& hdrValue, int timeoutMs, String* resp, size_t respMax) {
  if (resp) resp->clear();
  Collect col = {resp, respMax};
  esp_http_client_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.url = url.c_str();
  cfg.timeout_ms = timeoutMs;
  cfg.disable_auto_redirect = true;                       // redirects are not followed (as before)
  cfg.event_handler = onHttpEvent;
  cfg.user_data = &col;
  cfg.buffer_size = 1024;
  cfg.buffer_size_tx = 1024;
  const char* ca = HTTPS_ROOT_CA;
  if (ca) cfg.cert_pem = ca;
  else    cfg.skip_cert_common_name_check = true;         // no CA: certificate is not verified

  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (!c) return -102;
  esp_http_client_set_method(c, post ? HTTP_METHOD_POST : HTTP_METHOD_GET);
  if (post) {
    if (contentType) esp_http_client_set_header(c, "Content-Type", contentType);
    esp_http_client_set_post_field(c, body, (int)bodyLen);
  }
  if (hdrName.length() && hdrValue.length()) esp_http_client_set_header(c, hdrName.c_str(), hdrValue.c_str());

  const esp_err_t err = esp_http_client_perform(c);
  int code;
  if (err == ESP_OK) code = esp_http_client_get_status_code(c);
  else if (err == ESP_ERR_HTTP_CONNECT) code = -1;
  else if (err == ESP_ERR_HTTP_WRITE_DATA) code = -3;
  else if (err == ESP_ERR_HTTP_EAGAIN || err == ESP_ERR_HTTP_FETCH_HEADER) code = -11;
  else if (err == ESP_ERR_HTTP_INVALID_TRANSPORT) code = -102;
  else code = -1;
  esp_http_client_cleanup(c);
  return code;
}
