#include "fleet_ota.h"
#include "config.h"
#include "settings.h"
#include "net_manager.h"
#include "device_id.h"
#include "fleet_logic.h"
#include "ota_update.h"
#include "utils.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "http_util.h"
#include <mbedtls/sha256.h>

static SemaphoreHandle_t mux;
static volatile bool wantCheck = false, wantInstall = false;

// state shown on the web page (protected by mux)
static uint32_t lastCheckMs = 0;
static bool     everChecked = false;
static String   statusText = "not checked yet";
static bool     available = false, offerForce = false;
static String   offVersion, offUrl, offSha, offRule;
static uint32_t offSize = 0;
static String   checkedModel, checkedSerial;       // identity the offer was made for
static bool     installing = false;
static int      progress = 0;
static uint32_t nextCheckMs = 0;

static void setStatus(const String& s) {
  xSemaphoreTake(mux, portMAX_DELAY);
  statusText = s;
  xSemaphoreGive(mux);
  Console.printf("Fleet OTA: %s\n", s.c_str());
}

// ---------------------------------------------------------------- HTTP helpers
// Base URL = the update server. Empty setting = same scheme/host/port as the data server (and its header is sent too).
static bool buildBase(String& base, bool& sendHeader) {
  FleetCfg f;
  settingsGetFleet(f);
  if (f.baseUrl.length()) { base = f.baseUrl; sendHeader = false; return true; }
  ServerCfg s;
  settingsGetServer(s);
  if (!s.enabled()) return false;
  const uint16_t defPort = (s.scheme == "https") ? 443 : 80;
  base = s.scheme + "://" + s.host;
  if (s.port != 0 && s.port != defPort) base += ":" + String((unsigned)s.port);
  sendHeader = true;
  return true;
}

// the data server's header key is sent only when the update URL is that same server
static void headerFor(bool sendHeader, String& name, String& value) {
  name = "";
  value = "";
  if (!sendHeader) return;
  ServerCfg s;
  settingsGetServer(s);
  if (s.hdrName.length() && s.hdrValue.length()) { name = s.hdrName; value = s.hdrValue; }
}

static String enc(const String& s) { return String(fleetUrlEncode(s.c_str()).c_str()); }

