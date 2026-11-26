#include "http_sender.h"
#include "config.h"
#include "settings.h"
#include "net_manager.h"
#include "utils.h"
#include "http_util.h"

static QueueHandle_t        sendQueue;
static volatile uint32_t    postOk = 0, postFail = 0, postDropped = 0;
static volatile int         lastHttpCode = 0;
static volatile uint32_t    lastResultMs = 0;

uint32_t senderOkCount()      { return postOk; }
uint32_t senderFailCount()    { return postFail; }
uint32_t senderDroppedCount() { return postDropped; }
int      senderLastCode()     { return lastHttpCode; }

void senderEnqueue(const std::string& js) {
  if (!sendQueue) return;
  ServerCfg c;
  settingsGetServer(c);
  if (!c.enabled()) return;                           // sending disabled (empty host)

  char* copy = (char*)malloc(js.size() + 1);
  if (!copy) return;
  memcpy(copy, js.c_str(), js.size() + 1);
  if (xQueueSend(sendQueue, &copy, 0) != pdTRUE) {    // queue full -> drop the oldest
    char* old = nullptr;
    if (xQueueReceive(sendQueue, &old, 0) == pdTRUE) { free(old); postDropped++; }
    if (xQueueSend(sendQueue, &copy, 0) != pdTRUE) { free(copy); postDropped++; }
  }
}

static int doPost(const char* body) {
  if (!netIsUp()) return -100;                       // no IP address on the WiFi / LAN interface in use
  ServerCfg c;
  settingsGetServer(c);
  if (!c.enabled()) return -101;
  const String url = c.url();

  return gwHttpRequest(url, true, body, strlen(body), "application/json", c.hdrName, c.hdrValue, HTTP_TIMEOUT_MS, nullptr, 0);
}

static void senderTask(void*) {
  for (;;) {
    char* body = nullptr;
    if (xQueueReceive(sendQueue, &body, portMAX_DELAY) != pdTRUE) continue;
    int code = -1;
    for (int attempt = 0; attempt < 2; attempt++) {
      code = doPost(body);
      if (code >= 200 && code < 300) break;
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
    lastHttpCode = code;
    lastResultMs = millis() ? millis() : 1;
    if (code >= 200 && code < 300) postOk++;
    else {
      postFail++;
      ServerCfg sc;
      settingsGetServer(sc);
      Console.printf("POST failed, code=%d (%s)  ->  %s  via %s %s\n", code, httpCodeText(code).c_str(), sc.url().c_str(), netActiveIface(), netActiveIp().c_str());
    }
    free(body);
  }
}

void senderBegin() {
  sendQueue = xQueueCreate(QUEUE_LEN, sizeof(char*));
  xTaskCreatePinnedToCore(senderTask, "sender", 12288, nullptr, 1, nullptr, 0);
}

uint32_t senderLastResultAgeS() { return lastResultMs ? (millis() - lastResultMs) / 1000 : 0xFFFFFFFFu; }
