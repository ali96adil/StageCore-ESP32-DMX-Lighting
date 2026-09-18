#pragma once

#include <string>
#include <vector>

#include "esp_err.h"

namespace stagecore {

class DeviceIdentity {
 public:
  DeviceIdentity();
  ~DeviceIdentity();

  DeviceIdentity(const DeviceIdentity &) = delete;
  DeviceIdentity &operator=(const DeviceIdentity &) = delete;

  esp_err_t LoadOrCreate();

  const std::string &device_id() const { return device_id_; }
  const std::string &public_key_base64() const { return public_key_base64_; }

  esp_err_t SignAuthenticationMessage(const std::string &challenge_id,
                                      const std::string &nonce_base64,
                                      std::string *signature_base64);

 private:
  esp_err_t LoadOrCreateDeviceID();
  esp_err_t LoadOrCreateKey();
  esp_err_t RefreshPublicKey();

  std::string device_id_;
  std::string public_key_base64_;
  void *pk_context_;
};

}  // namespace stagecore
