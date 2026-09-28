#include <cassert>

#include "connection_loss_failsafe_policy.h"

int main() {
  using namespace stagecore;
  static_assert(connection_loss_policy_is_bounded());
  assert(kConnectionLossHoldMs == 500);
  assert(kConnectionLossFadeMs == 1000);
  assert(kConnectionLossSettleMarginMs == 500);
  assert(connection_loss_total_budget_ms() == 2000);
  return 0;
}
