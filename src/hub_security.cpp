#include "hub_security.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

#ifndef STAGECORE_FW_VERSION
#define STAGECORE_FW_VERSION "0.2.0-dev"
#endif

namespace stagecore {
namespace {

constexpr char kTag[] = "stagecore-auth";
constexpr int kHTTPTimeoutMS = 5000;
constexpr int kPairingPollMS = 1000;
constexpr int kPairingPollLimit = 300;

struct HttpResponse {
  int status = 0;
  std::string body;
};

esp_err_t http_event(esp_http_client_event_t *event) {
  if (event == nullptr || event->user_data == nullptr) return ESP_OK;
  if (event->event_id == HTTP_EVENT_ON_DATA && event->data != nullptr &&
      event->data_len > 0) {
    auto *response = static_cast<HttpResponse *>(event->user_data);
    if (response->body.size() + static_cast<size_t>(event->data_len) > 16384) {
      return ESP_ERR_NO_MEM;
    }
    response->body.append(static_cast<const char *>(event->data),
                          static_cast<size_t>(event->data_len));
  }
  return ESP_OK;
}

std::string endpoint_url(const VerifiedHub &hub, const char *path) {
  char url[192];
  std::snprintf(url, sizeof(url), "https://%s:%u%s",
                hub.address.c_str(), hub.port, path);
  return url;
}

esp_err_t post_json(const VerifiedHub &hub, const char *path,
                    const std::string &request_body, HttpResponse *response) {
  if (response == nullptr || hub.certificate_der.empty()) {
    return ESP_ERR_INVALID_ARG;
  }

  response->status = 0;
  response->body.clear();
  const std::string url = endpoint_url(hub, path);

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = HTTP_METHOD_POST;
  config.timeout_ms = kHTTPTimeoutMS;
  config.cert_der =
      reinterpret_cast<const char *>(hub.certificate_der.data());
  config.cert_len = hub.certificate_der.size();
  config.skip_cert_common_name_check = true;
  config.tls_version = ESP_HTTP_CLIENT_TLS_VER_TLS_1_3;
  config.event_handler = http_event;
  config.user_data = response;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) return ESP_ERR_NO_MEM;

  esp_err_t err =
      esp_http_client_set_header(client, "Content-Type", "application/json");
  if (err == ESP_OK) {
    err = esp_http_client_set_post_field(
        client, request_body.data(), static_cast<int>(request_body.size()));
  }
  if (err == ESP_OK) err = esp_http_client_perform(client);
  if (err == ESP_OK) response->status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  return err;
}

std::string json_string(const cJSON *root, const char *key) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
  if (!cJSON_IsString(item) || item->valuestring == nullptr) return {};
  return item->valuestring;
}

std::string error_code(const HttpResponse &response) {
  cJSON *root = cJSON_ParseWithLength(response.body.data(), response.body.size());
  if (root == nullptr) return {};
  const std::string code = json_string(root, "error_code");
  cJSON_Delete(root);
  return code;
}

std::string make_hostname(const std::string &device_id) {
  std::string compact;
  for (char ch : device_id) {
    if (ch != '-') compact.push_back(ch);
  }
  if (compact.size() > 8) compact = compact.substr(compact.size() - 8);
  return "stagecore-light-" + compact;
}

std::string random_nonce_base64() {
  std::array<unsigned char, 32> nonce{};
  esp_fill_random(nonce.data(), nonce.size());

  size_t encoded_length = 0;
  int rc = mbedtls_base64_encode(nullptr, 0, &encoded_length,
                                 nonce.data(), nonce.size());
  if (rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) return {};

  std::vector<unsigned char> encoded(encoded_length + 1, 0);
  rc = mbedtls_base64_encode(encoded.data(), encoded.size(), &encoded_length,
                             nonce.data(), nonce.size());
  if (rc != 0) return {};
  return std::string(reinterpret_cast<const char *>(encoded.data()),
                     encoded_length);
}

cJSON *capabilities_json() {
  cJSON *array = cJSON_CreateArray();
  if (array == nullptr) return nullptr;
  const char *caps[] = {
      "lighting.channels.set",
      "lighting.channels.fade",
      "lighting.blackout",
      "lighting.state.read",
      "lighting.identify",
      "lighting.config.read",
      "lighting.config.apply",
  };
  for (const char *cap : caps) {
    cJSON *item = cJSON_CreateString(cap);
    if (item == nullptr || !cJSON_AddItemToArray(array, item)) {
      if (item != nullptr) cJSON_Delete(item);
      cJSON_Delete(array);
      return nullptr;
    }
  }
  return array;
}

