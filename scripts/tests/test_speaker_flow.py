"""Verify the firmware's cumulative speaker-credit state without BLE hardware."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class SpeakerFlowTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if compiler is None:
            raise AssertionError("A C compiler is required for speaker-flow tests")
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.binary = Path(cls.temporary.name) / "speaker_flow"
        result = subprocess.run(
            [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-fsanitize=address,undefined", "-I", str(ROOT / "firmware/main"),
             str(Path(__file__).with_name("speaker_flow_harness.c")),
             "-o", str(cls.binary)], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True,
                                text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_first_pcm_requires_a_nonzero_stream_and_available_credit(self):
        self.run_case("initial-credit")

    def test_only_successfully_committed_packets_advance_received_bytes(self):
        self.run_case("transactional-commit")

    def test_duplicate_offsets_gaps_and_window_overruns_are_rejected(self):
        self.run_case("packet-bounds")

    def test_repeated_absolute_credit_does_not_mint_or_revoke_allowance(self):
        self.run_case("absolute-credit")

    def test_late_audio_and_replayed_begin_cannot_spend_a_new_streams_credit(self):
        self.run_case("stream-fence")

    def test_draining_rejects_more_pcm_and_preserves_received_for_done_ack(self):
        self.run_case("draining")

    def test_counters_and_stream_ids_cannot_wrap(self):
        self.run_case("counter-overflow")

    def test_invalid_pointers_do_not_mutate_state_or_crash(self):
        self.run_case("null-inputs")


if __name__ == "__main__":
    unittest.main()
