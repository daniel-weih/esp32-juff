"""Execute the production PCM writer with a byte-counting transport fake.

These checks cover the ES8311 wire format and short-write handling; they do not
simulate the codec's echo path or claim acoustic echo-cancellation performance.
"""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class AudioPcmFormatTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if compiler is None:
            raise AssertionError("A C compiler is required for PCM transport tests")
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.executable = Path(cls.temporary.name) / "audio_pcm_format"
        result = subprocess.run(
            [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
             "-I", str(ROOT / "firmware/main"),
             str(Path(__file__).with_name("audio_pcm_format_harness.c")),
             "-o", str(cls.executable)], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_scenario(self, scenario):
        result = subprocess.run([str(self.executable), scenario],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PCM wire format passed", result.stdout)

    def test_stereo_duplicates_every_sample_across_chunk_and_short_write_boundaries(self):
        self.run_scenario("stereo")

    def test_mono_preserves_original_bytes_with_unaligned_input_and_short_writes(self):
        self.run_scenario("mono")

    def test_invalid_sizes_and_buffers_never_reach_transport(self):
        self.run_scenario("invalid")

    def test_failed_zero_and_oversized_transport_writes_stop_without_retrying_forever(self):
        self.run_scenario("transport-errors")


if __name__ == "__main__":
    unittest.main()
