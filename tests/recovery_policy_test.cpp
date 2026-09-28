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

  return 0;
}
