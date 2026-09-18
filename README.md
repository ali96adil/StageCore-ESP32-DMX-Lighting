# StageCore ESP32 DMX Lighting Node

Production firmware repository for the StageCore ESP32 DMX Lighting Node.

## Current status

**Firmware Slice 1 — foundation in progress. Not a flash candidate yet.**

The StageCore-side integration is frozen and SOFTWARE READY at:

`243a7ea3cdb94af443d8410d5b568362503303c9`

This firmware must implement that contract without creating a second pairing,
command, Cue, show-history or configuration authority.

Active integration/physical tracker in the StageCore repository: Issue #221.

## Target hardware

- ESP32-WROOM / 38-pin DevKit / CP2102
- MAX485-class TTL ↔ RS-485 transceiver
- 12-channel DMX decoder
- 24 V LED power system
- 24 V → 5 V buck converter

Default development pin assignment:

- DMX TX: GPIO 17
- MAX485 DE/RE (RTS): GPIO 21
- DMX RX: unused for the transmit-only lighting node

The pin assignment is a firmware build setting and can be changed before the
physical wiring qualification.

## Build

The firmware uses PlatformIO with native ESP-IDF rather than the legacy Arduino
HTTP prototype. ESP-IDF is used deliberately so TLS 1.3 support can be enabled
explicitly for the StageCore secure device gateway.

Pinned build inputs:

- PlatformIO Core 6.2.0 in CI
- platform-espressif32 7.1.3
- ESP-IDF supplied by that platform
- esp_dmx compatibility fix commit `931d62c1ee6c9ddb0f5274e6da7808d3e923b6f0` from PR #223 (base: esp_dmx 4.1.0)

Build:

```bash
pio run -e esp32dev
```

Do not flash this repository until the current firmware milestone is explicitly
marked **FLASH CANDIDATE**.

## Safety invariant already implemented

Immediately after boot, the DMX engine is initialized with a NULL start code
and channels 1..12 at value 0. It continuously transmits this blackout frame.

Network/pairing/runtime work must never make boot restore an unexpected previous
brightness.

## Planned firmware slices

1. **Foundation**
   - build/CI
   - safe blackout DMX output
   - persistent provisioning settings
   - P-256 device identity
   - StageCore Hub discovery and certificate pinning
   - pairing/authentication
   - authenticated `stagecore.device/1` hello

2. **Lighting runtime**
   - all seven StageCore lighting commands
   - local multi-channel fade engine
   - command deadline and deduplication
   - observations/config hash
   - failsafe and reconnect behavior

3. **Flash candidate qualification**
   - build artifact freeze
   - ESP32 flash
   - MAX485/DMX bench checks
   - StageCore pairing and end-to-end physical qualification

The old HTTP endpoints (`/channel`, `/preset`, `/blackout`) are not part of
the production protocol.

## esp_dmx compatibility note

The upstream esp_dmx 4.1.0 release does not compile against ESP-IDF 5.3+ because `uart_signal_conn_t.module` was removed. This repository pins the single-commit compatibility fix from esp_dmx PR #223, which maps UART ports to the corresponding `PERIPH_UARTx_MODULE` values for ESP-IDF 5 while leaving the IDF 4 path unchanged. The PR reports DMX output verified on ESP32-D0WDQ6, UART2 / GPIO17, ESP-IDF 5.5.2.
