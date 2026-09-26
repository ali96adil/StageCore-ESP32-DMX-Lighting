#pragma once

#include <cstdint>
#include <string>

#include "esp_err.h"

// Experimental v2-only anti-rollback cache. Project assignment authority
// still belongs solely to the authenticated Hub; the project hash is stored
// ONLY to reject an inconsistent same-epoch Hub reply after reboot.
namespace stagecore::assignment_v2 {

enum class PersistedState : uint8_t {
  kUnassigned = 0,
  kBlocked = 1,
};

// Call ONLY after software-confirmed full physical-channel zero output.
// A lower epoch or same epoch with a different Project/state fails closed.
// Store writes a single versioned NVS blob; no Project ID is persisted.
esp_err_t confirm_zero_and_persist_epoch(
    uint64_t epoch, PersistedState state, const std::string &project_id);

}  // namespace stagecore::assignment_v2
