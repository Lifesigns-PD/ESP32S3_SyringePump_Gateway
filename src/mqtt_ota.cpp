#include "mqtt_ota.h"
#include "config.h"

#if MQTT_OTA_ENABLED

#include <string.h>
#include <stdio.h>
#include <string>
#include "settings.h"
#include "net_manager.h"
#include "device_id.h"
#include "fleet_logic.h"
#include "ota_update.h"
#include "utils.h"
#include "mqtt_client.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_mac.h"
#include <mbedtls/sha256.h>

#define CMD_MAX 1024

static esp_mqtt_client_handle_t client = nullptr;
static volatile bool   connected = false;
static TaskHandle_t    workerTask = nullptr;
static SemaphoreHandle_t mux = nullptr;

static char gwId[20];
static char tCmd[96], tAll[96], tStatus[96], tOnline[96];

// pending command (written by the MQTT event handler, read by the worker)
static char     pendingCmd[CMD_MAX + 1];
static volatile bool havePending = false;
static volatile bool working = false;

// ---------------------------------------------------------------- publishing
static void pubRaw(const char* topic, const String& payload, int qos, bool retain) {
  if (!client || !connected) return;
  esp_mqtt_client_publish(client, topic, payload.c_str(), (int)payload.length(), qos, retain ? 1 : 0);
}

static void pubStatus(const std::string& jobId, const char* state, int progress, const String& msg) {
  String j = "{\"id\":\"" + jsonEscape(String(jobId.c_str())) + "\",\"state\":\"" + state + "\",\"progress\":" + String(progress);
  j += ",\"fw\":\"" FW_VERSION "\",\"msg\":\"" + jsonEscape(msg) + "\"}";
  Console.printf("MQTT OTA: %s %d%% %s\n", state, progress, msg.c_str());
  pubRaw(tStatus, j, 1, false);
}

static void pubOnline() {
  DeviceIdentity id;
  deviceIdGet(id);
  String j = "{\"state\":\"online\",\"gw\":\"" + String(gwId) + "\",\"fw\":\"" FW_VERSION "\",\"iface\":\"" + String(netActiveIface()) + "\"";
  j += ",\"ip\":\"" + netActiveIp() + "\",\"class\":\"" + jsonEscape(id.cls) + "\",\"model\":\"" + jsonEscape(id.model) + "\",\"serial\":\"" + jsonEscape(id.serial) + "\"";
  j += ",\"device_live\":" + String(id.live ? "true" : "false") + ",\"ota_busy\":" + String(otaBusy() ? "true" : "false") + "}";
  pubRaw(tOnline, j, 1, true);
}

// ---------------------------------------------------------------- download, verify, switch  (same method as fleet_ota.cpp)
static bool downloadAndInstall(const std::string& jobId, const String& url, uint32_t expectLen, const std::string& wantSha, String& err) {
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

    if (esp_http_client_open(http, 0) != ESP_OK) { err = "download failed, could not connect"; break; }
    const int64_t len64 = esp_http_client_fetch_headers(http);
    const int code = esp_http_client_get_status_code(http);
    if (code != 200) { err = "download failed, HTTP code " + String(code); break; }
    const int len = (int)len64;
    if (len <= 0) { err = "the server did not send a file size (Content-Length)"; break; }
    if ((uint32_t)len != expectLen) { err = "file size " + String(len) + " differs from the command size " + String(expectLen); break; }

    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    if (!part) { err = "no update partition"; break; }
    if ((uint32_t)len > part->size) { err = "firmware file is larger than the update partition"; break; }
    const esp_err_t oe = esp_ota_begin(part, (size_t)len, &ota);
    if (oe != ESP_OK) { err = String("not enough room for the update: ") + esp_err_to_name(oe); break; }
    otaOpen = true;

    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    shaOpen = true;

    static uint8_t buf[1024];
    int written = 0, lastPct = -1;
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
      const int pct = (int)((int64_t)written * 100 / len);
      if (pct / 10 != lastPct / 10) { lastPct = pct; pubStatus(jobId, "downloading", pct, ""); }
    }
    if (failed) break;
    if (written != len) { err = "connection closed after " + String(written) + " of " + String(len) + " bytes"; break; }

    unsigned char digest[32];
    mbedtls_sha256_finish(&sha, digest);
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    if (!fleetHexEqual(hex, wantSha)) { err = String("SHA-256 mismatch (file ") + hex + ", command " + wantSha.c_str() + ") - image NOT installed"; break; }

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

