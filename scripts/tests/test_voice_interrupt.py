"""Compile the firmware's actual routing/state functions against host-side fakes.

Only hardware, JSON decoding and rendering are replaced. This deliberately executes
the production command handler and touch/BOOT handlers, including their branches.
"""

from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    """Extract a complete C function, ignoring braces in strings and comments."""
    match = re.search(r"(?m)^(?:static )?[\w *]+\b" + re.escape(name)
                      + r"\([^;]*?\)\s*\{", source)
    if match is None:
        raise AssertionError(f"Missing production function: {name}")
    start = source.index("{", match.start())
    tokens = re.finditer(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\''
                         r'|//[^\n]*|/\*[\s\S]*?\*/|[{}]', source[start:])
    depth = 0
    for token in tokens:
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[match.start():start + token.end()]
    raise AssertionError(f"Unterminated production function: {name}")


class VoiceInterruptTest(unittest.TestCase):
    def test_active_sessions_route_interrupts_during_playback(self):
        compiler = shutil.which("cc")
        self.assertIsNotNone(compiler, "A C compiler is required for firmware routing tests")
        selections = {
            "ble_manager": ["snapshot_link_state", "snapshot_audio_state", "set_link_state",
                            "ble_manager_is_connected", "ble_manager_audio_is_connected",
                            "ble_manager_is_voice_active", "ble_manager_is_voice_ready",
                            "build_status", "ble_manager_send_interrupt",
                            "ble_manager_try_voice_interrupt", "ble_manager_send_playback_event",
                            "handle_command"],
            "device_client": ["device_client_is_voice_active", "device_client_is_ready",
                              "device_client_is_connected", "device_client_send_interrupt",
                              "device_client_try_voice_interrupt"],
            "board_display": ["primary_clicked"],
            "app_main": ["microphone_pcm", "send_interrupt"],
        }
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            (output / "esp_err.h").write_text("typedef int esp_err_t;\n")
            for module, names in selections.items():
                source = (ROOT / "firmware/main" / f"{module}.c").read_text()
                (output / f"{module}.inc").write_text(
                    "\n\n".join(function(source, name) for name in names))
            result = subprocess.run(
                [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-Wno-unused-parameter", "-I", str(output),
                 "-I", str(ROOT / "firmware/main"),
                 str(Path(__file__).with_name("voice_interrupt_harness.c")),
                 "-o", str(output / "voice_interrupt")],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(output / "voice_interrupt")],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("Voice interruption routing passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
