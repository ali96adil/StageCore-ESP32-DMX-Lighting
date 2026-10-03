#pragma once

#include <cstdint>

namespace stagecore {
namespace wifi_reconnect {

constexpr uint32_t kInitialDelayMs = 1000;
constexpr uint32_t kMaxDelayMs = 15000;
constexpr uint32_t kRecoveryPortalDelayMs = 180000;

inline uint32_t next_delay_ms(uint32_t current_ms) {
  if (current_ms < kInitialDelayMs) return kInitialDelayMs;
  if (current_ms >= kMaxDelayMs) return kMaxDelayMs;
  const uint32_t doubled = current_ms * 2U;
  return doubled > kMaxDelayMs ? kMaxDelayMs : doubled;
}

inline bool recovery_portal_due(uint32_t offline_ms) {
  return offline_ms >= kRecoveryPortalDelayMs;
}

}  // namespace wifi_reconnect
}  // namespace stagecore
