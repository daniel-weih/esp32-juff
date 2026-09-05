#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t stream_id;
    uint32_t received;
    uint32_t limit;
    bool active;
    bool draining;
} speaker_flow_t;

typedef enum {
    SPEAKER_FLOW_OK,
    SPEAKER_FLOW_STALE,
    SPEAKER_FLOW_INVALID,
    SPEAKER_FLOW_WINDOW_EXCEEDED,
} speaker_flow_result_t;

// All counters are mu-law PAYLOAD bytes, excluding the eight-byte wire header.
// Start zeroed for a new BLE connection. Within one connection IDs strictly
// increase; cancelling keeps the ID fence so delayed/replayed BEGINs cannot
// revive old audio. Reconnect before exhausting the uint32_t ID space.
static inline bool speaker_flow_start(speaker_flow_t *flow, uint32_t stream_id,
                                      size_t initial_mu_budget)
{
    if (flow == NULL || stream_id == 0 || stream_id <= flow->stream_id ||
        initial_mu_budget > UINT32_MAX) {
        return false;
    }
    *flow = (speaker_flow_t){
        .stream_id = stream_id,
        .limit = (uint32_t)initial_mu_budget,
        .active = true,
    };
    return true;
}

// Preflight only: validate against the advertised absolute limit without
// acknowledging any bytes. The caller must hold its speaker mutex continuously
// across accept -> decode/enqueue -> commit. On any enqueue failure, cancel the
// response and discard its queued/staged audio instead of committing this packet.
static inline speaker_flow_result_t speaker_flow_accept(const speaker_flow_t *flow,
                                                        uint32_t stream_id,
                                                        uint32_t offset,
                                                        size_t bytes)
{
    if (flow == NULL || stream_id == 0) {
        return SPEAKER_FLOW_INVALID;
    }
    // A late packet cannot invalidate or spend the current response's credit.
    if (!flow->active || stream_id != flow->stream_id) {
        return SPEAKER_FLOW_STALE;
    }
    if (flow->draining || bytes == 0 || offset != flow->received ||
        bytes > UINT32_MAX || bytes > UINT32_MAX - offset) {
        return SPEAKER_FLOW_INVALID;
    }
    if (offset > flow->limit || bytes > flow->limit - offset) {
        return SPEAKER_FLOW_WINDOW_EXCEEDED;
    }
    return SPEAKER_FLOW_OK;
}

// Advance only after the whole packet is owned by the playback queue/staging
// buffer. Revalidate so a stale/cancelled transaction cannot acknowledge a new
// stream, even if an integration accidentally releases its mutex in between.
static inline speaker_flow_result_t speaker_flow_commit(speaker_flow_t *flow,
                                                        uint32_t stream_id,
                                                        uint32_t offset,
                                                        size_t bytes)
{
    const speaker_flow_result_t result = speaker_flow_accept(flow, stream_id, offset, bytes);
    if (result == SPEAKER_FLOW_OK) {
        flow->received += (uint32_t)bytes;
    }
    return result;
}

// free_mu_bytes is derived from the SAME snapshot as received/staging:
// max(0, PCM_budget - queued_PCM - staging_PCM) / 2.
// Credit is an absolute, non-revocable limit, not an additional allowance.
// Overflow rejects the publication without wrapping or mutating the state.
static inline bool speaker_flow_publish(speaker_flow_t *flow, size_t free_mu_bytes)
{
    if (flow == NULL || !flow->active || flow->draining ||
        free_mu_bytes > UINT32_MAX || free_mu_bytes > UINT32_MAX - flow->received) {
        return false;
    }
    const uint32_t candidate = flow->received + (uint32_t)free_mu_bytes;
    if (candidate > flow->limit) {
        flow->limit = candidate;
    }
    return true;
}

// Called after DONE validation and successful final PCM enqueue, immediately
// before enqueuing END so a fast playback callback cannot be overwritten.
// If END enqueue fails, the caller must cancel. This is not audible completion.
static inline bool speaker_flow_drain(speaker_flow_t *flow)
{
    if (flow == NULL || !flow->active || flow->draining) {
        return false;
    }
    flow->draining = true;
    return true;
}

static inline void speaker_flow_cancel(speaker_flow_t *flow)
{
    if (flow != NULL) {
        flow->active = false;
        flow->draining = false;
    }
}
