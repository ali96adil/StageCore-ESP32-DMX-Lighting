#include "stage_device_runtime.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "cJSON.h"
#include "assignment_epoch_store.h"
#include "assignment_v2.h"
#include "command_contract.h"
#include "lighting_contract.h"
#include "lighting_configuration.h"
#include "lighting_output.h"
#include "runtime_capabilities.h"
#include "state_probe_v2.h"
#include "trusted_clock.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#ifndef STAGECORE_FW_VERSION
#define STAGECORE_FW_VERSION "0.2.0-dev"
#endif

#ifndef STAGECORE_EXPERIMENTAL_DEVICE_V2
#define STAGECORE_EXPERIMENTAL_DEVICE_V2 0
#endif

#ifndef STAGECORE_EXPERIMENTAL_V2_STATE_PROBE
#define STAGECORE_EXPERIMENTAL_V2_STATE_PROBE 0
#endif

#ifndef STAGECORE_EXPERIMENTAL_V2_LIGHTING_ACTIVE
#define STAGECORE_EXPERIMENTAL_V2_LIGHTING_ACTIVE 0
#endif

namespace stagecore {
namespace {

constexpr char kTag[] = "stagecore-runtime";
#if STAGECORE_EXPERIMENTAL_DEVICE_V2
constexpr char kProtocolVersion[] = "stagecore.device/2";
constexpr EventBits_t kBlackoutBit = BIT5;
constexpr EventBits_t kEpochReceiptBit = BIT6;
#if STAGECORE_EXPERIMENTAL_V2_STATE_PROBE
constexpr EventBits_t kProbeBit = BIT7;
#endif
#if STAGECORE_EXPERIMENTAL_V2_LIGHTING_ACTIVE
constexpr EventBits_t kLightingActivationBit = BIT8;
constexpr EventBits_t kActiveReadyBit = BIT9;
#endif
constexpr int kPhysicalDMXChannels = 12;
#else
constexpr char kProtocolVersion[] = "stagecore.device/1";
#endif
constexpr char kProfileID[] = "stagecore.esp32-dmx-lighting-node";
constexpr EventBits_t kConnectedBit = BIT0;
constexpr EventBits_t kReadyBit = BIT1;
constexpr EventBits_t kDisconnectedBit = BIT2;
constexpr EventBits_t kProtocolErrorBit = BIT3;
constexpr EventBits_t kCommandBit = BIT4;
constexpr size_t kMaxInboundBytes = 8192;
constexpr int kReadyTimeoutMS = 5000;
constexpr int kHeartbeatMS = 10000;

struct RuntimeContext {
  EventGroupHandle_t events = nullptr;
  SemaphoreHandle_t command_lock = nullptr;
  std::string device_id;
  std::string project_id;
  std::string inbound;
  std::string pending_command_frame;
#if STAGECORE_EXPERIMENTAL_DEVICE_V2
  std::string pending_blackout_frame;
#if STAGECORE_EXPERIMENTAL_V2_STATE_PROBE
  std::string pending_probe_frame;
#endif
#if STAGECORE_EXPERIMENTAL_V2_LIGHTING_ACTIVE
  std::string pending_activation_frame;
  std::string runtime_snapshot_id;
  std::string configuration_hash;
  bool active_epoch = false;
  bool commands_enabled = false;
#endif
  std::atomic<int64_t> assignment_epoch{0};
  int64_t connection_generation = 0;
  bool blocked_epoch = false;
#endif
  std::string last_accepted_command_id;
  std::string last_applied_command_id;
  int expected_payload = 0;
  CommandDedupeCache dedupe;
};

const char *reset_reason_name(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXTERNAL";
    case ESP_RST_SW: return "SOFTWARE";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    default: return "UNKNOWN";
  }
}

cJSON *capabilities_json() {
  cJSON *array = cJSON_CreateArray();
  if (array == nullptr) return nullptr;
  for (const char *capability : runtime_advertised_capabilities(
           STAGECORE_EXPERIMENTAL_DEVICE_V2 != 0,
           STAGECORE_EXPERIMENTAL_V2_STATE_PROBE != 0,
           STAGECORE_EXPERIMENTAL_V2_LIGHTING_ACTIVE != 0)) {
    cJSON *item = cJSON_CreateString(capability);
    if (item == nullptr || !cJSON_AddItemToArray(array, item)) {
      if (item != nullptr) cJSON_Delete(item);
      cJSON_Delete(array);
      return nullptr;
    }
  }
  return array;
}

const char *runtime_readiness(const RuntimeContext *context = nullptr) {
#if STAGECORE_EXPERIMENTAL_DEVICE_V2
#if STAGECORE_EXPERIMENTAL_V2_LIGHTING_ACTIVE
  if (context == nullptr || !context->commands_enabled ||
      !lighting_configuration_ready() ||
      !lighting_output_dmx_healthy() ||
      lighting_authority() == "FAILSAFE") {
    return "BLOCKER";
  }
  if (esp_reset_reason() == ESP_RST_BROWNOUT) {
    return "WARNING";
  }
  return "READY";
#else
  // Blackout/probe v2 images never claim show readiness.
  (void)context;
  return "BLOCKER";
#endif
#else
  (void)context;
  if (!lighting_configuration_ready() ||
      !lighting_output_dmx_healthy() ||
      lighting_authority() == "FAILSAFE") {
    return "BLOCKER";
  }
  if (esp_reset_reason() == ESP_RST_BROWNOUT) {
    return "WARNING";
  }
  return "READY";
#endif
}

cJSON *observed_state_json(const RuntimeContext *context = nullptr) {
  cJSON *state = cJSON_CreateObject();
  if (state == nullptr) return nullptr;

  cJSON_AddNumberToObject(state, "schema_version", 1);
  cJSON_AddStringToObject(state, "firmware_version", STAGECORE_FW_VERSION);
  cJSON_AddNumberToObject(
      state, "uptime_seconds",
      static_cast<double>(esp_timer_get_time() / 1000000LL));
  cJSON_AddStringToObject(
      state, "reset_reason", reset_reason_name(esp_reset_reason()));

  wifi_ap_record_t ap{};
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
    cJSON_AddNumberToObject(state, "wifi_rssi_dbm", ap.rssi);
  }

  cJSON *levels = cJSON_CreateObject();
  if (levels == nullptr) {
    cJSON_Delete(state);
    return nullptr;
  }
  // Legacy v1 channel aliases may still exist in NVS for rollback. A v2
  // observation must not present those aliases as an active Project config.
  bool expose_runtime_config =
      runtime_exposes_legacy_configuration(STAGECORE_EXPERIMENTAL_DEVICE_V2 != 0);
#if STAGECORE_EXPERIMENTAL_V2_LIGHTING_ACTIVE
  expose_runtime_config =
      expose_runtime_config || (context != nullptr && context->commands_enabled);
#endif
  if (expose_runtime_config) {
    for (const auto &entry : lighting_current_levels()) {
      cJSON_AddNumberToObject(levels, entry.channel_key.c_str(), entry.level);
    }
  }
  cJSON_AddItemToObject(state, "current_levels", levels);
  cJSON_AddBoolToObject(state, "dmx_healthy",
                        lighting_output_dmx_healthy());
  if (expose_runtime_config) {
    const std::string configuration_hash = lighting_configuration_hash();
    if (!configuration_hash.empty()) {
      cJSON_AddStringToObject(state, "configuration_hash",
                              configuration_hash.c_str());
    }
  }
  LightingActiveFade fade;
  if (lighting_active_fade(&fade) && trusted_clock_ready()) {
    const int64_t now_ms = trusted_now_unix_ms();
    const int64_t started_ms = now_ms - fade.elapsed_ms;
    const int64_t ends_ms = started_ms + fade.duration_ms;
    const std::string started_at = format_rfc3339_utc_ms(started_ms);
    const std::string ends_at = format_rfc3339_utc_ms(ends_ms);
    if (!started_at.empty() && !ends_at.empty()) {
      cJSON *active = cJSON_CreateObject();
      cJSON *targets = cJSON_CreateObject();
      if (active != nullptr && targets != nullptr) {
        cJSON_AddStringToObject(active, "command_id",
                                fade.command_id.c_str());
        cJSON_AddStringToObject(active, "started_at", started_at.c_str());
        cJSON_AddStringToObject(active, "ends_at", ends_at.c_str());
        for (const auto &entry : fade.targets) {
          cJSON_AddNumberToObject(targets, entry.channel_key.c_str(),
                                  entry.level);
        }
        cJSON_AddItemToObject(active, "targets", targets);
        cJSON_AddItemToObject(state, "active_fade", active);
      } else {
        if (active != nullptr) cJSON_Delete(active);
        if (targets != nullptr) cJSON_Delete(targets);
      }
    }
  }

