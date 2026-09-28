#pragma once

#include <cstdint>
#include <limits>

namespace stagecore {

class LocalRecoveryHoldPolicy {
 public:
  explicit LocalRecoveryHoldPolicy(uint32_t required_hold_ms)
      : required_hold_ms_(required_hold_ms) {}

  bool sample(bool pressed, uint32_t elapsed_ms) {
    if (!pressed) {
      held_ms_ = 0;
      triggered_ = false;
      return false;
    }
    if (triggered_) return false;

    if (elapsed_ms > std::numeric_limits<uint32_t>::max() - held_ms_) {
      held_ms_ = std::numeric_limits<uint32_t>::max();
    } else {
      held_ms_ += elapsed_ms;
    }
    if (held_ms_ < required_hold_ms_) return false;

    triggered_ = true;
    return true;
  }

  uint32_t held_ms() const { return held_ms_; }

 private:
  uint32_t required_hold_ms_ = 0;
  uint32_t held_ms_ = 0;
  bool triggered_ = false;
};

}  // namespace stagecore
