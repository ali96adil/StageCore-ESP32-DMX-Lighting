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


## Protected local recovery and diagnostics

The configured node exposes one physical-presence recovery surface on the board
BOOT button (GPIO 0 by default). It does not expose a general local lighting
command API.

After the normal application has booted and joined the Stage LAN:

- **2-second continuous hold:** latch an emergency all-slot blackout under
  `LOCAL_WEB` authority. Nonzero/configuration-changing lighting operations
  are blocked until an attended reboot. After blackout is confirmed, a
  read-only diagnostics server is available for 60 seconds on TCP port 8088.
  It exposes only software/runtime health such as DMX task health, authority,
  configuration hash, logical levels, RSSI and whether Hub trust is configured.
  It does not expose credentials or any output-changing route.
- **10-second continuous hold:** while the same emergency blackout remains
  confirmed, erase only the remembered `hub_id`, `hub_fp`, and `hub_tls`
  values and reboot for normal StageCore discovery/pairing.

Wi-Fi settings, persistent device identity, lighting configuration and any
legacy Project configuration are preserved. The diagnostics status explicitly
states that logical/output-task state is not independent decoder or fixture
measurement.

Physical button timing, browser access, DMX stability under read-only local-web
activity, emergency blackout and re-pair behavior remain separate attended
qualification gates.

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

## Setup Wi-Fi access point

First-run provisioning and saved-network recovery use device-specific SSIDs with the shared StageCore setup password `12345678`. You no longer need Serial Monitor to discover a random AP password. The password may be overridden at build time with `STAGECORE_SETUP_AP_PASSWORD`; keep the same value across StageCore devices when using that override.


## TLS Foundation qualification candidate (2026-10-08)

This **Draft PR branch only** pins the Foundation TLS bootstrap fix from StageCore
[PR #447](https://github.com/ali96adil/StageCore/pull/447), commit
`4495a381cc40f06f44e0a16da4a4dc82a9bceb6c`.
It addresses the ESP-IDF self-signed Hub TLS setup failure encountered on
StageLaser. This is not a production firmware release or permission to flash.

Source build checks (no device update):

```sh
pio run -e esp32dev
pio run -e esp32dev-v2-active-experimental
```

The active v2 DMX environment remains EXPERIMENTAL. Do not flash the show node until attended blackout, DMX channels, reconnect, assignment epoch and Cues are verified with a safe rollback plan.

**Security gate:** The initial discovery fingerprint is advertised on the local
network. Until the Hub fingerprint is verified through an independently trusted
channel or an authenticated prior binding, a spoofed first discovery must not
be treated as authenticated trust. Review wrong-pin failure, pairing,
reconnect, and credential handling before merging the Foundation fix.

Pass CI and test all affected board targets before any attended physical flash.


### Existing ACTIVE assignment — blackout-only observer qualification

An installed v2 DMX node can have a Hub-owned `ACTIVE` assignment even when its
currently selected experimental image is the original blackout-only candidate.
That older image intentionally rejects the `ACTIVE` assignment frame, causing
a repeated authenticated WebSocket reconnect with `ESP_ERR_INVALID_RESPONSE`.

For non-actuating on-bench diagnostics only, use the separately gated image:

```sh
pio run -e esp32dev-v2-active-observe-only
```

This image accepts the Hub-issued ACTIVE assignment *envelope* only when its
full non-actuating shape is valid (project, published-snapshot ID, configuration
hash, forced blackout, new connection generation, and `commands_enabled=false`).
It immediately asserts a 12-slot software blackout and reports readiness
`BLOCKER`. Crucially it **never** sends the ACTIVE scope ACK, never enables
show commands, never applies the Project's lighting configuration, and does not
rewrite the persisted assignment-epoch anti-rollback metadata merely to observe
an ACTIVE device. The Hub retains the original ACTIVE assignment but grants no
show-output authority to this socket. Restoring a show-capable image requires a
separate, attended qualification of the authentic ACTIVE scope handshake.

**Safety:** This is not a lighting-control release. Keep the decoder/fixtures
isolated from the show network during tests. Restore the locally backed-up
previous working firmware if live lighting control is required before v2 ACTIVE
is physically qualified; do not clear NVS, force an assignment transition, or
bypass the Hub's epoch/snapshot checks.
