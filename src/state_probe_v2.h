#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Host-testable validation for the opt-in, read-only v2 logical DMX probe.
// This does NOT authenticate a Hub, prove physical DMX/LED output, activate a
// Project, or grant command authority. The transport MUST establish origin
// and the current socket before invoking this pure predicate.
namespace stagecore::state_probe_v2 {

constexpr uint16_t kChannels = 12;
constexpr uint64_t kMaxExactJSONInteger = 9007199254740991ULL;

struct Request {
  std::string device_id;
  uint64_t assignment_epoch = 0;
  uint64_t connection_generation = 0;
  std::string challenge;
  uint16_t expected_channels = 0;
  bool commands_enabled = true;
};

struct CurrentScope {
  std::string device_id;
  uint64_t assignment_epoch = 0;
  uint64_t connection_generation = 0;
};

bool valid_request(const Request &request, const CurrentScope &current);

struct LogicalReport {
  bool valid = false;
  bool levels_known = false;
  bool blackout = false;
  std::vector<uint8_t> channel_levels;
};

// An output-task-backed report is emitted only when all twelve slots were
// confirmed locally sent and the output driver is healthy. A zero logical
// frame in FAILSAFE is a software-blackout report, never physical proof.
LogicalReport classify_sample(const std::vector<uint8_t> &slots,
                              bool frame_sent_and_driver_healthy,
                              bool local_failsafe);

}  // namespace stagecore::state_probe_v2
