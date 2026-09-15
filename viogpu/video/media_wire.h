/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 DroidVM contributors.
 * Fixed-width subset of the virtio-media / Linux V4L2 64-bit wire ABI.
 * Protocol baseline: Droid-VM/virtio-media 6b6d2b3307ce75ed35b0ab5b4703d9d1bb830cf8.
 * Do not use native long, timeval, pointers, or Windows CTL_CODE on the wire.
 */
#ifndef VIOGPU_MEDIA_WIRE_H
#define VIOGPU_MEDIA_WIRE_H
#ifdef _KERNEL_MODE
/* /kernel defines _KERNEL_MODE. WDK provides size_t, offsetof and memory
 * intrinsics; user-mode stdint.h must not mix vcruntime.h with km/crt. */
#include <ntddk.h>
typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef unsigned __int64 uint64_t;
typedef __int64 int64_t;
#ifndef INT64_MAX
#define INT64_MAX 9223372036854775807i64
#endif
#ifndef INT64_MIN
#define INT64_MIN (-INT64_MAX - 1)
#endif
#ifndef UINT64_MAX
#define UINT64_MAX 0xffffffffffffffffui64
#endif
#else
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#endif

#define VMEDIA_DEVICE_ID           48u
#define VMEDIA_COMMAND_QUEUE       0u
#define VMEDIA_EVENT_QUEUE         1u
#define VMEDIA_CMD_OPEN            1u
#define VMEDIA_CMD_CLOSE           2u
#define VMEDIA_CMD_IOCTL           3u
#define VMEDIA_MEMORY_USERPTR      2u
#define VMEDIA_CAPTURE             9u
#define VMEDIA_OUTPUT              10u
#define VMEDIA_MAX_PLANES          8u
#define VMEDIA_EVT_ERROR           0u
#define VMEDIA_EVT_DQBUF           1u
#define VMEDIA_EVT_V4L2            2u
#define VMEDIA_EVENT_EOS           2u
#define VMEDIA_EVENT_SOURCE_CHANGE 5u
#define VMEDIA_FLAG_ERROR          0x40u
#define VMEDIA_FLAG_LAST           0x100000u
#define VMEDIA_FLAG_KEYFRAME       8u
#define VMEDIA_FLAG_TIMESTAMP_COPY 0x4000u
#define VMEDIA_FOURCC(a, b, c, d)  ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define VMEDIA_NV12                VMEDIA_FOURCC('N', 'V', '1', '2')
#define VMEDIA_H264                VMEDIA_FOURCC('H', '2', '6', '4')
#define VMEDIA_HEVC                VMEDIA_FOURCC('H', 'E', 'V', 'C')

#pragma pack(push, 8)
typedef struct VMEDIA_CMD
{
    uint32_t Command, Padding;
} VMEDIA_CMD;
typedef struct VMEDIA_IOCTL
{
    VMEDIA_CMD Header;
    uint32_t SessionId, Code;
} VMEDIA_IOCTL;
typedef struct VMEDIA_RESP
{
    uint32_t Errno, Padding;
} VMEDIA_RESP;
typedef struct VMEDIA_OPEN_RESP
{
    VMEDIA_RESP Header;
    uint32_t SessionId, Padding;
} VMEDIA_OPEN_RESP;
typedef struct VMEDIA_CONFIG
{
    uint32_t DeviceCaps, DeviceType;
    uint8_t Card[32];
} VMEDIA_CONFIG;
typedef struct VMEDIA_SG
{
    uint64_t Gpa;
    uint32_t Length, Padding;
} VMEDIA_SG;
typedef struct VMEDIA_TIMEVAL
{
    int64_t Seconds, Microseconds;
} VMEDIA_TIMEVAL;
typedef struct VMEDIA_BUFFER
{
    uint32_t Index, Type, BytesUsed, Flags, Field, Padding0;
    VMEDIA_TIMEVAL Timestamp;
    uint8_t Timecode[16];
    uint32_t Sequence, Memory;
    uint64_t PointerOrOffset;
    uint32_t Length, Reserved2, RequestFd, Padding1;
} VMEDIA_BUFFER;
typedef struct VMEDIA_PLANE
{
    uint32_t BytesUsed, Length;
    uint64_t PointerOrOffset;
    uint32_t DataOffset, Reserved[11];
} VMEDIA_PLANE;
/* The format union is 8-byte aligned even when only pix_mp is used. */
typedef struct VMEDIA_FORMAT
{
    uint32_t Type, Padding;
    uint8_t Data[200];
} VMEDIA_FORMAT;
typedef struct VMEDIA_REQBUFS
{
    uint32_t Count, Type, Memory, Capabilities;
    uint8_t Flags, Reserved[3];
} VMEDIA_REQBUFS;
typedef struct VMEDIA_EVENT
{
    uint32_t Event, SessionId;
    VMEDIA_BUFFER Buffer;
    VMEDIA_PLANE Planes[VMEDIA_MAX_PLANES];
} VMEDIA_EVENT;
typedef struct VMEDIA_V4L2_EVENT
{
    uint32_t Type, Padding;
    uint8_t Data[64];
    uint32_t Pending, Sequence;
    int64_t Seconds, Nanoseconds;
    uint32_t Id, Reserved[8], Padding1;
} VMEDIA_V4L2_EVENT;
#pragma pack(pop)

