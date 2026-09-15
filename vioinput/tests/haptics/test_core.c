/* SPDX-License-Identifier: BSD-3-Clause */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "HapticsCore.h"

/* Test-only type: not a negotiated or globally allocated virtio event number. */
#define TEST_TYPE 0xff80u

// Construct a message with nontrivial high words and both motor channels.
static DvhMessage sample(void)
{
    DvhMessage m = {DVH_SET_RUMBLE, UINT64_C(0xfedcba9876543210),
                    UINT64_C(0x123456789abcdef0), UINT64_C(0x8123456700000001),
                    UINT32_C(0xabcd1234), 500, 0};
    return m;
}

// Compare fields instead of comparing implementation-dependent structure padding.
static int equal(const DvhMessage *a, const DvhMessage *b)
{
    return a->opcode == b->opcode && a->epoch == b->epoch &&
           a->sequence == b->sequence && a->revision == b->revision &&
           a->motors == b->motors && a->lease_ms == b->lease_ms && a->detail == b->detail;
}

// Check canonical CRC32C and exact little-endian record construction.
static void test_codec(void)
{
    uint8_t bytes[DVH_FRAME_BYTES];
    DvhMessage m = sample(), decoded;
    assert(DvhCrc32c((const uint8_t *)"123456789", 9) == UINT32_C(0xe3069283));
    assert(DvhCrc32c(bytes, 0) == 0);
    assert(DvhEncode(bytes, sizeof(bytes), TEST_TYPE, &m));
    assert(bytes[0] == 0x80 && bytes[1] == 0xff);
    assert(bytes[4] == 1 && bytes[6] == DVH_SET_RUMBLE);
    assert(bytes[12] == 0x10 && bytes[15] == 0x76);
    assert(bytes[60] == 0x34 && bytes[61] == 0x12 && bytes[62] == 0xcd && bytes[63] == 0xab);
    assert(DvhDecode(bytes, sizeof(bytes), TEST_TYPE, &decoded));
    assert(equal(&m, &decoded));
}

// Every one-bit mutation, truncation and extension must fail without partial output.
static void test_corruption(void)
{
    uint8_t bytes[DVH_FRAME_BYTES + 1];
    DvhMessage m = sample(), sentinel = sample(), decoded;
    size_t i;
    unsigned int bit;
    sentinel.sequence = 123;
    assert(DvhEncode(bytes, DVH_FRAME_BYTES, TEST_TYPE, &m));
    for (i = 0; i < DVH_FRAME_BYTES; ++i)
    {
        for (bit = 0; bit < 8; ++bit)
        {
            bytes[i] ^= (uint8_t)(1u << bit);
            decoded = sentinel;
            assert(!DvhDecode(bytes, DVH_FRAME_BYTES, TEST_TYPE, &decoded));
            assert(equal(&decoded, &sentinel));
            bytes[i] ^= (uint8_t)(1u << bit);
        }
        assert(!DvhDecode(bytes, i, TEST_TYPE, &decoded));
    }
    assert(!DvhDecode(bytes, sizeof(bytes), TEST_TYPE, &decoded));
    assert(!DvhDecode(bytes, DVH_FRAME_BYTES, TEST_TYPE + 1, &decoded));
}

// Semantic violations are rejected even when their CRC is recomputed correctly.
static void test_semantics(void)
{
    uint8_t bytes[DVH_FRAME_BYTES];
    DvhMessage good = sample(), m = good, out;
    assert(!DvhEncode(bytes, sizeof(bytes), 0x15, &m));
    m.lease_ms = 0;
    assert(!DvhEncode(bytes, sizeof(bytes), TEST_TYPE, &m));
    m.lease_ms = DVH_MAX_LEASE_MS + 1;
    assert(!DvhMessageValid(&m));
    m = good; m.opcode = 99; assert(!DvhMessageValid(&m));
    m = good; m.epoch = 0; assert(!DvhMessageValid(&m));
    m = good; m.sequence = 0; assert(!DvhMessageValid(&m));
    m = good; m.revision = 0; assert(!DvhMessageValid(&m));
    m = good; m.motors = 0; assert(!DvhMessageValid(&m));
    m = good; m.detail = 1; assert(!DvhMessageValid(&m));
    assert(DvhEncode(bytes, sizeof(bytes), TEST_TYPE, &good));
    DvhWrite32(bytes + 10 * 8 + 4, 1);
    DvhWrite32(bytes + 11 * 8 + 4, DvhCrc32c(bytes, 88));
    assert(!DvhDecode(bytes, sizeof(bytes), TEST_TYPE, &out));
    assert(DvhEncode(bytes, sizeof(bytes), TEST_TYPE, &good));
    DvhWrite32(bytes + 4, 2u | ((uint32_t)DVH_SET_RUMBLE << 16));
    DvhWrite32(bytes + 92, DvhCrc32c(bytes, 88));
    assert(!DvhDecode(bytes, sizeof(bytes), TEST_TYPE, &out));
}

