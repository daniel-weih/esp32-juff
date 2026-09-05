"""Execute the actual BLE microphone sender with a 100 Hz RTOS and NimBLE fakes."""

from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from test_voice_interrupt import function


ROOT = Path(__file__).resolve().parents[2]


class BLEMicrophoneTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if compiler is None:
            raise AssertionError("A C compiler is required")
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        output = Path(cls.temporary.name)
        source = (ROOT / "firmware/main/ble_manager.c").read_text()
        constants = "\n".join(re.findall(r"(?m)^#define BLE_MIC_.*$", source))
        frame = re.search(r"typedef struct \{[^{}]*\} ble_audio_frame_t;", source)
        if frame is None:
            raise AssertionError("Missing microphone frame type")
        (output / "ble_microphone_config.h").write_text(constants + "\n" + frame.group())
        names = (
            "linear_pcm_to_mulaw", "set_link_state", "mark_connected",
            "snapshot_microphone_stream", "microphone_frame_is_current",
            "record_microphone_drop", "send_microphone_frame", "ble_manager_send_pcm",
        )
        (output / "ble_microphone.inc").write_text("\n\n".join(
            function(source, name) for name in names))
        cls.binary = output / "ble_microphone"
        result = subprocess.run([
            compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-I", str(output),
            str(Path(__file__).with_name("ble_microphone_harness.c")),
            "-o", str(cls.binary),
        ], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True,
                                text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_congestion_waits_real_ticks_and_retries_only_the_rejected_fragment(self):
        self.case("recover")

    def test_mbuf_allocation_failure_is_retried_without_leaks(self):
        self.case("allocation")

    def test_permanent_congestion_stops_at_200ms_including_tick_wraparound(self):
        self.case("timeout")
        self.case("wrap")

    def test_nontransient_error_does_not_wait_or_replay_the_accepted_prefix(self):
        self.case("fatal")

    def test_occasional_progress_cannot_extend_the_whole_frame_deadline(self):
        self.case("slow-progress")
        self.case("slow-progress-mtu23")

    def test_mute_resume_and_reused_connection_handle_cancel_old_frames(self):
        self.case("mute")
        self.case("reconnect")

    def test_ready_subscription_and_playback_changes_cancel_between_fragments(self):
        self.case("not-ready")
        self.case("unsubscribed")
        self.case("playing")

    def test_eight_history_frames_and_four_live_frames_fit_and_preserve_order(self):
        self.case("queue")

    def test_small_and_odd_mtu_payloads_are_unmodified_mulaw_bytes(self):
        self.case("mtu")

    def test_reset_boundary_must_succeed_before_any_new_pcm_and_has_bounded_retries(self):
        for case in ("reset-recover", "reset-timeout", "reset-mute"):
            with self.subTest(case=case): self.case(case)

    def test_partial_frame_discard_publishes_new_boundary_even_without_generation_change(self):
        self.case("trace-mute")
        self.case("trace-failure")


if __name__ == "__main__":
    unittest.main()
