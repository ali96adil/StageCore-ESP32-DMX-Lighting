#include "assignment_v2.h"

#include <cassert>
#include <cstdint>
#include <iostream>

using namespace stagecore::assignment_v2;

int main() {
  const Assignment unassigned{"node", "", 1, State::kUnassigned, ""};
  assert(validate(unassigned));
  assert(!authorize_command(unassigned, {"node", "project-A", 1, "", false}));
  assert(!validate({"node", "project-A", 1, State::kUnassigned, ""}));
  assert(!validate({"node", "project-A", 1, State::kLegacy, ""}));

  const Assignment current{"node", "project-A", 5, State::kActive, "old-snapshot"};
  assert(validate(current));
  assert(authorize_command(current, {"node", "project-A", 5, "old-snapshot", true}));
  assert(!authorize_command(current, {"node", "project-B", 5, "old-snapshot", true}));
  assert(!authorize_command(current, {"node", "project-A", 4, "old-snapshot", true}));
  assert(!authorize_command(current, {"node", "project-A", 6, "old-snapshot", true}));
  assert(!authorize_command(current, {"node", "project-A", 5, "stale-snapshot", true}));
  assert(!authorize_command(current, {"node", "project-A", 5, "", true}));

  const TransferIntent intent{"node", "project-A", "project-B", 5, 7, 12, "fresh"};
  const BlackoutAck ack{"node", 5, 7, "fresh", true, std::vector<uint8_t>(12, 0)};
  Assignment next;
  assert(verify_blackout(current, intent, ack, &next));
  assert(next.device_id == "node" && next.project_id == "project-B" &&
         next.epoch == 6 && next.state == State::kBlocked &&
         next.snapshot_id.empty());
  assert(current.project_id == "project-A" && current.epoch == 5);
  assert(!authorize_command(next, {"node", "project-B", 6, "", false}));

  auto bad_ack = ack;
  bad_ack.channel_levels.pop_back();
  assert(!verify_blackout(current, intent, bad_ack, &next));
  bad_ack = ack;
  bad_ack.channel_levels[3] = 1;
  assert(!verify_blackout(current, intent, bad_ack, &next));
  bad_ack = ack;
  bad_ack.connection_generation = 6;
  assert(!verify_blackout(current, intent, bad_ack, &next));
  bad_ack = ack;
  bad_ack.epoch = 4;
  assert(!verify_blackout(current, intent, bad_ack, &next));
  bad_ack = ack;
  bad_ack.challenge = "replayed";
  assert(!verify_blackout(current, intent, bad_ack, &next));
  bad_ack = ack;
  bad_ack.device_id = "other-node";
  assert(!verify_blackout(current, intent, bad_ack, &next));
  bad_ack = ack;
  bad_ack.blackout = false;
  assert(!verify_blackout(current, intent, bad_ack, &next));
  assert(!verify_blackout(current, intent, ack, nullptr));

  auto bad_intent = intent;
  bad_intent.from_project_id = "project-C";
  assert(!verify_blackout(current, bad_intent, ack, &next));
  bad_intent = intent;
  bad_intent.to_project_id = "project-A";
  assert(!verify_blackout(current, bad_intent, ack, &next));
  bad_intent = intent;
  bad_intent.expected_channels = 0;
  assert(!verify_blackout(current, bad_intent, ack, &next));
  bad_intent = intent;
  bad_intent.expected_epoch = 4;
  assert(!verify_blackout(current, bad_intent, ack, &next));

  const TransferIntent unassign{"node", "project-A", "", 5, 7, 12, "fresh"};
  assert(verify_blackout(current, unassign, ack, &next));
  assert(next.state == State::kUnassigned && next.project_id.empty() && next.epoch == 6);

  const Assignment max_epoch{"node", "project-A", kMaxPersistentEpoch,
                             State::kActive, ""};
  const TransferIntent overflow{"node", "project-A", "project-B",
                                kMaxPersistentEpoch, 7, 12, "fresh"};
  BlackoutAck max_ack = ack;
  max_ack.epoch = kMaxPersistentEpoch;
  assert(!verify_blackout(max_epoch, overflow, max_ack, &next));
  assert(!validate({"node", "project-A", kMaxPersistentEpoch + 1, State::kActive, ""}));

  std::cout << "assignment_v2 pure contract tests PASS\n";
  return 0;
}
