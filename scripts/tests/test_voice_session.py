"""Run the production conversation controller with a fake clock and NVS."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class VoiceSessionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        out = Path(cls.temp.name)
        (out / "esp_err.h").write_text("#pragma once\ntypedef int esp_err_t;\n")
        source = (ROOT / "firmware/main/voice_session.c").read_text()
        (out / "session.inc").write_text(re.sub(r'^#include.*\n', '', source, flags=re.M))
        cls.binary = out / "session"
        result = subprocess.run([shutil.which("cc") or "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-parameter", "-fsanitize=address,undefined", "-I", str(out),
            "-I", str(ROOT / "firmware/main"),
            str(Path(__file__).with_name("voice_session_harness.c")),
            "-o", str(cls.binary)], capture_output=True, text=True)
        if result.returncode: raise AssertionError(result.stdout + result.stderr)

    def case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_three_minute_boundary_requires_a_new_wake(self): self.case("timeout")
    def test_speech_and_playback_extend_the_idle_deadline(self): self.case("activity")
    def test_continuous_mode_never_times_out(self): self.case("continuous")
    def test_boot_disconnect_and_reconnect_require_wake(self): self.case("reconnect")
    def test_stale_or_malformed_events_cannot_restore_upload(self): self.case("stale")
    def test_failed_save_or_missing_model_keeps_the_existing_mode(self): self.case("persistence")


if __name__ == "__main__": unittest.main()
