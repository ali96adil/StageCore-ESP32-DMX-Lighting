# Experimental read-only v2 software-level probe

Tracking: Core [#255](https://github.com/ali96adil/StageCore/issues/255);
Hub stacked [Draft #258](https://github.com/ali96adil/StageCore/pull/258).
This contract is **source-only**: do not flash show hardware or repin the Phase C
physical qualification campaign from a software CI result.

## Build isolation

- `esp32dev`: unchanged v1 default.
- `esp32dev-v2-experimental`: unchanged projectless, blackout-only v2.
- `esp32dev-v2-probe-experimental`: opt-in v2 diagnostic extension, requires a
  compatible Hub source-only #258. It advertises `lighting.state_probe/1`
  in `device.hello.capabilities`. No commands or snapshot activation.

## Diagnostic protocol

The Hub first authenticates the paired device's current v2 socket and
allocates a new durable connection generation. The Hub only sends a probe when
the device advertises this optional capability. The request includes
`type=lighting.state_probe`, `schema_version=2`, the exact device ID,
assignment epoch, connection generation, a fresh canonical 64-lowercase-hex
challenge, `expected_channels=12` and `commands_enabled=false`.

Firmware checks all scope fields on receipt. It queues at most one probe,
never concurrently with an assignment blackout challenge, and rechecks epoch
and generation in the output task. It reads the local, mutex-protected
12-channel DMX frame **only** when the frame's generation was sent and the
local driver reports healthy. No device response is generated if that check
fails. The response `lighting.state_report` echoes scope and challenge and
contains `levels_known=true`, `blackout` and exactly 12 integer
`channel_levels`. `blackout` is true only if all software channels are zero
and the firmware reports FAILSAFE authority. A nonzero or non-blackout report
is diagnostic evidence of an unsafe unactivated state; it is never an
invitation to restore output.

The Hub checks full size/range, consumes only the current socket's pending
challenge, revalidates its credential/assignment and rejects late, malformed
or stale reports. It always returns `CommandsEnabled=false` and
`PhysicalOutputVerified=false`.

This is **not** a current-Cue comparison or a physical decoder/LED voltage
measurement. Hub desired-state derivation, automatic reconnect invocation,
v2 ACTIVE activation and safe partial corrections remain to be implemented
and independently qualified. Do not use ordinary `device.observation` or
this diagnostic as a GO, permission to exit blackout, or physical PASS.

## Batched qualification hardening (host tests and parallel CI)

The opt-in probe handler now reuses `state_probe_v2::valid_request` at both
receipt and the output task: exact current device ID, positive current Hub
assignment epoch and generation, 12 channels, canonical 32-byte lowercase
hex challenge, and `commands_enabled=false`. Its host tests reject
stale/future epochs, old/replacement sockets, incomplete/extra channel
universes, missing or noncanonical challenges and any command-enabled
request. The transport still independently authenticates the Hub.

`state_probe_v2::classify_sample` emits a logical report only after the
local output task confirmed an intact, healthy, sent 12-slot frame; partial,
unsent and unhealthy frames yield **no report**. Logical zero plus local
FAILSAFE classifies software blackout; a nonzero slot (including unused
slot 12), or zero outside FAILSAFE, is explicitly **not** blackout. It never
claims independent DMX decoder/LED measurements or output authority.

Firmware CI uses independent concurrent jobs for the pure host contract,
unchanged v1 build, blackout-only v2 and opt-in v2 probe. Cancel superseded
runs for the same PR. Faster CI and tests **do not** authorize flashing or
alter the canonical 79-gate Phase C physical qualification campaign.

## Physical handoff / evidence to collect separately

Capture the exact Hub and firmware build SHAs, device identity, current
Hub-assigned Project/epoch and fresh connection generation. Confirm the
real device's all-12-slot software blackout after boot, re-pair, Hub restart,
Wi-Fi dropout, power cycle and a stale challenge. Record independent DMX
decoder output and LED/driver voltage or observed fixture darkness
**separately** from the local logical report. Do not mark a physical PASS
from `lighting.state_report`, GitHub Actions, a serial-only log or a
self-reported DMX-health flag. Do not attempt nonzero output with this
blackout-only v2 image. Any positive physical activation gate and partial
correction must be qualified in a separate future v2 ACTIVE contract.

