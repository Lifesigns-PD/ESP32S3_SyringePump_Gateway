/* utils.h - small string helpers shared by several files */
#pragma once
#include "gw_compat.h"

String jsonEscape(const String& s);   // for building JSON by hand
String htmlEscape(const String& s);   // for text placed inside HTML
String httpCodeText(int code);        // 200 -> "OK", -1 -> "connection refused or no answer", -100 -> "no IP address" ...
