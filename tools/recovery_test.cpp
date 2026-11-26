// Unit test for src/recovery_logic.cpp.  g++ -std=c++11 -Wall -Wextra -I../src recovery_test.cpp ../src/recovery_logic.cpp -o recovery_test && ./recovery_test
#include "recovery_logic.h"
#include <stdio.h>
static int fails = 0, total = 0;
#define CHECK(c) do { total++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
int main() {
  bool r;
  // five power-ons in a row with limit 5: counts 1,2,3,4 then the fifth resets
  int n = 0;
  n = recoveryNextCount(n, true, 5, r); CHECK(n == 1 && !r);
  n = recoveryNextCount(n, true, 5, r); CHECK(n == 2 && !r);
  n = recoveryNextCount(n, true, 5, r); CHECK(n == 3 && !r);
  n = recoveryNextCount(n, true, 5, r); CHECK(n == 4 && !r);
  n = recoveryNextCount(n, true, 5, r); CHECK(n == 0 && r);                  // 5th power-on: reset, counter cleared
  // a normal start (counter was cleared after the window) never reaches the limit
  n = recoveryNextCount(0, true, 5, r); CHECK(n == 1 && !r);
  // a software restart (OTA, button reset, crash) breaks the sequence
  n = recoveryNextCount(3, false, 5, r); CHECK(n == 0 && !r);
  // feature off
  n = recoveryNextCount(4, true, 0, r); CHECK(n == 0 && !r);
  n = recoveryNextCount(4, true, -1, r); CHECK(n == 0 && !r);
  // garbage in flash is ignored, not trusted
  n = recoveryNextCount(250, true, 5, r); CHECK(n == 1 && !r);
  n = recoveryNextCount(-7, true, 5, r); CHECK(n == 1 && !r);
  // limit 1 would reset on every power-on - allowed by the function, config.h says use 3 or more
  n = recoveryNextCount(0, true, 3, r); CHECK(n == 1 && !r);
  n = recoveryNextCount(2, true, 3, r); CHECK(n == 0 && r);
  printf("%d/%d checks passed\n", total - fails, total);
  return fails ? 1 : 0;
}
