#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "esp_err.h"

namespace stagecore {

struct VerifiedHub {
  std::string hub_id;
  std::string display_name;
  std::string fingerprint;
  std::string tls_sha256;
  std::string address;
  uint16_t port = 0;
  std::vector<unsigned char> certificate_der;
};

esp_err_t discover_and_verify_hub(VerifiedHub *hub);

}  // namespace stagecore
