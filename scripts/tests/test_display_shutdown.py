"""Run the actual display shutdown/brightness functions with a fake LEDC driver."""

from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from test_voice_interrupt import function


ROOT = Path(__file__).resolve().parents[2]


class DisplayShutdownTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if compiler is None:
            raise RuntimeError("A C compiler is required for display shutdown tests")
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        output = Path(cls.directory.name)
        source = (ROOT / "firmware/main/board_display.c").read_text()
        state = re.search(r"typedef enum \{\s*DISPLAY_RUNNING,.*?"
                          r"\} display_shutdown_state_t;", source, re.S)
        if state is None:
            raise AssertionError("Missing display shutdown state")
        variables = ("s_state_mutex", "s_shutdown_lock", "s_shutdown_state",
                     "s_backlight_initialized", "s_brightness")
        declarations = [state.group(0)]
        for name in variables:
            declaration = re.search(r"^static [^;\n]+\b" + name + r"\b[^;\n]*;",
                                    source, re.M)
            if declaration is None:
                raise AssertionError(f"Missing display state: {name}")
            declarations.append(declaration.group(0))
        (output / "display_state.inc").write_text("\n".join(declarations))
        names = ("display_shutdown_state", "set_brightness", "initialize_backlight",
                 "board_display_prepare_shutdown", "board_display_power_off",
                 "board_display_cycle_brightness")
        (output / "display_shutdown.inc").write_text(
            "\n\n".join(function(source, name) for name in names))
        cls.binary = output / "display_shutdown"
        result = subprocess.run(
            [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
             "-fsanitize=address,undefined", "-I", str(output),
             str(Path(__file__).with_name("display_shutdown_harness.c")),
             "-o", str(cls.binary)], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True,
                                text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_uninitialized_shutdown_never_touches_ledc(self):
        self.run_case("uninitialized")

    def test_prepare_latches_and_rejects_brightness_changes(self):
        self.run_case("prepare")

    def test_power_off_survives_lock_and_driver_failures(self):
        self.run_case("retry")

    def test_power_off_finishes_after_an_inflight_brightness_write(self):
        self.run_case("inflight")


if __name__ == "__main__":
    unittest.main()
