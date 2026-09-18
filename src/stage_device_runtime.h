#pragma once

#include "config_store.h"
#include "device_identity.h"
#include "esp_err.h"
#include "hub_discovery.h"
#include "hub_security.h"

namespace stagecore {

// Opens one authenticated Stage Device runtime session and remains connected
// until the Hub, network, or protocol closes it. This Slice 1 runtime is
// intentionally transport-only: it reports BLOCKER/FAILSAFE and never executes
// lighting commands.
esp_err_t run_stage_device_runtime(const VerifiedHub &hub,
                                   const RuntimeCredential &credential,
                                   const DeviceIdentity &identity,
                                   const DeviceConfig &config);

}  // namespace stagecore
