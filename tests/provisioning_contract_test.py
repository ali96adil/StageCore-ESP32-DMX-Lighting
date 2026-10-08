from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ProvisioningContract(unittest.TestCase):
    def test_setup_and_recovery_use_shared_foundation_password_policy(self):
        source = (ROOT / "src" / "provisioning.cpp").read_text()
        store = (ROOT / "src" / "config_store.cpp").read_text()
        manifest = (ROOT / "src" / "idf_component.yml").read_text()

        self.assertIn('#include "foundation_contract.h"', source)
        self.assertIn("kDefaultSetupAPPassword", source)
        self.assertIn("foundation_store().EffectiveSetupAPPassword", source)
        self.assertIn("effective_setup_ap_password", source)
        self.assertGreaterEqual(
            source.count("const std::string password = effective_setup_ap_password();"),
            2,
        )

        self.assertIn("foundation_store().SaveSetupAPPasswordOverride", store)
        self.assertIn("foundation_store().ResetSetupAPPasswordToDefault", store)
        self.assertIn("stagecore_foundation:", manifest)
        self.assertIn("https://github.com/ali96adil/StageCore.git", manifest)
        self.assertIn(
            "path: firmware/foundation/esp-idf/components/stagecore_foundation",
            manifest,
        )
        self.assertIn(
            "version: d6946da3f003e8c0c2a72216804ef2035bf1288c",
            manifest,
        )

        self.assertNotIn("STAGECORE_SETUP_AP_PASSWORD", source)
        self.assertNotIn("random_ap_password", source)
        self.assertNotIn("esp_random()", source)
        self.assertNotIn("temporary AP password", source)
        self.assertIn("WIFI_AUTH_WPA2_PSK", source)

    def test_v2_setup_ap_maintenance_is_authenticated_and_non_output(self):
        source = (ROOT / "src" / "stage_device_runtime.cpp").read_text()
        store = (ROOT / "src" / "config_store.cpp").read_text()

        self.assertIn("device.maintenance.setup-ap-password", source)
        self.assertIn("maintenance.setup_ap_password", source)
        self.assertIn("maintenance.setup_ap_password.result", source)
        self.assertIn("connection_generation", source)
        self.assertIn("save_setup_ap_password", source)
        self.assertIn("clear_setup_ap_password", source)
        self.assertIn("SaveSetupAPPasswordOverride", store)
        self.assertIn("ResetSetupAPPasswordToDefault", store)
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

    def test_local_foundation_duplicates_are_removed(self):
        for name in (
            "device_identity.cpp",
            "device_identity.h",
            "hub_discovery.cpp",
            "hub_discovery.h",
            "hub_security.cpp",
            "hub_security.h",
            "trusted_clock.cpp",
            "trusted_clock.h",
        ):
            self.assertFalse((ROOT / "src" / name).exists(), name)


if __name__ == "__main__":
    unittest.main()
