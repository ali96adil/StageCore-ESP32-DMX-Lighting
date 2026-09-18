#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "esp_err.h"

namespace stagecore {

struct CommandEnvelopeV1 {
  std::string command_id;
  std::string command_type;
  int schema_version = 0;
  std::string issued_at;
  std::string deadline_at;
  std::string project_id;
  std::string runtime_snapshot_id;
  std::string issuer;
  std::string correlation_id;
  std::string causation_id;
  std::string priority;
  std::string idempotency_key;
  std::string payload_json;
  int64_t issued_at_unix_ms = 0;
  int64_t deadline_at_unix_ms = 0;
  bool has_deadline = false;
};

enum class CommandDisposition {
  kReady,
  kRejected,
  kTimedOut,
  kDuplicate,
};

struct CommandDecision {
  CommandDisposition disposition = CommandDisposition::kRejected;
  CommandEnvelopeV1 command;
  std::string terminal_result_json;
};

class CommandDedupeCache {
 public:
  static constexpr size_t kCapacity = 32;

  bool Lookup(const std::string &command_id,
              std::string *terminal_result_json) const;
  void Remember(const std::string &command_id,
                const std::string &terminal_result_json);

 private:
  struct Entry {
    std::string command_id;
    std::string terminal_result_json;
  };

  std::array<Entry, kCapacity> entries_{};
  size_t next_ = 0;
};

esp_err_t evaluate_command_execute_frame(
    const std::string &frame_json,
    const std::string &expected_device_id,
    const std::string &expected_project_id,
    CommandDedupeCache *dedupe,
    CommandDecision *decision);

std::string make_command_result(
    const std::string &device_id,
    const std::string &command_id,
    const char *status,
    const char *error_code,
    const char *category,
    const char *message,
    bool retryable);

}  // namespace stagecore
