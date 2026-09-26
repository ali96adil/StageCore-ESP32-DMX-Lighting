# V2 Lighting Node — resumable blackout evidence (read-only)

**Status:** source-only / physical qualification PENDING. This is not a
flashing procedure and does not activate v2 commands, the published Runtime
Snapshot, physical lights or the StageCore Pi. Related: StageCore #221, #239,
#255 and Firmware Draft #12/#13.

## Advertised capabilities and v1 rollback boundary

The authenticated `device.hello` capability list MUST describe what the
selected image actually executes, not what an eventual v2 ACTIVE design might
support. The default v1 image advertises its seven existing lighting
operations. The experimental v2 blackout-only image advertises **no show
commands**; assignment/blackout handshake frames are internal protocol
messages rather than general lighting capabilities. Only the separate opt-in
v2 probe image advertises `lighting.state_probe/1`, a read-only logical
software report. No v2 image accepts `command.execute`, grants `READY`,
restores a Cue or proves actual decoder/LED output.

The experimental v2 firmware also retains any previously stored v1 lighting
channel aliases and configuration bytes for a controlled rollback, but its
`device.observation` deliberately reports an **empty** `current_levels`
object and omits the legacy `configuration_hash`. That old hash belongs to
the v1 Project and must never look like a Hub-owned current v2 snapshot.
The separate opt-in `lighting.state_probe/1` reports actual local 12-slot
software levels, explicitly not physical DMX/LED measurements or command
authority. Default v1 observations retain their original aliases and hash.
The shared pure capability/observation contract has a host regression, and
all three images must pass exact-head CI.

An existing v1 `project_id` NVS value is retained through v2 configuration
for potential attended rollback. Its presence is not Hub-owned v2 assignment
authority; returning to v1 after a v2 transfer may restore **old** v1
project-scoped behavior. Do not downgrade/flash as a way to switch Projects.
An operator must independently reconcile the legacy Project and published
configuration, keep the physical output disconnected/blackout, and validate
trust and snapshot before any reviewed rollback. A v2 image against an
incompatible old Hub must fail closed, not silently turn into v1.

`tests/runtime_capabilities_test.cpp` and the three image-build jobs in
Firmware CI cover the source contract, **not** a live Hub/ESP handshake.
Current installed v1 CH1 Identify observations are separate physical evidence.

## Before collecting

1. **No live show.** Verify the intended device is not connected to a
   powered fixture that would become hazardous if USB serial opening resets
   the ESP. Opening a serial device can toggle DTR/RTS and reboot an ESP32.
   Keep the lighting system in its independent safe blackout.
2. Record actual installed Hub binary SHA, installed ESP build SHA/image,
   wiring and tester separately. A GitHub branch SHA is **not** proof of the
   firmware installed on the physical board.
3. The current pinned v1 board can provide a **v1 software baseline**
   without flashing, labeled `V1_SOFTWARE_LOG_CAPTURED` when complete.
   This is **not** v2 qualification. For the experimental v2 log gate, use
   only an independently approved and pinned blackout-only test image.
   Never flash a draft just to satisfy this collector.
4. Keep the raw UART log locally. It might contain network/device identifiers;
   review it before sharing. The JSON includes identifiers and the raw log's
   SHA-256 but never claims physical electrical measurement.

## Noninteractive re-analysis of an existing log

From the firmware repository, with Python 3.12+:

```bash
python3 tools/collect_blackout_evidence.py \
  --log "$HOME/stagecore-blackout-boot.log" \
  --expected-device-id "f2cc808f-d251-4ccc-a4ce-07ab638982c1" \
  --out "$HOME/stagecore-evidence/blackout-run-001.json"
```

The collector uses only the standard library for file analysis. Every new
capture needs a fresh `--out` path: previous evidence cannot be overwritten.
If the approved isolated test device is connected via USB and pyserial is
installed in the active Python environment, optionally capture *read-only*
UART for 30 seconds:

```bash
python3 tools/collect_blackout_evidence.py \
  --port /dev/cu.usbserial-0001 --seconds 30 \
  --raw-log-out "$HOME/stagecore-evidence/blackout-run-002.log" \
  --out "$HOME/stagecore-evidence/blackout-run-002.json"
```

**Note:** opening a port can cause a reboot; the 30-second window may begin
after the boot messages. A partial capture returns `INCOMPLETE`; use a
longer capture within the 120-second limit or reuse a complete existing log.
The script never flashes, sends a control command, touches Wi-Fi settings,
writes NVS or attempts a DMX command. Do not use it during a live show.


The serial capture requires `--raw-log-out` so the exact bytes hashed by
the JSON remain available for reproducible re-analysis. Both the raw log and
the JSON are created exclusively; choose new filenames per run. The script
never overwrites prior evidence. A baseline collected from the currently
installed v1 image must never be mislabeled as the experimental v2 probe or
as a physical PASS.

## Meaning of the report

The JSON stores the log SHA-256, device IDs, printed firmware version,
software-log signals and reported DMX pins and universe size. These are
**self-reported** observations and may be missing or misleading. An apparently
complete v2 FAILSAFE log produces `software_log_status:
SOFTWARE_LOG_CAPTURED`, **not physical PASS**. A complete pinned v1
boot/provisioning baseline is labeled `V1_SOFTWARE_LOG_CAPTURED`; it **never
claims v2 authority**. Missing evidence returns `INCOMPLETE`. A pin mismatch, identity mismatch or explicit local blackout
failure returns `UNSAFE_LOG`. Both incomplete and unsafe results cause a
nonzero process exit while preserving their JSON evidence.

Every report must keep:
`physical_qualification: PENDING_PHYSICAL`,
`physical_dmx_decoder_verified: false`,
`physical_led_output_verified: false`,
`commands_enabled: false`, and the installed firmware/Hub build SHA
verification flags false. Never upgrade these flags from UART, CI or an ESP
self-reported `dmx_healthy` value.

## Physical checks — separately witnessed, not inferred

Use an appropriate DMX analyzer or independently measured decoder and
fixture output. Record separately: actual MAX485 VCC/logic compatibility,
GPIO17/TX, GPIO21/RTS, data pair polarity and common/termination, decoder
addresses, independent 24 V supply and power safety. Observe physical
darkness and electrical output on boot, local failsafe, Wi-Fi loss, Hub
restart, reconnect, stale challenge and power cycle, each tied to the exact
installed build. Any nonzero output while the node is unactivated is a
**FAIL/stop** condition, not a reason to send a corrective GO.

Only after the production v2 ACTIVE contract and command fencing are
independently built, reviewed and physically qualified can a separate
bounded partial-correction protocol (Cue 5: [180,140,60] vs [180,0,60])
be considered. Until then, keep v2 BLOCKED and do not repin or mark
the canonical Phase C campaign qualified.
