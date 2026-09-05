"""Exercise the production codec writer's transport/cancellation boundary."""

from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from test_voice_interrupt import function


ROOT = Path(__file__).resolve().parents[2]


class AudioCodecWriteTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if compiler is None:
            raise AssertionError("A C compiler is required for codec write tests")
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        output = Path(cls.temporary.name)
        source = (ROOT / "firmware/main/audio_io.c").read_text()
        constants = re.search(r"#define CODEC_WRITE_CHUNK_FRAMES .*?#endif", source, re.S).group()
        context = re.search(r"typedef struct \{\s*uint32_t generation;\s*int64_t deadline_us;"
                            r"\s*\} codec_write_context_t;", source).group()
        (output / "codec_write_config.inc").write_text(constants)
        (output / "codec_write.inc").write_text(context + "\n\n" + "\n\n".join(
            function(source, name) for name in ("codec_write_generation", "cancel_codec_writes",
                "write_i2s_bytes", "reset_codec_tx_locked", "write_codec_pcm_mono")))
        cls.executables = {}
        for small in (False, True):
            for aec in (False, True):
                executable = output / f"audio_codec_write_{int(small)}_{int(aec)}"
                result = subprocess.run(
                    [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                     "-Wno-unused-parameter", "-fsanitize=address,undefined",
                     f"-DCONFIG_JUFF_BOARD_WAVESHARE_LCD_154={int(small)}",
                     f"-DCONFIG_JUFF_BOARD_WAVESHARE_LCD_35={int(not small)}",
                     f"-DCONFIG_JUFF_VOICE_BARGE_IN={int(aec)}",
                     "-I", str(output), "-I", str(ROOT / "firmware/main"),
                     str(Path(__file__).with_name("audio_codec_write_harness.c")),
                     "-o", str(executable)], capture_output=True, text=True)
                if result.returncode:
                    raise AssertionError(result.stdout + result.stderr)
                cls.executables[small, aec] = executable

    def run_scenario(self, small, aec, scenario):
        result = subprocess.run([str(self.executables[small, aec]), scenario],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Codec write boundary passed", result.stdout)

    def test_only_large_aec_build_emits_two_identical_slots(self):
        for small in (False, True):
            for aec in (False, True):
                with self.subTest(small=small, aec=aec):
                    self.run_scenario(small, aec, "format")

    def test_timeout_with_partial_progress_resumes_at_exact_byte(self):
        for small in (False, True):
            with self.subTest(small=small):
                self.run_scenario(small, True, "partial-timeout")

    def test_cancellation_and_deadline_mute_and_reset_partial_frame_before_unlock(self):
        for scenario in ("cancel", "deadline", "timeout-empty"):
            with self.subTest(scenario=scenario):
                self.run_scenario(False, True, scenario)

    def test_stale_generation_and_unavailable_mutex_do_not_start_a_write(self):
        for scenario in ("stale", "mutex"):
            with self.subTest(scenario=scenario):
                self.run_scenario(False, True, scenario)


if __name__ == "__main__":
    unittest.main()
