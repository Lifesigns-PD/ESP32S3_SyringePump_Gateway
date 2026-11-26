/*
 * http_util.h / http_util.cpp  -  one HTTP/HTTPS request on esp_http_client
 *
 * Returns the HTTP status (200 ...) or a negative local error, using the same codes the firmware has always reported:
 *   -1 could not connect   -3 could not send the data   -11 no answer in time   -102 bad address
 * https is NOT verified when no CA text is given (HTTPS_ROOT_CA nullptr in config.h; sdkconfig: ESP_TLS_SKIP_SERVER_CERT_VERIFY).
 */
#pragma once
#include "gw_compat.h"

int gwHttpRequest(const String& url, bool post, const char* body, size_t bodyLen, const char* contentType,
                  const String& hdrName, const String& hdrValue, int timeoutMs, String* resp, size_t respMax);
