#include <assert.h>
#include <limits.h>
#include <string.h>

#include "speaker_flow.h"

static void unchanged(speaker_flow_t expected, speaker_flow_t actual)
{
    assert(expected.stream_id == actual.stream_id);
    assert(expected.received == actual.received);
    assert(expected.limit == actual.limit);
    assert(expected.active == actual.active);
    assert(expected.draining == actual.draining);
}

static void initial_credit(void)
{
    speaker_flow_t flow = {0};
    assert(!speaker_flow_start(&flow, 0, 96000));
    assert(speaker_flow_accept(&flow, 1, 0, 64) == SPEAKER_FLOW_STALE);
    assert(speaker_flow_start(&flow, 1, 0));
    assert(flow.active && !flow.draining && flow.received == 0 && flow.limit == 0);
    assert(speaker_flow_accept(&flow, 1, 0, 64) == SPEAKER_FLOW_WINDOW_EXCEEDED);
    assert(speaker_flow_publish(&flow, 96000));
    assert(flow.limit == 96000);
    assert(speaker_flow_accept(&flow, 1, 0, 64) == SPEAKER_FLOW_OK);
}

static void transactional_commit(void)
{
    speaker_flow_t flow = {0};
    assert(speaker_flow_start(&flow, 1, 96000));
    const speaker_flow_t before = flow;
    assert(speaker_flow_accept(&flow, 1, 0, 500) == SPEAKER_FLOW_OK);
    unchanged(before, flow); // A successful preflight is not a receipt ACK.
    // Simulate decoding/queue allocation failing after preflight.
    speaker_flow_cancel(&flow);
    assert(flow.received == 0);
    assert(speaker_flow_commit(&flow, 1, 0, 500) == SPEAKER_FLOW_STALE);
    assert(flow.received == 0);
    assert(speaker_flow_start(&flow, 2, 96000));
    assert(speaker_flow_accept(&flow, 2, 0, 500) == SPEAKER_FLOW_OK);
    assert(speaker_flow_commit(&flow, 2, 0, 500) == SPEAKER_FLOW_OK);
    assert(flow.received == 500 && flow.limit == 96000);
    const speaker_flow_t committed = flow;
    assert(speaker_flow_commit(&flow, 2, 0, 500) == SPEAKER_FLOW_INVALID);
    unchanged(committed, flow); // Duplicate offset must not acknowledge twice.
}

static void packet_bounds(void)
{
    speaker_flow_t flow = {0};
    assert(speaker_flow_start(&flow, 9, 1000));
    assert(speaker_flow_accept(&flow, 9, 0, 0) == SPEAKER_FLOW_INVALID);
    assert(speaker_flow_accept(&flow, 0, 0, 1) == SPEAKER_FLOW_INVALID);
    assert(speaker_flow_accept(&flow, 9, 1, 1) == SPEAKER_FLOW_INVALID);
    assert(speaker_flow_accept(&flow, 9, 0, 1001) == SPEAKER_FLOW_WINDOW_EXCEEDED);
    assert(speaker_flow_commit(&flow, 9, 0, 1000) == SPEAKER_FLOW_OK);
    assert(flow.received == flow.limit);
    assert(speaker_flow_accept(&flow, 9, 1000, 1) == SPEAKER_FLOW_WINDOW_EXCEEDED);
    assert(speaker_flow_accept(&flow, 9, 999, 1) == SPEAKER_FLOW_INVALID);
}

static void absolute_credit(void)
{
    speaker_flow_t flow = {0};
    assert(speaker_flow_start(&flow, 1, 96000));
    assert(speaker_flow_commit(&flow, 1, 0, 1000) == SPEAKER_FLOW_OK);
    // The accepted 1000 bytes now occupy 2000 PCM bytes. Advertising the same
    // remaining capacity repeatedly must not mint new credit.
    for (unsigned i = 0; i < 100; ++i) {
        assert(speaker_flow_publish(&flow, 95000));
        assert(flow.limit == 96000);
    }
    assert(speaker_flow_publish(&flow, 95240)); // 240 mu-law-equivalent bytes consumed.
    assert(flow.limit == 96240);
    assert(speaker_flow_publish(&flow, 0)); // An issued allowance cannot be revoked.
    assert(flow.limit == 96240);
    assert(flow.received == 1000);
}

