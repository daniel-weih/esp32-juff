"""Compile the real Swift flow-control state machine without Bluetooth or audio."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class MacOSSpeakerFlowTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("swiftc")
        if compiler is None:
            raise unittest.SkipTest("Swift compiler unavailable on this host")
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        output = Path(cls.temporary.name)
        cls.binary = output / "speaker-flow-tests"
        result = subprocess.run(
            [compiler, "-swift-version", "5", "-module-cache-path", str(output / "modules"),
             str(ROOT / "macos/JuffBLE/Sources/SpeakerTransport.swift"),
             str(ROOT / "macos/JuffBLE/Tests/SpeakerTransportTests.swift"),
             "-o", str(cls.binary)], capture_output=True, text=True, timeout=120)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True,
                                text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("Speaker transport regression passed", result.stdout)

    def test_capability_detection_and_legacy_pacing(self):
        self.run_case("legacy")

    def test_credit_window_header_offsets_and_corebluetooth_backpressure(self):
        self.run_case("window")

    def test_response_order_end_receive_ack_and_terminal_credit(self):
        self.run_case("ordering")

    def test_clear_and_reconnect_reject_stale_credit_and_submitted_packet_generation(self):
        self.run_case("clear")

    def test_invalid_credit_cannot_release_more_audio(self):
        self.run_case("invalid")

    def test_no_progress_timeout_and_lost_terminal_event_recovery(self):
        self.run_case("timeout")

    def test_bounded_queue_and_legacy_end_order(self):
        self.run_case("limits")

    def test_stale_audio_is_ignored_but_current_queue_overflow_is_fatal(self):
        self.run_case("overflow")

    def test_audio_fits_one_link_packet_without_gaps_or_credit_overrun(self):
        self.run_case("link-packet")


if __name__ == "__main__":
    unittest.main()
