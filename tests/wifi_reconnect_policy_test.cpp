#include "wifi_reconnect_policy.h"

#include <cassert>

int main() {
  using stagecore::wifi_reconnect::next_delay_ms;

  assert(next_delay_ms(0) == 1000);
  assert(next_delay_ms(1000) == 2000);
  assert(next_delay_ms(2000) == 4000);
  assert(next_delay_ms(8000) == 15000);
  assert(next_delay_ms(15000) == 15000);
  assert(next_delay_ms(30000) == 15000);
  return 0;
}
