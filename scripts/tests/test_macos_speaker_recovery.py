"""Pass the real Mac failure-path messages through the firmware session gate."""
from pathlib import Path
import json
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class MacOSSpeakerRecoveryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("swiftc")
        if compiler is None:
            raise unittest.SkipTest("Swift compiler unavailable on this host")
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        out = Path(cls.temp.name)
        source = (ROOT / "macos/JuffBLE/Sources/main.swift").read_text()
        # Compile the production serializer and failure handler; only Bluetooth
        # writes and activity notifications are replaced by the test harness.
        normalize = source[source.index("private func normalizedMessage("):source.index("private func chunks(")]
        failure = source[source.index("    private func failSpeakerStream("):source.index("    private func scheduleSpeakerWatchdog(")]
        harness = (ROOT / "macos/JuffBLE/Tests/SpeakerRecoveryTests.swift").read_text()
        swift = out / "Recovery.swift"
        swift.write_text(harness.replace("// PRODUCTION_NORMALIZER", normalize)
                         .replace("    // PRODUCTION_FAILURE_HANDLER", failure))
        cls.swift = out / "mac-recovery"
        cls.compile([compiler, "-swift-version", "5", "-module-cache-path", str(out / "modules"),
                     str(ROOT / "macos/JuffBLE/Sources/SpeakerTransport.swift"), str(swift),
                     "-o", str(cls.swift)])
        (out / "esp_err.h").write_text("#pragma once\ntypedef int esp_err_t;\n")
        session = (ROOT / "firmware/main/voice_session.c").read_text()
        (out / "session.inc").write_text(re.sub(r'^#include.*\n', '', session, flags=re.M))
        cls.firmware = out / "device-recovery"
        cls.compile([shutil.which("cc") or "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                     "-Wno-unused-parameter", "-fsanitize=address,undefined", "-I", str(out),
                     "-I", str(ROOT / "firmware/main"),
                     str(ROOT / "scripts/tests/speaker_recovery_harness.c"), "-o", str(cls.firmware)])

    @staticmethod
    def compile(arguments):
        result = subprocess.run(arguments, capture_output=True, text=True, timeout=120)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_program(self, binary, *arguments):
        result = subprocess.run([str(binary), *arguments], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout.strip()

    def recovery(self, generation, reason="speaker-flow-timeout"):
        output = self.run_program(self.swift, generation, reason)
        return json.loads(output)

    def check_device(self, mode, event, accepted):
        self.assertEqual(event["type"], "playback.clear")
        self.assertEqual(event["reason"], "speaker-flow-failed")
        self.run_program(self.firmware, mode, event["type"],
                         str(event.get("conversationGeneration", "none")), str(int(accepted)))

    def test_all_failure_reasons_recover_acknowledged_continuous_and_wake_sessions(self):
        for mode in ("continuous", "wake"):
            generation = self.run_program(self.firmware, mode, "generation")
            for reason in ("speaker-flow-timeout", "speaker-overrun", "speaker-credit-invalid",
                           "speaker-begin-rejected", "speaker-end-rejected"):
                with self.subTest(mode=mode, reason=reason):
                    self.check_device(mode, self.recovery(generation, reason), True)

    def test_old_recovery_cannot_cancel_a_new_conversation(self):
        for mode in ("continuous", "wake"):
            generation = int(self.run_program(self.firmware, mode, "generation"))
            self.check_device(mode, self.recovery(str(generation - 1)), False)

    def test_legacy_recovery_remains_untagged_but_cannot_bypass_a_handshake(self):
        event = self.recovery("none")
        self.assertNotIn("conversationGeneration", event)
        self.check_device("legacy", event, True)
        for mode in ("continuous", "wake"):
            self.check_device(mode, event, False)

    def test_wrapped_generation_is_serialized_without_truncation(self):
        for generation in (1, 0xffffffff):
            self.assertEqual(self.recovery(str(generation))["conversationGeneration"], generation)


if __name__ == "__main__":
    unittest.main()