// ---------------------------------------------------------------- 1. ask the server
static void checkOnce() {
  DeviceIdentity id;
  deviceIdGet(id);
#if FLEET_REQUIRE_LIVE_DEVICE
  if (!id.known || !id.live) { setStatus("waiting: the connected device is not known yet (no frame from it in the last " + String(FLEET_LIVE_WINDOW_MS / 60000) + " min)"); return; }
#else
  if (!id.known) { setStatus("waiting: the connected device is not known yet"); return; }
#endif
  String base; bool hdr = false;
  if (!buildBase(base, hdr)) { setStatus("no update server: set a server on the Settings page or an update server URL below"); return; }
  FleetCfg f;
  settingsGetFleet(f);

  String url = String(fleetResolveUrl(base.c_str(), f.path.c_str()).c_str());
  url += "?gw=" + enc(HOSTNAME) + "&mac=" + enc(netMacString()) + "&fw=" + enc(FW_VERSION) + "&iface=" + enc(netActiveIface());
  url += "&class=" + enc(id.cls) + "&model=" + enc(id.model) + "&serial=" + enc(id.serial) + "&devver=" + enc(id.version);
  url += "&manual=" + String(id.manual ? 1 : 0) + "&last=" + enc(settingsNoteGet("fo_res"));

  String hn, hv, body;
  headerFor(hdr, hn, hv);
  int code = gwHttpRequest(url, false, nullptr, 0, nullptr, hn, hv, HTTP_TIMEOUT_MS, &body, 4097);
  if (code == -102) { setStatus("check failed: bad update server URL"); return; }
  if (code == 200 && body.length() > 4096) code = -200;

  xSemaphoreTake(mux, portMAX_DELAY);
  lastCheckMs = millis();
  everChecked = true;
  xSemaphoreGive(mux);

  if (code != 200) { setStatus(code == -200 ? String("check failed: answer too large") : "check failed, HTTP code " + String(code)); return; }

  const std::string b = body.c_str();
  bool upd = false;
  if (!fleetJsonBool(b, "update", upd)) { setStatus("check failed: the server answer has no \"update\" field"); return; }
  if (!upd) {
    std::string why;
    fleetJsonString(b, "reason", why);
    xSemaphoreTake(mux, portMAX_DELAY); available = false; xSemaphoreGive(mux);
    setStatus(String("up to date") + (why.empty() ? "" : String(" (") + why.c_str() + ")"));
    return;
  }

  std::string ver, uri, sha, rule;
  uint32_t size = 0;
  bool force = false;
  fleetJsonString(b, "version", ver);
  fleetJsonString(b, "url", uri);
  fleetJsonString(b, "sha256", sha);
  fleetJsonString(b, "rule", rule);
  fleetJsonUInt(b, "size", size);
  fleetJsonBool(b, "force", force);

  if (ver.empty() || uri.empty())  { setStatus("check failed: the update offer has no version or url"); return; }
  if (!fleetIsSha256Hex(sha))      { setStatus("refused: the update offer has no valid sha256"); return; }
  if (size == 0 || size > FLEET_MAX_IMAGE_BYTES) { setStatus("refused: the update offer has a missing or too large size (" + String(size) + " bytes)"); return; }
  if (ver == FW_VERSION)           { xSemaphoreTake(mux, portMAX_DELAY); available = false; xSemaphoreGive(mux); setStatus("up to date (server offers the version already running)"); return; }
  if (fleetVersionCompare(ver, FW_VERSION) <= 0 && !force) {
    xSemaphoreTake(mux, portMAX_DELAY); available = false; xSemaphoreGive(mux);
    setStatus("server offers " + String(ver.c_str()) + ", which is not newer than the running " FW_VERSION " - ignored");
    return;
  }

  xSemaphoreTake(mux, portMAX_DELAY);
  available = true;
  offerForce = force;
  offVersion = ver.c_str();
  offUrl = String(fleetResolveUrl(base.c_str(), uri).c_str());
  offSha = sha.c_str();
  offRule = rule.c_str();
  offSize = size;
  checkedModel = id.model;
  checkedSerial = id.serial;
  xSemaphoreGive(mux);
  setStatus("update available: " + String(ver.c_str()) + (rule.empty() ? "" : String("  (rule: ") + rule.c_str() + ")"));
}

