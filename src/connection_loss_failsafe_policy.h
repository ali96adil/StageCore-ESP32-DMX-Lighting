#pragma once

#include <cstdint>

namespace stagecore {

// First-show source proposal for the frozen "bounded brief hold, then fade to
// blackout" connection-loss contract. These values are intentionally local and
// deterministic; physical timing still requires attended qualification.
constexpr int64_t kConnectionLossHoldMs = 500;
constexpr int64_t kConnectionLossFadeMs = 1000;
constexpr int64_t kConnectionLossSettleMarginMs = 500;

constexpr int64_t connection_loss_total_budget_ms() {
  return kConnectionLossHoldMs + kConnectionLossFadeMs +
         kConnectionLossSettleMarginMs;
}

constexpr bool connection_loss_policy_is_bounded() {
  return kConnectionLossHoldMs >= 0 &&
         kConnectionLossHoldMs <= 1000 &&
         kConnectionLossFadeMs >= 250 &&
         kConnectionLossFadeMs <= 3000 &&
         connection_loss_total_budget_ms() <= 5000;
}

static_assert(connection_loss_policy_is_bounded(),
              "connection-loss failsafe must remain brief and bounded");

}  // namespace stagecore
