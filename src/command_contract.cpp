#include "command_contract.h"

#include <cstring>
#include <utility>

#include "cJSON.h"
#include "trusted_clock.h"

namespace stagecore {
namespace {

constexpr int kSchemaVersion = 1;
constexpr size_t kMaxID = 128;
constexpr size_t kMaxCommandType = 64;
constexpr size_t kMaxIssuer = 128;
constexpr size_t kMaxPriority = 32;
constexpr size_t kMaxIdempotency = 256;
constexpr size_t kMaxPayloadBytes = 4096;

bool string_field(const cJSON *object, const char *key,
                  std::string *value, bool required,
                  size_t max_length) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (item == nullptr) {
    if (required) return false;
    value->clear();
    return true;
  }
  if (!cJSON_IsString(item) || item->valuestring == nullptr) return false;
  const size_t n = std::strlen(item->valuestring);
  if ((required && n == 0) || n > max_length) return false;
  *value = item->valuestring;
  return true;
}

bool number_field(const cJSON *object, const char *key, int *value) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (!cJSON_IsNumber(item)) return false;
  *value = item->valueint;
  return true;
}

bool allowed_root_key(const char *key) {
  return std::strcmp(key, "type") == 0 ||
         std::strcmp(key, "schema_version") == 0 ||
         std::strcmp(key, "device_id") == 0 ||
         std::strcmp(key, "command") == 0;
}

bool allowed_command_key(const char *key) {
  static constexpr const char *kAllowed[] = {
      "command_id",
      "command_type",
      "schema_version",
      "issued_at",
      "deadline_at",
      "project_id",
      "runtime_snapshot_id",
      "issuer",
      "correlation_id",
      "causation_id",
      "priority",
      "idempotency_key",
      "payload",
  };
  for (const char *candidate : kAllowed) {
    if (std::strcmp(key, candidate) == 0) return true;
  }
  return false;
}

bool no_unknown_fields(const cJSON *object,
                       bool (*allowed)(const char *)) {
  if (!cJSON_IsObject(object) || allowed == nullptr) return false;
  const cJSON *child = nullptr;
  cJSON_ArrayForEach(child, object) {
    if (child->string == nullptr || !allowed(child->string)) return false;
  }
  return true;
}

bool supported_command_type(const std::string &type) {
  static constexpr const char *kTypes[] = {
      "LIGHTING_CHANNELS_SET",
      "LIGHTING_CHANNELS_FADE",
      "LIGHTING_BLACKOUT",
      "LIGHTING_STATE_READ",
      "LIGHTING_IDENTIFY",
      "LIGHTING_CONFIG_READ",
      "LIGHTING_CONFIG_APPLY",
  };
  for (const char *candidate : kTypes) {
    if (type == candidate) return true;
  }
  return false;
}

std::string print_json(cJSON *root) {
  if (root == nullptr) return {};
  char *text = cJSON_PrintUnformatted(root);
  std::string out = text != nullptr ? text : "";
  if (text != nullptr) cJSON_free(text);
  return out;
}

std::string rejection(const std::string &device_id,
                      const std::string &command_id,
                      const char *code,
                      const char *category,
                      const char *message,
                      bool retryable) {
  return make_command_result(device_id, command_id, "REJECTED",
                             code, category, message, retryable);
}

}  // namespace

bool CommandDedupeCache::Lookup(
    const std::string &command_id,
    std::string *terminal_result_json) const {
  if (command_id.empty() || terminal_result_json == nullptr) return false;
  for (const Entry &entry : entries_) {
    if (entry.command_id == command_id &&
        !entry.terminal_result_json.empty()) {
      *terminal_result_json = entry.terminal_result_json;
      return true;
    }
  }
  return false;
}

void CommandDedupeCache::Remember(
    const std::string &command_id,
    const std::string &terminal_result_json) {
  if (command_id.empty() || terminal_result_json.empty()) return;

  for (Entry &entry : entries_) {
    if (entry.command_id == command_id) {
      entry.terminal_result_json = terminal_result_json;
      return;
    }
  }

  entries_[next_].command_id = command_id;
  entries_[next_].terminal_result_json = terminal_result_json;
  next_ = (next_ + 1) % kCapacity;
}

