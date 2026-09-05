"""Execute the actual BLE mu-law acceptance/queue path against a failing queue."""

from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from test_voice_interrupt import function


ROOT = Path(__file__).resolve().parents[2]


class BLESpeakerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if compiler is None:
            raise AssertionError("A C compiler is required for BLE speaker tests")
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        output = Path(cls.temporary.name)
        source = (ROOT / "firmware/main/ble_manager.c").read_text()
        definitions = []
        for name in ("BLE_SPEAKER_CHUNK_BYTES", "BLE_SPEAKER_BUFFER_BYTES"):
            match = re.search(r"(?m)^#define " + name + r" .+$", source)
            if match is None:
                raise AssertionError(f"Missing production speaker capacity: {name}")
            definitions.append(match.group())
        (output / "ble_speaker_config.h").write_text("\n".join(definitions) + "\n")
        (output / "ble_speaker.inc").write_text("\n\n".join(
            function(source, name) for name in (
                "mulaw_to_linear_pcm", "valid_flow_response", "mark_speaker_terminal",
                "notify_speaker_credit", "flush_speaker_locked", "append_speaker_data_locked",
                "speaker_read_le32", "append_speaker_mulaw", "begin_ble_speaker",
                "end_ble_speaker")))
        cls.binary = output / "ble_speaker"
        result = subprocess.run([
            compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-I", str(output),
            "-I", str(ROOT / "firmware/main"),
            str(Path(__file__).with_name("ble_speaker_harness.c")),
            "-o", str(cls.binary),
        ], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True,
                                text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.strip(), "BLE speaker production path passed")

    def test_little_endian_header_is_excluded_from_pcm_and_receipt_count(self):
        self.run_case("valid-frame")

    def test_old_or_unstarted_stream_packets_do_not_change_current_audio(self):
        self.run_case("stale-frame")

    def test_bad_header_offset_and_credit_fail_closed_without_acknowledgement(self):
        for case in ("bad-header", "bad-offset", "over-credit"):
            with self.subTest(case=case):
                self.run_case(case)

    def test_queue_failure_discards_staging_and_previously_enqueued_audio(self):
        self.run_case("queue-failure")

    def test_cancellation_during_split_decode_cannot_commit_or_leave_staged_audio(self):
        self.run_case("cancel-in-enqueue")

    def test_legacy_payload_stays_headerless_and_partial_tail_flushes(self):
        self.run_case("legacy")

    def test_initial_credit_counts_previous_response_queue_occupancy(self):
        self.run_case("initial-budget")

    def test_invalid_replayed_or_failed_begin_does_not_replace_current_flow(self):
        self.run_case("begin-rejection")

    def test_done_validates_identity_flushes_tail_then_queues_one_end_marker(self):
        self.run_case("done-sequence")

    def test_immediate_playback_completion_keeps_ended_instead_of_draining_or_cancelled(self):
        self.run_case("done-inline")

    def test_tail_or_end_marker_queue_failure_cancels_without_a_draining_ack(self):
        for case in ("done-tail-failure", "done-marker-failure"):
            with self.subTest(case=case):
                self.run_case(case)

    def test_periodic_credit_counts_staged_pcm_and_preserves_terminal_counters(self):
        self.run_case("credit-accounting")

    def test_receive_contention_cancels_only_matching_stream_and_next_tick_clears_audio(self):
        self.run_case("receive-contention")

    def test_concurrent_publishers_cannot_send_newer_credit_before_older_snapshot(self):
        self.run_case("credit-race")


if __name__ == "__main__":
    unittest.main()
