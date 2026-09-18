#include "device_identity.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/base64.h"
#include "mbedtls/ecp.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

namespace stagecore {
namespace {

constexpr char kTag[] = "stagecore-id";
constexpr char kNamespace[] = "stagecore_id";
constexpr char kDeviceIDKey[] = "device_id";
constexpr char kPrivateKeyDERKey[] = "ec_priv_der";
constexpr size_t kKeyDERBufferSize = 256;

int random_bytes(void *, unsigned char *output, size_t length) {
  esp_fill_random(output, length);
  return 0;
}

std::string make_uuid_v4() {
  std::array<uint8_t, 16> bytes{};
  esp_fill_random(bytes.data(), bytes.size());
  bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0fU) | 0x40U);
  bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3fU) | 0x80U);

  char text[37];
  std::snprintf(
      text, sizeof(text),
      "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
      bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6],
      bytes[7], bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13],
      bytes[14], bytes[15]);
  return text;
}

esp_err_t load_nvs_string(nvs_handle_t handle, const char *key, std::string *value) {
  size_t length = 0;
  esp_err_t err = nvs_get_str(handle, key, nullptr, &length);
  if (err != ESP_OK) return err;
  std::vector<char> buffer(length);
  err = nvs_get_str(handle, key, buffer.data(), &length);
  if (err == ESP_OK) *value = buffer.data();
  return err;
}

}  // namespace

DeviceIdentity::DeviceIdentity() : pk_context_(new mbedtls_pk_context{}) {
  mbedtls_pk_init(static_cast<mbedtls_pk_context *>(pk_context_));
}

DeviceIdentity::~DeviceIdentity() {
  auto *pk = static_cast<mbedtls_pk_context *>(pk_context_);
  if (pk != nullptr) {
    mbedtls_pk_free(pk);
    delete pk;
    pk_context_ = nullptr;
  }
}

esp_err_t DeviceIdentity::LoadOrCreate() {
  esp_err_t err = LoadOrCreateDeviceID();
  if (err != ESP_OK) return err;
  err = LoadOrCreateKey();
  if (err != ESP_OK) return err;
  return RefreshPublicKey();
}

esp_err_t DeviceIdentity::LoadOrCreateDeviceID() {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
  if (err != ESP_OK) return err;

  err = load_nvs_string(handle, kDeviceIDKey, &device_id_);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    device_id_ = make_uuid_v4();
    err = nvs_set_str(handle, kDeviceIDKey, device_id_.c_str());
    if (err == ESP_OK) err = nvs_commit(handle);
    if (err == ESP_OK) {
      ESP_LOGI(kTag, "created persistent device identity %s", device_id_.c_str());
    }
  }
  nvs_close(handle);
  return err;
}

esp_err_t DeviceIdentity::LoadOrCreateKey() {
  auto *pk = static_cast<mbedtls_pk_context *>(pk_context_);
  nvs_handle_t handle;
  esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
  if (err != ESP_OK) return err;

  size_t der_length = 0;
  err = nvs_get_blob(handle, kPrivateKeyDERKey, nullptr, &der_length);
  if (err == ESP_OK && der_length > 0) {
    std::vector<unsigned char> der(der_length);
    err = nvs_get_blob(handle, kPrivateKeyDERKey, der.data(), &der_length);
    if (err == ESP_OK) {
      const int parse = mbedtls_pk_parse_key(pk, der.data(), der.size(), nullptr, 0,
                                             random_bytes, nullptr);
      if (parse != 0) {
        ESP_LOGE(kTag, "stored P-256 key parse failed: -0x%04x", -parse);
        err = ESP_ERR_INVALID_STATE;
      }
    }
    nvs_close(handle);
    return err;
  }

  if (err != ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    return err;
  }

  const mbedtls_pk_info_t *info = mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY);
  if (info == nullptr || mbedtls_pk_setup(pk, info) != 0) {
    nvs_close(handle);
    return ESP_FAIL;
  }
  const int generated = mbedtls_ecp_gen_key(
      MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(*pk), random_bytes, nullptr);
  if (generated != 0) {
    ESP_LOGE(kTag, "P-256 generation failed: -0x%04x", -generated);
    nvs_close(handle);
    return ESP_FAIL;
  }

  std::array<unsigned char, kKeyDERBufferSize> der{};
  const int written = mbedtls_pk_write_key_der(pk, der.data(), der.size());
  if (written <= 0) {
    ESP_LOGE(kTag, "P-256 DER serialization failed: %d", written);
    nvs_close(handle);
    return ESP_FAIL;
  }
  const size_t offset = der.size() - static_cast<size_t>(written);
  err = nvs_set_blob(handle, kPrivateKeyDERKey, der.data() + offset,
                     static_cast<size_t>(written));
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);

  if (err == ESP_OK) ESP_LOGI(kTag, "created persistent P-256 identity key");
  return err;
}