std::string make_command_result(
    const std::string &device_id,
    const std::string &command_id,
    const char *status,
    const char *error_code,
    const char *category,
    const char *message,
    bool retryable) {
  cJSON *root = cJSON_CreateObject();
  if (root == nullptr) return {};

  cJSON_AddStringToObject(root, "type", "command.result");
  cJSON_AddNumberToObject(root, "schema_version", 1);
  cJSON_AddStringToObject(root, "device_id", device_id.c_str());
  cJSON_AddStringToObject(root, "command_id", command_id.c_str());
  cJSON_AddStringToObject(root, "status", status);

  if (error_code != nullptr && error_code[0] != '\0') {
    cJSON *error = cJSON_CreateObject();
    if (error == nullptr) {
      cJSON_Delete(root);
      return {};
    }
    cJSON_AddStringToObject(error, "error_code", error_code);
    cJSON_AddStringToObject(error, "category",
                            category != nullptr ? category : "RUNTIME");
    cJSON_AddStringToObject(error, "message",
                            message != nullptr ? message : "");
    cJSON_AddBoolToObject(error, "retryable", retryable);
    cJSON_AddStringToObject(error, "affected_entity_id",
                            device_id.c_str());
    cJSON_AddItemToObject(root, "error", error);
  }

  const std::string result = print_json(root);
  cJSON_Delete(root);
  return result;
}