// Exercise fixed-space assembly, normal-input interleaving and absolute expiry.
static void test_receiver(void)
{
    uint8_t bytes[DVH_FRAME_BYTES], normal[8] = {1, 0};
    DvhMessage m = sample(), out;
    DvhReceiver rx;
    unsigned int i;
    assert(DvhEncode(bytes, sizeof(bytes), TEST_TYPE, &m));
    DvhReceiverReset(&rx);
    for (i = 0; i < DVH_RECORDS; ++i)
    {
        assert(DvhReceiveRecord(&rx, bytes + i * 8, TEST_TYPE, 100 + i, 100, &out) == (i == 11));
        assert(DvhReceiveRecord(&rx, normal, TEST_TYPE, 100 + i, 100, &out) == -1);
    }
    assert(equal(&m, &out));
    assert(!rx.records);
    assert(!DvhReceiveRecord(&rx, bytes, TEST_TYPE, 100, 10, &out));
    assert(!DvhReceiveRecord(&rx, bytes + 8, TEST_TYPE, 110, 10, &out));
    assert(!rx.records); /* deadline is not extended by partial traffic */
    assert(!DvhReceiveRecord(&rx, bytes, TEST_TYPE, UINT64_MAX - 2, 10, &out));
    assert(!rx.records);
    assert(!DvhReceiveRecord(&rx, bytes, TEST_TYPE, 100, 100, &out));
    assert(!DvhReceiveRecord(&rx, bytes + 16, TEST_TYPE, 101, 100, &out));
    assert(!rx.records); /* out-of-order code aborts a partial frame */
    for (i = 0; i < DVH_RECORDS; ++i)
    {
        assert(DvhReceiveRecord(&rx, bytes + i * 8, TEST_TYPE, 200 + i, 100, &out) == (i == 11));
    }
}

// Only negotiated lifecycle code can arm the core, and no enqueue means no commit.
static void test_transaction(void)
{
    DvhGuestState s;
    DvhMessage first, retry;
    DvhGuestReset(&s);
    assert(!DvhGuestPrepareOutput(&s, 10, 20, 500, &first));
    assert(!DvhGuestBegin(&s, 0));
    assert(DvhGuestBegin(&s, 10));
    assert(!DvhGuestReady(&s, 11));
    assert(!DvhGuestPrepareOutput(&s, 10, 20, 500, &first));
    assert(DvhGuestReady(&s, 10));
    assert(DvhGuestPrepareOutput(&s, 10, 20, 500, &first));
    assert(s.phase == DVH_IDLE && s.sequence == 0 && s.revision == 0);
    /* Simulate queue full: do not commit, retry produces the same sequence. */
    assert(DvhGuestPrepareOutput(&s, 10, 20, 500, &retry));
    assert(equal(&first, &retry));
    assert(DvhGuestCommit(&s, &first));
    assert(!DvhGuestCommit(&s, &first));
    assert(s.phase == DVH_PLAYING && s.sequence == 1 && s.revision == 1);
}

// STOP/revoke are barriers: already prepared renewals cannot restart vibration.
static void test_stop_and_revoke(void)
{
    DvhGuestState s;
    DvhMessage m, renew, stop;
    DvhGuestReset(&s);
    assert(DvhGuestBegin(&s, 100));
    assert(DvhGuestReady(&s, 100));
    assert(DvhGuestPrepareOutput(&s, 65535, 0, 500, &m));
    assert(DvhGuestCommit(&s, &m));
    assert(DvhGuestPrepareKeepalive(&s, &renew));
    assert(renew.revision == 1 && renew.sequence == 2);
    assert(DvhGuestCommit(&s, &renew));
    assert(s.revision == 1);
    assert(DvhGuestPrepareKeepalive(&s, &renew));
    assert(DvhGuestPrepareOutput(&s, 0, 0, 999999, &stop));
    assert(stop.opcode == DVH_STOP && !stop.lease_ms);
    assert(DvhGuestCommit(&s, &stop));
    assert(!DvhGuestCommit(&s, &renew));
    assert(!DvhGuestPrepareKeepalive(&s, &renew));
    assert(!s.motors && s.phase == DVH_IDLE);
    assert(DvhGuestPrepareOutput(&s, 1, 2, 500, &m));
    DvhGuestRevoke(&s);
    assert(!DvhGuestCommit(&s, &m));
    assert(!DvhGuestReady(&s, 100));
    assert(!DvhGuestBegin(&s, 100));
    assert(!DvhGuestPrepareKeepalive(&s, &renew));
    assert(DvhGuestBegin(&s, 101));
    assert(DvhGuestReady(&s, 101));
    assert(!s.motors && s.phase == DVH_IDLE);
    assert(!DvhGuestCommit(&s, &m));
}

// Sequence/revision exhaustion must not wrap to a previously valid message.
static void test_wrap_and_mismatch(void)
{
    DvhGuestState s;
    DvhMessage m, bad;
    DvhGuestReset(&s);
    assert(DvhGuestBegin(&s, 50));
    assert(DvhGuestReady(&s, 50));
    s.sequence = UINT64_MAX;
    assert(!DvhGuestPrepareOutput(&s, 1, 1, 500, &m));
    s.sequence = 0; s.revision = UINT64_MAX;
    assert(!DvhGuestPrepareOutput(&s, 1, 1, 500, &m));
    s.revision = 0;
    assert(DvhGuestPrepareOutput(&s, 1, 1, 500, &m));
    bad = m; ++bad.sequence; assert(!DvhGuestCommit(&s, &bad));
    bad = m; ++bad.revision; assert(!DvhGuestCommit(&s, &bad));
    bad = m; ++bad.epoch; assert(!DvhGuestCommit(&s, &bad));
    assert(DvhGuestCommit(&s, &m));
    assert(DvhGuestPrepareKeepalive(&s, &m));
    bad = m; ++bad.motors; assert(!DvhGuestCommit(&s, &bad));
    bad = m; ++bad.lease_ms; assert(!DvhGuestCommit(&s, &bad));
    bad = m; ++bad.revision; assert(!DvhGuestCommit(&s, &bad));
}

// Execute deterministic tests against the actual header, not a second model.
int main(void)
{
    test_codec();
    test_corruption();
    test_semantics();
    test_receiver();
    test_transaction();
    test_stop_and_revoke();
    test_wrap_and_mismatch();
    puts("haptics core: 7 test groups passed; 768 single-bit mutations rejected");
    return 0;
}
