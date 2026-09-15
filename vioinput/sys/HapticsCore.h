/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 DroidVM contributors
 *
 * Allocation-free wire/state core. Haptics.c integrates this with the opt-in
 * XInputHID profile. See docs/HAPTICS-ABI.md for the private transport contract.
 */
#ifndef VIOINPUT_HAPTICS_CORE_H
#define VIOINPUT_HAPTICS_CORE_H

#include <stddef.h>
#if defined(_KERNEL_MODE)
/* Use the WDK's integer definitions, not the incompatible user-mode vcruntime.
 * The fixed-width names remain shared with the freestanding test harness.
 */
#include <ntddk.h>
typedef unsigned char uint8_t;
typedef signed char int8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef signed int int32_t;
typedef unsigned long long uint64_t;
typedef signed long long int64_t;
#ifndef UINT32_C
#define UINT32_C(value) value##U
#endif
#ifndef UINT32_MAX
#define UINT32_MAX 0xffffffffU
#endif
#ifndef UINT64_MAX
#define UINT64_MAX 0xffffffffffffffffULL
#endif
#ifndef INT32_MAX
#define INT32_MAX 2147483647
#endif
#ifndef INT32_MIN
#define INT32_MIN (-2147483647 - 1)
#endif
C_ASSERT(sizeof(uint8_t) == 1 && sizeof(uint16_t) == 2);
C_ASSERT(sizeof(uint32_t) == 4 && sizeof(uint64_t) == 8);
C_ASSERT(sizeof(int8_t) == 1 && sizeof(int32_t) == 4 && sizeof(int64_t) == 8);
#else
#include <stdint.h>
#endif

#define DVH_VERSION 1u
#define DVH_RECORD_BYTES 8u
#define DVH_RECORDS 12u
#define DVH_FRAME_BYTES (DVH_RECORD_BYTES * DVH_RECORDS)
#define DVH_MAX_LEASE_MS 5000u

enum DvhOpcode
{
    DVH_HOST_HELLO = 1,
    DVH_GUEST_READY = 2,
    DVH_HOST_READY = 3,
    DVH_SET_RUMBLE = 4,
    DVH_KEEPALIVE = 5,
    DVH_STOP = 6,
    DVH_HOST_STATUS = 7,
    DVH_HOST_REVOKE = 8,
    DVH_CLOSE = 9,
    DVH_SET_XINPUT_REPORT = 10,
    DVH_KEEPALIVE_XINPUT_REPORT = 11
};

typedef struct DvhMessage
{
    uint16_t opcode;
    uint64_t epoch;
    uint64_t sequence;
    uint64_t revision;
    uint32_t motors;
    uint32_t lease_ms;
    uint32_t detail;
} DvhMessage;

// Read an unaligned little-endian word without type punning.
static inline uint32_t DvhRead32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Read an unaligned little-endian half-word.
static inline uint16_t DvhRead16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

// Write a little-endian word without exposing native padding or pointers.
static inline void DvhWrite32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

// Write a little-endian half-word.
static inline void DvhWrite16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

// CRC32C: reflected Castagnoli, init/xorout 0xffffffff, not authentication.
static inline uint32_t DvhCrc32c(const uint8_t *p, size_t n)
{
    uint32_t crc = UINT32_MAX;
    size_t i;
    unsigned int bit;
    for (i = 0; i < n; ++i)
    {
        crc ^= p[i];
        for (bit = 0; bit < 8; ++bit)
        {
            crc = (crc >> 1) ^ ((crc & 1u) ? UINT32_C(0x82f63b78) : 0u);
        }
    }
    return crc ^ UINT32_MAX;
}

// Check the four percentage magnitudes and the actuator mask in a raw XInputHID report.
static inline int DvhXinputReportActive(uint32_t magnitudes, uint32_t timing)
{
    unsigned int i;
    int active = 0;
    if ((timing & 0xf0u) || !(timing & 0x0fu))
    {
        return 0;
    }
    for (i = 0; i < 4; ++i)
    {
        uint32_t magnitude = (magnitudes >> (i * 8)) & 0xffu;
        if (magnitude > 100u)
        {
            return 0;
        }
        if ((timing & (8u >> i)) && magnitude)
        {
            active = 1;
        }
    }
    return active && ((timing >> 8) & 0xffu);
}