  if (context != nullptr) {
    if (!context->last_accepted_command_id.empty()) {
      cJSON_AddStringToObject(
          state, "last_accepted_command_id",
          context->last_accepted_command_id.c_str());
    }
    if (!context->last_applied_command_id.empty()) {
      cJSON_AddStringToObject(
          state, "last_applied_command_id",
          context->last_applied_command_id.c_str());
    }
  }

  cJSON_AddBoolToObject(
      state, "brownout_warning",
      esp_reset_reason() == ESP_RST_BROWNOUT);
  const std::string authority = lighting_authority();
  cJSON_AddStringToObject(state, "authority", authority.c_str());
  return state;
}

cJSON *network_state_json(const VerifiedHub &hub) {
  cJSON *network = cJSON_CreateObject();
  if (network == nullptr) return nullptr;
  cJSON_AddStringToObject(network, "transport", "TLS_WEBSOCKET");
  cJSON_AddStringToObject(network, "protocol", kProtocolVersion);
  cJSON_AddStringToObject(network, "hub_id", hub.hub_id.c_str());
  cJSON_AddBoolToObject(network, "certificate_pinned", true);
  return network;
}

std::string print_json(cJSON *root) {
  if (root == nullptr) return {};
  char *text = cJSON_PrintUnformatted(root);
  std::string out = text != nullptr ? text : "";
  if (text != nullptr) cJSON_free(text);
  return out;
}

std::string make_hello(const VerifiedHub &hub,
                       const DeviceIdentity &identity,
                       const DeviceConfig &config) {
  cJSON *root = cJSON_CreateObject();
  if (root == nullptr) return {};

  cJSON_AddStringToObject(root, "type", "device.hello");
  cJSON_AddNumberToObject(root, "schema_version", 1);
  cJSON_AddStringToObject(root, "device_id", identity.device_id().c_str());
#if !STAGECORE_EXPERIMENTAL_DEVICE_V2
  cJSON_AddStringToObject(root, "project_id", config.project_id.c_str());
#endif
  cJSON_AddStringToObject(root, "profile_id", kProfileID);
  cJSON_AddStringToObject(root, "device_kind", "GENERIC");
  cJSON_AddStringToObject(root, "display_name", config.display_name.c_str());
  cJSON_AddStringToObject(root, "platform", "esp32");
  cJSON_AddStringToObject(root, "architecture", "xtensa");
  cJSON_AddStringToObject(root, "client_version", STAGECORE_FW_VERSION);
  cJSON_AddStringToObject(root, "protocol_version", kProtocolVersion);

  cJSON *caps = capabilities_json();
  cJSON *observed = observed_state_json();
  cJSON *network = network_state_json(hub);
  if (caps == nullptr || observed == nullptr || network == nullptr) {
    if (caps != nullptr) cJSON_Delete(caps);
    if (observed != nullptr) cJSON_Delete(observed);
    if (network != nullptr) cJSON_Delete(network);
    cJSON_Delete(root);
    return {};
  }
  cJSON_AddItemToObject(root, "capabilities", caps);
  cJSON_AddStringToObject(root, "readiness", runtime_readiness());
  cJSON_AddItemToObject(root, "observed_state", observed);
  cJSON_AddItemToObject(root, "network_state", network);

  const std::string result = print_json(root);
  cJSON_Delete(root);
  return result;
}

std::string make_observation(const VerifiedHub &hub,
                             const DeviceIdentity &identity,
                             const RuntimeContext *context) {
  cJSON *root = cJSON_CreateObject();
  if (root == nullptr) return {};

  cJSON_AddStringToObject(root, "type", "device.observation");
#if STAGECORE_EXPERIMENTAL_DEVICE_V2
  cJSON_AddNumberToObject(root, "schema_version", 2);
#else
  cJSON_AddNumberToObject(root, "schema_version", 1);
#endif
  cJSON_AddStringToObject(root, "device_id", identity.device_id().c_str());
  cJSON_AddStringToObject(root, "readiness", runtime_readiness(context));

  cJSON *observed = observed_state_json(context);
  cJSON *network = network_state_json(hub);
  if (observed == nullptr || network == nullptr) {
    if (observed != nullptr) cJSON_Delete(observed);
    if (network != nullptr) cJSON_Delete(network);
    cJSON_Delete(root);
    return {};
  }
  cJSON_AddItemToObject(root, "observed_state", observed);
  cJSON_AddItemToObject(root, "network_state", network);

  const std::string result = print_json(root);
  cJSON_Delete(root);
  return result;
}

#if STAGECORE_EXPERIMENTAL_DEVICE_V2
bool positive_wire_integer(const cJSON *root, const char *key, int64_t *out) {
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
  if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
      value->valuedouble < 1.0 || value->valuedouble > 9007199254740991.0 ||
      std::floor(value->valuedouble) != value->valuedouble ||
      out == nullptr) {
    return false;
  }
  *out = static_cast<int64_t>(value->valuedouble);
  return true;
}

bool canonical_hex_nonce(const cJSON *nonce) {
  if (!cJSON_IsString(nonce) || nonce->valuestring == nullptr ||
      std::strlen(nonce->valuestring) != 64) {
    return false;
  }
  for (const char *p = nonce->valuestring; *p != '\0'; ++p) {
    if (!(*p >= '0' && *p <= '9') && !(*p >= 'a' && *p <= 'f')) {
      return false;
    }
  }
  return true;
}
#endif

