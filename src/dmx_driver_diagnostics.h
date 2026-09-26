#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct stagecore_dmx_driver_snapshot_t {
  bool enabled;
  bool is_controller;
  int status;
  int progress;
  int head;
  int size;
  uintptr_t task_waiting;
  int64_t controller_eop_timestamp;
} stagecore_dmx_driver_snapshot_t;

// Diagnostic-only, read-only snapshot of esp_dmx internal software state.
// The function never changes DMX frames, driver state, interrupts, or timers.
bool stagecore_dmx_capture_driver_snapshot(
    int dmx_port, stagecore_dmx_driver_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
