#pragma once

#include "esp_err.h"

namespace stagecore {

// Starts the physical-presence local recovery monitor.
// - 2 s hold: latch LOCAL_WEB emergency blackout and expose a 60 s read-only
//   diagnostics window on the configured Stage LAN.
// - 10 s continuous hold: after confirmed blackout, clear only remembered Hub
//   trust and reboot for normal discovery/pairing.
// It never exposes a network command for nonzero output or trust reset.
esp_err_t start_local_hub_trust_reset_monitor();

}  // namespace stagecore
