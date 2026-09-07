"""Verify real wake-word routing against a deterministic model interface."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class WakeWordTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        out = Path(cls.temp.name)
        (out / "esp_err.h").write_text("#pragma once\ntypedef int esp_err_t;\n")
        source = (ROOT / "firmware/main/wake_word.c").read_text()
        (out / "wake.inc").write_text(re.sub(r'^#include.*\n', '', source, flags=re.M))
        cls.binary = out / "wake"
        result = subprocess.run([shutil.which("cc") or "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-parameter", "-fsanitize=address,undefined", "-DCONFIG_JUFF_WAKE_WORD=1",
            "-I", str(out), "-I", str(ROOT / "firmware/main"),
            str(Path(__file__).with_name("wake_word_harness.c")), "-o", str(cls.binary)], capture_output=True, text=True)
        if result.returncode: raise AssertionError(result.stdout + result.stderr)

    def case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_both_channel_layouts_feed_only_resampled_microphone(self): self.case("routing")
    def test_detected_phrase_is_discarded_and_detector_stops_during_conversation(self): self.case("wake")
    def test_second_phrase_wakes_without_first_phrase(self): self.case("wake-second")
    def test_two_matches_create_only_one_conversation_and_both_rearm(self): self.case("wake-both")
    def test_playback_tail_diagnostics_and_unacknowledged_transport_cannot_wake(self): self.case("gates")
    def test_missing_model_does_not_open_the_microphone(self): self.case("missing")
    def test_missing_second_model_does_not_advertise_partial_support(self): self.case("missing-second")
    def test_different_model_frame_sizes_fail_closed(self): self.case("mismatch")
    def test_second_model_creation_failure_releases_first_model(self): self.case("second-create-failure")
    def test_activity_vad_does_not_extend_timer_for_low_energy_noise(self): self.case("activity")
    def test_model_reset_recreates_once_and_failure_keeps_standby_silent(self): self.case("reset")
    def test_second_model_reset_failure_releases_both_models(self): self.case("reset-second-failure")


if __name__ == "__main__": unittest.main()
