"""Shared production declarations for the native playback and shutdown harnesses."""

import re


def write_playback_declarations(output, source, header):
    constants = source[source.index('#define GATEWAY_INPUT_SAMPLE_RATE'):source.index('typedef enum {')]
    item = re.search(r'typedef enum \{\s*AUDIO_ITEM_BEGIN,.*?\} audio_item_type_t;', source, re.S).group()
    item += '\n' + re.search(r'typedef struct \{\s*audio_item_type_t type;.*?\} audio_item_t;', source, re.S).group()
    status = re.search(r'typedef struct \{.*?\} audio_playback_buffer_status_t;', header, re.S).group()
    globals_ = source[source.index('static uint32_t s_codec_write_generation;'):source.index('static bool s_self_test_running;')]
    (output / 'audio_item.inc').write_text(constants + '\n' + item + '\n' + status)
    (output / 'audio_buffer_globals.inc').write_text(globals_)