esp_err_t DeviceIdentity::RefreshPublicKey() {
  auto *pk = static_cast<mbedtls_pk_context *>(pk_context_);
  auto *ec = mbedtls_pk_ec(*pk);
  if (ec == nullptr) return ESP_ERR_INVALID_STATE;

  std::array<unsigned char, MBEDTLS_ECP_MAX_PT_LEN> point{};
  size_t point_length = 0;
  const int encoded = mbedtls_ecp_point_write_binary(
      &ec->MBEDTLS_PRIVATE(grp), &ec->MBEDTLS_PRIVATE(Q),
      MBEDTLS_ECP_PF_UNCOMPRESSED, &point_length, point.data(), point.size());
  if (encoded != 0 || point_length != 65 || point[0] != 0x04) {
    ESP_LOGE(kTag, "P-256 X9.63 encoding failed");
    return ESP_FAIL;
  }

  size_t base64_length = 0;
  int rc = mbedtls_base64_encode(nullptr, 0, &base64_length, point.data(),
                                 point_length);
  if (rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) return ESP_FAIL;

  std::vector<unsigned char> base64(base64_length + 1, 0);
  rc = mbedtls_base64_encode(base64.data(), base64.size(), &base64_length,
                             point.data(), point_length);
  if (rc != 0) return ESP_FAIL;

  public_key_base64_.assign(reinterpret_cast<const char *>(base64.data()),
                            base64_length);
  return ESP_OK;
}

esp_err_t DeviceIdentity::SignAuthenticationMessage(
    const std::string &challenge_id, const std::string &nonce_base64,
    std::string *signature_base64) {
  if (signature_base64 == nullptr || device_id_.empty() ||
      challenge_id.empty() || nonce_base64.empty()) {
    return ESP_ERR_INVALID_ARG;
  }

  const std::string message =
      "StageCore Companion Authentication v1\n" + device_id_ + "\n" +
      challenge_id + "\n" + nonce_base64;

  std::array<unsigned char, 32> digest{};
  if (mbedtls_sha256_ret(
          reinterpret_cast<const unsigned char *>(message.data()),
          message.size(), digest.data(), 0) != 0) {
    return ESP_FAIL;
  }

  std::array<unsigned char, MBEDTLS_PK_SIGNATURE_MAX_SIZE> signature{};
  size_t signature_length = 0;
  auto *pk = static_cast<mbedtls_pk_context *>(pk_context_);
  const int signed_rc = mbedtls_pk_sign(
      pk, MBEDTLS_MD_SHA256, digest.data(), digest.size(), signature.data(),
      signature.size(), &signature_length, random_bytes, nullptr);
  if (signed_rc != 0 || signature_length == 0) {
    ESP_LOGE(kTag, "P-256 auth signing failed: -0x%04x", -signed_rc);
    return ESP_FAIL;
  }

  size_t base64_length = 0;
  int rc = mbedtls_base64_encode(nullptr, 0, &base64_length,
                                 signature.data(), signature_length);
  if (rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) return ESP_FAIL;

  std::vector<unsigned char> base64(base64_length + 1, 0);
  rc = mbedtls_base64_encode(base64.data(), base64.size(), &base64_length,
                             signature.data(), signature_length);
  if (rc != 0) return ESP_FAIL;

  signature_base64->assign(
      reinterpret_cast<const char *>(base64.data()), base64_length);
  return ESP_OK;
}

}  // namespace stagecore
