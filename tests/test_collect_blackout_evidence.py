import hashlib
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

TOOL_PATH = Path(__file__).resolve().parents[1] / "tools" / "collect_blackout_evidence.py"
spec = importlib.util.spec_from_file_location("blackout_evidence", TOOL_PATH)
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)

DEVICE = "f2cc808f-d251-4ccc-a4ce-07ab638982c1"
GOOD = ("I (1) stagecore-main: StageCore ESP32 DMX Lighting Node 0.2.0-dev.1\n"
        "I (2) stagecore-main: safe boot: blackout\n"
        "I (3) stagecore-dmx: safe blackout initialized tx=17 rts=21 channels=12\n"
        "I (4) stagecore-main: device_id=" + DEVICE + "\n"
        "W (5) stagecore-main: EXPERIMENTAL v2: projectless, blackout-only image; device=" + DEVICE + "\n"
        "W (6) stagecore-runtime: v2 Hub assignment received; project commands disabled\n"
        "I (7) stagecore-runtime: Hub persisted software-zero ACK for BLOCKED epoch\n"
        "W (8) stagecore-runtime: v2 output remains FAILSAFE; only assignment.blackout accepted\n").encode()

class BlackoutEvidenceTests(unittest.TestCase):
    def test_full_log_is_evidence_only_not_physical_pass(self):
        report = tool.inspect_log(GOOD, expected_device=DEVICE,
                                  captured_at="2026-09-21T19:00:00+00:00")
        self.assertEqual(report["software_log_status"], "SOFTWARE_LOG_CAPTURED")
        self.assertEqual(report["raw_log_sha256"], hashlib.sha256(GOOD).hexdigest())
        self.assertEqual(report["device_ids_reported"], [DEVICE])
        self.assertEqual(report["firmware_versions_reported"], ["0.2.0-dev.1"])
        self.assertEqual(report["reported_output_configurations"][0]["dmx_channels"], 12)
        self.assertTrue(report["log_signals"]["blocked_epoch_ack_receipt"])
        self.assertFalse(report["commands_enabled"])
        self.assertFalse(report["physical_dmx_decoder_verified"])
        self.assertFalse(report["physical_led_output_verified"])
        self.assertEqual(report["physical_qualification"], "PENDING_PHYSICAL")
        self.assertFalse(report["firmware_build_sha_verified"])
        self.assertFalse(report["hub_build_sha_verified"])

    def test_partial_boot_and_v1_are_not_v2_qualified(self):
        truncated = GOOD.split(b"EXPERIMENTAL v2")[0]
        report = tool.inspect_log(truncated)
        self.assertEqual(report["software_log_status"], "INCOMPLETE")
        self.assertFalse(report["log_signals"]["v2_blackout_only"])
        report = tool.inspect_log(GOOD.replace(b"EXPERIMENTAL v2: projectless, blackout-only image",
                                              b"provisioned for project A as node"))
        self.assertEqual(report["software_log_status"], "V1_SOFTWARE_LOG_CAPTURED")
        self.assertFalse(report["physical_dmx_decoder_verified"])
        self.assertEqual(report["physical_qualification"], "PENDING_PHYSICAL")

    def test_self_reported_failure_or_pin_mismatch_blocks_log(self):
        bad = tool.inspect_log(GOOD + b"E: failsafe blackout failed after runtime exit: ESP_FAIL\n")
        self.assertEqual(bad["software_log_status"], "UNSAFE_LOG")
        wrong = tool.inspect_log(GOOD.replace(b"tx=17 rts=21", b"tx=18 rts=21"))
        self.assertEqual(wrong["software_log_status"], "UNSAFE_LOG")
        changed = tool.inspect_log(GOOD, expected_device="11111111-1111-1111-1111-111111111111")
        self.assertEqual(changed["software_log_status"], "UNSAFE_LOG")

    def test_missing_serial_identity_is_not_success(self):
        data = GOOD.replace(("device_id=" + DEVICE).encode(), b"device_id=unknown")
        report = tool.inspect_log(data)
        self.assertEqual(report["software_log_status"], "INCOMPLETE")
        self.assertEqual(report["device_ids_reported"], [])

    def test_same_build_with_different_device_ids_is_not_success(self):
        data = GOOD + b"I: device_id=11111111-1111-1111-1111-111111111111\n"
        report = tool.inspect_log(data)
        self.assertEqual(report["software_log_status"], "INCOMPLETE")

    def test_cli_does_not_overwrite_evidence_or_issue_commands(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            log = path / "boot.log"
            result = path / "evidence.json"
            log.write_bytes(GOOD)
            status = tool.main(["--log", str(log), "--out", str(result),
                                "--expected-device-id", DEVICE])
            self.assertEqual(status, 0)
            payload = json.loads(result.read_text())
            self.assertEqual(payload["source"], str(log))
            self.assertEqual(payload["physical_qualification"], "PENDING_PHYSICAL")
            with self.assertRaises(SystemExit):
                tool.main(["--log", str(log), "--out", str(result)])

    def test_serial_capture_requires_write_once_raw_log(self):
        with self.assertRaises(SystemExit):
            tool.main(["--port", "/dev/cu.usbserial-0001", "--out", "evidence.json"])

    def test_v1_baseline_does_not_qualify_experimental_v2(self):
        raw = GOOD.replace(b"EXPERIMENTAL v2: projectless, blackout-only image",
                           b"provisioned for project A as node")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            log = path / "v1.log"
            report_path = path / "v1.json"
            log.write_bytes(raw)
            self.assertEqual(tool.main(["--log", str(log), "--out", str(report_path)]), 0)
            result = json.loads(report_path.read_text())
            self.assertEqual(result["software_log_status"], "V1_SOFTWARE_LOG_CAPTURED")
            self.assertEqual(result["physical_qualification"], "PENDING_PHYSICAL")
            self.assertFalse(result["commands_enabled"])

    def test_cli_incomplete_returns_nonzero_but_writes_replayable_report(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            log = path / "partial.log"
            result = path / "evidence.json"
            log.write_bytes(GOOD[:70])
            self.assertEqual(tool.main(["--log", str(log), "--out", str(result)]), 1)
            self.assertEqual(json.loads(result.read_text())["software_log_status"], "INCOMPLETE")


if __name__ == "__main__":
    unittest.main()
