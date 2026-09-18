#pragma once

#include <string>

#include "device_identity.h"
#include "esp_err.h"
#include "hub_discovery.h"

namespace stagecore {

struct RuntimeCredential {
  std::string session_id;
  std::string token;
};

esp_err_t ensure_paired_and_authenticate(const VerifiedHub &hub,
                                         DeviceIdentity *identity,
                                         const std::string &display_name,
                                         RuntimeCredential *credential);

}  // namespace stagecore