esp_err_t evaluate_command_execute_frame(
    const std::string &frame_json,
    const std::string &expected_device_id,
    const std::string &expected_project_id,
    CommandDedupeCache *dedupe,
    CommandDecision *decision,
    int expected_outer_schema,
    std::string expected_runtime_snapshot_id) {
  if (expected_device_id.empty() || expected_project_id.empty() ||
      dedupe == nullptr || decision == nullptr ||
      frame_json.empty() || frame_json.size() > 8192) {
    return ESP_ERR_INVALID_ARG;
  }

  *decision = CommandDecision{};

  cJSON *root =
      cJSON_ParseWithLength(frame_json.data(), frame_json.size());
  if (root == nullptr || !cJSON_IsObject(root)) {
    if (root != nullptr) cJSON_Delete(root);
    return ESP_ERR_INVALID_RESPONSE;
  }

  if (!no_unknown_fields(root, allowed_root_key)) {
    cJSON_Delete(root);
    return ESP_ERR_INVALID_RESPONSE;
  }

  std::string type;
  std::string device_id;
  int outer_schema = 0;
  const bool outer_ok =
      string_field(root, "type", &type, true, 64) &&
      number_field(root, "schema_version", &outer_schema) &&
      string_field(root, "device_id", &device_id, true, kMaxID) &&
      type == "command.execute" &&
      (expected_outer_schema == 1 || expected_outer_schema == 2) &&
      outer_schema == expected_outer_schema &&
      device_id == expected_device_id;

  const cJSON *command =
      cJSON_GetObjectItemCaseSensitive(root, "command");
  if (!outer_ok || !cJSON_IsObject(command) ||
      !no_unknown_fields(command, allowed_command_key)) {
    cJSON_Delete(root);
    return ESP_ERR_INVALID_RESPONSE;
  }

  CommandEnvelopeV1 parsed;
  int command_schema = 0;
  const bool fields_ok =
      string_field(command, "command_id", &parsed.command_id,
                   true, kMaxID) &&
      string_field(command, "command_type", &parsed.command_type,
                   true, kMaxCommandType) &&
      number_field(command, "schema_version", &command_schema) &&
      string_field(command, "issued_at", &parsed.issued_at,
                   true, 64) &&
      string_field(command, "deadline_at", &parsed.deadline_at,
                   false, 64) &&
      string_field(command, "project_id", &parsed.project_id,
                   true, kMaxID) &&
      string_field(command, "runtime_snapshot_id",
                   &parsed.runtime_snapshot_id, false, kMaxID) &&
      string_field(command, "issuer", &parsed.issuer,
                   true, kMaxIssuer) &&
      string_field(command, "correlation_id",
                   &parsed.correlation_id, false, kMaxID) &&
      string_field(command, "causation_id",
                   &parsed.causation_id, false, kMaxID) &&
      string_field(command, "priority", &parsed.priority,
                   true, kMaxPriority) &&
      string_field(command, "idempotency_key",
                   &parsed.idempotency_key, false, kMaxIdempotency);

  const cJSON *payload =
      cJSON_GetObjectItemCaseSensitive(command, "payload");

  parsed.schema_version = command_schema;
  parsed.has_deadline = !parsed.deadline_at.empty();

  if (!fields_ok || !cJSON_IsObject(payload)) {
    parsed.command_id =
        parsed.command_id.empty() ? "unknown" : parsed.command_id;
    decision->command = parsed;
    decision->disposition = CommandDisposition::kRejected;
    decision->terminal_result_json =
        rejection(expected_device_id, parsed.command_id,
                  "DEVICE_COMMAND_INVALID", "VALIDATION",
                  "Command envelope is malformed", false);
    if (parsed.command_id != "unknown") {
      dedupe->Remember(parsed.command_id,
                       decision->terminal_result_json);
    }
    cJSON_Delete(root);
    return ESP_OK;
  }

  std::string cached;
  if (dedupe->Lookup(parsed.command_id, &cached)) {
    decision->command = parsed;
    decision->disposition = CommandDisposition::kDuplicate;
    decision->terminal_result_json = cached;
    cJSON_Delete(root);
    return ESP_OK;
  }

  if (parsed.schema_version != kSchemaVersion ||
      parsed.project_id != expected_project_id ||
      (!expected_runtime_snapshot_id.empty() &&
       parsed.runtime_snapshot_id != expected_runtime_snapshot_id) ||
      !supported_command_type(parsed.command_type)) {
    decision->command = parsed;
    decision->disposition = CommandDisposition::kRejected;
    decision->terminal_result_json =
        rejection(expected_device_id, parsed.command_id,
                  "DEVICE_COMMAND_INVALID", "VALIDATION",
                  "Command schema, project, snapshot, or type is not accepted", false);
    dedupe->Remember(parsed.command_id,
                     decision->terminal_result_json);
    cJSON_Delete(root);
    return ESP_OK;
  }

  char *payload_text = cJSON_PrintUnformatted(payload);
  if (payload_text == nullptr) {
    cJSON_Delete(root);
    return ESP_ERR_NO_MEM;
  }
  parsed.payload_json = payload_text;
  cJSON_free(payload_text);
  if (parsed.payload_json.size() > kMaxPayloadBytes) {
    decision->command = parsed;
    decision->disposition = CommandDisposition::kRejected;
    decision->terminal_result_json =
        rejection(expected_device_id, parsed.command_id,
                  "DEVICE_COMMAND_INVALID", "VALIDATION",
                  "Command payload exceeds device bounds", false);
    dedupe->Remember(parsed.command_id,
                     decision->terminal_result_json);
    cJSON_Delete(root);
    return ESP_OK;
  }

  if (!parse_rfc3339_unix_ms(parsed.issued_at,
                             &parsed.issued_at_unix_ms) ||
      (parsed.has_deadline &&
       !parse_rfc3339_unix_ms(parsed.deadline_at,
                              &parsed.deadline_at_unix_ms)) ||
      (parsed.has_deadline &&
       parsed.deadline_at_unix_ms < parsed.issued_at_unix_ms)) {
    decision->command = parsed;
    decision->disposition = CommandDisposition::kRejected;
    decision->terminal_result_json =
        rejection(expected_device_id, parsed.command_id,
                  "DEVICE_COMMAND_INVALID", "TIMING",
                  "Command timestamps are invalid", false);
    dedupe->Remember(parsed.command_id,
                     decision->terminal_result_json);
    cJSON_Delete(root);
    return ESP_OK;
  }

  if (parsed.has_deadline) {
    if (!trusted_clock_ready()) {
      decision->command = parsed;
      decision->disposition = CommandDisposition::kRejected;
      decision->terminal_result_json =
          rejection(expected_device_id, parsed.command_id,
                    "DEVICE_CLOCK_UNTRUSTED", "TIMING",
                    "Device has no Hub-trusted UTC clock", true);
      dedupe->Remember(parsed.command_id,
                       decision->terminal_result_json);
      cJSON_Delete(root);
      return ESP_OK;
    }

    const int64_t now_ms = trusted_now_unix_ms();
    if (now_ms <= 0 || now_ms > parsed.deadline_at_unix_ms) {
      decision->command = parsed;
      decision->disposition = CommandDisposition::kTimedOut;
      decision->terminal_result_json =
          make_command_result(expected_device_id, parsed.command_id,
                              "TIMED_OUT",
                              "DEVICE_COMMAND_EXPIRED", "TIMING",
                              "Command deadline has expired", false);
      dedupe->Remember(parsed.command_id,
                       decision->terminal_result_json);
      cJSON_Delete(root);
      return ESP_OK;
    }
  }

  decision->command = std::move(parsed);
  decision->disposition = CommandDisposition::kReady;
  decision->terminal_result_json.clear();
  cJSON_Delete(root);
  return ESP_OK;
}

}  // namespace stagecore