bool handle_complete_text(RuntimeContext *context, const std::string &text) {
  cJSON *root = cJSON_ParseWithLength(text.data(), text.size());
  if (root == nullptr) return false;

  const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
  const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
  const cJSON *device = cJSON_GetObjectItemCaseSensitive(root, "device_id");

#if STAGECORE_EXPERIMENTAL_DEVICE_V2
  bool ok = cJSON_IsString(type) && type->valuestring != nullptr &&
            cJSON_IsNumber(schema) &&
            assignment_v2::valid_v2_wire_schema(schema->valuedouble) &&
            cJSON_IsString(device) && device->valuestring != nullptr &&
            context->device_id == device->valuestring;
  if (ok && std::strcmp(type->valuestring, "assignment.state") == 0) {
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    const cJSON *project = cJSON_GetObjectItemCaseSensitive(root, "project_id");
    const cJSON *blackout = cJSON_GetObjectItemCaseSensitive(root, "blackout_required");
    const cJSON *commands = cJSON_GetObjectItemCaseSensitive(root, "commands_enabled");
    const cJSON *ack_required = cJSON_GetObjectItemCaseSensitive(root, "epoch_ack_required");
    int64_t epoch = 0;
    int64_t generation = 0;
    const bool unassigned = cJSON_IsString(state) && state->valuestring &&
        std::strcmp(state->valuestring, "UNASSIGNED") == 0;
    const bool blocked = cJSON_IsString(state) && state->valuestring &&
        std::strcmp(state->valuestring, "BLOCKED") == 0;
    bool active = false;
#if STAGECORE_EXPERIMENTAL_V2_LIGHTING_ACTIVE
    const cJSON *snapshot =
        cJSON_GetObjectItemCaseSensitive(root, "runtime_snapshot_id");
    const cJSON *configuration_hash =
        cJSON_GetObjectItemCaseSensitive(root, "configuration_hash");
    const cJSON *scope_ack_required =
        cJSON_GetObjectItemCaseSensitive(root, "scope_ack_required");
    active = cJSON_IsString(state) && state->valuestring &&
        std::strcmp(state->valuestring, "ACTIVE") == 0;
#endif
    const bool project_unassigned = project == nullptr ||
        (cJSON_IsString(project) && project->valuestring &&
         project->valuestring[0] == '\0');
    const bool project_assigned = cJSON_IsString(project) &&
        project->valuestring && project->valuestring[0] != '\0';
    const bool base_shape =
        unassigned && project_unassigned && ack_required == nullptr;
    const bool blocked_shape =
        blocked && project_assigned && cJSON_IsTrue(ack_required);
#if STAGECORE_EXPERIMENTAL_V2_LIGHTING_ACTIVE
    const bool active_shape =
        active && project_assigned &&
        cJSON_IsString(snapshot) && snapshot->valuestring &&
        snapshot->valuestring[0] != '\0' &&
        canonical_hex_nonce(configuration_hash) &&
        cJSON_IsTrue(scope_ack_required) && ack_required == nullptr;
#else
    const bool active_shape = false;
#endif
    ok = positive_wire_integer(root, "assignment_epoch", &epoch) &&
         positive_wire_integer(root, "connection_generation", &generation) &&
         (base_shape || ((blocked_shape || active_shape) && epoch > 1)) &&
         cJSON_IsTrue(blackout) && cJSON_IsFalse(commands) &&
         context->assignment_epoch.load() == 0;
    if (ok) {
      context->project_id = (blocked || active) ? project->valuestring : "";
      context->connection_generation = generation;
      context->blocked_epoch = blocked;
#if STAGECORE_EXPERIMENTAL_V2_LIGHTING_ACTIVE
      context->active_epoch = active;
      context->commands_enabled = false;
      if (active) {
        context->runtime_snapshot_id = snapshot->valuestring;
        context->configuration_hash = configuration_hash->valuestring;
      }
#endif
      context->assignment_epoch.store(epoch);
      xEventGroupSetBits(context->events, kReadyBit);
    }
  } else if (ok && std::strcmp(type->valuestring, "assignment.blackout") == 0) {
    int64_t epoch = 0;
    int64_t generation = 0;
    int64_t channels = 0;
    const cJSON *transfer = cJSON_GetObjectItemCaseSensitive(root, "transfer_id");
    const cJSON *nonce = cJSON_GetObjectItemCaseSensitive(root, "challenge");
    const cJSON *required = cJSON_GetObjectItemCaseSensitive(root, "blackout_required");
    ok = context->assignment_epoch.load() > 0 &&
         positive_wire_integer(root, "assignment_epoch", &epoch) &&
         epoch == context->assignment_epoch.load() &&
         positive_wire_integer(root, "connection_generation", &generation) &&
         generation == context->connection_generation &&
         positive_wire_integer(root, "expected_channels", &channels) &&
         channels == kPhysicalDMXChannels &&
         cJSON_IsString(transfer) && transfer->valuestring &&
         std::strlen(transfer->valuestring) == 36 &&
         canonical_hex_nonce(nonce) && cJSON_IsTrue(required);
    if (ok && context->command_lock != nullptr &&
        xSemaphoreTake(context->command_lock, 0) == pdTRUE) {
      if (!context->pending_blackout_frame.empty()
#if STAGECORE_EXPERIMENTAL_V2_STATE_PROBE
          || !context->pending_probe_frame.empty()
#endif
#if STAGECORE_EXPERIMENTAL_V2_LIGHTING_ACTIVE
          || !context->pending_activation_frame.empty()
#endif
          ) {
        ok = false;
      } else {
        context->pending_blackout_frame = text;
        xEventGroupSetBits(context->events, kBlackoutBit);
      }
      xSemaphoreGive(context->command_lock);
    } else {
      ok = false;
    }
#if STAGECORE_EXPERIMENTAL_V2_LIGHTING_ACTIVE
  } else if (ok && std::strcmp(type->valuestring, "lighting.assignment.activate") == 0) {
    int64_t epoch = 0;
    int64_t generation = 0;
    int64_t channels = 0;
    const cJSON *activation =
        cJSON_GetObjectItemCaseSensitive(root, "activation_id");
    const cJSON *project =
        cJSON_GetObjectItemCaseSensitive(root, "project_id");
    const cJSON *snapshot =
        cJSON_GetObjectItemCaseSensitive(root, "runtime_snapshot_id");
    const cJSON *nonce =
        cJSON_GetObjectItemCaseSensitive(root, "challenge");
    const cJSON *configuration =
        cJSON_GetObjectItemCaseSensitive(root, "configuration");
    const cJSON *configuration_hash =
        cJSON_GetObjectItemCaseSensitive(root, "configuration_hash");
    const cJSON *required =
        cJSON_GetObjectItemCaseSensitive(root, "blackout_required");
    const cJSON *commands =
        cJSON_GetObjectItemCaseSensitive(root, "commands_enabled");
    ok = context->blocked_epoch && !context->active_epoch &&
         context->assignment_epoch.load() > 1 &&
         positive_wire_integer(root, "assignment_epoch", &epoch) &&
         epoch == context->assignment_epoch.load() &&
         positive_wire_integer(root, "connection_generation", &generation) &&
         generation == context->connection_generation &&
         positive_wire_integer(root, "expected_channels", &channels) &&
         channels == kPhysicalDMXChannels &&
         cJSON_IsString(activation) && activation->valuestring &&
         std::strlen(activation->valuestring) == 36 &&
         cJSON_IsString(project) && project->valuestring &&
         context->project_id == project->valuestring &&
         cJSON_IsString(snapshot) && snapshot->valuestring &&
         snapshot->valuestring[0] != '\0' &&
         canonical_hex_nonce(nonce) &&
         canonical_hex_nonce(configuration_hash) &&
         cJSON_IsObject(configuration) &&
         cJSON_IsTrue(required) && cJSON_IsFalse(commands);
    if (ok && context->command_lock != nullptr &&
        xSemaphoreTake(context->command_lock, 0) == pdTRUE) {
      if (!context->pending_blackout_frame.empty() ||
          !context->pending_activation_frame.empty()
#if STAGECORE_EXPERIMENTAL_V2_STATE_PROBE
          || !context->pending_probe_frame.empty()
#endif
          || !context->pending_command_frame.empty()) {
        ok = false;
      } else {
        context->pending_activation_frame = text;
        xEventGroupSetBits(context->events, kLightingActivationBit);
      }
      xSemaphoreGive(context->command_lock);
    } else {
      ok = false;
    }
#endif
#if STAGECORE_EXPERIMENTAL_V2_STATE_PROBE
  } else if (ok && std::strcmp(type->valuestring, "lighting.state_probe") == 0) {
    int64_t epoch = 0;
    int64_t generation = 0;
    int64_t channels = 0;
    const cJSON *nonce = cJSON_GetObjectItemCaseSensitive(root, "challenge");
    const cJSON *commands = cJSON_GetObjectItemCaseSensitive(root, "commands_enabled");
    ok = positive_wire_integer(root, "assignment_epoch", &epoch) &&
         positive_wire_integer(root, "connection_generation", &generation) &&
         positive_wire_integer(root, "expected_channels", &channels) &&
         channels == kPhysicalDMXChannels && canonical_hex_nonce(nonce) &&
         cJSON_IsFalse(commands) &&
         state_probe_v2::valid_request(
             {context->device_id, static_cast<uint64_t>(epoch),
              static_cast<uint64_t>(generation), nonce->valuestring,
              static_cast<uint16_t>(channels), false},
             {context->device_id,
              static_cast<uint64_t>(context->assignment_epoch.load()),
              static_cast<uint64_t>(context->connection_generation)});
    if (ok && context->command_lock != nullptr &&
        xSemaphoreTake(context->command_lock, 0) == pdTRUE) {
      if (!context->pending_blackout_frame.empty() ||
          !context->pending_probe_frame.empty()
#if STAGECORE_EXPERIMENTAL_V2_LIGHTING_ACTIVE
          || !context->pending_activation_frame.empty()
#endif
          ) {
        ok = false;
      } else {
        context->pending_probe_frame = text;
        xEventGroupSetBits(context->events, kProbeBit);
      }
      xSemaphoreGive(context->command_lock);
    } else {
      ok = false;
    }
#endif
  } else if (ok && std::strcmp(type->valuestring, "assignment.epoch_ack_receipt") == 0) {
    const cJSON *project = cJSON_GetObjectItemCaseSensitive(root, "project_id");
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    const cJSON *persisted = cJSON_GetObjectItemCaseSensitive(root, "persisted");
    const cJSON *commands = cJSON_GetObjectItemCaseSensitive(root, "commands_enabled");
    int64_t epoch = 0;
    int64_t generation = 0;
    ok = context->blocked_epoch && context->assignment_epoch.load() > 1 &&
         cJSON_IsString(project) && project->valuestring &&
         context->project_id == project->valuestring &&
         cJSON_IsString(state) && state->valuestring &&
         std::strcmp(state->valuestring, "BLOCKED") == 0 &&
         positive_wire_integer(root, "assignment_epoch", &epoch) &&
         epoch == context->assignment_epoch.load() &&
         positive_wire_integer(root, "connection_generation", &generation) &&
         generation == context->connection_generation &&
         cJSON_IsTrue(persisted) && cJSON_IsFalse(commands);
    if (ok) xEventGroupSetBits(context->events, kEpochReceiptBit);
#if STAGECORE_EXPERIMENTAL_V2_LIGHTING_ACTIVE
  } else if (ok && std::strcmp(type->valuestring, "runtime.ready") == 0) {
    const cJSON *protocol =
        cJSON_GetObjectItemCaseSensitive(root, "protocol_version");
    const cJSON *project =
        cJSON_GetObjectItemCaseSensitive(root, "project_id");
    const cJSON *snapshot =
        cJSON_GetObjectItemCaseSensitive(root, "runtime_snapshot_id");
    const cJSON *configuration_hash =
        cJSON_GetObjectItemCaseSensitive(root, "configuration_hash");
    const cJSON *commands =
        cJSON_GetObjectItemCaseSensitive(root, "commands_enabled");
    int64_t epoch = 0;
    int64_t generation = 0;
    ok = context->active_epoch && !context->commands_enabled &&
         cJSON_IsString(protocol) && protocol->valuestring &&
         std::strcmp(protocol->valuestring, kProtocolVersion) == 0 &&
         cJSON_IsString(project) && project->valuestring &&
         context->project_id == project->valuestring &&
         cJSON_IsString(snapshot) && snapshot->valuestring &&
         context->runtime_snapshot_id == snapshot->valuestring &&
         canonical_hex_nonce(configuration_hash) &&
         context->configuration_hash == configuration_hash->valuestring &&
         positive_wire_integer(root, "assignment_epoch", &epoch) &&
         epoch == context->assignment_epoch.load() &&
         positive_wire_integer(root, "connection_generation", &generation) &&
         generation == context->connection_generation &&
         cJSON_IsTrue(commands);
    if (ok) {
      context->commands_enabled = true;
      xEventGroupSetBits(context->events, kActiveReadyBit);
    }
  } else if (ok && std::strcmp(type->valuestring, "command.execute") == 0) {
    ok = context->active_epoch && context->commands_enabled &&
         context->command_lock != nullptr &&
         xSemaphoreTake(context->command_lock, 0) == pdTRUE;
    if (ok) {
      if (!context->pending_command_frame.empty() ||
          !context->pending_activation_frame.empty() ||
          !context->pending_blackout_frame.empty()) {
        ok = false;
      } else {
        context->pending_command_frame = text;
        xEventGroupSetBits(context->events, kCommandBit);
      }
      xSemaphoreGive(context->command_lock);
    }
#endif
  } else {
    // Blackout-only v2 images never accept show commands or ACTIVE state.
    // The active source-only image reaches command.execute only after the
    // exact Hub-owned Project/Snapshot/config scope handshake above.
    ok = false;
  }
#else
  bool ok = cJSON_IsString(type) && type->valuestring != nullptr &&
            cJSON_IsNumber(schema) && schema->valueint == 1;

  if (ok && std::strcmp(type->valuestring, "runtime.ready") == 0) {
    const cJSON *protocol =
        cJSON_GetObjectItemCaseSensitive(root, "protocol_version");
    ok = cJSON_IsString(device) && device->valuestring != nullptr &&
         context->device_id == device->valuestring &&
         cJSON_IsString(protocol) && protocol->valuestring != nullptr &&
         std::strcmp(protocol->valuestring, kProtocolVersion) == 0;
    if (ok) xEventGroupSetBits(context->events, kReadyBit);
  } else if (ok && std::strcmp(type->valuestring, "command.execute") == 0) {
    if (context->command_lock == nullptr ||
        xSemaphoreTake(context->command_lock, 0) != pdTRUE) {
      ESP_LOGE(kTag, "command queue lock unavailable");
      ok = false;
    } else {
      if (!context->pending_command_frame.empty()) {
        ESP_LOGE(kTag, "command queue overflow; fail closed");
        ok = false;
      } else {
        context->pending_command_frame = text;
        xEventGroupSetBits(context->events, kCommandBit);
      }
      xSemaphoreGive(context->command_lock);
    }
  } else {
    ESP_LOGE(kTag, "unexpected Stage Device runtime frame");
    ok = false;
  }
#endif

  cJSON_Delete(root);
  return ok;
}

