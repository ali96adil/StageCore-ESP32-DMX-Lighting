#include "assignment_v2.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace stagecore::assignment_v2 {
namespace {

bool normalized(const std::string &value) {
  return value.find_first_of(" \t\r\n") == std::string::npos;
}

}  // namespace

bool allow_epoch_cache_update(const EpochCache &stored,
                              const EpochCache &candidate) {
  if (candidate.epoch == 0 || candidate.epoch > kMaxPersistentEpoch ||
      (candidate.state != State::kUnassigned &&
       candidate.state != State::kBlocked) ||
      stored.epoch > kMaxPersistentEpoch ||
      (stored.epoch != 0 && stored.state != State::kUnassigned &&
       stored.state != State::kBlocked)) {
    return false;
  }
  if (stored.epoch == 0) {
    // Fresh v2 image has no committed epoch. Only a verified Hub reply may
    // supply a candidate, followed by software-confirmed output blackout.
    return true;
  }
  if (candidate.epoch < stored.epoch) return false;
  if (candidate.epoch == stored.epoch) {
    return candidate.state == stored.state &&
           candidate.project_digest == stored.project_digest;
  }
  return true;
}

bool validate(const Assignment &assignment) {
  if (assignment.device_id.empty() || !normalized(assignment.device_id) ||
      assignment.epoch == 0 || assignment.epoch > kMaxPersistentEpoch ||
      !normalized(assignment.project_id) || !normalized(assignment.snapshot_id)) {
    return false;
  }
  switch (assignment.state) {
    case State::kUnassigned:
      return assignment.project_id.empty() && assignment.snapshot_id.empty();
    case State::kPreparing:
    case State::kBlocked:
      return assignment.snapshot_id.empty();
    case State::kActive:
      return !assignment.project_id.empty();
    case State::kLegacy:
    default:
      // A v1 NVS project is never evidence of a committed v2 assignment.
      return false;
  }
}

bool authorize_command(const Assignment &assignment,
                       const CommandScope &command) {
  if (!validate(assignment) || assignment.state != State::kActive ||
      command.device_id != assignment.device_id || command.project_id.empty() ||
      command.project_id != assignment.project_id ||
      command.epoch != assignment.epoch) {
    return false;
  }
  if (command.require_published_snapshot &&
      (assignment.snapshot_id.empty() || command.snapshot_id.empty())) {
    return false;
  }
  if (!command.snapshot_id.empty() &&
      (assignment.snapshot_id.empty() || command.snapshot_id != assignment.snapshot_id)) {
    return false;
  }
  return true;
}

bool verify_blackout(const Assignment &current, const TransferIntent &intent,
                     const BlackoutAck &ack, Assignment *candidate) {
  if (candidate == nullptr || !validate(current) ||
      (current.state != State::kActive && current.state != State::kBlocked &&
       current.state != State::kUnassigned) ||
      intent.device_id.empty() || !normalized(intent.device_id) ||
      !normalized(intent.from_project_id) || !normalized(intent.to_project_id) ||
      intent.from_project_id == intent.to_project_id ||
      intent.expected_epoch == 0 ||
      intent.expected_epoch >= kMaxPersistentEpoch ||
      intent.connection_generation == 0 ||
      intent.expected_channels == 0 ||
      intent.expected_channels > kMaxReportedChannels ||
      intent.challenge.empty() || !normalized(intent.challenge) ||
      intent.device_id != current.device_id ||
      intent.from_project_id != current.project_id ||
      intent.expected_epoch != current.epoch ||
      ack.device_id != intent.device_id || ack.epoch != intent.expected_epoch ||
      ack.connection_generation != intent.connection_generation ||
      ack.challenge != intent.challenge || !ack.blackout ||
      ack.channel_levels.size() != intent.expected_channels ||
      !std::all_of(ack.channel_levels.begin(), ack.channel_levels.end(),
                   [](uint8_t level) { return level == 0; })) {
    return false;
  }

  Assignment next;
  next.device_id = intent.device_id;
  next.project_id = intent.to_project_id;
  next.epoch = current.epoch + 1;
  next.state = next.project_id.empty() ? State::kUnassigned : State::kBlocked;
  *candidate = std::move(next);
  return true;
}

}  // namespace stagecore::assignment_v2
