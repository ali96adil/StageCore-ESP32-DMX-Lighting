#include "runtime_protocol.h"

#include <cassert>

int main() {
  static_assert(stagecore::runtime_outer_schema_version_for(false) == 1);
  static_assert(stagecore::runtime_outer_schema_version_for(true) == 2);
  assert(stagecore::runtime_outer_schema_version_for(false) == 1);
  assert(stagecore::runtime_outer_schema_version_for(true) == 2);
  return 0;
}