// ---------------------------------------------------------------- one "update" command
static void processUpdate(const std::string& js) {
  std::string job, ver, url, sha;
  uint32_t size = 0;
  bool force = false;
  fleetJsonString(js, "id", job);
  fleetJsonString(js, "version", ver);
  fleetJsonString(js, "url", url);
  fleetJsonString(js, "sha256", sha);
  fleetJsonUInt(js, "size", size);
  fleetJsonBool(js, "force", force);

  if (ver.empty() || url.empty())                         { pubStatus(job, "rejected", 0, "command needs version and url"); return; }
  if (url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0) { pubStatus(job, "rejected", 0, "url must start with http:// or https://"); return; }
  if (!fleetIsSha256Hex(sha))                             { pubStatus(job, "rejected", 0, "command needs a valid 64-hex sha256"); return; }
  if (size == 0)                                          { pubStatus(job, "rejected", 0, "command needs the file size in bytes"); return; }
  const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
  if (!part || size > part->size)                         { pubStatus(job, "rejected", 0, "image is larger than the update partition"); return; }
  if (!force && fleetVersionCompare(ver, FW_VERSION) <= 0) { pubStatus(job, "rejected", 0, "version " + String(ver.c_str()) + " is not newer than the running " FW_VERSION " (use force)"); return; }

  // target guard: the pump connected RIGHT NOW must match class / model / serial
  FleetTarget t;
  fleetJsonString(js, "class", t.cls);
  fleetJsonString(js, "model", t.model);
  fleetJsonString(js, "serial", t.serial);
  DeviceIdentity id;
  deviceIdGet(id);
  if (!t.any()) {
    if (!id.known || (FLEET_REQUIRE_LIVE_DEVICE && !id.live)) { pubStatus(job, "rejected", 0, "target given but the connected device is not known / live"); return; }
    std::string why;
    if (!fleetTargetMatches(t, id.cls.c_str(), id.model.c_str(), id.serial.c_str(), why)) { pubStatus(job, "rejected", 0, String("target mismatch: ") + why.c_str()); return; }
  }
  if (FLEET_AVOID_INFUSING && id.known && id.live && id.infusing && !force) { pubStatus(job, "rejected", 0, "the pump reports an infusion in progress (use force)"); return; }
  if (!netIsUp())                                         { pubStatus(job, "failed", 0, "no network"); return; }

  pubStatus(job, "downloading", 0, "from " + String(url.c_str()));
  String err;
  const bool ok = downloadAndInstall(job, String(url.c_str()), size, sha, err);
  if (ok) {
    settingsNoteSet("mo_pend", String(ver.c_str()));        // checked at the next boot
    settingsNoteSet("mo_job", String(job.c_str()));
    pubStatus(job, "installed", 100, "version " + String(ver.c_str()) + " installed - rebooting");
    delay(1500);                                            // let the status message leave
    gwRestart();
  } else {
    pubStatus(job, "failed", 0, err);
  }
}

static void workerFn(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    std::string js;
    xSemaphoreTake(mux, portMAX_DELAY);
    if (havePending) { js = pendingCmd; havePending = false; }
    xSemaphoreGive(mux);
    if (js.empty()) continue;
    working = true;
    std::string cmd = "update";
    fleetJsonString(js, "cmd", cmd);
    if (cmd == "status")      pubOnline();
    else if (cmd == "update") processUpdate(js);
    else { std::string job; fleetJsonString(js, "id", job); pubStatus(job, "rejected", 0, String("unknown cmd '") + cmd.c_str() + "'"); }
    working = false;
  }
}

// ---------------------------------------------------------------- MQTT events (run in the esp-mqtt task: keep them short)
static char     aBuf[CMD_MAX + 1];
static bool     aOk = false, aRetain = false;

static void onCommandText(const char* text) {
  if (working) {                                           // one job at a time
    std::string job;
    fleetJsonString(text, "id", job);
    pubStatus(job, "busy", 0, "another command is being processed");
    return;
  }
  xSemaphoreTake(mux, portMAX_DELAY);
  strncpy(pendingCmd, text, CMD_MAX);
  pendingCmd[CMD_MAX] = 0;
  havePending = true;
  xSemaphoreGive(mux);
  std::string job;
  fleetJsonString(text, "id", job);
  pubStatus(job, "received", 0, "");
  xTaskNotifyGive(workerTask);
}