void runtime_event_handler(void *arg, esp_event_base_t,
                           int32_t event_id, void *event_data) {
  auto *context = static_cast<RuntimeContext *>(arg);
  if (context == nullptr || context->events == nullptr) return;

  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
      xEventGroupSetBits(context->events, kConnectedBit);
      break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
    case WEBSOCKET_EVENT_ERROR:
      xEventGroupSetBits(context->events, kDisconnectedBit);
      break;
    case WEBSOCKET_EVENT_DATA: {
      auto *data = static_cast<esp_websocket_event_data_t *>(event_data);
      if (data == nullptr) {
        xEventGroupSetBits(context->events, kProtocolErrorBit);
        break;
      }
      // Ignore control frames; only text/continuation payloads are application
      // protocol input.
      if (data->op_code != 0x1 && data->op_code != 0x0) break;
      if (data->payload_len <= 0 ||
          static_cast<size_t>(data->payload_len) > kMaxInboundBytes ||
          data->payload_offset < 0 || data->data_len < 0) {
        xEventGroupSetBits(context->events, kProtocolErrorBit);
        break;
      }
      if (data->payload_offset == 0) {
        context->inbound.clear();
        context->expected_payload = data->payload_len;
        context->inbound.reserve(static_cast<size_t>(data->payload_len));
      }
      if (context->expected_payload != data->payload_len ||
          static_cast<int>(context->inbound.size()) != data->payload_offset ||
          context->inbound.size() + static_cast<size_t>(data->data_len) >
              kMaxInboundBytes) {
        xEventGroupSetBits(context->events, kProtocolErrorBit);
        break;
      }
      if (data->data_ptr != nullptr && data->data_len > 0) {
        context->inbound.append(data->data_ptr,
                                static_cast<size_t>(data->data_len));
      }
      const bool complete =
          data->fin &&
          static_cast<int>(context->inbound.size()) == context->expected_payload;
      if (complete &&
          !handle_complete_text(context, context->inbound)) {
        xEventGroupSetBits(context->events, kProtocolErrorBit);
      }
      break;
    }
    default:
      break;
  }
}

esp_err_t send_text(esp_websocket_client_handle_t client,
                    const std::string &message) {
  if (message.empty()) return ESP_ERR_INVALID_ARG;
  const int sent = esp_websocket_client_send_text(
      client, message.data(), static_cast<int>(message.size()),
      pdMS_TO_TICKS(3000));
  return sent == static_cast<int>(message.size()) ? ESP_OK : ESP_FAIL;
}

