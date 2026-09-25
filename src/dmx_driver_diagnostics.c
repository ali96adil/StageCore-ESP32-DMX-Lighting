#include "dmx_driver_diagnostics.h"

#include <string.h>

#include "sdkconfig.h"
#include "dmx/include/service.h"

#if !defined(CONFIG_DMX_ISR_IN_IRAM) || !CONFIG_DMX_ISR_IN_IRAM
#error "StageCore requires esp_dmx ISR functions to be compiled into IRAM"
#endif

#if !defined(CONFIG_GPTIMER_ISR_IRAM_SAFE) || !CONFIG_GPTIMER_ISR_IRAM_SAFE
#error "StageCore requires GPTimer ISR IRAM safety for DMX"
#endif

#if !defined(CONFIG_GPTIMER_CTRL_FUNC_IN_IRAM) || !CONFIG_GPTIMER_CTRL_FUNC_IN_IRAM
#error "StageCore requires GPTimer control functions in IRAM for DMX"
#endif

bool stagecore_dmx_capture_driver_snapshot(
    int dmx_port, stagecore_dmx_driver_snapshot_t *snapshot) {
  if (snapshot == NULL || dmx_port < 0 || dmx_port >= DMX_NUM_MAX) {
    return false;
  }

  memset(snapshot, 0, sizeof(*snapshot));

  dmx_driver_t *driver = dmx_driver[dmx_port];
  if (driver == NULL) {
    return false;
  }

  // Read only the esp_dmx software state under the driver's own spinlock.
  // Do not acknowledge interrupts, modify task_waiting, or touch timer state.
  taskENTER_CRITICAL(DMX_SPINLOCK(dmx_port));
  snapshot->enabled = driver->is_enabled;
  snapshot->is_controller = driver->is_controller;
  snapshot->status = driver->dmx.status;
  snapshot->progress = driver->dmx.progress;
  snapshot->head = driver->dmx.head;
  snapshot->size = driver->dmx.size;
  snapshot->task_waiting = (uintptr_t)driver->task_waiting;
  snapshot->controller_eop_timestamp = driver->dmx.controller_eop_timestamp;
  taskEXIT_CRITICAL(DMX_SPINLOCK(dmx_port));

  return true;
}
