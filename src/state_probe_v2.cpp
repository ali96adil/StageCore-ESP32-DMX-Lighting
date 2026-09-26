#include "state_probe_v2.h"

#include <algorithm>

namespace stagecore::state_probe_v2 {
namespace {

bool canonical_challenge(const std::string &challenge) {
  if (challenge.size() != 64) return false;
  return std::all_of(challenge.begin(), challenge.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}

}  // namespace

bool valid_request(const Request &r, const CurrentScope &current) {
  return !r.device_id.empty() && r.device_id == current.device_id &&
         current.assignment_epoch > 0 &&
         current.assignment_epoch <= kMaxExactJSONInteger &&
         current.connection_generation > 0 &&
         current.connection_generation <= kMaxExactJSONInteger &&
         r.assignment_epoch == current.assignment_epoch &&
         r.connection_generation == current.connection_generation &&
         r.expected_channels == kChannels &&
         !r.commands_enabled && canonical_challenge(r.challenge);
}

LogicalReport classify_sample(const std::vector<uint8_t> &slots,
                              bool frame_sent_and_driver_healthy,
                              bool local_failsafe) {
  LogicalReport result;
  if (!frame_sent_and_driver_healthy || slots.size() != kChannels) {
    return result;
  }
  result.valid = true;
  result.levels_known = true;
  result.channel_levels = slots;
  result.blackout = local_failsafe &&
      std::all_of(slots.begin(), slots.end(),
                  [](uint8_t value) { return value == 0; });
  return result;
}

}  // namespace stagecore::state_probe_v2