#if STAGECORE_EXPERIMENTAL_DEVICE_V2
esp_err_t process_pending_blackout(RuntimeContext *context,
                                   esp_websocket_client_handle_t client) {
  if (context == nullptr || context->command_lock == nullptr || client == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  std::string frame;
  if (xSemaphoreTake(context->command_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }
  frame.swap(context->pending_blackout_frame);
  xSemaphoreGive(context->command_lock);
  xEventGroupClearBits(context->events, kBlackoutBit);
  if (frame.empty()) return ESP_ERR_INVALID_STATE;

  cJSON *root = cJSON_ParseWithLength(frame.data(), frame.size());
  if (root == nullptr) return ESP_ERR_INVALID_RESPONSE;
  const cJSON *transfer = cJSON_GetObjectItemCaseSensitive(root, "transfer_id");
  const cJSON *nonce = cJSON_GetObjectItemCaseSensitive(root, "challenge");
  int64_t epoch = 0;
  int64_t generation = 0;
  int64_t channels = 0;
  const bool scope_ok = context->assignment_epoch.load() > 0 &&
      positive_wire_integer(root, "assignment_epoch", &epoch) &&
      epoch == context->assignment_epoch.load() &&
      positive_wire_integer(root, "connection_generation", &generation) &&
      generation == context->connection_generation &&
      positive_wire_integer(root, "expected_channels", &channels) &&
      channels == kPhysicalDMXChannels &&
      cJSON_IsString(transfer) && transfer->valuestring != nullptr &&
      std::strlen(transfer->valuestring) == 36 &&
      canonical_hex_nonce(nonce);
  if (!scope_ok) {
    cJSON_Delete(root);
    return ESP_ERR_INVALID_RESPONSE;
  }

  // Apply the ordinary logical failsafe (cancel fades, identify and queued
  // commands), then explicitly zero ALL 12 physical DMX slots and wait for
  // output-task confirmation. Do not claim hardware-independent darkness.
  esp_err_t err = lighting_blackout(true);
  if (err == ESP_OK) err = lighting_output_blackout_immediate();
  if (err != ESP_OK || !lighting_output_dmx_healthy()) {
    cJSON_Delete(root);
    return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;  // no forged ACK
  }
  cJSON *ack = cJSON_CreateObject();
  cJSON *levels = cJSON_CreateArray();
  if (ack == nullptr || levels == nullptr) {
    if (ack != nullptr) cJSON_Delete(ack);
    if (levels != nullptr) cJSON_Delete(levels);
    cJSON_Delete(root);
    return ESP_ERR_NO_MEM;
  }
  cJSON_AddStringToObject(ack, "type", "assignment.blackout_ack");
  cJSON_AddNumberToObject(ack, "schema_version", 2);
  cJSON_AddStringToObject(ack, "device_id", context->device_id.c_str());
  cJSON_AddStringToObject(ack, "transfer_id", transfer->valuestring);
  cJSON_AddNumberToObject(ack, "assignment_epoch", static_cast<double>(epoch));
  cJSON_AddNumberToObject(ack, "connection_generation", static_cast<double>(generation));
  cJSON_AddStringToObject(ack, "challenge", nonce->valuestring);
  cJSON_AddBoolToObject(ack, "blackout", true);
  for (int i = 0; i < kPhysicalDMXChannels; ++i) {
    cJSON_AddItemToArray(levels, cJSON_CreateNumber(0));
  }
  cJSON_AddItemToObject(ack, "channel_levels", levels);
  const std::string payload = print_json(ack);
  cJSON_Delete(ack);
  cJSON_Delete(root);
  return send_text(client, payload);
}
#endif

#if STAGECORE_EXPERIMENTAL_DEVICE_V2 && STAGECORE_EXPERIMENTAL_V2_STATE_PROBE
esp_err_t process_pending_probe(RuntimeContext *context,
                                esp_websocket_client_handle_t client) {
  if (context == nullptr || context->command_lock == nullptr || client == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  std::string frame;
  if (xSemaphoreTake(context->command_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }
  frame.swap(context->pending_probe_frame);
  xSemaphoreGive(context->command_lock);
  xEventGroupClearBits(context->events, kProbeBit);
  if (frame.empty()) return ESP_ERR_INVALID_STATE;

  cJSON *request = cJSON_ParseWithLength(frame.data(), frame.size());
  if (request == nullptr) return ESP_ERR_INVALID_RESPONSE;
  const cJSON *nonce = cJSON_GetObjectItemCaseSensitive(request, "challenge");
  int64_t epoch = 0;
  int64_t generation = 0;
  int64_t channels = 0;
  const cJSON *commands = cJSON_GetObjectItemCaseSensitive(request, "commands_enabled");
  const bool scope_ok = positive_wire_integer(request, "assignment_epoch", &epoch) &&
      positive_wire_integer(request, "connection_generation", &generation) &&
      positive_wire_integer(request, "expected_channels", &channels) &&
      channels == kPhysicalDMXChannels && canonical_hex_nonce(nonce) &&
      cJSON_IsFalse(commands) &&
      state_probe_v2::valid_request(
          {context->device_id, static_cast<uint64_t>(epoch),
           static_cast<uint64_t>(generation), nonce->valuestring,
           static_cast<uint16_t>(channels), false},
          {context->device_id,
           static_cast<uint64_t>(context->assignment_epoch.load()),
           static_cast<uint64_t>(context->connection_generation)});
  if (!scope_ok) {
    cJSON_Delete(request);
    return ESP_ERR_INVALID_RESPONSE;
  }

  // A logical output-task-backed report is not an independent DMX decoder
  // measurement and never grants Project/ACTIVE/command authority.
  std::vector<uint8_t> slots;
  const esp_err_t read_error = lighting_output_read_slots(&slots);
  const auto sample = state_probe_v2::classify_sample(
      slots, read_error == ESP_OK && lighting_output_dmx_healthy(),
      lighting_authority() == "FAILSAFE");
  if (!sample.valid) {
    cJSON_Delete(request);
    return read_error != ESP_OK ? read_error : ESP_ERR_INVALID_STATE;
  }
  cJSON *report = cJSON_CreateObject();
  cJSON *levels = cJSON_CreateArray();
  if (report == nullptr || levels == nullptr) {
    if (report != nullptr) cJSON_Delete(report);
    if (levels != nullptr) cJSON_Delete(levels);
    cJSON_Delete(request);
    return ESP_ERR_NO_MEM;
  }
  cJSON_AddStringToObject(report, "type", "lighting.state_report");
  cJSON_AddNumberToObject(report, "schema_version", 2);
  cJSON_AddStringToObject(report, "device_id", context->device_id.c_str());
  cJSON_AddNumberToObject(report, "assignment_epoch", static_cast<double>(epoch));
  cJSON_AddNumberToObject(report, "connection_generation", static_cast<double>(generation));
  cJSON_AddStringToObject(report, "challenge", nonce->valuestring);
  cJSON_AddBoolToObject(report, "levels_known", sample.levels_known);
  cJSON_AddBoolToObject(report, "blackout", sample.blackout);
  for (uint8_t level : sample.channel_levels) {
    cJSON_AddItemToArray(levels, cJSON_CreateNumber(level));
  }
  cJSON_AddItemToObject(report, "channel_levels", levels);
  const std::string payload = print_json(report);
  cJSON_Delete(report);
  cJSON_Delete(request);
  return send_text(client, payload);
}
#endif

#if STAGECORE_EXPERIMENTAL_DEVICE_V2
std::string make_blocked_epoch_ack(const RuntimeContext &context) {
  if (!context.blocked_epoch || context.project_id.size() != 36 ||
      context.assignment_epoch.load() <= 1 || context.connection_generation <= 0) {
    return {};
  }
  cJSON *root = cJSON_CreateObject();
  cJSON *levels = cJSON_CreateArray();
  if (root == nullptr || levels == nullptr) {
    if (root != nullptr) cJSON_Delete(root);
    if (levels != nullptr) cJSON_Delete(levels);
    return {};
  }
  cJSON_AddStringToObject(root, "type", "assignment.epoch_ack");
  cJSON_AddNumberToObject(root, "schema_version", 2);
  cJSON_AddStringToObject(root, "device_id", context.device_id.c_str());
  cJSON_AddStringToObject(root, "project_id", context.project_id.c_str());
  cJSON_AddNumberToObject(root, "assignment_epoch",
                          static_cast<double>(context.assignment_epoch.load()));
  cJSON_AddNumberToObject(root, "connection_generation",
                          static_cast<double>(context.connection_generation));
  cJSON_AddBoolToObject(root, "blackout", true);
  for (int i = 0; i < kPhysicalDMXChannels; ++i) {
    cJSON_AddItemToArray(levels, cJSON_CreateNumber(0));
  }
  cJSON_AddItemToObject(root, "channel_levels", levels);
  const std::string result = print_json(root);
  cJSON_Delete(root);
  return result;
}
#endif

std::string completed_result(const std::string &device_id,
                             const std::string &command_id,
                             cJSON *payload) {
  cJSON *root = cJSON_CreateObject();
  if (root == nullptr) {
    if (payload != nullptr) cJSON_Delete(payload);
    return {};
  }
  cJSON_AddStringToObject(root, "type", "command.result");
  cJSON_AddNumberToObject(root, "schema_version", 1);
  cJSON_AddStringToObject(root, "device_id", device_id.c_str());
  cJSON_AddStringToObject(root, "command_id", command_id.c_str());
  cJSON_AddStringToObject(root, "status", "COMPLETED");
  if (payload != nullptr) cJSON_AddItemToObject(root, "payload", payload);
  const std::string result = print_json(root);
  cJSON_Delete(root);
  return result;
}

std::string levels_payload(
    const std::vector<ChannelLevelV1> &levels) {
  cJSON *payload = cJSON_CreateObject();
  cJSON *values = cJSON_CreateObject();
  if (payload == nullptr || values == nullptr) {
    if (payload != nullptr) cJSON_Delete(payload);
    if (values != nullptr) cJSON_Delete(values);
    return {};
  }
  for (const auto &entry : levels) {
    cJSON_AddNumberToObject(values, entry.channel_key.c_str(), entry.level);
  }
  cJSON_AddItemToObject(payload, "levels", values);
  const std::string out = print_json(payload);
  cJSON_Delete(payload);
  return out;
}

std::string lighting_event_result(
    const std::string &device_id,
    const LightingCommandEvent &event) {
  if (event.status == "COMPLETED") {
    cJSON *payload = cJSON_CreateObject();
    if (payload == nullptr) return {};
    if (event.identify) {
      cJSON_AddStringToObject(payload, "channel_key",
                              event.channel_key.c_str());
      cJSON_AddNumberToObject(payload, "level", event.level);
      cJSON_AddNumberToObject(payload, "duration_ms",
                              static_cast<double>(event.duration_ms));
    } else if (event.blackout) {
      cJSON_AddBoolToObject(payload, "blackout", true);
      cJSON_AddNumberToObject(payload, "fade_ms",
                              static_cast<double>(event.fade_ms));
    } else {
      cJSON *levels = cJSON_CreateObject();
      if (levels == nullptr) {
        cJSON_Delete(payload);
        return {};
      }
      for (const auto &entry : event.levels) {
        cJSON_AddNumberToObject(levels, entry.channel_key.c_str(),
                                entry.level);
      }
      cJSON_AddItemToObject(payload, "levels", levels);
    }
    return completed_result(device_id, event.command_id, payload);
  }

  return make_command_result(
      device_id, event.command_id, event.status.c_str(),
      event.error_code.c_str(), event.category.c_str(),
      event.message.c_str(), event.status == "FAILED");
}

esp_err_t flush_lighting_events(RuntimeContext *context,
                                esp_websocket_client_handle_t client) {
  if (context == nullptr || client == nullptr) return ESP_ERR_INVALID_ARG;

  LightingCommandEvent event;
  while (lighting_pop_command_event(&event)) {
    const std::string result =
        lighting_event_result(context->device_id, event);
    if (result.empty()) return ESP_FAIL;
    if (event.status == "COMPLETED") {
      context->last_applied_command_id = event.command_id;
    }
    context->dedupe.Remember(event.command_id, result);
    const esp_err_t err = send_text(client, result);
    if (err != ESP_OK) return err;
  }
  return ESP_OK;
}

void note_command_accepted(RuntimeContext *context,
                           const std::string &command_id) {
  if (context != nullptr && !command_id.empty()) {
    context->last_accepted_command_id = command_id;
  }
}

void note_command_applied(RuntimeContext *context,
                          const std::string &command_id) {
  if (context != nullptr && !command_id.empty()) {
    context->last_accepted_command_id = command_id;
    context->last_applied_command_id = command_id;
  }
}

esp_err_t process_pending_command(RuntimeContext *context,
                                  esp_websocket_client_handle_t client) {
  if (context == nullptr || context->command_lock == nullptr ||
      client == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }

  std::string frame;
  if (xSemaphoreTake(context->command_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }
  frame.swap(context->pending_command_frame);
  xSemaphoreGive(context->command_lock);
  xEventGroupClearBits(context->events, kCommandBit);

  if (frame.empty()) return ESP_OK;

  CommandDecision decision;
  esp_err_t err = evaluate_command_execute_frame(
      frame, context->device_id, context->project_id,
      &context->dedupe, &decision);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "malformed command.execute frame; closing runtime");
    return err;
  }

  if (decision.disposition == CommandDisposition::kDuplicate ||
      decision.disposition == CommandDisposition::kRejected ||
      decision.disposition == CommandDisposition::kTimedOut) {
    return send_text(client, decision.terminal_result_json);
  }

  LightingPayloadV1 payload;
  std::string payload_error;
  if (validate_lighting_payload(decision.command, &payload,
                                &payload_error) != ESP_OK) {
    const std::string result = make_command_result(
        context->device_id, decision.command.command_id, "REJECTED",
        "DEVICE_COMMAND_INVALID", "VALIDATION",
        payload_error.c_str(), false);
    context->dedupe.Remember(decision.command.command_id, result);
    return send_text(client, result);
  }

  if (decision.command.command_type == "LIGHTING_CONFIG_APPLY") {
    if (decision.command.runtime_snapshot_id.empty()) {
      const std::string result = make_command_result(
          context->device_id, decision.command.command_id, "REJECTED",
          "RUNTIME_SNAPSHOT_REQUIRED", "CONFIGURATION",
          "CONFIG_APPLY requires a Published Runtime Snapshot identity",
          false);
      context->dedupe.Remember(decision.command.command_id, result);
      return send_text(client, result);
    }

    std::string configuration_hash;
    const esp_err_t apply_err =
        lighting_configuration_apply(payload.configuration,
                                     &configuration_hash);
    const esp_err_t event_err =
        flush_lighting_events(context, client);
    if (event_err != ESP_OK) return event_err;
    if (apply_err != ESP_OK) {
      const std::string result = make_command_result(
          context->device_id, decision.command.command_id, "FAILED",
          "CONFIGURATION_APPLY_FAILED", "CONFIGURATION",
          "Validated configuration could not be persisted in safe blackout",
          true);
      context->dedupe.Remember(decision.command.command_id, result);
      return send_text(client, result);
    }

    cJSON *result_payload = cJSON_CreateObject();
    if (result_payload == nullptr) return ESP_ERR_NO_MEM;
    cJSON_AddBoolToObject(result_payload, "configuration_applied", true);
    cJSON_AddStringToObject(result_payload, "safe_state", "BLACKOUT");
    cJSON_AddStringToObject(result_payload, "configuration_hash",
                            configuration_hash.c_str());
    const std::string result = completed_result(
        context->device_id, decision.command.command_id, result_payload);
    if (result.empty()) return ESP_FAIL;
    note_command_applied(context, decision.command.command_id);
    context->dedupe.Remember(decision.command.command_id, result);
    return send_text(client, result);
  }

  if (decision.command.command_type == "LIGHTING_CONFIG_READ") {
    const std::string canonical = lighting_configuration_json();
    if (canonical.empty()) {
      const std::string result = make_command_result(
          context->device_id, decision.command.command_id, "REJECTED",
          "DEVICE_CONFIGURATION_REQUIRED", "CONFIGURATION",
          "No validated lighting configuration is installed", true);
      context->dedupe.Remember(decision.command.command_id, result);
      return send_text(client, result);
    }
    cJSON *configuration =
        cJSON_ParseWithLength(canonical.data(), canonical.size());
    if (configuration == nullptr) return ESP_ERR_INVALID_RESPONSE;
    const std::string result = completed_result(
        context->device_id, decision.command.command_id, configuration);
    if (result.empty()) return ESP_FAIL;
    note_command_applied(context, decision.command.command_id);
    context->dedupe.Remember(decision.command.command_id, result);
    return send_text(client, result);
  }

  if (decision.command.command_type == "LIGHTING_CHANNELS_SET") {
    std::vector<ChannelLevelV1> normalized;
    std::string set_error;
    const esp_err_t set_err =
        lighting_channels_set(payload.channels, &normalized, &set_error);
    const esp_err_t event_err =
        flush_lighting_events(context, client);
    if (event_err != ESP_OK) return event_err;
    if (set_err != ESP_OK) {
      const bool configuration_missing =
          set_err == ESP_ERR_INVALID_STATE;
      const bool output_failed =
          set_err != ESP_ERR_INVALID_STATE &&
          set_err != ESP_ERR_NOT_FOUND;
      const std::string result = make_command_result(
          context->device_id, decision.command.command_id,
          output_failed ? "FAILED" : "REJECTED",
          configuration_missing ? "DEVICE_CONFIGURATION_REQUIRED"
                                : (output_failed ? "DMX_OUTPUT_FAILED"
                                                 : "CHANNEL_LEVEL_INVALID"),
          configuration_missing ? "CONFIGURATION"
                                : (output_failed ? "DEVICE" : "VALIDATION"),
          set_error.c_str(),
          configuration_missing || output_failed);
      context->dedupe.Remember(decision.command.command_id, result);
      return send_text(client, result);
    }

    const std::string payload_json = levels_payload(normalized);
    if (payload_json.empty()) return ESP_FAIL;
    cJSON *result_payload =
        cJSON_ParseWithLength(payload_json.data(), payload_json.size());
    if (result_payload == nullptr) return ESP_ERR_INVALID_RESPONSE;
    const std::string result = completed_result(
        context->device_id, decision.command.command_id, result_payload);
    if (result.empty()) return ESP_FAIL;
    note_command_applied(context, decision.command.command_id);
    context->dedupe.Remember(decision.command.command_id, result);
    return send_text(client, result);
  }

  if (decision.command.command_type == "LIGHTING_CHANNELS_FADE") {
    std::vector<ChannelLevelV1> normalized;
    std::string fade_error;
    const esp_err_t fade_err = lighting_channels_fade(
        decision.command.command_id, payload.channels, payload.fade_ms,
        &normalized, &fade_error);

    if (fade_err != ESP_OK) {
      const esp_err_t event_err =
          flush_lighting_events(context, client);
      if (event_err != ESP_OK) return event_err;
      const bool configuration_missing =
          fade_err == ESP_ERR_INVALID_STATE;
      const bool invalid_channel =
          fade_err == ESP_ERR_NOT_FOUND;
      const std::string result = make_command_result(
          context->device_id, decision.command.command_id,
          (configuration_missing || invalid_channel) ? "REJECTED" : "FAILED",
          configuration_missing ? "DEVICE_CONFIGURATION_REQUIRED"
                                : (invalid_channel ? "CHANNEL_LEVEL_INVALID"
                                                   : "DMX_OUTPUT_FAILED"),
          configuration_missing ? "CONFIGURATION"
                                : (invalid_channel ? "VALIDATION" : "DEVICE"),
          fade_error.c_str(),
          configuration_missing || !invalid_channel);
      context->dedupe.Remember(decision.command.command_id, result);
      return send_text(client, result);
    }

    const std::string accepted = make_command_result(
        context->device_id, decision.command.command_id, "ACCEPTED",
        "", "", "", false);
    note_command_accepted(context, decision.command.command_id);
    context->dedupe.Remember(decision.command.command_id, accepted);
    const esp_err_t accepted_err = send_text(client, accepted);
    if (accepted_err != ESP_OK) return accepted_err;
    return flush_lighting_events(context, client);
  }

  if (decision.command.command_type == "LIGHTING_BLACKOUT") {
    if (payload.fade_ms > 0) {
      std::string fade_error;
      const esp_err_t fade_err = lighting_blackout_fade(
          decision.command.command_id, payload.fade_ms, &fade_error);

      if (fade_err != ESP_OK) {
        const esp_err_t event_err =
            flush_lighting_events(context, client);
        if (event_err != ESP_OK) return event_err;
        const bool configuration_missing =
            fade_err == ESP_ERR_INVALID_STATE;
        const std::string result = make_command_result(
            context->device_id, decision.command.command_id,
            configuration_missing ? "REJECTED" : "FAILED",
            configuration_missing ? "DEVICE_CONFIGURATION_REQUIRED"
                                  : "DMX_OUTPUT_FAILED",
            configuration_missing ? "CONFIGURATION" : "DEVICE",
            fade_error.c_str(), true);
        context->dedupe.Remember(decision.command.command_id, result);
        return send_text(client, result);
      }

      const std::string accepted = make_command_result(
          context->device_id, decision.command.command_id, "ACCEPTED",
          "", "", "", false);
      note_command_accepted(context, decision.command.command_id);
      context->dedupe.Remember(decision.command.command_id, accepted);
      const esp_err_t accepted_err = send_text(client, accepted);
      if (accepted_err != ESP_OK) return accepted_err;
      return flush_lighting_events(context, client);
    }

    const esp_err_t output_err = lighting_blackout(false);
    const esp_err_t event_err =
        flush_lighting_events(context, client);
    if (event_err != ESP_OK) return event_err;
    if (output_err != ESP_OK) {
      const std::string result = make_command_result(
          context->device_id, decision.command.command_id, "FAILED",
          "DMX_OUTPUT_FAILED", "DEVICE",
          "DMX blackout frame was not confirmed on the output task", true);
      context->dedupe.Remember(decision.command.command_id, result);
      return send_text(client, result);
    }

    cJSON *result_payload = cJSON_CreateObject();
    if (result_payload == nullptr) return ESP_ERR_NO_MEM;
    cJSON_AddBoolToObject(result_payload, "blackout", true);
    cJSON_AddNumberToObject(result_payload, "fade_ms", 0);
    const std::string result = completed_result(
        context->device_id, decision.command.command_id, result_payload);
    if (result.empty()) return ESP_FAIL;
    note_command_applied(context, decision.command.command_id);
    context->dedupe.Remember(decision.command.command_id, result);
    return send_text(client, result);
  }

  if (decision.command.command_type == "LIGHTING_IDENTIFY") {
    std::string identify_error;
    const esp_err_t identify_err = lighting_identify(
        decision.command.command_id, payload.channel_key, payload.level,
        payload.duration_ms, &identify_error);

    if (identify_err != ESP_OK) {
      const esp_err_t event_err =
          flush_lighting_events(context, client);
      if (event_err != ESP_OK) return event_err;
      const bool configuration_missing =
          identify_err == ESP_ERR_INVALID_STATE;
      const bool invalid_channel =
          identify_err == ESP_ERR_NOT_FOUND;
      const std::string result = make_command_result(
          context->device_id, decision.command.command_id,
          (configuration_missing || invalid_channel) ? "REJECTED" : "FAILED",
          configuration_missing ? "DEVICE_CONFIGURATION_REQUIRED"
                                : (invalid_channel ? "CHANNEL_INVALID"
                                                   : "DMX_OUTPUT_FAILED"),
          configuration_missing ? "CONFIGURATION"
                                : (invalid_channel ? "VALIDATION" : "DEVICE"),
          identify_error.c_str(),
          configuration_missing || !invalid_channel);
      context->dedupe.Remember(decision.command.command_id, result);
      return send_text(client, result);
    }

    const std::string accepted = make_command_result(
        context->device_id, decision.command.command_id, "ACCEPTED",
        "", "", "", false);
    note_command_accepted(context, decision.command.command_id);
    context->dedupe.Remember(decision.command.command_id, accepted);
    const esp_err_t accepted_err = send_text(client, accepted);
    if (accepted_err != ESP_OK) return accepted_err;
    return flush_lighting_events(context, client);
  }

  if (decision.command.command_type == "LIGHTING_STATE_READ") {
    note_command_applied(context, decision.command.command_id);
    cJSON *state = observed_state_json(context);
    if (state == nullptr) return ESP_ERR_NO_MEM;
    const std::string result = completed_result(
        context->device_id, decision.command.command_id, state);
    if (result.empty()) return ESP_FAIL;
    context->dedupe.Remember(decision.command.command_id, result);
    return send_text(client, result);
  }

  const std::string result = make_command_result(
      context->device_id, decision.command.command_id, "REJECTED",
      "DEVICE_COMMAND_NOT_IMPLEMENTED", "CAPABILITY",
      "Lighting command payload is valid but execution is not enabled yet",
      false);
  context->dedupe.Remember(decision.command.command_id, result);
  return send_text(client, result);
}

}  // namespace

esp_err_t run_stage_device_runtime(const VerifiedHub &hub,
                                   const RuntimeCredential &credential,
                                   const DeviceIdentity &identity,
                                   const DeviceConfig &config) {
  if (hub.certificate_der.empty() || hub.address.empty() || hub.port == 0 ||
      credential.token.empty() || identity.device_id().empty() ||
      !config.complete()) {
    return ESP_ERR_INVALID_ARG;
  }

  RuntimeContext context;
  context.events = xEventGroupCreate();
  context.command_lock = xSemaphoreCreateMutex();
  context.device_id = identity.device_id();
  context.project_id = config.project_id;
  if (context.events == nullptr || context.command_lock == nullptr) {
    if (context.command_lock != nullptr) vSemaphoreDelete(context.command_lock);
    if (context.events != nullptr) vEventGroupDelete(context.events);
    return ESP_ERR_NO_MEM;
  }

  char uri[192];
  std::snprintf(uri, sizeof(uri),
                "wss://%s:%u/api/v1/stage-devices/runtime",
                hub.address.c_str(), hub.port);
  const std::string headers =
      "Authorization: StageCoreSession " + credential.token + "\r\n";

  esp_websocket_client_config_t ws_config = {};
  ws_config.uri = uri;
  ws_config.disable_auto_reconnect = true;
  ws_config.user_context = &context;
  ws_config.buffer_size = 4096;
  ws_config.cert_pem =
      reinterpret_cast<const char *>(hub.certificate_der.data());
  ws_config.cert_len = hub.certificate_der.size();
  ws_config.subprotocol = kProtocolVersion;
  ws_config.headers = headers.c_str();
  ws_config.skip_cert_common_name_check = true;
  ws_config.network_timeout_ms = 5000;
  ws_config.ping_interval_sec = 10;
  ws_config.pingpong_timeout_sec = 20;
  ws_config.keep_alive_enable = true;
  ws_config.keep_alive_idle = 10;
  ws_config.keep_alive_interval = 5;
  ws_config.keep_alive_count = 3;

  esp_websocket_client_handle_t client =
      esp_websocket_client_init(&ws_config);
  if (client == nullptr) {
    vSemaphoreDelete(context.command_lock);
    vEventGroupDelete(context.events);
    return ESP_ERR_NO_MEM;
  }

  esp_err_t err = esp_websocket_register_events(
      client, WEBSOCKET_EVENT_ANY, &runtime_event_handler, &context);
  if (err != ESP_OK) {
    esp_websocket_client_destroy(client);
    vSemaphoreDelete(context.command_lock);
    vEventGroupDelete(context.events);
    return err;
  }

  err = esp_websocket_client_start(client);
  if (err != ESP_OK) {
    esp_websocket_client_destroy(client);
    vSemaphoreDelete(context.command_lock);
    vEventGroupDelete(context.events);
    return err;
  }

  int64_t last_heartbeat_us = 0;

  EventBits_t bits = xEventGroupWaitBits(
      context.events,
      kConnectedBit | kDisconnectedBit | kProtocolErrorBit,
      pdFALSE, pdFALSE, pdMS_TO_TICKS(kReadyTimeoutMS));
  if ((bits & kConnectedBit) == 0) {
    err = (bits & kProtocolErrorBit) ? ESP_ERR_INVALID_RESPONSE
                                     : ESP_ERR_TIMEOUT;
    goto cleanup;
  }

  {
    const std::string hello = make_hello(hub, identity, config);
    err = send_text(client, hello);
    if (err != ESP_OK) goto cleanup;
  }

  bits = xEventGroupWaitBits(
      context.events,
      kReadyBit | kDisconnectedBit | kProtocolErrorBit,
      pdFALSE, pdFALSE, pdMS_TO_TICKS(kReadyTimeoutMS));
  if ((bits & kReadyBit) == 0) {
    err = (bits & kProtocolErrorBit) ? ESP_ERR_INVALID_RESPONSE
                                     : ESP_ERR_TIMEOUT;
    goto cleanup;
  }

#if STAGECORE_EXPERIMENTAL_DEVICE_V2
  ESP_LOGW(kTag, "v2 Hub assignment received; project commands disabled");
  // Neither a Hub assignment nor this software-zero report activates DMX
  // channels. Always cancel fades/events and write all 12 physical slots to
  // zero before persisting the Hub epoch or acknowledging it.
  err = lighting_blackout(true);
  if (err == ESP_OK) err = lighting_output_blackout_immediate();
  if (err != ESP_OK || !lighting_output_dmx_healthy()) {
    if (err == ESP_OK) err = ESP_ERR_INVALID_STATE;
    goto cleanup;
  }
  err = assignment_v2::confirm_zero_and_persist_epoch(
      static_cast<uint64_t>(context.assignment_epoch.load()),
      context.blocked_epoch ? assignment_v2::PersistedState::kBlocked
                            : assignment_v2::PersistedState::kUnassigned,
      context.project_id);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "Hub v2 epoch rolled back, changed Project or NVS failed; keep blackout");
    goto cleanup;
  }
  if (context.blocked_epoch) {
    err = send_text(client, make_blocked_epoch_ack(context));
    if (err != ESP_OK) goto cleanup;
    // Receipt only confirms that Hub persisted our software-zero report.
    // It does not authorize a snapshot, lighting commands or ACTIVE state.
    bits = xEventGroupWaitBits(
        context.events,
        kEpochReceiptBit | kDisconnectedBit | kProtocolErrorBit,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(kReadyTimeoutMS));
    if ((bits & kEpochReceiptBit) == 0) {
      err = (bits & kProtocolErrorBit) ? ESP_ERR_INVALID_RESPONSE
                                       : ESP_ERR_TIMEOUT;
      goto cleanup;
    }
    ESP_LOGI(kTag, "Hub persisted software-zero ACK for BLOCKED epoch");
  }
#else
  ESP_LOGI(kTag, "Stage Device runtime.ready accepted");
  lighting_runtime_authority_acquired();
#endif

  {
    const std::string observation =
        make_observation(hub, identity, &context);
    err = send_text(client, observation);
    if (err != ESP_OK) goto cleanup;
  }

#if STAGECORE_EXPERIMENTAL_DEVICE_V2
  ESP_LOGW(kTag, "v2 output remains FAILSAFE; only assignment.blackout accepted");
#else
  ESP_LOGI(kTag,
           "all seven lighting capabilities enabled; readiness=%s",
           runtime_readiness());
#endif

  last_heartbeat_us = esp_timer_get_time();
  while (true) {
    bits = xEventGroupWaitBits(
        context.events,
#if STAGECORE_EXPERIMENTAL_DEVICE_V2
        kDisconnectedBit | kProtocolErrorBit | kBlackoutBit
#if STAGECORE_EXPERIMENTAL_V2_STATE_PROBE
        | kProbeBit
#endif
        ,
#else
        kDisconnectedBit | kProtocolErrorBit | kCommandBit,
#endif
        pdFALSE, pdFALSE, pdMS_TO_TICKS(50));
    if (bits & kProtocolErrorBit) {
      err = ESP_ERR_INVALID_RESPONSE;
      break;
    }
    if (bits & kDisconnectedBit) {
      err = ESP_ERR_INVALID_STATE;
      break;
    }
    if (!esp_websocket_client_is_connected(client)) {
      err = ESP_ERR_INVALID_STATE;
      break;
    }
#if STAGECORE_EXPERIMENTAL_DEVICE_V2
    if (bits & kBlackoutBit) {
      err = process_pending_blackout(&context, client);
      if (err != ESP_OK) break;
    }
#if STAGECORE_EXPERIMENTAL_V2_STATE_PROBE
    if (bits & kProbeBit) {
      err = process_pending_probe(&context, client);
      if (err != ESP_OK) break;
    }
#endif
#else
    if (bits & kCommandBit) {
      err = process_pending_command(&context, client);
      if (err != ESP_OK) break;
    }

    err = flush_lighting_events(&context, client);
    if (err != ESP_OK) break;
#endif

    const int64_t now_us = esp_timer_get_time();
    if (now_us - last_heartbeat_us >=
        static_cast<int64_t>(kHeartbeatMS) * 1000LL) {
      const std::string observation =
          make_observation(hub, identity, &context);
      err = send_text(client, observation);
      if (err != ESP_OK) break;
      last_heartbeat_us = now_us;
    }
  }

cleanup:
  if (esp_websocket_client_is_connected(client)) {
    (void)esp_websocket_client_stop(client);
  }
  esp_websocket_client_destroy(client);
  vSemaphoreDelete(context.command_lock);
  vEventGroupDelete(context.events);
  return err;
}

}  // namespace stagecore
