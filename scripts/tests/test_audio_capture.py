"""Run actual capture routing for both boards with a known MIC/REF stream.

The codec boundary is fake; the production task performs channel extraction,
half-duplex fallback, and resampling. No microphone or real AEC is required.
"""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_voice_interrupt import function


ROOT = Path(__file__).resolve().parents[2]


class AudioCaptureTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if compiler is None:
            raise AssertionError("A C compiler is required for capture routing tests")
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        output = Path(cls.temporary.name)
        (output / "esp_err.h").write_text("typedef int esp_err_t;\n")
        source = (ROOT / "firmware/main/audio_io.c").read_text()
        (output / "capture_task.inc").write_text(function(source, "capture_task"))
        (output / "resample.inc").write_text(function(source, "resample_24k_to_16k"))
        cls.executables = {}
        for small in (False, True):
            for aec in (False, True):
                executable = output / f"audio_capture_{int(small)}_{int(aec)}"
                result = subprocess.run(
                    [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                     "-Wno-unused-parameter", "-Wno-unused-variable", "-Wno-unused-function",
                     "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                     f"-DCONFIG_JUFF_BOARD_WAVESHARE_LCD_154={int(small)}",
                     f"-DCONFIG_JUFF_BOARD_WAVESHARE_LCD_35={int(not small)}",
                     f"-DCONFIG_JUFF_VOICE_BARGE_IN={int(aec)}",
                     "-I", str(output), "-I", str(ROOT / "firmware/main"),
                     str(Path(__file__).with_name("audio_capture_harness.c")),
                     "-o", str(executable)], capture_output=True, text=True)
                if result.returncode:
                    raise AssertionError(result.stdout + result.stderr)
                cls.executables[small, aec] = executable

    def run_scenario(self, small, aec, scenario):
        result = subprocess.run([str(self.executables[small, aec]), scenario],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Capture routing passed", result.stdout)

    def test_allocation_fallback_uploads_only_resampled_mic_without_reference(self):
        for small in (False, True):
            with self.subTest(small=small):
                self.run_scenario(small, True, "fallback")

    def test_fallback_respects_playback_mute_and_missing_callback(self):
        for small in (False, True):
            for aec in (False, True):
                for scenario in ("fallback-playing", "fallback-muted", "fallback-no-callback"):
                    with self.subTest(small=small, aec=aec, scenario=scenario):
                        self.run_scenario(small, aec, scenario)

    def test_aec_receives_both_channels_with_playback_and_detection_gates(self):
        for small in (False, True):
            for scenario in ("frontend", "frontend-muted", "frontend-diagnostic", "frontend-no-allowed"):
                with self.subTest(small=small, scenario=scenario):
                    self.run_scenario(small, True, scenario)

    def test_aec_disabled_build_keeps_mono_microphone_upload(self):
        for small in (False, True):
            with self.subTest(small=small):
                self.run_scenario(small, False, "fallback")


if __name__ == "__main__":
    unittest.main()