static void stream_fence(void)
{
    speaker_flow_t flow = {0};
    assert(speaker_flow_start(&flow, 11, 96000));
    assert(speaker_flow_commit(&flow, 11, 0, 100) == SPEAKER_FLOW_OK);
    speaker_flow_cancel(&flow);
    assert(!speaker_flow_start(&flow, 11, 96000));
    assert(!speaker_flow_start(&flow, 10, 96000));
    assert(speaker_flow_start(&flow, 12, 96000));
    const speaker_flow_t fresh = flow;
    assert(speaker_flow_accept(&flow, 11, 100, 200) == SPEAKER_FLOW_STALE);
    assert(speaker_flow_commit(&flow, 11, 100, 200) == SPEAKER_FLOW_STALE);
    assert(speaker_flow_accept(&flow, 13, 0, 200) == SPEAKER_FLOW_STALE);
    unchanged(fresh, flow);
    assert(speaker_flow_commit(&flow, 12, 0, 200) == SPEAKER_FLOW_OK);
    assert(flow.received == 200);
}

static void draining(void)
{
    speaker_flow_t flow = {0};
    assert(!speaker_flow_drain(&flow));
    assert(speaker_flow_start(&flow, 3, 1000));
    assert(speaker_flow_commit(&flow, 3, 0, 123) == SPEAKER_FLOW_OK);
    assert(speaker_flow_drain(&flow));
    assert(flow.active && flow.draining && flow.received == 123);
    assert(!speaker_flow_drain(&flow));
    assert(!speaker_flow_publish(&flow, 1000));
    assert(speaker_flow_accept(&flow, 3, 123, 1) == SPEAKER_FLOW_INVALID);
    assert(speaker_flow_accept(&flow, 2, 123, 1) == SPEAKER_FLOW_STALE);
    assert(speaker_flow_start(&flow, 4, 500)); // Next ordered response can use remaining capacity.
    assert(!flow.draining && flow.received == 0 && flow.limit == 500);
}

static void counter_overflow(void)
{
    speaker_flow_t flow = {0};
    assert(speaker_flow_start(&flow, 1, UINT32_MAX));
    assert(speaker_flow_commit(&flow, 1, 0, UINT32_MAX - 10U) == SPEAKER_FLOW_OK);
    const speaker_flow_t before = flow;
    assert(speaker_flow_accept(&flow, 1, UINT32_MAX - 10U, 11) == SPEAKER_FLOW_INVALID);
    assert(!speaker_flow_publish(&flow, 11));
    unchanged(before, flow);
    assert(speaker_flow_commit(&flow, 1, UINT32_MAX - 10U, 10) == SPEAKER_FLOW_OK);
    assert(flow.received == UINT32_MAX);
    assert(speaker_flow_accept(&flow, 1, UINT32_MAX, 1) == SPEAKER_FLOW_INVALID);
    assert(speaker_flow_publish(&flow, 0));
    assert(!speaker_flow_publish(&flow, 1));
#if SIZE_MAX > UINT32_MAX
    const size_t oversized = (size_t)UINT32_MAX + 1U;
    assert(!speaker_flow_start(&flow, 2, oversized));
    assert(speaker_flow_accept(&flow, 1, UINT32_MAX, oversized) == SPEAKER_FLOW_INVALID);
    assert(!speaker_flow_publish(&flow, oversized));
#endif
    assert(speaker_flow_start(&flow, UINT32_MAX, 1));
    speaker_flow_cancel(&flow);
    assert(!speaker_flow_start(&flow, 1, 1));
    assert(!speaker_flow_start(&flow, 0, 1));
    flow = (speaker_flow_t){0}; // A newly established connection resets its ID namespace.
    assert(speaker_flow_start(&flow, 1, 1));
}

static void null_inputs(void)
{
    assert(!speaker_flow_start(NULL, 1, 100));
    assert(speaker_flow_accept(NULL, 1, 0, 1) == SPEAKER_FLOW_INVALID);
    assert(speaker_flow_commit(NULL, 1, 0, 1) == SPEAKER_FLOW_INVALID);
    assert(!speaker_flow_publish(NULL, 100));
    assert(!speaker_flow_drain(NULL));
    speaker_flow_cancel(NULL);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (strcmp(argv[1], "initial-credit") == 0) initial_credit();
    else if (strcmp(argv[1], "transactional-commit") == 0) transactional_commit();
    else if (strcmp(argv[1], "packet-bounds") == 0) packet_bounds();
    else if (strcmp(argv[1], "absolute-credit") == 0) absolute_credit();
    else if (strcmp(argv[1], "stream-fence") == 0) stream_fence();
    else if (strcmp(argv[1], "draining") == 0) draining();
    else if (strcmp(argv[1], "counter-overflow") == 0) counter_overflow();
    else if (strcmp(argv[1], "null-inputs") == 0) null_inputs();
    else assert(!"Unknown scenario");
    return 0;
}