// ---------------------------------------------------------------- 2. download, verify, switch
static bool downloadAndInstall(const String& url, bool sendHeader, uint32_t expectLen, const String& wantSha, const String& version, String& err) {
  (void)version;
  if (!otaBusyTry()) { err = "another firmware update is running"; return false; }

  esp_http_client_handle_t http = nullptr;
  bool ok = false;
  mbedtls_sha256_context sha;
  bool shaOpen = false;
  esp_ota_handle_t ota = 0;
  bool otaOpen = false;

  do {
    esp_http_client_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.url = url.c_str();
    cfg.timeout_ms = HTTP_TIMEOUT_MS;
    cfg.disable_auto_redirect = true;
    cfg.buffer_size = 1024;
    cfg.buffer_size_tx = 1024;
    const char* ca = HTTPS_ROOT_CA;
    if (ca) cfg.cert_pem = ca; else cfg.skip_cert_common_name_check = true;
    http = esp_http_client_init(&cfg);
    if (!http) { err = "bad firmware URL"; break; }
    String hn, hv;
    headerFor(sendHeader, hn, hv);
    if (hn.length()) esp_http_client_set_header(http, hn.c_str(), hv.c_str());

    if (esp_http_client_open(http, 0) != ESP_OK) { err = "download failed, could not connect"; break; }
    const int64_t len64 = esp_http_client_fetch_headers(http);
    const int code = esp_http_client_get_status_code(http);
    if (code != 200) { err = "download failed, HTTP code " + String(code); break; }
    const int len = (int)len64;
    if (len <= 0)                          { err = "the server did not send a file size (Content-Length)"; break; }
    if ((uint32_t)len > FLEET_MAX_IMAGE_BYTES) { err = "firmware file is too large"; break; }
    if (expectLen && (uint32_t)len != expectLen) { err = "file size " + String(len) + " differs from the offered size " + String(expectLen); break; }

    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    if (!part) { err = "no update partition"; break; }
    const esp_err_t oe = esp_ota_begin(part, (size_t)len, &ota);
    if (oe != ESP_OK) { err = String("not enough room for the update: ") + esp_err_to_name(oe); break; }
    otaOpen = true;

    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    shaOpen = true;

    static uint8_t buf[1024];
    int written = 0;
    bool failed = false;
    while (written < len) {
      const int n = esp_http_client_read(http, (char*)buf, (int)sizeof(buf));
      if (n < 0) { failed = true; err = "download timed out or read error after " + String(written) + " of " + String(len) + " bytes"; break; }
      if (n == 0) {
        if (esp_http_client_is_complete_data_received(http)) break;
        failed = true; err = "connection closed after " + String(written) + " of " + String(len) + " bytes"; break;
      }
      const esp_err_t we = esp_ota_write(ota, buf, (size_t)n);
      if (we != ESP_OK) { failed = true; err = String("flash write failed: ") + esp_err_to_name(we); break; }
      mbedtls_sha256_update(&sha, buf, (size_t)n);
      written += n;
      xSemaphoreTake(mux, portMAX_DELAY); progress = (int)((int64_t)written * 100 / len); xSemaphoreGive(mux);
    }
    if (failed) break;
    if (written != len) { err = "connection closed after " + String(written) + " of " + String(len) + " bytes"; break; }

    unsigned char digest[32];
    mbedtls_sha256_finish(&sha, digest);
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    if (!fleetHexEqual(hex, wantSha.c_str())) { err = String("SHA-256 mismatch (file ") + hex + ", offered " + wantSha + ") - image NOT installed"; break; }

    const esp_err_t ee = esp_ota_end(ota);                // checks the image
    otaOpen = false;
    if (ee != ESP_OK) { err = String("update could not be finished: ") + esp_err_to_name(ee); break; }
    const esp_err_t be = esp_ota_set_boot_partition(part);
    if (be != ESP_OK) { err = String("could not select the new firmware: ") + esp_err_to_name(be); break; }
    ok = true;
  } while (false);

  if (http) { esp_http_client_close(http); esp_http_client_cleanup(http); }
  if (shaOpen) mbedtls_sha256_free(&sha);
  if (!ok && otaOpen) esp_ota_abort(ota);                 // nothing was activated
  otaBusyClear();
  return ok;
}

static void installOffer(bool manual) {
  String url, sha, ver, mdl, ser;
  uint32_t size;
  xSemaphoreTake(mux, portMAX_DELAY);
  url = offUrl; sha = offSha; ver = offVersion; size = offSize; mdl = checkedModel; ser = checkedSerial;
  xSemaphoreGive(mux);

  // the device must still be the one the rule was made for
  DeviceIdentity id;
  deviceIdGet(id);
#if FLEET_REQUIRE_LIVE_DEVICE
  if (!id.known || !id.live) { setStatus("install postponed: the connected device is no longer known"); return; }
#endif
  if (id.model != mdl || id.serial != ser) { setStatus("install postponed: a different device is connected now - checking again"); wantCheck = true; return; }
  if (!manual && FLEET_AVOID_INFUSING && id.infusing) { setStatus("install postponed: the device reports an infusion in progress (will retry)"); return; }
  if (!manual && settingsNoteGet("fo_bad") == ver) {
    setStatus("not installed again: version " + ver + " was installed before but the new image still reported another version (is FW_VERSION in config.h of that build correct?). Use Install now to force it.");
    xSemaphoreTake(mux, portMAX_DELAY); available = false; xSemaphoreGive(mux);
    return;
  }

  bool sendHdr = false;
  { String base; buildBase(base, sendHdr); if (!url.startsWith(base)) sendHdr = false; }   // never send the data-server key to another host

  xSemaphoreTake(mux, portMAX_DELAY); installing = true; progress = 0; xSemaphoreGive(mux);
  setStatus("installing " + ver + " ...");
  String err;
  bool ok = downloadAndInstall(url, sendHdr, size, sha, ver, err);
  xSemaphoreTake(mux, portMAX_DELAY); installing = false; xSemaphoreGive(mux);

  if (ok) {
    settingsNoteSet("fo_pend", ver);                  // checked at the next boot (fleetBegin)
    setStatus("installed " + ver + " - rebooting");
    delay(800);
    gwRestart();
  } else {
    settingsNoteSet("fo_res", "fail: " + err);
    xSemaphoreTake(mux, portMAX_DELAY); available = false; xSemaphoreGive(mux);      // retried at the next scheduled check
    setStatus("install failed: " + err);
  }
}

