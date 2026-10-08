import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
FOUNDATION_SHA = "d6946da3f003e8c0c2a72216804ef2035bf1288c"


class SharedFoundationDependencyTest(unittest.TestCase):
    def test_dependency_is_exactly_pinned(self):
        manifest = (SRC / "idf_component.yml").read_text(encoding="utf-8")
        self.assertIn("stagecore_foundation:", manifest)
        self.assertIn("git: https://github.com/ali96adil/StageCore.git", manifest)
        self.assertIn(
            "path: firmware/foundation/esp-idf/components/stagecore_foundation",
            manifest,
        )
        self.assertIn(f"version: {FOUNDATION_SHA}", manifest)

    def test_duplicate_foundation_sources_are_removed(self):
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
            self.assertFalse((SRC / name).exists(), name)

    def test_dmx_component_requires_shared_foundation(self):
        cmake = (SRC / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("stagecore_foundation", cmake)
        for duplicate in (
            '"device_identity.cpp"',
            '"hub_discovery.cpp"',
            '"hub_security.cpp"',
            '"trusted_clock.cpp"',
        ):
            self.assertNotIn(duplicate, cmake)

    def test_device_adapter_preserves_pairing_metadata(self):
        main = (SRC / "main.cpp").read_text(encoding="utf-8")
        self.assertIn('FoundationStore foundation_store("stagecore")', main)
        self.assertIn('hostname_prefix = "stagecore-light-"', main)
        self.assertIn('platform = "esp32"', main)
        self.assertIn('architecture = "xtensa"', main)
        for capability in (
            "lighting.channels.set",
            "lighting.channels.fade",
            "lighting.blackout",
            "lighting.state.read",
            "lighting.identify",
            "lighting.config.read",
            "lighting.config.apply",
        ):
            self.assertIn(capability, main)

    def test_existing_config_api_delegates_generic_persistence(self):
        source = (SRC / "config_store.cpp").read_text(encoding="utf-8")
        self.assertIn("LoadSetupAPPasswordOverride", source)
        self.assertIn("SaveSetupAPPasswordOverride", source)
        self.assertIn("ResetSetupAPPasswordToDefault", source)
        self.assertIn("LoadHubBinding", source)
        self.assertIn("SaveHubBinding", source)
        self.assertIn("ClearHubBinding", source)


if __name__ == "__main__":
    unittest.main()
