"""Run the production BLE status encoder and newline/MTU fragmentation."""

import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from test_voice_interrupt import function


ROOT = Path(__file__).resolve().parents[2]


class BLEStatusTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if compiler is None:
            raise AssertionError("A C compiler is required for BLE status tests")
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        output = Path(cls.temporary.name)
        source = (ROOT / "firmware/main/ble_manager.c").read_text()
        message_limit = re.search(r"(?m)^#define BLE_MESSAGE_MAX_BYTES \d+$", source)
        if message_limit is None:
            raise AssertionError("Missing production BLE message capacity")
        (output / "ble_status_config.h").write_text(message_limit.group() + "\n")
        (output / "esp_err.h").write_text("typedef int esp_err_t;\n")
        (output / "sdkconfig.h").write_text("#pragma once\n")
        (output / "ble_status.inc").write_text("\n\n".join(
            function(source, name) for name in
            ("build_status", "notify_text", "notify_current_status")))
        cls.binaries = {}
        for small in (False, True):
            binary = output / f"ble_status_{int(small)}"
            result = subprocess.run(
                [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-pthread",
                 f"-DCONFIG_JUFF_BOARD_WAVESHARE_LCD_154={int(small)}",
                 f"-DCONFIG_JUFF_BOARD_WAVESHARE_LCD_35={int(not small)}",
                 "-I", str(output), "-I", str(ROOT / "firmware/main"),
                 str(Path(__file__).with_name("ble_status_harness.c")),
                 "-o", str(binary)], capture_output=True, text=True)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
            cls.binaries[small] = binary

    def run_case(self, case, small=True):
        result = subprocess.run([str(self.binaries[small]), case], capture_output=True,
                                text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.count("\n"), 1)
        self.assertLess(len(result.stdout.encode()), 512)
        return json.loads(result.stdout)

    def test_small_board_publishes_exact_snapshot_without_extra_fields(self):
        status = self.run_case("valid")
        self.assertEqual(status["speakerFlow"], 1)
        self.assertEqual((status["phyTx"], status["phyRx"]), (2, 1))
        self.assertEqual(status["battery"], {
            "valid": True, "charging": False, "percent": 40, "millivolts": 3790})

    def test_charging_publishes_the_numeric_percentage_from_the_snapshot(self):
        self.assertEqual(self.run_case("charging")["battery"], {
            "valid": True, "charging": True, "percent": 66, "millivolts": 3930})

    def test_missing_percent_is_json_null_independently_of_charging(self):
        for case, charging in (("missing-percent", True), ("invalid", False)):
            with self.subTest(case=case):
                self.assertEqual(self.run_case(case)["battery"], {
                    "valid": False, "charging": charging,
                    "percent": None, "millivolts": 0})

    def test_empty_and_full_percentages_are_numbers(self):
        for case, percent in (("empty", 0), ("full", 100)):
            with self.subTest(case=case):
                value = self.run_case(case)["battery"]["percent"]
                self.assertIs(type(value), int)
                self.assertEqual(value, percent)

    def test_large_board_omits_battery(self):
        status = self.run_case("valid", small=False)
        self.assertNotIn("battery", status)
        self.assertEqual(status["speakerFlow"], 1)
        self.assertEqual((status["phyTx"], status["phyRx"]), (2, 1))

    def run_transport_case(self, case):
        result = subprocess.run([str(self.binaries[True]), case], capture_output=True,
                                text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout.splitlines()

    def test_concurrent_messages_hold_the_mutex_across_every_fragment(self):
        lines = self.run_transport_case("concurrent")
        self.assertEqual([json.loads(line) for line in lines], [
            {"type": "status", "payload": "A" * 64},
            {"type": "speaker.credit", "payload": "B" * 64},
        ])

    def test_partial_notification_failure_delimits_the_next_complete_message(self):
        for case in ("recover-notify", "recover-allocation"):
            with self.subTest(case=case):
                lines = self.run_transport_case(case)
                self.assertEqual(len(lines), 2)
                self.assertEqual(len(lines[0].encode()), 20)
                with self.assertRaises(json.JSONDecodeError):
                    json.loads(lines[0])
                self.assertEqual(json.loads(lines[1]), {
                    "type": "speaker.credit", "streamId": 17,
                    "received": 320, "limit": 96000})

    def test_invalid_input_contention_and_disconnect_release_or_skip_the_mutex(self):
        self.assertEqual(self.run_transport_case("rejected"), ['{"type":"status"}'])

    def test_maximum_status_fits_and_fragments_into_one_newline_delimited_message(self):
        for small in (False, True):
            with self.subTest(small=small):
                status = self.run_case("maximum", small=small)
                self.assertEqual(len(status["name"]), 15)
                self.assertEqual(len(status["firmware"]), 31)
                if small:
                    self.assertEqual(status["battery"]["percent"], 2147483647)


if __name__ == "__main__":
    unittest.main()
