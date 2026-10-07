from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ProvisioningContract(unittest.TestCase):
    def test_setup_and_recovery_use_persisted_override_or_shared_default(self):
        source = (ROOT / "src" / "provisioning.cpp").read_text()
        store = (ROOT / "src" / "config_store.cpp").read_text()
        self.assertIn('#define STAGECORE_SETUP_AP_PASSWORD "12345678"', source)
        self.assertIn("effective_setup_ap_password", source)
        self.assertGreaterEqual(
            source.count("const std::string password = effective_setup_ap_password();"),
            2,
        )
        self.assertIn('kSetupAPPasswordKey[] = "setup_ap_pass"', store)
        self.assertIn("save_setup_ap_password", store)
        self.assertIn("clear_setup_ap_password", store)
        self.assertNotIn("random_ap_password", source)
        self.assertNotIn("esp_random()", source)
        self.assertNotIn("temporary AP password", source)
        self.assertIn("WIFI_AUTH_WPA2_PSK", source)

    def test_v2_setup_ap_maintenance_is_authenticated_and_non_output(self):
        source = (ROOT / "src" / "stage_device_runtime.cpp").read_text()
        self.assertIn("device.maintenance.setup-ap-password", source)
        self.assertIn("maintenance.setup_ap_password", source)
        self.assertIn("maintenance.setup_ap_password.result", source)
        self.assertIn("connection_generation", source)
        self.assertIn("save_setup_ap_password", source)
        self.assertIn("clear_setup_ap_password", source)
        self.assertNotIn('cJSON_AddStringToObject(result, "password"', source)

        start = source.index("esp_err_t process_pending_setup_ap_maintenance")
        end = source.index("esp_err_t process_pending_blackout", start)
        maintenance = source[start:end]
        for forbidden in (
            "lighting_blackout(",
            "lighting_output_blackout_immediate(",
            "lighting_set(",
            "lighting_fade(",
            "lighting_runtime_authority_acquired(",
        ):
            self.assertNotIn(forbidden, maintenance)


if __name__ == "__main__":
    unittest.main()
