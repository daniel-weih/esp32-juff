"""Exercise the real power-key state machine with timed physical gestures."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class PowerButtonTest(unittest.TestCase):
    def test_long_press_release_bounce_startup_and_timer_wrap(self):
        compiler = shutil.which("cc")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "power_test.c"
            binary = Path(directory) / "power_test"
            source.write_text(r'''
#include <assert.h>
#include <limits.h>
#include "power_button.h"

static power_button_t button;
static uint32_t now;
static unsigned prepare_count, off_count;

static void sample(bool pressed, unsigned milliseconds) {
    for (unsigned i = 0; i < milliseconds; i += 10) {
        power_button_event_t event = power_button_update(&button, pressed, now);
        if (event == POWER_BUTTON_PREPARE) ++prepare_count;
        if (event == POWER_BUTTON_OFF) ++off_count;
        now += 10;
    }
}

static void reset(uint32_t start) {
    button = (power_button_t){0};
    now = start;
    prepare_count = off_count = 0;
}

int main(void) {
    // Startup can finish long after timer zero. A brief released bounce is not
    // enough to arm the key even if the first observation happens seconds later.
    reset(10000);
    sample(false, 20);
    sample(true, 10000);
    assert(prepare_count == 0 && off_count == 0);

    // The press used to turn on or wake must never become a shutdown gesture.
    reset(0);
    sample(true, 10000);
    assert(prepare_count == 0 && off_count == 0);
    sample(false, 100);
    sample(true, 3100);
    assert(prepare_count == 1 && off_count == 0);
    sample(true, 10000);
    assert(prepare_count == 1 && off_count == 0);
    sample(false, 100);
    assert(off_count == 1);
    sample(true, 10000);
    sample(false, 100);
    assert(prepare_count == 1 && off_count == 1);

    // Short presses never accumulate into a long press.
    reset(0);
    sample(false, 100);
    for (unsigned i = 0; i < 5; ++i) {
        sample(true, 2900);
        sample(false, 100);
    }
    assert(prepare_count == 0 && off_count == 0);

    // Contact bounce cannot arm a held startup key or prematurely cut power.
    reset(0);
    sample(true, 5000);
    sample(false, 20);
    sample(true, 5000);
    assert(prepare_count == 0);
    sample(false, 100);
    sample(true, 20);
    sample(false, 20);
    sample(true, 3100);
    assert(prepare_count == 1 && off_count == 0);
    sample(false, 20);
    sample(true, 100);
    assert(off_count == 0);
    sample(false, 100);
    assert(off_count == 1);

    // 3 seconds are measured from the debounced press, including uint32 wrap.
    reset(UINT32_MAX - 1000);
    sample(false, 100);
    sample(true, 3050);
    assert(prepare_count == 0);
    sample(true, 10);
    assert(prepare_count == 1 && off_count == 0);
    sample(false, 50);
    assert(off_count == 0);
    sample(false, 10);
    assert(off_count == 1);
    return 0;
}
''')
            result = subprocess.run(
                [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-I", str(ROOT / "firmware/main"),
                 str(source), "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
