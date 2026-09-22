#pragma once

#include <vector>

namespace stagecore {

// A capability is an executable/implemented transport promise, not a
// description of what the physical decoder could eventually do. Experimental
// v2 is blackout-only; the opt-in probe is diagnostic, never command authority.
// A v2 device may retain a v1 lighting configuration in NVS solely for an
// attended rollback. Its names/hash are NOT a current Hub-owned v2 snapshot,
// so never include that legacy configuration in a v2 observation.
inline bool runtime_exposes_legacy_configuration(bool experimental_v2) {
  return !experimental_v2;
}

inline std::vector<const char *> runtime_advertised_capabilities(
    bool experimental_v2, bool read_only_probe) {
  if (experimental_v2) {
    if (read_only_probe) return {"lighting.state_probe/1"};
    return {};
  }

  // Preserve the default v1 device. Its seven existing capabilities and
  // command protocol are unchanged by the dormant v2 implementation.
  return {
      "lighting.channels.set",
      "lighting.channels.fade",
      "lighting.blackout",
      "lighting.identify",
      "lighting.state.read",
      "lighting.config.read",
      "lighting.config.apply",
  };
}

}  // namespace stagecore
