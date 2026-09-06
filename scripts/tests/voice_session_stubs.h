// Existing transport tests run with an active continuous conversation.
#include "../../firmware/main/voice_session.h"
static uint32_t test_conversation_generation;
voice_session_status_t voice_session_get(void)
{ return (voice_session_status_t){ .active = true, .ready = true, .supported = true,
    .generation = test_conversation_generation }; }
bool voice_session_can_stream(void) { return true; }
bool voice_session_waiting(void) { return false; }
bool voice_session_handle_control(const cJSON *message) { (void)message; return true; }
const char *voice_session_mode_name(voice_mode_t mode) { (void)mode; return "continuous"; }
void wake_word_observe_activity(const uint8_t *data, size_t size) { (void)data; (void)size; }
