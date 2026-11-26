#include "ota_update.h"
#include "web_server.h"
#include "pump_reader.h"
#include "device_id.h"
#include "fleet_logic.h"
#include "config.h"
#include <mbedtls/sha256.h>
#include "esp_ota_ops.h"
#include "esp_partition.h"

static volatile bool          busy = false;
static bool                   haveLock = false;       // this web upload holds the lock
static bool                   uploadOk = false;       // upload started and no error so far
static int                    rejectCode = 0;         // != 0 -> answer with this HTTP code
static String                 rejectText;
static mbedtls_sha256_context sha;
static bool                   shaOpen = false;
static char                   shaHex[65] = "";
static uint32_t               totalBytes = 0;
static esp_ota_handle_t       otaHandle = 0;          // the flash slot being written
static const esp_partition_t* otaPart = nullptr;
static bool                   otaOpen = false;
static String                 otaErr;                  // first flash error of this upload (empty = none)
static portMUX_TYPE           lockMux = portMUX_INITIALIZER_UNLOCKED;

bool otaBusyTry() {
  bool got = false;
  taskENTER_CRITICAL(&lockMux);
  if (!busy) { busy = true; got = true; }
  taskEXIT_CRITICAL(&lockMux);
  return got;
}
void otaBusyClear() { busy = false; }
bool otaBusy()      { return busy; }

static void shaClose(bool finish) {
  if (!shaOpen) return;
  if (finish) {
    unsigned char out[32];
    mbedtls_sha256_finish(&sha, out);
    for (int i = 0; i < 32; i++) snprintf(shaHex + i * 2, 3, "%02x", out[i]);
  }
  mbedtls_sha256_free(&sha);
  shaOpen = false;
}

static void reject(int code, const String& why) {
  rejectCode = code;
  rejectText = why;
  Console.printf("OTA refused: %s\n", why.c_str());
}

void otaHandleDone() {
  if (!webCheckAuth()) return;
  server.sendHeader("Connection", "close");
  if (rejectCode) {
    server.send(rejectCode, "text/plain", "Update REFUSED: " + rejectText);
    rejectCode = 0;
    if (haveLock) { otaBusyClear(); haveLock = false; }
    return;
  }
  bool ok = uploadOk && otaErr.length() == 0;
  if (ok) server.send(200, "text/plain", String("Update OK, sha256 ") + shaHex + ", rebooting");
  else    server.send(500, "text/plain", String("Update FAILED: ") + (otaErr.length() ? otaErr : String("upload incomplete")));
  delay(500);
  if (haveLock) { otaBusyClear(); haveLock = false; }
  if (ok) gwRestart();
}

void otaHandleUpload() {
  if (!server.authenticate(WEB_USER, WEB_PASS)) return;
  WebUpload& up = server.upload();

  if (up.status == UPLOAD_FILE_START) {
    rejectCode = 0; uploadOk = false; totalBytes = 0; shaHex[0] = 0; otaErr = ""; otaOpen = false;
    Console.printf("OTA start: %s\n", up.filename.c_str());

    if (!otaBusyTry()) { haveLock = false; reject(409, "another firmware update is already running"); return; }
    haveLock = true;

    // ---- target guard: only accept the image if the connected device matches
    FleetTarget t;
    t.cls    = server.arg("tcls").c_str();
    t.model  = server.arg("tmodel").c_str();
    t.serial = server.arg("tserial").c_str();
    if (!t.any()) {
      DeviceIdentity id;
      deviceIdGet(id);
      std::string why;
      if (!id.known || !id.live) {
        reject(409, "a target (class / model / serial) was given, but this gateway does not know what is connected "
                    "(no frame from the device in the last " + String(FLEET_LIVE_WINDOW_MS / 60000) + " minutes). Power on the device or type it in on the Settings page.");
        return;
      }
      if (!fleetTargetMatches(t, id.cls.c_str(), id.model.c_str(), id.serial.c_str(), why)) {
        reject(409, String("this gateway is connected to ") + id.model + " serial " + id.serial + ": " + why.c_str());
        return;
      }
    }

    otaPart = esp_ota_get_next_update_partition(nullptr);
    if (!otaPart) { otaErr = "no update partition"; Console.println("OTA: no update partition"); return; }
    const esp_err_t be = esp_ota_begin(otaPart, OTA_SIZE_UNKNOWN, &otaHandle);
    if (be != ESP_OK) { otaErr = esp_err_to_name(be); Console.printf("OTA begin failed: %s\n", otaErr.c_str()); return; }
    otaOpen = true;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    shaOpen = true;
    uploadOk = true;

  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!uploadOk || rejectCode) return;
    mbedtls_sha256_update(&sha, up.buf, up.currentSize);
    totalBytes += up.currentSize;
    const esp_err_t we = esp_ota_write(otaHandle, up.buf, up.currentSize);
    if (we != ESP_OK) { otaErr = String("flash write failed: ") + esp_err_to_name(we); Console.printf("OTA %s\n", otaErr.c_str()); uploadOk = false; }
    // (the pump UART keeps being drained by the loop task while this upload runs in the web server task)

  } else if (up.status == UPLOAD_FILE_END) {
    if (!uploadOk || rejectCode) { shaClose(false); return; }
    shaClose(true);
    otaOpen = false;
    esp_err_t ee = esp_ota_end(otaHandle);                // checks the image
    if (ee == ESP_OK) ee = esp_ota_set_boot_partition(otaPart);
    if (ee == ESP_OK) Console.printf("OTA success: %u bytes, sha256 %s\n", (unsigned)up.totalSize, shaHex);
    else { otaErr = String("image rejected: ") + esp_err_to_name(ee); Console.printf("OTA %s\n", otaErr.c_str()); uploadOk = false; }

  } else if (up.status == UPLOAD_FILE_ABORTED) {
    shaClose(false);
    if (otaOpen) { esp_ota_abort(otaHandle); otaOpen = false; }
    uploadOk = false;
    if (haveLock) { otaBusyClear(); haveLock = false; }
    Console.println("OTA aborted");
  }
}
