"""Execute the real playback task with fake queue/codec boundaries."""

from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from test_voice_interrupt import function


ROOT = Path(__file__).resolve().parents[2]


class PlaybackTimingTest(unittest.TestCase):
    def test_first_pcm_and_response_generation_are_published_at_codec_boundary(self):
        compiler = shutil.which("cc")
        self.assertIsNotNone(compiler, "A C compiler is required for playback timing tests")
        source = (ROOT / "firmware/main/audio_io.c").read_text()
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            (output / "esp_err.h").write_text("typedef int esp_err_t;\n")
            definitions = [re.search(r"typedef enum \{\s*AUDIO_ITEM_BEGIN,.*?"
                                     r"\} audio_item_type_t;", source, re.S).group(),
                           re.search(r"typedef struct \{\s*audio_item_type_t type;.*?"
                                     r"\} audio_item_t;", source, re.S).group()]
            (output / "audio_item.inc").write_text("\n".join(definitions))
            (output / "playback_state.inc").write_text("\n\n".join(
                function(source, name) for name in ["begin_frontend_playback",
                    "mark_frontend_pcm_started", "snapshot_frontend_playback",
                    "snapshot_frontend_playback_after_read", "frontend_rx_overflow",
                    "recover_frontend_rx_overflow"]))
            (output / "playback_task.inc").write_text(function(source, "playback_task"))
            executable = output / "playback_timing"
            result = subprocess.run(
                [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-Wno-unused-parameter", "-I", str(output),
                 "-I", str(ROOT / "firmware/main"),
                 str(Path(__file__).with_name("playback_timing_harness.c")),
                 "-o", str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("Playback timing passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
