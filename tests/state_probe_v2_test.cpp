#include "state_probe_v2.h"

#include <cassert>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using namespace stagecore::state_probe_v2;

int main() {
  const CurrentScope current{"device-1", 7, 39};
  const Request fresh{"device-1", 7, 39, std::string(64, 'a'), 12, false};
  assert(valid_request(fresh, current));

  auto bad = fresh;
  bad.device_id = "old-device";
  assert(!valid_request(bad, current));
  bad = fresh;
  bad.assignment_epoch = 6;
  assert(!valid_request(bad, current));
  bad = fresh;
  bad.assignment_epoch = 8;
  assert(!valid_request(bad, current));
  bad = fresh;
  bad.connection_generation = 38;
  assert(!valid_request(bad, current));
  bad = fresh;
  bad.connection_generation = 40;
  assert(!valid_request(bad, current));
  bad = fresh;
  bad.expected_channels = 11;
  assert(!valid_request(bad, current));
  bad = fresh;
  bad.expected_channels = 13;
  assert(!valid_request(bad, current));
  bad = fresh;
  bad.commands_enabled = true;
  assert(!valid_request(bad, current));
  bad = fresh;
  bad.challenge.clear();
  assert(!valid_request(bad, current));
  bad = fresh;
  bad.challenge.pop_back();
  assert(!valid_request(bad, current));
  bad = fresh;
  bad.challenge[12] = 'G';
  assert(!valid_request(bad, current));
  bad = fresh;
  bad.challenge[12] = 'z';
  assert(!valid_request(bad, current));
  assert(!valid_request(fresh, {"device-1", 0, 39}));
  assert(!valid_request(fresh, {"device-1", 7, 0}));
  assert(!valid_request(fresh, {"device-1", 7, kMaxExactJSONInteger + 1}));

  const std::vector<uint8_t> zero(kChannels, 0);
  const LogicalReport clean = classify_sample(zero, true, true);
  assert(clean.valid && clean.levels_known && clean.blackout);
  assert(clean.channel_levels == zero);

  // The firmware can report nonzero logical output as UNSAFE diagnosis, but
  // neither this report nor zero grants READY or measures a physical fixture.
  auto nonzero = zero;
  nonzero[1] = 140; // Cue 5 desired [180,140,60], observed [180,0,60]
  const LogicalReport drift = classify_sample(nonzero, true, true);
  assert(drift.valid && drift.levels_known && !drift.blackout);
  assert(drift.channel_levels[1] == 140);
  auto spare = zero;
  spare[11] = 1;
  const LogicalReport unsafe_spare = classify_sample(spare, true, true);
  assert(unsafe_spare.valid && !unsafe_spare.blackout);
  const LogicalReport no_failsafe = classify_sample(zero, true, false);
  assert(no_failsafe.valid && !no_failsafe.blackout);

  const LogicalReport stale_frame = classify_sample(zero, false, true);
  assert(!stale_frame.valid && !stale_frame.levels_known &&
         stale_frame.channel_levels.empty() && !stale_frame.blackout);
  const LogicalReport partial = classify_sample(std::vector<uint8_t>(11, 0), true, true);
  assert(!partial.valid && !partial.levels_known);
  const LogicalReport oversized = classify_sample(std::vector<uint8_t>(13, 0), true, true);
  assert(!oversized.valid && !oversized.levels_known);
  const LogicalReport empty = classify_sample({}, true, true);
  assert(!empty.valid && !empty.levels_known);

  std::cout << "state_probe_v2 pure logical contract tests PASS\n";
  return 0;
}