static void onMqttEvent(void*, esp_event_base_t, int32_t id, void* data) {
  esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)data;
  switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
      connected = true;
      Console.printf("MQTT OTA: connected to broker via %s, listening on %s\n", netActiveIface(), tCmd);
      esp_mqtt_client_subscribe(client, tCmd, 1);
      esp_mqtt_client_subscribe(client, tAll, 1);
      pubOnline();
      {
        // first connect after an MQTT update: did the new image really come up with the commanded version?
        String pend = settingsNoteGet("mo_pend");
        if (pend.length()) {
          const std::string job = settingsNoteGet("mo_job").c_str();
          settingsNoteSet("mo_pend", "");
          settingsNoteSet("mo_job", "");
          if (pend == FW_VERSION) pubStatus(job, "ok", 100, "update to " + pend + " confirmed");
          else                    pubStatus(job, "bad", 100, "commanded " + pend + " but the new image reports " FW_VERSION " (is FW_VERSION in config.h correct?)");
        }
      }
      break;
    case MQTT_EVENT_DISCONNECTED:
      connected = false;
      Console.println("MQTT OTA: disconnected (will reconnect)");
      break;
    case MQTT_EVENT_DATA:
      if (ev->current_data_offset == 0) { aOk = ev->total_data_len > 0 && ev->total_data_len <= CMD_MAX; aRetain = ev->retain; }
      if (aOk && ev->current_data_offset + ev->data_len <= CMD_MAX) memcpy(aBuf + ev->current_data_offset, ev->data, ev->data_len);
      if (ev->current_data_offset + ev->data_len >= ev->total_data_len) {
        if (!aOk)         Console.println("MQTT OTA: command too large - ignored");
        else if (aRetain) Console.println("MQTT OTA: retained command ignored");
        else { aBuf[ev->total_data_len] = 0; onCommandText(aBuf); }
      }
      break;
    default: break;
  }
}

// ---------------------------------------------------------------- start
static void mqttTask(void*) {
  // wait for the first IP address on WiFi or LAN (whichever net_manager brings up)
  while (!netIsUp()) vTaskDelay(pdMS_TO_TICKS(1000));

  esp_mqtt_client_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.broker.address.uri = MQTT_OTA_BROKER_URI;
  cfg.credentials.client_id = gwId;
  if (strlen(MQTT_OTA_USER)) cfg.credentials.username = MQTT_OTA_USER;
  if (strlen(MQTT_OTA_PASS)) cfg.credentials.authentication.password = MQTT_OTA_PASS;
  const char* ca = MQTT_OTA_ROOT_CA;
  if (ca) cfg.broker.verification.certificate = ca;        // no CA = same as the HTTPS upload: not verified
  cfg.session.keepalive = MQTT_OTA_KEEPALIVE_S;
  cfg.session.last_will.topic = tOnline;
  cfg.session.last_will.msg = "{\"state\":\"offline\"}";
  cfg.session.last_will.msg_len = 0;
  cfg.session.last_will.qos = 1;
  cfg.session.last_will.retain = 1;
  cfg.network.reconnect_timeout_ms = 5000;
  cfg.network.timeout_ms = 10000;
  cfg.buffer.size = 1536;
  cfg.buffer.out_size = 1024;

  client = esp_mqtt_client_init(&cfg);
  if (!client) { Console.println("MQTT OTA: client init failed"); vTaskDelete(nullptr); return; }
  esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, onMqttEvent, nullptr);
  esp_mqtt_client_start(client);
  Console.printf("MQTT OTA: client %s -> %s\n", gwId, MQTT_OTA_BROKER_URI);
  vTaskDelete(nullptr);                                    // the esp-mqtt task keeps running and reconnects by itself
}

void mqttOtaBegin() {
  if (!strlen(MQTT_OTA_BROKER_URI)) { Console.println("MQTT OTA: no broker set (MQTT_OTA_BROKER_URI in config.h) - off"); return; }
  uint8_t m[6];
  esp_read_mac(m, ESP_MAC_WIFI_STA);                       // base MAC: the same whether WiFi or LAN is in use
  snprintf(gwId, sizeof(gwId), "gw-%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
  snprintf(tCmd,    sizeof(tCmd),    "%s/%s/ota/cmd",    MQTT_OTA_TOPIC_PREFIX, gwId);
  snprintf(tAll,    sizeof(tAll),    "%s/all/ota/cmd",   MQTT_OTA_TOPIC_PREFIX);
  snprintf(tStatus, sizeof(tStatus), "%s/%s/ota/status", MQTT_OTA_TOPIC_PREFIX, gwId);
  snprintf(tOnline, sizeof(tOnline), "%s/%s/status",     MQTT_OTA_TOPIC_PREFIX, gwId);
  mux = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(workerFn, "mqttOta", 12288, nullptr, 1, &workerTask, 0);
  xTaskCreatePinnedToCore(mqttTask, "mqttInit", 4096, nullptr, 1, nullptr, 0);
}

bool mqttOtaConnected() { return connected; }

#else  // MQTT_OTA_ENABLED == 0
void mqttOtaBegin() {}
bool mqttOtaConnected() { return false; }
#endif
