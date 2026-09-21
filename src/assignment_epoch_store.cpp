#include "assignment_epoch_store.h"

#include "assignment_v2.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include "mbedtls/sha256.h"
#include "nvs.h"

namespace stagecore::assignment_v2 {
namespace {

// One NVS blob rather than separately committed keys: a partial epoch/hash
// write must not make a mismatched Project appear authorized on reboot.
constexpr char kNamespace[] = "stagecore_v2";
constexpr char kKey[] = "epoch_cache";
constexpr uint8_t kVersion = 1;
constexpr size_t kBlobSize = 2 + sizeof(uint64_t) + 32;
constexpr size_t kDigestOffset = 2 + sizeof(uint64_t);
constexpr uint64_t kMaxEpoch = INT64_MAX;

using EpochBlob = std::array<uint8_t, kBlobSize>;

esp_err_t load_blob(EpochBlob *blob, bool *found) {
  if (blob == nullptr || found == nullptr) return ESP_ERR_INVALID_ARG;
  *found = false;
  nvs_handle_t handle;
  esp_err_t err = nvs_open(kNamespace, NVS_READONLY, &handle);
  if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
  if (err != ESP_OK) return err;
  size_t size = 0;
  err = nvs_get_blob(handle, kKey, nullptr, &size);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    return ESP_OK;
  }
  if (err != ESP_OK || size != blob->size()) {
    nvs_close(handle);
    return err != ESP_OK ? err : ESP_ERR_INVALID_SIZE;
  }
  err = nvs_get_blob(handle, kKey, blob->data(), &size);
  nvs_close(handle);
  if (err != ESP_OK) return err;
  if (size != blob->size() || (*blob)[0] != kVersion ||
      (*blob)[1] > static_cast<uint8_t>(PersistedState::kBlocked)) {
    return ESP_ERR_INVALID_RESPONSE;
  }
  *found = true;
  return ESP_OK;
}

uint64_t decode_epoch(const EpochBlob &blob) {
  uint64_t epoch = 0;
  for (size_t i = 2; i < 10; ++i) {
    epoch = (epoch << 8) | blob[i];
  }
  return epoch;
}

void encode_epoch(EpochBlob *blob, uint64_t epoch) {
  for (size_t i = 0; i < sizeof(epoch); ++i) {
    (*blob)[9 - i] = static_cast<uint8_t>(epoch & 0xff);
    epoch >>= 8;
  }
}

}  // namespace

esp_err_t confirm_zero_and_persist_epoch(
    uint64_t epoch, PersistedState state, const std::string &project_id) {
  const bool blocked = state == PersistedState::kBlocked;
  if (epoch == 0 || epoch > kMaxEpoch ||
      (blocked && project_id.size() != 36) ||
      (!blocked && !project_id.empty())) {
    return ESP_ERR_INVALID_ARG;
  }
  EpochBlob desired{};
  desired[0] = kVersion;
  desired[1] = static_cast<uint8_t>(state);
  encode_epoch(&desired, epoch);
  if (mbedtls_sha256(
          reinterpret_cast<const unsigned char *>(project_id.data()),
          project_id.size(), desired.data() + kDigestOffset, 0) != 0) {
    return ESP_FAIL;
  }

  EpochBlob existing{};
  bool found = false;
  esp_err_t err = load_blob(&existing, &found);
  if (err != ESP_OK) return err;
  EpochCache stored{};
  if (found) {
    stored.epoch = decode_epoch(existing);
    stored.state = existing[1] == static_cast<uint8_t>(PersistedState::kBlocked)
                       ? State::kBlocked : State::kUnassigned;
    std::memcpy(stored.project_digest.data(),
                existing.data() + kDigestOffset,
                stored.project_digest.size());
  }
  EpochCache candidate{};
  candidate.epoch = epoch;
  candidate.state = blocked ? State::kBlocked : State::kUnassigned;
  std::memcpy(candidate.project_digest.data(),
              desired.data() + kDigestOffset,
              candidate.project_digest.size());
  if (!allow_epoch_cache_update(stored, candidate)) {
    return ESP_ERR_INVALID_STATE;
  }
  if (found && stored.epoch == epoch) return ESP_OK; // exact reconnect

  nvs_handle_t handle;
  err = nvs_open(kNamespace, NVS_READWRITE, &handle);
  if (err != ESP_OK) return err;
  err = nvs_set_blob(handle, kKey, desired.data(), desired.size());
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  return err;
}

}  // namespace stagecore::assignment_v2
