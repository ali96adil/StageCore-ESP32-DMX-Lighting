#pragma once

#include <cstdint>
#include <string>

#include "esp_err.h"

namespace stagecore {

// Trust is established only from an HTTPS response received through the
// already-pinned StageCore Hub certificate.
esp_err_t update_trusted_clock_from_http_date(const char *http_date);
bool trusted_clock_ready();
int64_t trusted_now_unix_ms();
std::string format_rfc3339_utc_ms(int64_t unix_ms);

// Parses RFC3339 / RFC3339Nano timestamps with Z or numeric offsets.
bool parse_rfc3339_unix_ms(const std::string &value, int64_t *unix_ms);

}  // namespace stagecore
