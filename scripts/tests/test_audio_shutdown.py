"""Exercise permanent audio shutdown against the actual production functions."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_voice_interrupt import function
from audio_playback_fixture import write_playback_declarations


ROOT = Path(__file__).resolve().parents[2]


class AudioShutdownTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        output = Path(cls.temporary.name)
        (output / "esp_err.h").write_text("#pragma once\ntypedef int esp_err_t;\n")
        source = (ROOT / "firmware/main/audio_io.c").read_text()
        write_playback_declarations(output, source, (ROOT / "firmware/main/audio_io.h").read_text())
        names = ("free_item", "enqueue_item", "codec_status", "set_amplifier_enabled",
                 "deliver_captured_pcm", "request_voice_interrupt", "audio_io_shutdown",
                 "audio_io_set_capture_enabled", "audio_io_begin_response", "codec_write_generation",
                 "push_pcm_for_generation", "audio_io_push_pcm",
                 "audio_io_end_response", "audio_io_is_playing", "audio_io_is_available",
                 "audio_io_supports_voice_barge_in")
        (output / "shutdown.inc").write_text("\n\n".join(function(source, name) for name in names))
        cls.executable = output / "audio_shutdown"
        result = subprocess.run(
            [shutil.which("cc") or "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-Wno-unused-parameter", "-Wno-unused-variable", "-fsanitize=address,undefined",
             "-I", str(output), str(Path(__file__).with_name("audio_shutdown_harness.c")),
             "-o", str(cls.executable)], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def test_shutdown_blocks_late_transport_audio_and_reports_hardware_failures(self):
        for scenario in ("normal", "writer-busy", "pa-failure", "mute-failure"):
            with self.subTest(scenario=scenario):
                result = subprocess.run([str(self.executable), scenario], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("Audio shutdown passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
