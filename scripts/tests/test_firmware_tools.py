import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
SPEC = importlib.util.spec_from_file_location("package_firmware", REPO / "scripts/package_firmware.py")
PACKAGER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGER)
BOARDS = ("waveshare-lcd-3.5", "waveshare-lcd-1.54")


class FirmwareToolsTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        shutil.copytree(REPO / "firmware/boards", self.root / "firmware/boards")
        shutil.copy2(REPO / "LICENSE", self.root / "LICENSE")
        (self.root / "scripts").mkdir()
        shutil.copy2(REPO / "scripts/idf.sh", self.root / "scripts/idf.sh")
        (self.root / "bin").mkdir()
        stub = self.root / "bin/idf.py"
        stub.write_text('#!/usr/bin/env python3\nimport json, sys\nprint(json.dumps(sys.argv[1:]))\n')
        stub.chmod(0o755)

    def idf(self, board, *arguments, port=None):
        env = dict(os.environ)
        env.pop("JUFF_BOARD", None)
        env.pop("ESPPORT", None)
        if board is not None:
            env["JUFF_BOARD"] = board
        if port is not None:
            env["ESPPORT"] = port
        env["PATH"] = str(self.root / "bin") + os.pathsep + env["PATH"]
        return subprocess.run(["bash", str(self.root / "scripts/idf.sh"), *arguments],
                              env=env, text=True, capture_output=True, check=False)

    def build_fixture(self, board_id):
        build = self.root / "firmware/build" / board_id
        (build / "config").mkdir(parents=True)
        board = json.loads((self.root / "firmware/boards" / board_id / "board.json").read_text())
        config = {board["kconfig_symbol"]: True, "JUFF_WIFI_SSID": "", "JUFF_WIFI_PASSWORD": "",
                  "JUFF_DEVICE_TOKEN": ""}
        (build / "config/sdkconfig.json").write_text(json.dumps(config))
        idf = self.root / "idf-source"
        idf.mkdir(exist_ok=True)
        (idf / "LICENSE").write_text("Apache-2.0 license fixture\n")
        infos = {}
        linked = []
        def component(name, root, notices, source="source.c", prebuilt=None):
            root.mkdir(parents=True, exist_ok=True)
            for filename, text in notices.items():
                path = root / filename
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text)
            src = root / source
            src.parent.mkdir(parents=True, exist_ok=True)
            src.write_text("/* Copyright Fixture Author. Permission is hereby granted for this fixture. */\nint fixture;\n")
            archive = root / prebuilt if prebuilt else build / "esp-idf" / name / ("lib" + name + ".a")
            archive.parent.mkdir(parents=True, exist_ok=True)
            archive.write_bytes(b"archive fixture: " + name.encode())
            infos[name] = {"dir": str(root), "sources": [str(src)]}
            linked.append(f"{archive}({src.name}.obj)\n    fixture reference\n")
        component("core", idf / "components/core", {})
        component("bt", idf / "components/bt", {
            "host/nimble/nimble/LICENSE": "Apache-2.0 NimBLE fixture\n",
            "host/nimble/nimble/NOTICE": "NimBLE copyright fixture\n",
            "common/tinycrypt/LICENSE": "TinyCrypt BSD fixture\n",
        }, source="host/nimble/nimble/src/nimble.c")
        component("esp_phy", idf / "components/esp_phy", {"lib/LICENSE": "Radio binary license fixture\n"},
                  prebuilt="lib/esp32s3/libphy.a")
        managed = self.root / "firmware/managed_components"
        component("lvgl__lvgl", managed / "lvgl__lvgl", {"LICENCE.txt": "LVGL MIT copyright fixture\n"})
        component("espressif__esp-sr", managed / "espressif__esp-sr", {"LICENSE": "Espressif binary license fixture\n"},
                  prebuilt="lib/esp32s3/libesp_audio_processor.a")
        sr = managed / "espressif__esp-sr"
        (sr / ".component_hash").write_text("sr-fixture-hash")
        supplement = self.root / "licenses/espressif__esp-sr"
        supplement.mkdir(parents=True, exist_ok=True)
        (supplement / "WEBRTC_LICENSE").write_text("WebRTC copyright fixture\n")
        (supplement / "provenance.json").write_text(json.dumps({
            "component_hash": "sr-fixture-hash", "files": ["WEBRTC_LICENSE"],
            "sha256": {"WEBRTC_LICENSE": hashlib.sha256((supplement / "WEBRTC_LICENSE").read_bytes()).hexdigest()}}))
        # An installed but unlinked dependency must not enter the release inventory.
        unused = managed / "unused"
        unused.mkdir(parents=True, exist_ok=True)
        (unused / "LICENSE").write_text("Unlinked fixture license\n")
        infos["unused"] = {"dir": str(unused), "sources": []}
        toolchain = self.root / "toolchain"
        runtime = toolchain / "xtensa-esp-elf/lib/libgcc.a"
        runtime.parent.mkdir(parents=True, exist_ok=True)
        runtime.write_bytes(b"gcc runtime fixture")
        for name in ("COPYING3", "COPYING.RUNTIME"):
            path = toolchain / "share/licenses/gcc" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("GCC runtime license fixture: " + name)
        linked.append(f"{runtime}(libgcc.o)\n    fixture reference\n")
        description = {"project_version": "0.5.0", "target": "esp32s3", "config_file": str(build / "sdkconfig"),
                       "idf_path": str(idf), "app_elf": "fixture.elf", "build_component_info": infos}
        (build / "project_description.json").write_text(json.dumps(description))
        (build / "fixture.map").write_text("Archive member included to satisfy reference\n" + "".join(linked)
                                          + "Allocating common symbols\nphy 0x4 invalid.a(member.o)\nDiscarded input sections\n")
        boot = build / "bootloader"
        boot.mkdir()
        boot_info = {"core": infos["core"]}
        (boot / "project_description.json").write_text(json.dumps({
            "idf_path": str(idf), "app_elf": "bootloader.elf", "build_component_info": boot_info}))
        boot_archive = boot / "esp-idf/core/libcore.a"
        boot_archive.parent.mkdir(parents=True)
        boot_archive.write_bytes(b"bootloader core fixture")
        (boot / "bootloader.map").write_text("Archive member included to satisfy reference\n"
                                             "esp-idf/core/libcore.a(source.c.obj)\n    reference\nDiscarded input sections\n")
        images = {"0x0": "bootloader.bin", "0x8000": "partition-table.bin", "0x10000": "app.bin"}
        flasher = {"flash_files": images, "extra_esptool_args": {"chip": "esp32s3"},
                   "write_flash_args": ["--flash_size", "16MB"], "flash_settings": {"flash_size": "16MB"}}
        for offset, name in images.items():
            role = Path(name).stem
            flasher[role] = {"file": name, "offset": offset}
            (build / name).write_bytes(role.encode() + b"\0" + board_id.encode() + b"\0")
        (build / "flasher_args.json").write_text(json.dumps(flasher))
        return build

    def test_build_requires_known_hardware_and_isolates_every_profile(self):
        for board in (None, "unknown-board"):
            with self.subTest(board=board):
                result = self.idf(board, "build")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Set JUFF_BOARD=", result.stderr)
        for board in BOARDS:
            with self.subTest(board=board):
                result = self.idf(board, "build")
                self.assertEqual(result.returncode, 0, result.stderr)
                args = json.loads(result.stdout)
                build = self.root / "firmware/build" / board
                self.assertEqual(args[args.index("-B") + 1], str(build))
                self.assertIn(f"SDKCONFIG={build}/sdkconfig", args)
                self.assertIn(f"SDKCONFIG_DEFAULTS={self.root}/firmware/sdkconfig.defaults;"
                              f"{self.root}/firmware/boards/{board}/sdkconfig.defaults", args)

    def test_aec_defaults_keep_microphone_gain_and_reference_control_board_specific(self):
        # Ordinary tools tests need no IDF installation. If one is configured,
        # use its actual Kconfig parser rather than reimplementing conditions.
        candidates = [sys.executable]
        for board in BOARDS:
            cache = REPO / "firmware/build" / board / "CMakeCache.txt"
            if cache.is_file():
                match = re.search(r"(?m)^PYTHON:[^=]*=(.+)$", cache.read_text())
                if match:
                    candidates.append(match.group(1))
        python = None
        for candidate in dict.fromkeys(candidates):
            if not Path(candidate).is_file():
                continue
            probe = subprocess.run([candidate, "-c", "import kconfiglib"], capture_output=True)
            if probe.returncode == 0:
                python = candidate
                break
        if python is None:
            self.skipTest("Install ESP-IDF to validate board defaults with its Kconfig parser")
        root_config = self.root / "Kconfig"
        root_config.write_text(
            'mainmenu "JUFF test"\n'
            'config CODEC_ES8311_SUPPORT\n    bool\n'
            'config CODEC_ES7210_SUPPORT\n    bool\n'
            f'source "{REPO}/firmware/main/Kconfig.projbuild"\n')
        runner = """
import json, sys
import kconfiglib
rows = []
for small in (False, True):
    for aec in (False, True):
        config = kconfiglib.Kconfig(sys.argv[1], warn=False)
        config.syms['JUFF_BOARD_WAVESHARE_LCD_154' if small else 'JUFF_BOARD_WAVESHARE_LCD_35'].set_value('y')
        config.syms['JUFF_VOICE_BARGE_IN'].set_value('y' if aec else 'n')
        rows.append({'small': small, 'aec': aec,
                     'aec_enabled': config.syms['JUFF_VOICE_BARGE_IN'].str_value == 'y',
                     'gain': int(config.syms['JUFF_CODEC_MIC_GAIN_DB'].str_value),
                     'reference_visible': config.syms['JUFF_CODEC_REFERENCE_GAIN_DB'].visibility > 0})
print(json.dumps(rows))
"""
        result = subprocess.run([python, "-c", runner, str(root_config)],
                                text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for row in json.loads(result.stdout):
            with self.subTest(small=row["small"], aec=row["aec"]):
                self.assertEqual(row["aec_enabled"], row["aec"])
                self.assertEqual(row["gain"], 24 if row["aec"] and not row["small"] else 30)
                self.assertEqual(row["reference_visible"], row["aec"] and row["small"])

    def test_serial_actions_require_an_explicit_device(self):
        for action in ("flash", "app-flash", "bootloader-flash", "erase-flash", "erase_flash", "monitor"):
            with self.subTest(action=action):
                result = self.idf(BOARDS[0], action)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("serial auto-selection is disabled", result.stderr)
        for args, env_port in ((("-p", "/dev/test-device", "flash"), None),
                               (("--port=/dev/test-device", "flash"), None),
                               (("flash",), "/dev/test-device")):
            result = self.idf(BOARDS[1], *args, port=env_port)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_saved_board_mismatch_and_config_overrides_are_rejected(self):
        for override in ("-B/tmp/wrong", "--build-dir=/tmp/wrong", "-DSDKCONFIG=/tmp/wrong",
                         "SDKCONFIG_DEFAULTS=/tmp/wrong", "SDKCONFIG:FILEPATH=/tmp/wrong",
                         "-D=SDKCONFIG=/tmp/wrong", "--define-cache-entry=SDKCONFIG=/tmp/wrong"):
            with self.subTest(override=override):
                result = self.idf(BOARDS[0], override, "build")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("do not override", result.stderr)
        build = self.root / "firmware/build" / BOARDS[0]
        build.mkdir(parents=True)
        (build / "sdkconfig").write_text("CONFIG_JUFF_BOARD_WAVESHARE_LCD_154=y\n")
        result = self.idf(BOARDS[0], "build")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not match", result.stderr)
        self.assertEqual(self.idf(None, "--help").returncode, 0)

    def test_packages_keep_both_hardware_versions_and_match_image_checksums(self):
        archives = []
        for board in BOARDS:
            self.build_fixture(board)
            archive = PACKAGER.package_firmware(self.root, board)
            archives.append(archive)
            with zipfile.ZipFile(archive) as bundle:
                manifest = json.loads(bundle.read(f"{board}/manifest.json"))
                self.assertEqual(manifest["board"]["id"], board)
                self.assertEqual(manifest["firmware_version"], "0.5.0")
                self.assertEqual({image["offset"] for image in manifest["images"]},
                                 {"0x0", "0x8000", "0x10000"})
                for image in manifest["images"]:
                    self.assertIn(board, image["file"])
                    data = bundle.read(f"{board}/{image['file']}")
                    self.assertEqual(image["size"], len(data))
                    self.assertEqual(image["sha256"], hashlib.sha256(data).hexdigest())
                self.assertIn("--flash_size 16MB", bundle.read(f"{board}/flash_args").decode())
        self.assertNotEqual(*archives)
        self.assertTrue(all(archive.exists() for archive in archives))

    def test_aec_manifest_matches_each_hardware_profile_and_actual_build_option(self):
        for board, reference in ((BOARDS[0], "es8311-digital"),
                                 (BOARDS[1], "es7210-analog")):
            build = self.build_fixture(board)
            config_path = build / "config/sdkconfig.json"
            config = json.loads(config_path.read_text())
            # Repackage the same target in both directions to catch stale
            # capability metadata surviving an export over an earlier build.
            for enabled in (True, False):
                with self.subTest(board=board, enabled=enabled):
                    config_path.write_text(json.dumps(config | {"JUFF_VOICE_BARGE_IN": enabled}))
                    archive = PACKAGER.package_firmware(self.root, board)
                    with zipfile.ZipFile(archive) as bundle:
                        manifest = json.loads(bundle.read(f"{board}/manifest.json"))
                        features = manifest["features"]
                        self.assertIs(features["voice_interrupt"], enabled)
                        self.assertIs(features["acoustic_echo_cancellation"], enabled)
                        self.assertEqual(features["aec_reference"], reference if enabled else "none")

    def test_wrong_hardware_or_private_config_cannot_be_packaged(self):
        board = BOARDS[1]
        build = self.build_fixture(board)
        config_path = build / "config/sdkconfig.json"
        config = json.loads(config_path.read_text())
        cases = ({"JUFF_BOARD_WAVESHARE_LCD_35": True}, {"JUFF_DEVICE_TOKEN": "private-test-token"})
        for changes in cases:
            with self.subTest(changes=changes):
                config_path.write_text(json.dumps(config | changes))
                with self.assertRaises(ValueError):
                    PACKAGER.package_firmware(self.root, board)
                self.assertFalse((self.root / "dist").exists())
        config_path.write_text(json.dumps(config))
        (build / "app.bin").write_bytes(b"wrong hardware binary\0")
        with self.assertRaisesRegex(ValueError, "hardware ID"):
            PACKAGER.package_firmware(self.root, board)
        self.assertFalse((self.root / "dist").exists())

    def test_licenses_follow_linked_components_and_include_binary_runtime_notices(self):
        board = BOARDS[1]
        self.build_fixture(board)
        archive = PACKAGER.package_firmware(self.root, board)
        with zipfile.ZipFile(archive) as package:
            names = {name.removeprefix(board + "/") for name in package.namelist()}
            expected = {"LICENSE", "THIRD_PARTY_NOTICES.md", "licenses/inventory.json",
                        "licenses/esp-idf/LICENSE", "licenses/managed/lvgl__lvgl/LICENCE.txt",
                        "licenses/managed/espressif__esp-sr/LICENSE", "licenses/managed/espressif__esp-sr/WEBRTC_LICENSE",
                        "licenses/esp-idf/components/bt/host/nimble/nimble/NOTICE",
                        "licenses/esp-idf/components/esp_phy/lib/LICENSE",
                        "licenses/toolchain/gcc/COPYING.RUNTIME"}
            self.assertTrue(expected <= names, expected - names)
            inventory = package.read(f"{board}/licenses/inventory.json").decode()
            self.assertNotIn(str(self.root), inventory)
            self.assertNotIn("unused", inventory)
            self.assertEqual(len(json.loads(inventory)["archives"]), 7)
            for name in names:
                if name.startswith("licenses/") and name.endswith(".txt"):
                    self.assertNotIn(str(self.root).encode(), package.read(f"{board}/{name}"))

    def test_missing_licenses_fail_before_overwriting_an_existing_export(self):
        board = BOARDS[1]
        build = self.build_fixture(board)
        archive = PACKAGER.package_firmware(self.root, board)
        original = archive.read_bytes()
        original_app = next((archive.parent / board).glob("*-app.bin")).read_bytes()
        for missing in (self.root / "LICENSE", self.root / "idf-source/components/bt/host/nimble/nimble/NOTICE",
                        self.root / "firmware/managed_components/lvgl__lvgl/LICENCE.txt",
                        self.root / "idf-source/components/esp_phy/lib/LICENSE",
                        self.root / "licenses/espressif__esp-sr/WEBRTC_LICENSE",
                        self.root / "toolchain/share/licenses/gcc/COPYING.RUNTIME"):
            with self.subTest(missing=missing.name):
                data = missing.read_bytes()
                missing.unlink()
                try:
                    with self.assertRaises((ValueError, FileNotFoundError)):
                        PACKAGER.package_firmware(self.root, board)
                    self.assertEqual(archive.read_bytes(), original)
                    self.assertEqual(next((archive.parent / board).glob("*-app.bin")).read_bytes(), original_app)
                finally:
                    missing.write_bytes(data)

    def test_unknown_archive_and_mismatched_supplement_are_rejected(self):
        board = BOARDS[1]
        build = self.build_fixture(board)
        provenance = self.root / "licenses/espressif__esp-sr/provenance.json"
        data = provenance.read_bytes()
        info = json.loads(data)
        info["component_hash"] = "different-build"
        provenance.write_text(json.dumps(info))
        with self.assertRaisesRegex(ValueError, "do not match"):
            PACKAGER.package_firmware(self.root, board)
        provenance.write_bytes(data)
        unknown = self.root / "unknown.a"
        unknown.write_bytes(b"unreviewed archive")
        linkmap = build / "fixture.map"
        linkmap.write_text(linkmap.read_text().replace("Allocating common symbols", f"{unknown}(unknown.o)\nAllocating common symbols"))
        with self.assertRaisesRegex(ValueError, "Unreviewed linked archive"):
            PACKAGER.package_firmware(self.root, board)
        self.assertFalse((self.root / "dist").exists())

    def test_license_file_and_parent_symlinks_cannot_export_private_configuration(self):
        board = BOARDS[1]
        self.build_fixture(board)
        archive = PACKAGER.package_firmware(self.root, board)
        def snapshot():
            return {path.relative_to(archive.parent).as_posix(): path.read_bytes()
                    for path in archive.parent.rglob("*") if path.is_file()}
        before = snapshot()
        private = self.root / "private"
        private.mkdir()
        config = private / ".env"
        config.write_text("PRIVATE_CONFIGURATION_FIXTURE\n")
        license_path = self.root / "firmware/managed_components/lvgl__lvgl/LICENCE.txt"
        original = license_path.read_bytes()
        license_path.unlink()
        license_path.symlink_to(config)
        try:
            with self.assertRaisesRegex(ValueError, "Symlink"):
                PACKAGER.package_firmware(self.root, board)
            self.assertEqual(snapshot(), before)
        finally:
            license_path.unlink()
            license_path.write_bytes(original)
        # Test ancestor directories too: checking only the final file is insufficient.
        for source in (self.root / "idf-source/components/bt/host/nimble/nimble",
                       self.root / "licenses/espressif__esp-sr"):
            with self.subTest(parent=source.name):
                moved = private / source.name
                source.rename(moved)
                source.symlink_to(moved, target_is_directory=True)
                try:
                    with self.assertRaisesRegex(ValueError, "Symlink"):
                        PACKAGER.package_firmware(self.root, board)
                    self.assertEqual(snapshot(), before)
                finally:
                    source.unlink()
                    moved.rename(source)

    def test_license_named_source_files_are_not_copied_whole(self):
        board = BOARDS[1]
        build = self.build_fixture(board)
        component = self.root / "idf-source/components/core"
        helper = component / "license_helper.c"
        helper.write_text("/* Copyright Fixture Helper Author. */\nint source_body_should_not_ship = 1;\n")
        for name in ("LICENSE.c", "LICENSE_helper.py", "COPYING.cpp", "NOTICE.h"):
            (component / name).write_text("int source_body_should_not_ship = 1;\n")
        description_path = build / "project_description.json"
        description = json.loads(description_path.read_text())
        description["build_component_info"]["core"]["sources"].append(str(helper))
        description_path.write_text(json.dumps(description))
        archive = PACKAGER.package_firmware(self.root, board)
        with zipfile.ZipFile(archive) as package:
            names = package.namelist()
            self.assertFalse(any(name.endswith((".c", ".cpp", ".h", ".py")) for name in names))
            notices = package.read(f"{board}/licenses/esp-idf/components/core/SOURCE_NOTICES.txt")
            self.assertIn(b"Copyright Fixture Helper Author", notices)
            self.assertNotIn(b"source_body_should_not_ship", notices)
            self.assertFalse(any(b"source_body_should_not_ship" in package.read(name)
                                 for name in names if "/licenses/" in name))

    def test_sensitive_values_in_notice_text_fail_before_export(self):
        board = BOARDS[1]
        self.build_fixture(board)
        license_path = self.root / "firmware/managed_components/lvgl__lvgl/LICENCE.txt"
        for private_text in (f"JUFF_DEVICE_{'TOKEN'}=fixture-only-secret", "sk-" + "x" * 24,
                             '{"PASSWORD": "fixture-only-secret"}', "-----BEGIN " + "PRIVATE KEY-----"):
            with self.subTest(kind=private_text.split("=")[0][:15]):
                license_path.write_text("Copyright Fixture Author\n" + private_text + "\n")
                with self.assertRaisesRegex(ValueError, "Private configuration or credential"):
                    PACKAGER.package_firmware(self.root, board)
                self.assertFalse((self.root / "dist").exists())

    def test_source_header_alias_must_stay_inside_its_component(self):
        board = BOARDS[1]
        self.build_fixture(board)
        component = self.root / "idf-source/components/core"
        original = component / "original.h"
        original.write_text("/* Copyright Internal Header Author. */\nint do_not_copy_header_body;\n")
        alias = component / "alias.h"
        alias.symlink_to(original)
        archive = PACKAGER.package_firmware(self.root, board)
        previous = archive.read_bytes()
        with zipfile.ZipFile(archive) as package:
            notice = package.read(f"{board}/licenses/esp-idf/components/core/SOURCE_NOTICES.txt")
            self.assertIn(b"Copyright Internal Header Author", notice)
            self.assertNotIn(b"do_not_copy_header_body", notice)
        outside = self.root / "private-source.h"
        outside.write_text("/* Copyright PRIVATE_CONFIGURATION_FIXTURE */\n")
        alias.unlink()
        alias.symlink_to(outside)
        with self.assertRaisesRegex(ValueError, "Symlink"):
            PACKAGER.package_firmware(self.root, board)
        self.assertEqual(archive.read_bytes(), previous)


if __name__ == "__main__":
    unittest.main()
