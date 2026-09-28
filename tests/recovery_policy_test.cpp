#include <cassert>
#include <cstdint>

#include "recovery_policy.h"

int main() {
  {
    stagecore::LocalRecoveryHoldPolicy policy(10000);
    for (int i = 0; i < 199; ++i) {
      assert(!policy.sample(true, 50));
    }
    assert(policy.held_ms() == 9950);
    assert(policy.sample(true, 50));
    assert(!policy.sample(true, 50));
  }

  {
    stagecore::LocalRecoveryHoldPolicy policy(10000);
    for (int i = 0; i < 100; ++i) {
      assert(!policy.sample(true, 50));
    }
    assert(!policy.sample(false, 50));
    assert(policy.held_ms() == 0);
    for (int i = 0; i < 199; ++i) {
      assert(!policy.sample(true, 50));
    }
    assert(policy.sample(true, 50));
  }

  {
    stagecore::LocalRecoveryHoldPolicy policy(10000);
    assert(!policy.sample(true, 9999));
    assert(policy.sample(true, 1));
    assert(!policy.sample(true, UINT32_MAX));
    assert(!policy.sample(false, 50));
    assert(!policy.sample(true, 9999));
    assert(policy.sample(true, 1));
  }

  // Product gesture composition: the same continuous physical hold triggers
  // emergency blackout first and the more destructive trust reset only later.
  {
    stagecore::LocalRecoveryHoldPolicy emergency(2000);
    stagecore::LocalRecoveryHoldPolicy trust_reset(10000);
    bool emergency_triggered = false;
    bool trust_triggered = false;
    for (int i = 0; i < 200; ++i) {
      const bool e = emergency.sample(true, 50);
      const bool t = trust_reset.sample(true, 50);
      if (i == 39) emergency_triggered = e;
      if (i < 199) assert(!t);
      if (i == 199) trust_triggered = t;
    }
    assert(emergency_triggered);
    assert(trust_triggered);
  }

  return 0;
}
