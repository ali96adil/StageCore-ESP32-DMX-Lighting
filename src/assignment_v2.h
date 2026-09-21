#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Pure validation for a FUTURE StageCore device/2 transport. This header
// intentionally has no network, NVS, pairing or DMX side effects. Production
// device/1 remains unchanged until the Hub and firmware activate a reviewed,
// authenticated versioned handshake. A successful validation is NOT proof of
// physical output darkness.
namespace stagecore::assignment_v2 {

constexpr uint64_t kMaxPersistentEpoch = INT64_MAX;
constexpr uint16_t kMaxReportedChannels = 512;

enum class State {
  kLegacy,
  kUnassigned,
  kPreparing,
  kBlocked,
  kActive,
};

struct Assignment {
  std::string device_id;
  std::string project_id;
  uint64_t epoch = 0;
  State state = State::kLegacy;
  std::string snapshot_id;
};

struct CommandScope {
  std::string device_id;
  std::string project_id;
  uint64_t epoch = 0;
  std::string snapshot_id;
  bool require_published_snapshot = false;
};

struct TransferIntent {
  std::string device_id;
  std::string from_project_id;
  std::string to_project_id;
  uint64_t expected_epoch = 0;
  uint64_t connection_generation = 0;
  uint16_t expected_channels = 0;
  std::string challenge;
};

struct BlackoutAck {
  std::string device_id;
  uint64_t epoch = 0;
  uint64_t connection_generation = 0;
  std::string challenge;
  bool blackout = false;
  std::vector<uint8_t> channel_levels;
};

// A non-authoritative NVS cache is used only to reject rollback or changing
// Project/state at an already recorded epoch after reboot. A higher epoch is
// still never activation authority: only an authenticated Hub reply followed
// by all-channel software-zero confirmation may persist it.
struct EpochCache {
  uint64_t epoch = 0;
  State state = State::kUnassigned;
  std::array<uint8_t, 32> project_digest{};
};

bool allow_epoch_cache_update(const EpochCache &stored,
                              const EpochCache &candidate);

// These predicates assume a caller has FIRST verified Hub authentication and
// message origin. They MUST NOT be used to promote unauthenticated mDNS, a
// pairing code, local-web input or a raw WS frame into transfer authority.
bool validate(const Assignment &assignment);
bool authorize_command(const Assignment &assignment, const CommandScope &command);

// Returns a blocked (or unassigned) VALUE candidate only. The Hub must still
// perform an authorized, transactional CAS, fence the old runtime generation,
// and separately acknowledge the committed epoch. No output change occurs.
bool verify_blackout(const Assignment &current, const TransferIntent &intent,
                     const BlackoutAck &ack, Assignment *candidate);

}  // namespace stagecore::assignment_v2