#if defined(__cplusplus)
#define VMEDIA_ASSERT(x) static_assert((x), #x)
#else
#define VMEDIA_ASSERT_JOIN_(a, b) a##b
#define VMEDIA_ASSERT_JOIN(a, b)  VMEDIA_ASSERT_JOIN_(a, b)
#define VMEDIA_ASSERT(x)          typedef char VMEDIA_ASSERT_JOIN(vmedia_assert_, __LINE__)[(x) ? 1 : -1]
#endif
VMEDIA_ASSERT(sizeof(VMEDIA_CMD) == 8);
VMEDIA_ASSERT(sizeof(VMEDIA_IOCTL) == 16);
VMEDIA_ASSERT(sizeof(VMEDIA_OPEN_RESP) == 16);
VMEDIA_ASSERT(sizeof(VMEDIA_CONFIG) == 40);
VMEDIA_ASSERT(sizeof(VMEDIA_SG) == 16);
VMEDIA_ASSERT(sizeof(VMEDIA_BUFFER) == 88);
VMEDIA_ASSERT(offsetof(VMEDIA_BUFFER, Timestamp) == 24);
VMEDIA_ASSERT(offsetof(VMEDIA_BUFFER, PointerOrOffset) == 64);
VMEDIA_ASSERT(sizeof(VMEDIA_PLANE) == 64);
VMEDIA_ASSERT(sizeof(VMEDIA_FORMAT) == 208);
VMEDIA_ASSERT(sizeof(VMEDIA_REQBUFS) == 20);
VMEDIA_ASSERT(sizeof(VMEDIA_EVENT) == 608);
VMEDIA_ASSERT(sizeof(VMEDIA_V4L2_EVENT) == 136);