// Validate V1 fields before serialization or any state transition.
static inline int DvhMessageValid(const DvhMessage *m)
{
    if (!m || !m->epoch || !m->sequence)
    {
        return 0;
    }
    switch (m->opcode)
    {
        case DVH_HOST_HELLO:
        case DVH_GUEST_READY:
        case DVH_HOST_READY:
            return !m->revision && !m->motors && !m->lease_ms && !m->detail;
        case DVH_SET_RUMBLE:
        case DVH_KEEPALIVE:
            return m->revision && m->motors && m->lease_ms &&
                   m->lease_ms <= DVH_MAX_LEASE_MS && !m->detail;
        case DVH_SET_XINPUT_REPORT:
        case DVH_KEEPALIVE_XINPUT_REPORT:
            return m->revision && m->lease_ms && m->lease_ms <= DVH_MAX_LEASE_MS &&
                   DvhXinputReportActive(m->motors, m->detail);
        case DVH_STOP:
            return m->revision && !m->motors && !m->lease_ms && !m->detail;
        case DVH_CLOSE:
            return !m->motors && !m->lease_ms && !m->detail;
        case DVH_HOST_STATUS:
        case DVH_HOST_REVOKE:
            return !m->motors && !m->lease_ms && m->detail <= 8u;
        default:
            return 0;
    }
}

// Encode one complete frame; event_type must come from explicit negotiation.
static inline int DvhEncode(
    uint8_t *out,
    size_t size,
    uint16_t event_type,
    const DvhMessage *m)
{
    uint32_t words[DVH_RECORDS];
    unsigned int i;
    if (!out || size != DVH_FRAME_BYTES || event_type <= 0x1fu || !DvhMessageValid(m))
    {
        return 0;
    }
    words[0] = DVH_VERSION | ((uint32_t)m->opcode << 16);
    words[1] = (uint32_t)m->epoch;
    words[2] = (uint32_t)(m->epoch >> 32);
    words[3] = (uint32_t)m->sequence;
    words[4] = (uint32_t)(m->sequence >> 32);
    words[5] = (uint32_t)m->revision;
    words[6] = (uint32_t)(m->revision >> 32);
    words[7] = m->motors;
    words[8] = m->lease_ms;
    words[9] = m->detail;
    words[10] = 0;
    for (i = 0; i < DVH_RECORDS; ++i)
    {
        uint8_t *record = out + i * DVH_RECORD_BYTES;
        DvhWrite16(record, event_type);
        DvhWrite16(record + 2, (uint16_t)i);
        DvhWrite32(record + 4, i == 11 ? DvhCrc32c(out, 11u * DVH_RECORD_BYTES) : words[i]);
    }
    return 1;
}

// Decode atomically: a malformed frame leaves the caller's message untouched.
static inline int DvhDecode(
    const uint8_t *bytes,
    size_t size,
    uint16_t event_type,
    DvhMessage *out)
{
    DvhMessage m;
    uint32_t words[DVH_RECORDS];
    unsigned int i;
    if (!bytes || !out || size != DVH_FRAME_BYTES || event_type <= 0x1fu)
    {
        return 0;
    }
    for (i = 0; i < DVH_RECORDS; ++i)
    {
        const uint8_t *record = bytes + i * DVH_RECORD_BYTES;
        if (DvhRead16(record) != event_type || DvhRead16(record + 2) != i)
        {
            return 0;
        }
        words[i] = DvhRead32(record + 4);
    }
    if ((words[0] & 0xffffu) != DVH_VERSION || words[10] ||
        words[11] != DvhCrc32c(bytes, 11u * DVH_RECORD_BYTES))
    {
        return 0;
    }
    m.opcode = (uint16_t)(words[0] >> 16);
    m.epoch = words[1] | ((uint64_t)words[2] << 32);
    m.sequence = words[3] | ((uint64_t)words[4] << 32);
    m.revision = words[5] | ((uint64_t)words[6] << 32);
    m.motors = words[7];
    m.lease_ms = words[8];
    m.detail = words[9];
    if (!DvhMessageValid(&m))
    {
        return 0;
    }
    *out = m;
    return 1;
}

