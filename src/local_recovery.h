#pragma once

#include "esp_err.h"

namespace stagecore {

// Starts a physical-button-only recovery monitor. A long intentional hold
// forces confirmed blackout before clearing only the remembered Hub trust
// binding and rebooting. It never changes Wi-Fi, device identity or Project
// configuration.
esp_err_t start_local_hub_trust_reset_monitor();

}  // namespace stagecore
