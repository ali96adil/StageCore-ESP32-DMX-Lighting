from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ProvisioningContract(unittest.TestCase):
    def test_setup_and_recovery_use_shared_password(self):
        source = (ROOT / "src" / "provisioning.cpp").read_text()
        self.assertIn('#define STAGECORE_SETUP_AP_PASSWORD "12345678"', source)
        self.assertIn("kSetupApPassword", source)
        self.assertGreaterEqual(source.count("const std::string password = kSetupApPassword;"), 2)
        self.assertNotIn("random_ap_password", source)
        self.assertNotIn("esp_random()", source)
        self.assertIn("WIFI_AUTH_WPA2_PSK", source)


if __name__ == "__main__":
    unittest.main()
