"""Exercise the production battery estimator with deterministic input traces."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class BatteryEstimateTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if compiler is None:
            raise AssertionError("A C compiler is required for battery tests")
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.binary = Path(cls.temporary.name) / "battery_estimate"
        result = subprocess.run(
            [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-fsanitize=address,undefined", "-I", str(ROOT / "firmware/main"),
             str(Path(__file__).with_name("battery_estimate_harness.c")),
             "-o", str(cls.binary)], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True,
                                text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_voltage_curve_is_monotonic_and_bounded(self):
        self.run_case("curve")

    def test_valid_voltage_always_displays_an_immediate_one_percent_number(self):
        self.run_case("continuous-numbers")

    def test_invalid_or_uncalibrated_readings_clear_history_and_recover_immediately(self):
        self.run_case("invalid")

    def test_charge_edges_preserve_the_number_then_use_the_first_fresh_voltage(self):
        self.run_case("charging-transition")

    def test_one_isolated_high_or_low_pulse_does_not_change_the_estimate(self):
        self.run_case("isolated-pulses")

    def test_sustained_changes_follow_within_two_samples_without_monotonic_clamping(self):
        self.run_case("sustained-changes")

    def test_sample_gaps_reset_history_and_duplicate_timestamps_are_ignored(self):
        self.run_case("gaps-duplicates")

    def test_filter_and_gap_detection_tolerate_uint32_wrap(self):
        self.run_case("clock-wrap")

    def test_zero_and_full_estimates_recover_when_voltage_leaves_the_endpoint(self):
        self.run_case("endpoint-recovery")

    def test_charging_edges_bounce_startup_and_clock_wrap(self):
        self.run_case("charge")


class BatteryMonitorDriverTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if compiler is None:
            raise AssertionError("A C compiler is required for battery tests")
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        output = Path(cls.temporary.name)
        # The harness defines the fake boundary; compile the real monitor C in
        # the same translation unit so its private worker can run deterministically.
        for name in ("sdkconfig.h", "esp_err.h", "driver/gpio.h", "esp_adc/adc_cali.h",
                     "esp_adc/adc_cali_scheme.h", "esp_adc/adc_oneshot.h", "esp_log.h",
                     "esp_timer.h", "freertos/FreeRTOS.h", "freertos/task.h"):
            header = output / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text("#pragma once\n")
        cls.binaries = {}
        for small in (False, True):
            binary = output / f"battery_monitor_{int(small)}"
            result = subprocess.run(
                [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-Wno-unused-parameter", "-Wno-unused-variable", "-Wno-unused-function",
                 "-fsanitize=address,undefined",
                 f"-DCONFIG_JUFF_BOARD_WAVESHARE_LCD_154={int(small)}",
                 f"-DCONFIG_JUFF_BOARD_WAVESHARE_LCD_35={int(not small)}",
                 "-I", str(output), "-I", str(ROOT / "firmware/main"),
                 str(Path(__file__).with_name("battery_monitor_harness.c")),
                 "-o", str(binary)], capture_output=True, text=True)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
            cls.binaries[small] = binary

    def run_case(self, case, small=True):
        result = subprocess.run([str(self.binaries[small]), case], capture_output=True,
                                text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_samples_are_trimmed_calibrated_scaled_and_rate_limited(self):
        self.run_case("sampling")

    def test_read_calibration_and_range_failures_invalidate_then_recover(self):
        self.run_case("measurement-failures")

    def test_charge_edges_preserve_number_until_fresh_voltage_sample(self):
        self.run_case("charge-transition")

    def test_initialization_failures_release_resources_without_fake_percentage(self):
        for case in ("gpio-failure", "adc-failure", "channel-failure",
                     "calibration-failure", "task-failure"):
            with self.subTest(case=case):
                self.run_case(case)

    def test_stop_during_adc_prevents_more_reads_calibration_and_publication(self):
        self.run_case("stop-in-read")

    def test_stop_before_init_is_permanent(self):
        self.run_case("stop-before-init")

    def test_large_board_never_initializes_battery_hardware(self):
        self.run_case("unsupported", small=False)


if __name__ == "__main__":
    unittest.main()
