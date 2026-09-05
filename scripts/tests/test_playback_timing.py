"""Run the actual playback task and producer APIs with a timed PCM/DMA boundary."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_voice_interrupt import function
from audio_playback_fixture import write_playback_declarations

ROOT = Path(__file__).resolve().parents[2]


class PlaybackTimingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        output = Path(cls.temporary.name)
        source = (ROOT / 'firmware/main/audio_io.c').read_text()
        write_playback_declarations(output, source, (ROOT / 'firmware/main/audio_io.h').read_text())
        (output / 'esp_err.h').write_text('typedef int esp_err_t;\n')
        names = ('begin_frontend_playback', 'mark_frontend_pcm_started',
                 'snapshot_frontend_playback', 'snapshot_frontend_playback_after_read',
                 'frontend_rx_overflow', 'recover_frontend_rx_overflow', 'copy_response_id',
                 'free_item', 'enqueue_item', 'codec_write_generation', 'cancel_playback_state_locked', 'cancel_codec_writes',
                 'audio_io_playback_buffer_status', 'audio_io_begin_response',
                 'push_pcm_for_generation', 'audio_io_push_pcm',
                 'audio_io_end_response', 'audio_io_clear', 'playback_generation_current',
                 'set_playback_buffering', 'wait_for_playback_buffer', 'stop_playback_output',
                 'finish_playback_state', 'clear_failed_playback', 'audio_io_push_pcm_wait',
                 'playback_task')
        (output / 'playback.inc').write_text('\n\n'.join(function(source, name) for name in names))
        cls.executable = output / 'playback_timing'
        result = subprocess.run([
            shutil.which('cc') or 'cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
            '-Wno-unused-parameter', '-fsanitize=address,undefined', '-I', str(output),
            '-I', str(ROOT / 'firmware/main'),
            str(Path(__file__).with_name('playback_timing_harness.c')),
            '-o', str(cls.executable)], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_scenario(self, scenario):
        result = subprocess.run([str(self.executable), scenario], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('Playback timing passed', result.stdout)

    def test_fast_synthesis_buffers_without_dropping_or_clipping_tail(self):
        self.run_scenario('burst')

    def test_wifi_burst_larger_than_capacity_waits_for_space_and_preserves_every_byte(self):
        self.run_scenario('wifi-burst')

    def test_wifi_stall_cancels_once_and_old_wait_cannot_enter_or_cancel_new_response(self):
        for scenario in ('wifi-timeout', 'wifi-clear', 'wifi-replace'):
            with self.subTest(scenario=scenario): self.run_scenario(scenario)

    def test_short_and_empty_end_do_not_wait_for_prefill(self):
        for scenario in ('short', 'empty'):
            with self.subTest(scenario=scenario): self.run_scenario(scenario)

    def test_consecutive_natural_responses_keep_tx_running_and_restart_frontend_generation(self):
        self.run_scenario('consecutive')

    def test_first_block_deadline_and_gap_refill_are_bounded(self):
        for scenario in ('deadline', 'refill'):
            with self.subTest(scenario=scenario): self.run_scenario(scenario)

    def test_clear_and_new_response_cancel_buffering_and_dma(self):
        for scenario in ('clear-buffer', 'clear-write', 'replace', 'clear-pending'):
            with self.subTest(scenario=scenario): self.run_scenario(scenario)

    def test_capacity_allocation_send_and_generation_accounting(self):
        self.run_scenario('accounting')

    def test_frontend_warmup_and_overflow_generation_boundaries(self):
        self.run_scenario('frontend')


if __name__ == '__main__': unittest.main()
