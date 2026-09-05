"""Stream/control tests for the real frontend with deterministic AEC/VAD fakes.

The fake AEC subtracts its reference exactly. These tests do not measure the ESP
AEC's echo suppression, its accuracy, or the real VAD's classification quality.
"""

from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
MOCK_HEADERS = {
    "sdkconfig.h": "#define CONFIG_JUFF_VOICE_BARGE_IN 1\n",
    "esp_err.h": """#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_NOT_SUPPORTED 2
""",
    "esp_aec.h": """#pragma once
#include <stdint.h>
typedef struct mock_aec aec_handle_t;
typedef struct {
    int mic_num, ref_num, out_num, filter_length, sample_rate;
    unsigned caps;
    int mode, nlp_level;
} aec_config_t;
#define AEC_MODE_FD_LOW_COST 5
#define AEC_NLP_LEVEL_AGGR 1
#define AEC_NLP_LEVEL_VERYAGGR 2
#define AEC_MODE_FD_HIGH_PERF 6
#define AEC_NLP_LEVEL_NORMAL 0
aec_handle_t *aec_create_from_config(aec_config_t *config);
void aec_destroy(aec_handle_t *handle);
int aec_get_chunksize(const aec_handle_t *handle);
void aec_process(const aec_handle_t *handle, int16_t *mic,
                 int16_t *reference, int16_t *clean);
""",
    "esp_vad.h": """#pragma once
#include <stdint.h>
typedef struct mock_vad *vad_handle_t;
typedef enum { VAD_SILENCE, VAD_SPEECH } vad_state_t;
#define VAD_MODE_3 3
vad_handle_t vad_create_with_param(int mode, int rate, int frame_ms,
                                   int speech_ms, int silence_ms);
void vad_destroy(vad_handle_t handle);
void vad_reset_trigger(vad_handle_t handle);
vad_state_t vad_process_with_trigger(vad_handle_t handle, int16_t *samples);
""",
    "esp_heap_caps.h": """#pragma once
#include <stddef.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
void *heap_caps_aligned_alloc(size_t alignment, size_t size, unsigned caps);
void *heap_caps_malloc(size_t size, unsigned caps);
void frontend_test_free(void *pointer);
// Track production cleanup without intercepting the harness's own input buffers.
#define free frontend_test_free
""",
    "esp_log.h": """#pragma once
void frontend_test_log(const char *tag, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
#define ESP_LOGI(...) frontend_test_log(__VA_ARGS__)
""",
    "esp_timer.h": """#pragma once
#include <stdint.h>
int64_t esp_timer_get_time(void);
""",
}


class VoiceFrontendTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if compiler is None:
            raise AssertionError("A C compiler is required for frontend stream tests")
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        output = Path(cls.temporary.name)
        # Host-only tests remain usable before IDF setup. When the pinned SDK
        # is installed, reject drift between its actual enums and our fakes.
        sdk_headers = ROOT / "firmware/managed_components/espressif__esp-sr/include/esp32s3"
        for filename, names in {
            "esp_aec.h": ("AEC_MODE_FD_LOW_COST", "AEC_MODE_FD_HIGH_PERF"),
            "esp_aec_nlp.h": ("AEC_NLP_LEVEL_NORMAL", "AEC_NLP_LEVEL_AGGR", "AEC_NLP_LEVEL_VERYAGGR"),
        }.items():
            header = sdk_headers / filename
            if not header.is_file():
                continue
            source = header.read_text()
            for name in names:
                actual = re.search(r"\b" + name + r"\s*=\s*(\d+)\b", source)
                mock = re.search(r"#define " + name + r" (\d+)\b", MOCK_HEADERS["esp_aec.h"])
                if actual is None or mock is None or actual.group(1) != mock.group(1):
                    raise AssertionError(f"AEC mock enum does not match installed SDK: {name}")
        for name, text in MOCK_HEADERS.items():
            (output / name).write_text(text)
        cls.executables = {}
        for board, small in (("waveshare-lcd-3.5", False), ("waveshare-lcd-1.54", True)):
            executable = output / f"voice_frontend_{board}"
            result = subprocess.run(
                [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-Wno-unused-parameter",
                 f"-DCONFIG_JUFF_BOARD_WAVESHARE_LCD_35={int(not small)}",
                 f"-DCONFIG_JUFF_BOARD_WAVESHARE_LCD_154={int(small)}",
                 "-I", str(output), "-I", str(ROOT / "firmware/main"),
                 str(ROOT / "firmware/main/voice_frontend.c"),
                 str(Path(__file__).with_name("voice_frontend_harness.c")),
                 "-lm", "-o", str(executable)], capture_output=True, text=True)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
            cls.executables[board] = executable

    def run_scenario(self, scenario, chunk=160):
        for board, executable in self.executables.items():
            with self.subTest(board=board):
                result = subprocess.run([str(executable), scenario, str(chunk)],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("Frontend stream/control passed", result.stdout)

    def test_resampling_keeps_mic_and_reference_in_phase(self):
        for chunk in (160, 256, 512):
            with self.subTest(chunk=chunk):
                self.run_scenario("phase", chunk)

    def test_raw_microphone_peak_and_fullscale_telemetry_reset_between_windows(self):
        self.run_scenario("mic-telemetry")

    def test_history_and_pending_use_psram_with_full_sample_capacities(self):
        self.run_scenario("allocation-caps")

    def test_psram_allocation_failure_releases_resources_and_allows_retry(self):
        for scenario in ("history-allocation-failure", "pending-allocation-failure"):
            with self.subTest(scenario=scenario):
                self.run_scenario(scenario)

    def test_playback_suppresses_upload_and_reference_echo_cannot_trigger(self):
        for chunk in (160, 256, 512):
            with self.subTest(chunk=chunk):
                self.run_scenario("echo", chunk)

    def test_vad_energy_warmup_and_detector_gates_and_one_interrupt(self):
        self.run_scenario("gates")

    def test_stale_vad_speech_with_single_frame_spikes_cannot_interrupt(self):
        self.run_scenario("spikes")

    def test_short_mute_resets_partially_qualified_speech(self):
        self.run_scenario("candidate-mute")

    def test_waiting_for_first_pcm_cannot_consume_playback_warmup(self):
        self.run_scenario("delayed-pcm")

    def test_silent_first_pcm_waits_for_reference_before_400ms_warmup(self):
        self.run_scenario("reference-warmup")

    def test_reference_pause_does_not_revoke_completed_warmup(self):
        self.run_scenario("reference-pause")

    def test_new_response_restarts_warmup_when_no_idle_capture_was_observed(self):
        self.run_scenario("generation")

    def test_new_response_discards_the_previous_pending_history(self):
        self.run_scenario("generation-history")

    def test_first_pcm_and_generation_drop_partial_dsp_blocks(self):
        for chunk in (256, 512):
            with self.subTest(chunk=chunk):
                self.run_scenario("pcm-boundary", chunk)

    def test_rejected_interrupt_does_not_upload_playback_history(self):
        self.run_scenario("rejected")

    def test_accepted_interrupt_uploads_ordered_300ms_history_without_duplication(self):
        self.run_scenario("history")

    def test_full_pending_history_does_not_drop_the_first_live_frame(self):
        self.run_scenario("full-history")

    def test_external_upload_disable_discards_pending_and_partial_audio(self):
        self.run_scenario("muted")

    def test_short_external_mute_also_discards_pending_audio(self):
        self.run_scenario("short-mute")

    def test_mute_and_unmute_while_speaker_is_active_cannot_replay_muted_audio(self):
        self.run_scenario("playing-mute")

    def test_reset_discards_residual_aec_vad_upload_and_pending_history(self):
        for chunk in (160, 256, 512):
            with self.subTest(chunk=chunk):
                self.run_scenario("reset", chunk)


if __name__ == "__main__":
    unittest.main()