std::string print_json(cJSON *root) {
  if (root == nullptr) return {};
  char *text = cJSON_PrintUnformatted(root);
  std::string out = text != nullptr ? text : "";
  if (text != nullptr) cJSON_free(text);
  return out;
}

esp_err_t request_pairing(const VerifiedHub &hub, DeviceIdentity *identity,
                          const std::string &display_name,
                          std::string *request_id,
                          std::string *pairing_code) {
  const std::string nonce = random_nonce_base64();
  if (nonce.empty()) return ESP_FAIL;

  cJSON *root = cJSON_CreateObject();
  if (root == nullptr) return ESP_ERR_NO_MEM;

  cJSON_AddStringToObject(root, "companion_id", identity->device_id().c_str());
  cJSON_AddStringToObject(root, "display_name", display_name.c_str());
  const std::string hostname = make_hostname(identity->device_id());
  cJSON_AddStringToObject(root, "hostname", hostname.c_str());
  cJSON_AddStringToObject(root, "platform", "esp32");
  cJSON_AddStringToObject(root, "architecture", "xtensa");
  cJSON_AddStringToObject(root, "version", STAGECORE_FW_VERSION);
  cJSON *caps = capabilities_json();
  if (caps == nullptr) {
    cJSON_Delete(root);
    return ESP_ERR_NO_MEM;
  }
  cJSON_AddItemToObject(root, "capabilities", caps);
  cJSON_AddStringToObject(root, "public_key_algorithm", "P256_X963_SHA256");
  cJSON_AddStringToObject(root, "public_key_base64",
                          identity->public_key_base64().c_str());
  cJSON_AddStringToObject(root, "client_nonce_base64", nonce.c_str());

  const std::string body = print_json(root);
  cJSON_Delete(root);
  if (body.empty()) return ESP_FAIL;

  HttpResponse response;
  esp_err_t err = post_json(
      hub, "/api/v1/companion/pairing/requests", body, &response);
  if (err != ESP_OK) return err;
  if (response.status != 202) {
    ESP_LOGE(kTag, "pairing request rejected: HTTP %d code=%s",
             response.status, error_code(response).c_str());
    return ESP_FAIL;
  }

  cJSON *reply =
      cJSON_ParseWithLength(response.body.data(), response.body.size());
  if (reply == nullptr) return ESP_ERR_INVALID_RESPONSE;
  *request_id = json_string(reply, "request_id");
  *pairing_code = json_string(reply, "pairing_code");
  cJSON_Delete(reply);

  if (request_id->empty() || pairing_code->empty()) {
    return ESP_ERR_INVALID_RESPONSE;
  }
  return ESP_OK;
}

esp_err_t wait_for_pairing(const VerifiedHub &hub,
                           const std::string &request_id,
                           const std::string &pairing_code) {
  for (int attempt = 0; attempt < kPairingPollLimit; ++attempt) {
    cJSON *root = cJSON_CreateObject();
    if (root == nullptr) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(root, "request_id", request_id.c_str());
    cJSON_AddStringToObject(root, "pairing_code", pairing_code.c_str());
    const std::string body = print_json(root);
    cJSON_Delete(root);
    if (body.empty()) return ESP_FAIL;

    HttpResponse response;
    esp_err_t err = post_json(
        hub, "/api/v1/companion/pairing/status", body, &response);
    if (err != ESP_OK) return err;
    if (response.status != 200) {
      ESP_LOGE(kTag, "pairing status failed: HTTP %d code=%s",
               response.status, error_code(response).c_str());
      return ESP_FAIL;
    }

    cJSON *reply =
        cJSON_ParseWithLength(response.body.data(), response.body.size());
    if (reply == nullptr) return ESP_ERR_INVALID_RESPONSE;
    const std::string status = json_string(reply, "status");
    cJSON_Delete(reply);

    if (status == "APPROVED") return ESP_OK;
    if (status == "REJECTED" || status == "EXPIRED") {
      ESP_LOGE(kTag, "pairing ended with status %s", status.c_str());
      return ESP_FAIL;
    }
    if (status != "PENDING") return ESP_ERR_INVALID_RESPONSE;
    vTaskDelay(pdMS_TO_TICKS(kPairingPollMS));
  }
  return ESP_ERR_TIMEOUT;
}

