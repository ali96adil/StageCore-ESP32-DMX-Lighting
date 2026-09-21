#!/usr/bin/env python3
"""Capture or inspect ESP32 blackout evidence without flashing or sending commands.

Evidence from UART is NEVER independent proof of DMX decoder or LED output.
Usage:
  python3 tools/collect_blackout_evidence.py --log boot.log --out evidence.json
  python3 tools/collect_blackout_evidence.py --port /dev/cu.usbserial-0001 --seconds 30 --out evidence.json

Opening a serial port can reset an ESP32 via DTR/RTS; use only with a node
already kept on safe blackout and NEVER during a show. --port needs pyserial.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

DEVICE_ID = re.compile(r"\bdevice_id=([0-9a-fA-F-]{36})\b")
VERSION = re.compile(r"StageCore ESP32 DMX Lighting Node\s+(\S+)")
SAFE_OUTPUT = re.compile(r"safe blackout initialized tx=(\d+) rts=(\d+) channels=(\d+)")
UNSAFE = (
    "safe failure:",
    "DMX initialization failed",
    "DMX task startup failed",
    "failsafe blackout failed after runtime exit:",
    "v2 Hub epoch rolled back",
)
SIGNALS = (
    ("safe_boot_log", "safe boot: blackout"),
    ("failsafe_output_initialized", "safe blackout initialized"),
    ("v2_blackout_only", "EXPERIMENTAL v2: projectless, blackout-only image"),
    ("v1_legacy_provisioned", "provisioned for project "),
    ("v2_assignment_received", "v2 Hub assignment received; project commands disabled"),
    ("v2_remains_failsafe", "v2 output remains FAILSAFE"),
    ("blocked_epoch_ack_receipt", "Hub persisted software-zero ACK for BLOCKED epoch"),
)


def inspect_log(
    raw: bytes,
    *,
    expected_device: str = "",
    source: str = "log",
    captured_at: str | None = None,
) -> dict:
    """Parse evidence and mark physical measurement permanently NOT_PERFORMED."""
    text = raw.decode("utf-8", errors="replace")
    signals = {name: phrase in text for name, phrase in SIGNALS}
    found_ids = sorted(set(DEVICE_ID.findall(text)))
    versions = sorted(set(VERSION.findall(text)))
    output = [tuple(map(int, match)) for match in SAFE_OUTPUT.findall(text)]
    hazard = sorted({phrase for phrase in UNSAFE if phrase in text})
    if output and any(tx != 17 or rts != 21 or channels != 12 for tx, rts, channels in output):
        hazard.append("unexpected DMX pin/channel configuration")
    if expected_device and (len(found_ids) != 1 or found_ids[0].lower() != expected_device.lower()):
        hazard.append("expected device ID mismatch or unavailable")
    common = (
        signals["safe_boot_log"]
        and signals["failsafe_output_initialized"]
        and len(found_ids) == 1
        and len(versions) == 1
        and output != []
    )
    v2_complete = common and signals["v2_blackout_only"] and signals["v2_remains_failsafe"]
    v1_baseline = common and signals["v1_legacy_provisioned"] and not signals["v2_blackout_only"]
    status = (
        "UNSAFE_LOG" if hazard else
        "SOFTWARE_LOG_CAPTURED" if v2_complete else
        "V1_SOFTWARE_LOG_CAPTURED" if v1_baseline else "INCOMPLETE"
    )
    return {
        "schema_version": 1,
        "source": source,
        "captured_at_utc": captured_at or datetime.now(timezone.utc).isoformat(),
        "raw_log_sha256": hashlib.sha256(raw).hexdigest(),
        "raw_log_bytes": len(raw),
        "firmware_versions_reported": versions,
        "device_ids_reported": found_ids,
        "expected_device_id": expected_device,
        "log_signals": signals,
        "reported_output_configurations": [
            {"dmx_tx_gpio": tx, "dmx_rts_gpio": rts, "dmx_channels": n}
            for tx, rts, n in output
        ],
        "safety_warnings": sorted(set(hazard)),
        "software_log_status": status,
        "physical_dmx_decoder_verified": False,
        "physical_led_output_verified": False,
        "physical_qualification": "PENDING_PHYSICAL",
        "commands_enabled": False,
        "firmware_build_sha_verified": False,
        "hub_build_sha_verified": False,
        "note": "UART is self-report only; physical DMX/LED, exact installed build hashes, and v2 ACTIVE remain unverified.",
    }


def read_serial(port: str, seconds: float) -> bytes:
    try:
        import serial  # pyserial, already used by PlatformIO
    except ImportError as exc:
        raise RuntimeError("Install pyserial in your active flash virtualenv; no flash is needed") from exc
    # Opening USB serial MAY reset the board via DTR/RTS. Do not use on show hardware.
    chunks: list[bytes] = []
    with serial.Serial(port, baudrate=115200, timeout=0.25) as device:
        device.dtr = False
        device.rts = False
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            portion = device.read(4096)
            if portion:
                chunks.append(portion)
    return b"".join(chunks)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--log", type=Path, help="Previously captured UTF-8 UART log")
    source.add_argument("--port", help="USB serial device; read-only but opening may reset ESP32")
    parser.add_argument("--seconds", type=float, default=30.0, help="Serial capture duration (1..120)")
    parser.add_argument("--out", type=Path, required=True, help="JSON evidence path (must not exist)")
    parser.add_argument("--raw-log-out", type=Path, help="Write new raw serial capture, exclusive; required with --port")
    parser.add_argument("--expected-device-id", default="", help="Optional exact 36-character device identity")
    args = parser.parse_args(argv)
    if not 1 <= args.seconds <= 120:
        parser.error("--seconds must be between 1 and 120")
    if args.expected_device_id and not re.fullmatch(r"[0-9a-fA-F-]{36}", args.expected_device_id):
        parser.error("--expected-device-id must be a 36-character UUID")
    if args.port and args.raw_log_out is None:
        parser.error("--port requires --raw-log-out to preserve the original UART evidence")
    if args.raw_log_out and not args.port:
        parser.error("--raw-log-out is only supported with --port")
    if args.out.exists() or (args.raw_log_out is not None and args.raw_log_out.exists()):
        parser.error("refusing to overwrite existing evidence or UART log (choose new paths)")
    if args.raw_log_out is not None and args.raw_log_out.resolve() == args.out.resolve():
        parser.error("--out and --raw-log-out must be distinct")
    try:
        raw = args.log.read_bytes() if args.log else read_serial(args.port, args.seconds)
        if args.raw_log_out is not None:
            args.raw_log_out.parent.mkdir(parents=True, exist_ok=True)
            with args.raw_log_out.open("xb") as original:
                original.write(raw)
        result = inspect_log(raw, expected_device=args.expected_device_id,
                             source=str(args.log) if args.log else str(args.raw_log_out))
        args.out.parent.mkdir(parents=True, exist_ok=True)
        with args.out.open("x", encoding="utf-8") as stream:
            json.dump(result, stream, indent=2, ensure_ascii=False, sort_keys=True)
            stream.write("\n")
    except (OSError, RuntimeError) as exc:
        print("Evidence collection failed: " + str(exc), file=sys.stderr)
        return 2
    print("Wrote " + str(args.out) + ": " + result["software_log_status"])
    print("Physical DMX / LED: PENDING_PHYSICAL; no flash or output command was sent")
    return 0 if result["software_log_status"] in ("SOFTWARE_LOG_CAPTURED", "V1_SOFTWARE_LOG_CAPTURED") else 1


if __name__ == "__main__":
    raise SystemExit(main())