typedef struct DvhReceiver
{
    uint8_t bytes[DVH_FRAME_BYTES];
    unsigned int records;
    uint64_t deadline_ms;
} DvhReceiver;

// Forget a partial control frame without allocating or touching normal input.
static inline void DvhReceiverReset(DvhReceiver *rx)
{
    rx->records = 0;
    rx->deadline_ms = 0;
}

// Feed a record after transport framing: 1=frame, 0=incomplete/rejected, -1=not ours.
static inline int DvhReceiveRecord(
    DvhReceiver *rx,
    const uint8_t *record,
    uint16_t event_type,
    uint64_t now_ms,
    uint32_t timeout_ms,
    DvhMessage *out)
{
    unsigned int i;
    uint16_t code;
    if (!rx || !record || !out || event_type <= 0x1fu || !timeout_ms)
    {
        return 0;
    }
    if (rx->records && now_ms >= rx->deadline_ms)
    {
        DvhReceiverReset(rx);
    }
    if (DvhRead16(record) != event_type)
    {
        return -1;
    }
    code = DvhRead16(record + 2);
    if (code == 0)
    {
        DvhReceiverReset(rx);
        if (now_ms > UINT64_MAX - timeout_ms)
        {
            return 0;
        }
        rx->deadline_ms = now_ms + timeout_ms;
    }
    else if (!rx->records)
    {
        return 0;
    }
    if (code != rx->records || rx->records >= DVH_RECORDS)
    {
        DvhReceiverReset(rx);
        return 0;
    }
    for (i = 0; i < DVH_RECORD_BYTES; ++i)
    {
        rx->bytes[rx->records * DVH_RECORD_BYTES + i] = record[i];
    }
    ++rx->records;
    if (rx->records == DVH_RECORDS)
    {
        DvhReceiverReset(rx);
        return DvhDecode(rx->bytes, sizeof(rx->bytes), event_type, out);
    }
    return 0;
}

enum DvhGuestPhase
{
    DVH_DISABLED,
    DVH_WAIT_READY,
    DVH_IDLE,
    DVH_PLAYING,
    DVH_REVOKED
};

typedef struct DvhGuestState
{
    enum DvhGuestPhase phase;
    uint64_t epoch;
    uint64_t sequence;
    uint64_t revision;
    uint32_t motors;
    uint32_t lease_ms;
    uint32_t detail;
} DvhGuestState;

// Disable the guest core; the driver must quiesce DMA separately before freeing it.
static inline void DvhGuestReset(DvhGuestState *s)
{
    s->phase = DVH_DISABLED;
    s->epoch = 0;
    s->sequence = 0;
    s->revision = 0;
    s->motors = 0;
    s->lease_ms = 0;
    s->detail = 0;
}

// Called only by a trusted, negotiated lifecycle adapter, never for arbitrary input.
static inline int DvhGuestBegin(DvhGuestState *s, uint64_t epoch)
{
    if (!epoch || epoch == s->epoch)
    {
        return 0;
    }
    DvhGuestReset(s);
    s->epoch = epoch;
    s->phase = DVH_WAIT_READY;
    return 1;
}

// Authorize a fresh epoch; do not restore the previous epoch's nonzero state.
static inline int DvhGuestReady(DvhGuestState *s, uint64_t epoch)
{
    if (s->phase != DVH_WAIT_READY || s->epoch != epoch)
    {
        return 0;
    }
    s->phase = DVH_IDLE;
    return 1;
}

