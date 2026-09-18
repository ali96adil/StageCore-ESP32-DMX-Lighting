#include "stage_device_runtime.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "cJSON.h"
#include "command_contract.h"
#include "lighting_contract.h"
#include "lighting_configuration.h"
#include "lighting_output.h"
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

namespace stagecore {
namespace {

constexpr char kTag[] = "stagecore-runtime";
constexpr char kProtocolVersion[] = "stagecore.device/1";
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
  static constexpr const char *kCapabilities[] = {
      "lighting.channels.set",
      "lighting.channels.fade",
      "lighting.blackout",
      "lighting.identify",
      "lighting.state.read",
      "lighting.config.read",
      "lighting.config.apply",
  };
  for (const char *capability : kCapabilities) {
    cJSON *item = cJSON_CreateString(capability);
    if (item == nullptr || !cJSON_AddItemToArray(array, item)) {
      if (item != nullptr) cJSON_Delete(item);
      cJSON_Delete(array);
      return nullptr;
    }
  }
  return array;
}

const char *runtime_readiness() {
  if (!lighting_configuration_ready() ||
      !lighting_output_dmx_healthy() ||
      lighting_authority() == "FAILSAFE") {
    return "BLOCKER";
  }
  if (esp_reset_reason() == ESP_RST_BROWNOUT) {
    return "WARNING";
  }
  return "READY";
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
  for (const auto &entry : lighting_current_levels()) {
    cJSON_AddNumberToObject(levels, entry.channel_key.c_str(), entry.level);
  }
  cJSON_AddItemToObject(state, "current_levels", levels);
  cJSON_AddBoolToObject(state, "dmx_healthy",
                        lighting_output_dmx_healthy());
  const std::string configuration_hash = lighting_configuration_hash();
  if (!configuration_hash.empty()) {
    cJSON_AddStringToObject(state, "configuration_hash",
                            configuration_hash.c_str());
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
  cJSON_AddStringToObject(root, "project_id", config.project_id.c_str());
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
  cJSON_AddNumberToObject(root, "schema_version", 1);
  cJSON_AddStringToObject(root, "device_id", identity.device_id().c_str());
  cJSON_AddStringToObject(root, "readiness", runtime_readiness());

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

bool handle_complete_text(RuntimeContext *context, const std::string &text) {
  cJSON *root = cJSON_ParseWithLength(text.data(), text.size());
  if (root == nullptr) return false;

  const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
  const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
  const cJSON *device = cJSON_GetObjectItemCaseSensitive(root, "device_id");

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
      event.message.c_str(), false);
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
    const esp_err_t event_err =
        flush_lighting_events(context, client);
    if (event_err != ESP_OK) return event_err;

    if (fade_err != ESP_OK) {
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
    return send_text(client, accepted);
  }

  if (decision.command.command_type == "LIGHTING_BLACKOUT") {
    if (payload.fade_ms > 0) {
      std::string fade_error;
      const esp_err_t fade_err = lighting_blackout_fade(
          decision.command.command_id, payload.fade_ms, &fade_error);
      const esp_err_t event_err =
          flush_lighting_events(context, client);
      if (event_err != ESP_OK) return event_err;

      if (fade_err != ESP_OK) {
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
      return send_text(client, accepted);
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
    const esp_err_t event_err =
        flush_lighting_events(context, client);
    if (event_err != ESP_OK) return event_err;

    if (identify_err != ESP_OK) {
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
    return send_text(client, accepted);
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

  ESP_LOGI(kTag, "Stage Device runtime.ready accepted");
  lighting_runtime_authority_acquired();

  {
    const std::string observation =
        make_observation(hub, identity, &context);
    err = send_text(client, observation);
    if (err != ESP_OK) goto cleanup;
  }

  ESP_LOGI(kTag,
           "all seven lighting capabilities enabled; readiness=%s",
           runtime_readiness());

  last_heartbeat_us = esp_timer_get_time();
  while (true) {
    bits = xEventGroupWaitBits(
        context.events,
        kDisconnectedBit | kProtocolErrorBit | kCommandBit,
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
    if (bits & kCommandBit) {
      err = process_pending_command(&context, client);
      if (err != ESP_OK) break;
    }

    err = flush_lighting_events(&context, client);
    if (err != ESP_OK) break;

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