// ---------------------------------------------------------------- task
static void fleetTask(void*) {
  const uint32_t bootMs = millis();
  uint32_t lastCheck = 0, lastAutoTry = 0;
  bool first = true;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    FleetCfg f;
    settingsGetFleet(f);
    if (f.mode == 0) { wantCheck = false; wantInstall = false; continue; }
    if (!netIsUp()) continue;

    const uint32_t now = millis();
    const bool due = first ? (now - bootMs >= FLEET_FIRST_CHECK_MS) : (now - lastCheck >= (uint32_t)f.intervalMin * 60000UL);
    xSemaphoreTake(mux, portMAX_DELAY);
    nextCheckMs = first ? (bootMs + FLEET_FIRST_CHECK_MS) : (lastCheck + (uint32_t)f.intervalMin * 60000UL);
    xSemaphoreGive(mux);
    if (wantCheck || due) {
      wantCheck = false;
      first = false;
      lastCheck = millis();
      checkOnce();
    }

    bool avail;
    xSemaphoreTake(mux, portMAX_DELAY); avail = available; xSemaphoreGive(mux);
    if (wantInstall) {
      wantInstall = false;
      if (avail) installOffer(true); else setStatus("nothing to install: check first");
    } else if (f.mode == 2 && avail && (lastAutoTry == 0 || millis() - lastAutoTry >= 30000)) {
      lastAutoTry = millis() ? millis() : 1;
      installOffer(false);
    }
  }
}

void fleetBegin() {
  mux = xSemaphoreCreateMutex();
  // First boot after a server-managed install: did the new image really come up with the offered version?
  String pend = settingsNoteGet("fo_pend");
  if (pend.length()) {
    settingsNoteSet("fo_pend", "");
    if (pend == FW_VERSION) { settingsNoteSet("fo_res", "ok " + pend); settingsNoteSet("fo_bad", ""); Console.printf("Fleet OTA: update to %s confirmed\n", pend.c_str()); }
    else { settingsNoteSet("fo_res", "fail: new image reports " FW_VERSION " instead of " + pend); settingsNoteSet("fo_bad", pend); Console.printf("Fleet OTA: WARNING offered %s but running %s\n", pend.c_str(), FW_VERSION); }
  }
  xTaskCreatePinnedToCore(fleetTask, "fleetOta", 12288, nullptr, 1, nullptr, 0);
}

void fleetCheckNow() { wantCheck = true; }

bool fleetInstallNow() {
  bool avail;
  xSemaphoreTake(mux, portMAX_DELAY); avail = available; xSemaphoreGive(mux);
  if (!avail) return false;
  wantInstall = true;
  return true;
}

String fleetStatusJson() {
  FleetCfg f;
  settingsGetFleet(f);
  xSemaphoreTake(mux, portMAX_DELAY);
  String j = "{\"mode\":" + String(f.mode);
  j += ",\"checked\":" + String(everChecked ? "true" : "false");
  j += ",\"ago_s\":" + String(everChecked ? (millis() - lastCheckMs) / 1000 : 0);
  { int32_t d = (int32_t)(nextCheckMs - millis()); j += ",\"next_s\":" + String(d > 0 ? d / 1000 : 0); }
  j += ",\"status\":\"" + jsonEscape(statusText) + "\"";
  j += ",\"available\":" + String(available ? "true" : "false");
  j += ",\"version\":\"" + jsonEscape(offVersion) + "\",\"rule\":\"" + jsonEscape(offRule) + "\"";
  j += ",\"force\":" + String(offerForce ? "true" : "false") + ",\"size\":" + String(offSize);
  j += ",\"installing\":" + String(installing ? "true" : "false") + ",\"progress\":" + String(progress);
  j += ",\"running\":\"" FW_VERSION "\"";
  j += ",\"last_result\":\"" + jsonEscape(settingsNoteGet("fo_res")) + "\"";
  j += ",\"avoid_infusing\":" + String(FLEET_AVOID_INFUSING ? "true" : "false");
  j += "}";
  xSemaphoreGive(mux);
  return j;
}
