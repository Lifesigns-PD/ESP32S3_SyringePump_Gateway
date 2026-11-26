#include "recovery_logic.h"

int recoveryNextCount(int prev, bool powerOn, int limit, bool& doReset) {
  doReset = false;
  if (limit <= 0 || !powerOn) return 0;
  if (prev < 0 || prev > 100) prev = 0;            // garbage in flash
  int n = prev + 1;
  if (n >= limit) { doReset = true; return 0; }
  return n;
}
