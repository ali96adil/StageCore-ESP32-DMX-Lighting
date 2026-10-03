#pragma once

#ifndef STAGECORE_EXPERIMENTAL_DEVICE_V2
#define STAGECORE_EXPERIMENTAL_DEVICE_V2 0
#endif

namespace stagecore {

constexpr int runtime_outer_schema_version_for(bool v2_enabled) {
  return v2_enabled ? 2 : 1;
}

constexpr int runtime_outer_schema_version() {
  return runtime_outer_schema_version_for(
      STAGECORE_EXPERIMENTAL_DEVICE_V2 != 0);
}

}  // namespace stagecore
