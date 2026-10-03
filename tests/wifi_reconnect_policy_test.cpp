#include "wifi_reconnect_policy.h"

#include <cassert>

int main() {
  using stagecore::wifi_reconnect::next_delay_ms;
  using stagecore::wifi_reconnect::recovery_portal_due;

  assert(next_delay_ms(0) == 1000);
  assert(next_delay_ms(1000) == 2000);
  assert(next_delay_ms(2000) == 4000);
  assert(next_delay_ms(8000) == 15000);
  assert(next_delay_ms(15000) == 15000);
  assert(next_delay_ms(30000) == 15000);

  assert(!recovery_portal_due(0));
  assert(!recovery_portal_due(179999));
  assert(recovery_portal_due(180000));
  assert(recovery_portal_due(600000));
  return 0;
}
