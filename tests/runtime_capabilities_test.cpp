#include "runtime_capabilities.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

namespace {
std::vector<std::string> copy(bool v2, bool probe) {
  const auto advertised = stagecore::runtime_advertised_capabilities(v2, probe);
  return {advertised.begin(), advertised.end()};
}
}  // namespace

int main() {
  const auto v1 = copy(false, false);
  const std::vector<std::string> expected_v1 = {
      "lighting.channels.set", "lighting.channels.fade", "lighting.blackout",
      "lighting.identify", "lighting.state.read", "lighting.config.read",
      "lighting.config.apply",
  };
  assert(v1 == expected_v1);
  // Probe flag alone must never alter the installed/default v1 image.
  assert(copy(false, true) == expected_v1);
  // Only default v1 may expose retained legacy aliases/configuration hash.
  // The v2 images preserve NVS for rollback but must not represent the old
  // Project's configuration as a current v2 assignment/snapshot.
  assert(stagecore::runtime_exposes_legacy_configuration(false));
  assert(!stagecore::runtime_exposes_legacy_configuration(true));

  const auto v2_blackout = copy(true, false);
  assert(v2_blackout.empty()); // assignment/blackout frames are not show commands

  const auto v2_probe = copy(true, true);
  assert(v2_probe == std::vector<std::string>{"lighting.state_probe/1"});
  for (const auto &caps : {v2_blackout, v2_probe}) {
    for (const auto &capability : caps) {
      assert(capability.find("lighting.channels.") == std::string::npos);
      assert(capability != "lighting.identify");
      assert(capability.find("lighting.config.") == std::string::npos);
      assert(capability != "lighting.blackout");
    }
  }
  const auto v2_active = runtime_advertised_capabilities(true, true, true);
  assert(std::find(v2_active.begin(), v2_active.end(),
                   std::string("lighting.channels.set")) != v2_active.end());
  assert(std::find(v2_active.begin(), v2_active.end(),
                   std::string("lighting.config.apply")) != v2_active.end());
  assert(std::find(v2_active.begin(), v2_active.end(),
                   std::string("lighting.state_probe/1")) != v2_active.end());

  std::cout << "runtime capability/protocol contract PASS\n";
}
