#include "stage_device_runtime.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "cJSON.h"
#include "command_contract.h"
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
  // Slice 1 intentionally advertises no executable runtime capabilities.
  // Firmware Slice 2 will advertise the seven lighting capabilities only
  // after their command handlers, deadlines, dedupe, fades and failsafe exist.
  return cJSON_CreateArray();
}

cJSON *observed_state_json() {
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
  cJSON_AddItemToObject(state, "current_levels", levels);
  cJSON_AddBoolToObject(state, "dmx_healthy", true);
  cJSON_AddBoolToObject(state, "brownout_warning", false);
  cJSON_AddStringToObject(state, "authority", "FAILSAFE");
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
  cJSON_AddStringToObject(root, "readiness", "BLOCKER");
  cJSON_AddItemToObject(root, "observed_state", observed);
  cJSON_AddItemToObject(root, "network_state", network);

  const std::string result = print_json(root);
  cJSON_Delete(root);
  return result;
}

std::string make_observation(const VerifiedHub &hub,
                             const DeviceIdentity &identity) {
  cJSON *root = cJSON_CreateObject();
  if (root == nullptr) return {};

  cJSON_AddStringToObject(root, "type", "device.observation");
  cJSON_AddNumberToObject(root, "schema_version", 1);
  cJSON_AddStringToObject(root, "device_id", identity.device_id().c_str());
  cJSON_AddStringToObject(root, "readiness", "BLOCKER");

  cJSON *observed = observed_state_json();
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

  // The envelope is canonical and timely, but this bounded sub-slice has not
  // enabled payload execution yet. Return an explicit terminal rejection and
  // remember it so the same command_id can never execute on retry.
  const std::string result = make_command_result(
      context->device_id, decision.command.command_id, "REJECTED",
      "DEVICE_COMMAND_NOT_IMPLEMENTED", "CAPABILITY",
      "Lighting payload execution is not enabled in this firmware slice",
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
  ESP_LOGW(kTag,
           "Slice 2 validation active; lighting payload execution disabled");

  while (true) {
    bits = xEventGroupWaitBits(
        context.events,
        kDisconnectedBit | kProtocolErrorBit | kCommandBit,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(kHeartbeatMS));
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
      continue;
    }

    const std::string observation = make_observation(hub, identity);
    err = send_text(client, observation);
    if (err != ESP_OK) break;
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
