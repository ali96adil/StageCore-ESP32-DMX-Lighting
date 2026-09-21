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
