#include <stdlib.h>
#define main conversation_policy_main
#include "voice_session_harness.c"
#undef main

int main(int argc, char **argv)
{
    assert(argc == 3 || argc == 5);
    saved = true;
    const bool wake_mode = strcmp(argv[1], "wake") == 0;
    const bool legacy = strcmp(argv[1], "legacy") == 0;
    stored = wake_mode ? VOICE_MODE_WAKE : VOICE_MODE_CONTINUOUS;
    assert(voice_session_init() == ESP_OK);
    voice_session_set_supported(true);
    ble = true;
    session_tick();
    if (wake_mode) wake();
    else if (!legacy) {
        acknowledge();
        assert(control("voice.ready", voice_session_get().generation, NULL));
    }
    if (argc == 3) {
        assert(!strcmp(argv[2], "generation"));
        printf("%u\n", voice_session_get().generation);
        return 0;
    }
    // Python parses the actual Swift JSON into this existing cJSON boundary.
    const cJSON type = { .valuestring = argv[2] },
        epoch = { .number = true, .valuedouble = strtod(argv[3], NULL) },
        message = { .type = &type, .epoch = !strcmp(argv[3], "none") ? NULL : &epoch };
    playing = true; // A stream failed before delivering audio.done.
    clock_ms += 10000;
    session_tick();
    const bool accepted = voice_session_handle_control(&message);
    assert(accepted == (atoi(argv[4]) != 0));
    if (accepted) audio_io_clear("speaker-flow-failed");
    assert(playing == !accepted);
    if (!legacy) assert(voice_session_can_stream());
    clock_ms += VOICE_SESSION_IDLE_MS;
    session_tick();
    assert(voice_session_waiting() == (wake_mode && accepted));
    puts("Speaker recovery and idle deadline passed");
    return 0;
}