// Revoke immediately; stale queued work and keepalives cannot re-arm this state.
static inline void DvhGuestRevoke(DvhGuestState *s)
{
    s->phase = DVH_REVOKED;
    s->motors = 0;
    s->lease_ms = 0;
    s->detail = 0;
}

// Prepare, but do not commit, an actual output report; zero maps to STOP.
static inline int DvhGuestPrepareOutput(
    const DvhGuestState *s,
    uint16_t low,
    uint16_t high,
    uint32_t lease_ms,
    DvhMessage *out)
{
    DvhMessage m;
    if (!out || (s->phase != DVH_IDLE && s->phase != DVH_PLAYING) ||
        s->sequence == UINT64_MAX || s->revision == UINT64_MAX)
    {
        return 0;
    }
    m.motors = (uint32_t)low | ((uint32_t)high << 16);
    m.opcode = m.motors ? DVH_SET_RUMBLE : DVH_STOP;
    m.epoch = s->epoch;
    m.sequence = s->sequence + 1;
    m.revision = s->revision + 1;
    m.lease_ms = m.motors ? lease_ms : 0;
    m.detail = 0;
    if (!DvhMessageValid(&m))
    {
        return 0;
    }
    *out = m;
    return 1;
}

// Preserve all eight output payload bytes; the broker interprets timing, not the lease.
static inline int DvhGuestPrepareXinputReport(
    const DvhGuestState *s,
    uint32_t magnitudes,
    uint32_t timing,
    uint32_t lease_ms,
    DvhMessage *out)
{
    DvhMessage m;
    if (!out || !DvhGuestPrepareOutput(s, 0, 0, 0, &m))
    {
        return 0;
    }
    m.opcode = DVH_SET_XINPUT_REPORT;
    m.motors = magnitudes;
    m.detail = timing;
    m.lease_ms = lease_ms;
    if (!DvhMessageValid(&m))
    {
        return 0;
    }
    *out = m;
    return 1;
}

// A keepalive only renews the exact last committed nonzero state.
static inline int DvhGuestPrepareKeepalive(const DvhGuestState *s, DvhMessage *out)
{
    if (!out || s->phase != DVH_PLAYING || s->sequence == UINT64_MAX)
    {
        return 0;
    }
    out->opcode = s->detail ? DVH_KEEPALIVE_XINPUT_REPORT : DVH_KEEPALIVE;
    out->epoch = s->epoch;
    out->sequence = s->sequence + 1;
    out->revision = s->revision;
    out->motors = s->motors;
    out->lease_ms = s->lease_ms;
    out->detail = s->detail;
    return DvhMessageValid(out);
}

// Serialize prepare/enqueue/commit under one caller lock. Commit ONLY after enqueue.
static inline int DvhGuestCommit(DvhGuestState *s, const DvhMessage *m)
{
    if (!DvhMessageValid(m) || (s->phase != DVH_IDLE && s->phase != DVH_PLAYING) ||
        s->epoch != m->epoch || s->sequence == UINT64_MAX || m->sequence != s->sequence + 1)
    {
        return 0;
    }
    if (m->opcode == DVH_KEEPALIVE || m->opcode == DVH_KEEPALIVE_XINPUT_REPORT)
    {
        if (s->phase != DVH_PLAYING || m->revision != s->revision ||
            m->motors != s->motors || m->lease_ms != s->lease_ms || m->detail != s->detail)
        {
            return 0;
        }
    }
    else if (m->opcode == DVH_SET_RUMBLE || m->opcode == DVH_STOP || m->opcode == DVH_SET_XINPUT_REPORT)
    {
        if (s->revision == UINT64_MAX || m->revision != s->revision + 1)
        {
            return 0;
        }
        s->revision = m->revision;
        s->motors = m->motors;
        s->lease_ms = m->lease_ms;
        s->detail = m->detail;
        s->phase = m->opcode == DVH_STOP ? DVH_IDLE : DVH_PLAYING;
    }
    else
    {
        return 0;
    }
    s->sequence = m->sequence;
    return 1;
}

#endif