esp_err_t begin_authentication(const VerifiedHub &hub,
                               const std::string &device_id,
                               std::string *challenge_id,
                               std::string *nonce_base64,
                               std::string *server_error) {
  cJSON *root = cJSON_CreateObject();
  if (root == nullptr) return ESP_ERR_NO_MEM;
  cJSON_AddStringToObject(root, "companion_id", device_id.c_str());
  const std::string body = print_json(root);
  cJSON_Delete(root);
  if (body.empty()) return ESP_FAIL;

  HttpResponse response;
  esp_err_t err = post_json(
      hub, "/api/v1/companion/auth/challenges", body, &response);
  if (err != ESP_OK) return err;

  if (response.status != 200) {
    if (server_error != nullptr) *server_error = error_code(response);
    return ESP_ERR_INVALID_STATE;
  }

  cJSON *reply =
      cJSON_ParseWithLength(response.body.data(), response.body.size());
  if (reply == nullptr) return ESP_ERR_INVALID_RESPONSE;
  *challenge_id = json_string(reply, "challenge_id");
  *nonce_base64 = json_string(reply, "nonce_base64");
  cJSON_Delete(reply);

  if (challenge_id->empty() || nonce_base64->empty()) {
    return ESP_ERR_INVALID_RESPONSE;
  }
  return ESP_OK;
}

esp_err_t complete_authentication(const VerifiedHub &hub,
                                  DeviceIdentity *identity,
                                  const std::string &challenge_id,
                                  const std::string &nonce_base64,
                                  RuntimeCredential *credential) {
  std::string signature;
  esp_err_t err = identity->SignAuthenticationMessage(
      challenge_id, nonce_base64, &signature);
  if (err != ESP_OK) return err;

  cJSON *root = cJSON_CreateObject();
  if (root == nullptr) return ESP_ERR_NO_MEM;
  cJSON_AddStringToObject(root, "companion_id",
                          identity->device_id().c_str());
  cJSON_AddStringToObject(root, "challenge_id", challenge_id.c_str());
  cJSON_AddStringToObject(root, "signature_base64", signature.c_str());
  const std::string body = print_json(root);
  cJSON_Delete(root);
  if (body.empty()) return ESP_FAIL;

  HttpResponse response;
  err = post_json(hub, "/api/v1/companion/auth/sessions", body, &response);
  if (err != ESP_OK) return err;
  if (response.status != 201) {
    ESP_LOGE(kTag, "authentication proof rejected: HTTP %d code=%s",
             response.status, error_code(response).c_str());
    return ESP_FAIL;
  }

  cJSON *reply =
      cJSON_ParseWithLength(response.body.data(), response.body.size());
  if (reply == nullptr) return ESP_ERR_INVALID_RESPONSE;
  credential->session_id = json_string(reply, "session_id");
  credential->token = json_string(reply, "session_token");
  cJSON_Delete(reply);

  if (credential->session_id.empty() || credential->token.empty()) {
    return ESP_ERR_INVALID_RESPONSE;
  }
  return ESP_OK;
}

esp_err_t authenticate_once(const VerifiedHub &hub, DeviceIdentity *identity,
                            RuntimeCredential *credential,
                            std::string *server_error) {
  std::string challenge_id;
  std::string nonce_base64;
  esp_err_t err = begin_authentication(
      hub, identity->device_id(), &challenge_id, &nonce_base64, server_error);
  if (err != ESP_OK) return err;
  return complete_authentication(
      hub, identity, challenge_id, nonce_base64, credential);
}

}  // namespace

esp_err_t ensure_paired_and_authenticate(const VerifiedHub &hub,
                                         DeviceIdentity *identity,
                                         const std::string &display_name,
                                         RuntimeCredential *credential) {
  if (identity == nullptr || credential == nullptr ||
      display_name.empty() || hub.certificate_der.empty()) {
    return ESP_ERR_INVALID_ARG;
  }

  std::string server_error;
  esp_err_t err = authenticate_once(
      hub, identity, credential, &server_error);
  if (err == ESP_OK) {
    ESP_LOGI(kTag, "authenticated with StageCore Hub");
    return ESP_OK;
  }

  if (server_error != "COMPANION_UNPAIRED") {
    ESP_LOGE(kTag, "authentication failed before pairing: %s",
             server_error.c_str());
    return err;
  }

  std::string request_id;
  std::string pairing_code;
  err = request_pairing(
      hub, identity, display_name, &request_id, &pairing_code);
  if (err != ESP_OK) return err;

  ESP_LOGW(kTag, "PAIRING APPROVAL REQUIRED");
  ESP_LOGW(kTag, "StageCore pairing code: %s", pairing_code.c_str());
  ESP_LOGW(kTag, "approve this device in the StageCore Operator");

  err = wait_for_pairing(hub, request_id, pairing_code);
  if (err != ESP_OK) return err;

  server_error.clear();
  credential->session_id.clear();
  credential->token.clear();
  err = authenticate_once(hub, identity, credential, &server_error);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "post-pair authentication failed: %s",
             server_error.c_str());
    return err;
  }

  ESP_LOGI(kTag, "pairing approved and runtime session authenticated");
  return ESP_OK;
}

}  // namespace stagecore
