"""Replay actual firmware notification traces through the Mac frame assembler."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

import test_ble_microphone


ROOT = Path(__file__).resolve().parents[2]


class MacOSMicrophoneTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("swiftc")
        if compiler is None:
            raise unittest.SkipTest("Swift compiler unavailable on this host")
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        output = Path(cls.temporary.name)
        cls.binary = output / "microphone-tests"
        result = subprocess.run([
            compiler, "-swift-version", "5", "-module-cache-path", str(output / "modules"),
            str(ROOT / "macos/JuffBLE/Sources/MicrophoneTransport.swift"),
            str(ROOT / "macos/JuffBLE/Tests/MicrophoneTransportTests.swift"),
            "-o", str(cls.binary)], capture_output=True, text=True, timeout=120)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        # Reuse the extraction/build fixture, not a second implementation of BLE.
        cls.sender = type("WireSender", (test_ble_microphone.BLEMicrophoneTest,), {})
        cls.addClassCleanup(cls.sender.doClassCleanups)
        cls.sender.setUpClass()

    def test_legacy_fragmentation_and_reconnect(self):
        result = subprocess.run([str(self.binary), "legacy"], capture_output=True,
                                text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_firmware_mute_and_failed_frame_cannot_contaminate_next_mac_frame(self):
        for case in ("trace-mute", "trace-failure"):
            with self.subTest(case=case):
                wire = subprocess.run([str(self.sender.binary), case], capture_output=True,
                                      text=True, timeout=10)
                self.assertEqual(wire.returncode, 0, wire.stdout + wire.stderr)
                result = subprocess.run([str(self.binary), "wire-trace"], input=wire.stdout,
                                        capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