/* Read an unaligned little-endian field without a native pointer cast. */
static inline uint32_t VmediaRead32(const void *p)
{
    const uint8_t *b = (const uint8_t *)p;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
/* Write an unaligned little-endian field. */
static inline void VmediaWrite32(void *p, uint32_t value)
{
    uint8_t *b = (uint8_t *)p;
    b[0] = (uint8_t)value;
    b[1] = (uint8_t)(value >> 8);
    b[2] = (uint8_t)(value >> 16);
    b[3] = (uint8_t)(value >> 24);
}
/* Only the two multi-planar codec queues are accepted by this frontend. */
static inline int VmediaQueueIndex(uint32_t type)
{
    return type == VMEDIA_OUTPUT ? 0 : (type == VMEDIA_CAPTURE ? 1 : -1);
}
/* Canonical timeval conversion, including negative presentation timestamps. */
static inline VMEDIA_TIMEVAL VmediaTimeFromUs(int64_t us)
{
    VMEDIA_TIMEVAL t;
    t.Seconds = us / 1000000;
    t.Microseconds = us % 1000000;
    if (t.Microseconds < 0)
    {
        --t.Seconds;
        t.Microseconds += 1000000;
    }
    return t;
}
/* Reject unrepresentable timestamps rather than overflowing signed arithmetic. */
static inline int VmediaTimeToUs(VMEDIA_TIMEVAL t, int64_t *us)
{
    const int64_t minimumSeconds = INT64_MIN / 1000000 - 1;
    const int64_t minimumMicros = 1000000 + INT64_MIN % 1000000;
    if (!us || t.Microseconds < 0 || t.Microseconds >= 1000000 || t.Seconds > INT64_MAX / 1000000 ||
        t.Seconds < minimumSeconds)
    {
        return 0;
    }
    if (t.Seconds == minimumSeconds)
    {
        if (t.Microseconds < minimumMicros)
        {
            return 0;
        }
        *us = INT64_MIN + (t.Microseconds - minimumMicros);
        return 1;
    }
    *us = t.Seconds * 1000000;
    if (*us > INT64_MAX - t.Microseconds)
    {
        return 0;
    }
    *us += t.Microseconds;
    return 1;
}
/* Fixed-size, pointer-free controls. Queue/buffer ioctls are intentionally excluded. */
static inline uint32_t VmediaControlSize(uint32_t code)
{
    switch (code)
    {
        case 2:
            return 64; /* ENUM_FMT */
        case 4:
        case 5:
        case 64:
            return 208; /* G/S/TRY_FMT */
        case 21:
        case 22:
            return 204; /* G/S_PARM */
        case 27:
        case 28:
            return 8; /* G/S_CTRL */
        case 36:
            return 68; /* QUERYCTRL */
        case 74:
            return 44; /* ENUM_FRAMESIZES */
        case 75:
            return 52; /* ENUM_FRAMEINTERVALS */
        case 77:
        case 78:
            return 40; /* ENCODER_CMD / TRY */
        case 90:
        case 91:
            return 32; /* SUBSCRIBE / UNSUBSCRIBE_EVENT */
        case 94:
        case 95:
            return 64; /* G/S_SELECTION */
        case 96:
        case 97:
            return 72; /* DECODER_CMD / TRY */
        case 103:
            return 232; /* QUERY_EXT_CTRL */
        default:
            return 0;
    }
}
/* _IOW commands have no payload after the response header. */
static inline int VmediaControlHasReply(uint32_t code)
{
    return code != 90 && code != 91;
}
/* Validate a negotiated single-plane format; buffers still travel as MPLANE. */
static inline int VmediaFormatSize(const VMEDIA_FORMAT *f, uint32_t limit, uint32_t *bytes)
{
    uint32_t size;
    if (!f || !bytes || VmediaQueueIndex(f->Type) < 0 || f->Data[180] != 1)
    {
        return 0;
    }
    size = VmediaRead32(f->Data + 20); /* pix_mp.plane_fmt[0].sizeimage */
    if (!size || size > limit)
    {
        return 0;
    }
    *bytes = size;
    return 1;
}
/* Build the actual QBUF packet, with exactly one driver-owned GPA run. */
static inline int VmediaBuildQbuf(uint32_t session,
                                  uint32_t type,
                                  uint32_t index,
                                  uint64_t gpa,
                                  uint32_t capacity,
                                  uint32_t used,
                                  int64_t pts,
                                  void *packet,
                                  size_t packetBytes)
{
    VMEDIA_IOCTL cmd;
    VMEDIA_BUFFER buffer;
    VMEDIA_PLANE plane;
    VMEDIA_SG sg;
    uint8_t *out = (uint8_t *)packet;
    const size_t required = sizeof(cmd) + sizeof(buffer) + sizeof(plane) + sizeof(sg);
    if (!out || packetBytes < required || VmediaQueueIndex(type) < 0 || !gpa || !capacity || used > capacity ||
        (type == VMEDIA_CAPTURE && used) || gpa > UINT64_MAX - capacity)
    {
        return 0;
    }
    memset(&cmd, 0, sizeof(cmd));
    memset(&buffer, 0, sizeof(buffer));
    memset(&plane, 0, sizeof(plane));
    memset(&sg, 0, sizeof(sg));
    cmd.Header.Command = VMEDIA_CMD_IOCTL;
    cmd.SessionId = session;
    cmd.Code = 15;
    buffer.Index = index;
    buffer.Type = type;
    buffer.Memory = VMEDIA_MEMORY_USERPTR;
    buffer.Field = 1;
    buffer.Length = 1;
    buffer.Timestamp = VmediaTimeFromUs(pts);
    buffer.Flags = VMEDIA_FLAG_TIMESTAMP_COPY;
    /* Cookie only: all actual addressing comes from the appended GPA SG. */
    plane.PointerOrOffset = 1;
    plane.Length = capacity;
    plane.BytesUsed = used;
    sg.Gpa = gpa;
    sg.Length = capacity;
    memcpy(out, &cmd, sizeof(cmd));
    out += sizeof(cmd);
    memcpy(out, &buffer, sizeof(buffer));
    out += sizeof(buffer);
    memcpy(out, &plane, sizeof(plane));
    out += sizeof(plane);
    memcpy(out, &sg, sizeof(sg));
    return (int)required;
}
#endif
