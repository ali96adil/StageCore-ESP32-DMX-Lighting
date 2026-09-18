#include "trusted_clock.h"

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/time.h>

namespace stagecore {
namespace {

std::atomic<bool> g_trusted_clock{false};

bool leap_year(int year) {
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

int days_in_month(int year, int month) {
  static constexpr int kDays[] = {
      31,28,31,30,31,30,31,31,30,31,30,31
  };
  if (month < 1 || month > 12) return 0;
  if (month == 2 && leap_year(year)) return 29;
  return kDays[month - 1];
}

int64_t days_from_civil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy =
      (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<int64_t>(era) * 146097 +
         static_cast<int64_t>(doe) - 719468;
}

bool valid_datetime(int year, int month, int day,
                    int hour, int minute, int second) {
  return year >= 1970 && year <= 9999 &&
         month >= 1 && month <= 12 &&
         day >= 1 && day <= days_in_month(year, month) &&
         hour >= 0 && hour <= 23 &&
         minute >= 0 && minute <= 59 &&
         second >= 0 && second <= 60;
}

bool two_digits(const char *text, int *value) {
  if (text == nullptr || value == nullptr ||
      !std::isdigit(static_cast<unsigned char>(text[0])) ||
      !std::isdigit(static_cast<unsigned char>(text[1]))) {
    return false;
  }
  *value = (text[0] - '0') * 10 + (text[1] - '0');
  return true;
}

bool four_digits(const char *text, int *value) {
  if (text == nullptr || value == nullptr) return false;
  int result = 0;
  for (int i = 0; i < 4; ++i) {
    if (!std::isdigit(static_cast<unsigned char>(text[i]))) return false;
    result = result * 10 + (text[i] - '0');
  }
  *value = result;
  return true;
}

int month_from_http(const char *text) {
  static constexpr const char *kMonths[] = {
      "Jan","Feb","Mar","Apr","May","Jun",
      "Jul","Aug","Sep","Oct","Nov","Dec"
  };
  for (int i = 0; i < 12; ++i) {
    if (std::strncmp(text, kMonths[i], 3) == 0) return i + 1;
  }
  return 0;
}

bool compose_unix_ms(int year, int month, int day,
                     int hour, int minute, int second,
                     int offset_seconds, int fractional_ms,
                     int64_t *unix_ms) {
  if (unix_ms == nullptr ||
      !valid_datetime(year, month, day, hour, minute, second)) {
    return false;
  }

  // RFC3339 allows leap second 60. Treat it as the first second of the
  // following minute for monotonic expiry purposes.
  const int normalized_second = second == 60 ? 59 : second;
  int64_t seconds =
      days_from_civil(year, static_cast<unsigned>(month),
                      static_cast<unsigned>(day)) * 86400LL +
      static_cast<int64_t>(hour) * 3600LL +
      static_cast<int64_t>(minute) * 60LL +
      normalized_second;
  if (second == 60) ++seconds;
  seconds -= offset_seconds;

  *unix_ms = seconds * 1000LL + fractional_ms;
  return true;
}

}  // namespace

esp_err_t update_trusted_clock_from_http_date(const char *http_date) {
  // IMF-fixdate: "Sun, 06 Nov 1994 08:49:37 GMT"
  if (http_date == nullptr || std::strlen(http_date) != 29 ||
      http_date[3] != ',' || http_date[4] != ' ' ||
      http_date[7] != ' ' || http_date[11] != ' ' ||
      http_date[16] != ' ' || http_date[19] != ':' ||
      http_date[22] != ':' || http_date[25] != ' ' ||
      std::strncmp(http_date + 26, "GMT", 3) != 0) {
    return ESP_ERR_INVALID_ARG;
  }

  int day = 0, year = 0, hour = 0, minute = 0, second = 0;
  if (!two_digits(http_date + 5, &day) ||
      !four_digits(http_date + 12, &year) ||
      !two_digits(http_date + 17, &hour) ||
      !two_digits(http_date + 20, &minute) ||
      !two_digits(http_date + 23, &second)) {
    return ESP_ERR_INVALID_ARG;
  }
  const int month = month_from_http(http_date + 8);

  int64_t unix_ms = 0;
  if (!compose_unix_ms(year, month, day, hour, minute, second,
                       0, 0, &unix_ms)) {
    return ESP_ERR_INVALID_ARG;
  }

  timeval tv{};
  tv.tv_sec = static_cast<time_t>(unix_ms / 1000LL);
  tv.tv_usec = 0;
  if (settimeofday(&tv, nullptr) != 0) return ESP_FAIL;

  g_trusted_clock.store(true);
  return ESP_OK;
}

bool trusted_clock_ready() {
  return g_trusted_clock.load();
}

int64_t trusted_now_unix_ms() {
  if (!trusted_clock_ready()) return 0;
  timeval tv{};
  if (gettimeofday(&tv, nullptr) != 0) return 0;
  return static_cast<int64_t>(tv.tv_sec) * 1000LL +
         static_cast<int64_t>(tv.tv_usec / 1000);
}

std::string format_rfc3339_utc_ms(int64_t unix_ms) {
  if (unix_ms <= 0) return {};
  time_t seconds = static_cast<time_t>(unix_ms / 1000LL);
  int milliseconds = static_cast<int>(unix_ms % 1000LL);
  if (milliseconds < 0) {
    milliseconds += 1000;
    --seconds;
  }
  tm utc{};
  if (gmtime_r(&seconds, &utc) == nullptr) return {};
  char buffer[40];
  const int written = std::snprintf(
      buffer, sizeof(buffer),
      "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
      utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
      utc.tm_hour, utc.tm_min, utc.tm_sec, milliseconds);
  if (written <= 0 || static_cast<size_t>(written) >= sizeof(buffer)) {
    return {};
  }
  return std::string(buffer, static_cast<size_t>(written));
}

bool parse_rfc3339_unix_ms(const std::string &value, int64_t *unix_ms) {
  if (unix_ms == nullptr || value.size() < 20) return false;
  const char *text = value.c_str();

  int year = 0, month = 0, day = 0;
  int hour = 0, minute = 0, second = 0;
  if (!four_digits(text, &year) ||
      text[4] != '-' ||
      !two_digits(text + 5, &month) ||
      text[7] != '-' ||
      !two_digits(text + 8, &day) ||
      (text[10] != 'T' && text[10] != 't') ||
      !two_digits(text + 11, &hour) ||
      text[13] != ':' ||
      !two_digits(text + 14, &minute) ||
      text[16] != ':' ||
      !two_digits(text + 17, &second)) {
    return false;
  }

  size_t pos = 19;
  int fractional_ms = 0;
  if (pos < value.size() && value[pos] == '.') {
    ++pos;
    int digits = 0;
    while (pos < value.size() &&
           std::isdigit(static_cast<unsigned char>(value[pos]))) {
      if (digits < 3) {
        fractional_ms = fractional_ms * 10 + (value[pos] - '0');
      }
      ++digits;
      ++pos;
    }
    if (digits == 0) return false;
    while (digits < 3) {
      fractional_ms *= 10;
      ++digits;
    }
  }

  int offset_seconds = 0;
  if (pos >= value.size()) return false;
  if (value[pos] == 'Z' || value[pos] == 'z') {
    ++pos;
  } else if (value[pos] == '+' || value[pos] == '-') {
    const int sign = value[pos] == '+' ? 1 : -1;
    if (pos + 6 != value.size() ||
        value[pos + 3] != ':') {
      return false;
    }
    int offset_hour = 0, offset_minute = 0;
    if (!two_digits(text + pos + 1, &offset_hour) ||
        !two_digits(text + pos + 4, &offset_minute) ||
        offset_hour > 23 || offset_minute > 59) {
      return false;
    }
    offset_seconds =
        sign * (offset_hour * 3600 + offset_minute * 60);
    pos += 6;
  } else {
    return false;
  }

  if (pos != value.size()) return false;
  return compose_unix_ms(year, month, day, hour, minute, second,
                         offset_seconds, fractional_ms, unix_ms);
}

}  // namespace stagecore
