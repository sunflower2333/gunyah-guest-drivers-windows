/* SPDX-License-Identifier: BSD-3-Clause
 * Pure validation helpers shared by the actual driver and its host-side tests.
 */
#ifndef VIOGPU_MEDIA_STATE_H
#define VIOGPU_MEDIA_STATE_H
#include "video_ioctl.h"
enum VGPU_BUFFER_STATE
{
    VgpuBufferIdle,
    VgpuBufferQueued,
    VgpuBufferReturned
};
typedef struct VGPU_BUFFER_TRACKER
{
    uint32_t State, Capacity, DataOffset, BytesUsed, Flags;
    int64_t TimestampUs;
} VGPU_BUFFER_TRACKER;
/* Check requests before treating the buffered input as any larger structure. */
static inline int VgpuVideoHeaderValid(const VGPU_VIDEO_HEADER *h,
                                       size_t received,
                                       size_t expected,
                                       uint64_t generation)
{
    return h && received >= expected && h->Version == VGPU_VIDEO_VERSION && h->Size == expected &&
           h->Generation == generation;
}
/* A successful command submission/ack is not a completion of the video buffer. */
static inline int VgpuVideoBeginQueue(VGPU_BUFFER_TRACKER *b)
{
    if (!b || !b->Capacity || b->State == VgpuBufferQueued)
    {
        return 0;
    }
    b->State = VgpuBufferQueued;
    b->DataOffset = 0;
    b->BytesUsed = 0;
    b->Flags = 0;
    return 1;
}
/* Validate the wire event before accessing a queue index, buffer, or payload. */
static inline int VgpuVideoParseEvent(const void *data,
                                      size_t length,
                                      uint32_t session,
                                      VGPU_VIDEO_EVENT *out,
                                      uint32_t *offset)
{
    const uint8_t *raw = (const uint8_t *)data;
    uint32_t kind;
    if (!raw || !out || !offset || length < 8 || length > sizeof(VMEDIA_EVENT))
    {
        return 0;
    }
    kind = VmediaRead32(raw);
    if (VmediaRead32(raw + 4) != session)
    {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    *offset = 0;
    out->Event = kind;
    if (kind == VMEDIA_EVT_ERROR)
    {
        if (length < 16)
        {
            return 0;
        }
        out->LinuxErrno = VmediaRead32(raw + 8);
        return 1;
    }
    if (kind == VMEDIA_EVT_V4L2)
    {
        if (length < 8 + sizeof(VMEDIA_V4L2_EVENT))
        {
            return 0;
        }
        out->V4l2Type = VmediaRead32(raw + 8);
        out->Changes = VmediaRead32(raw + 16);
        return 1;
    }
    if (kind == VMEDIA_EVT_DQBUF)
    {
        VMEDIA_BUFFER b;
        VMEDIA_PLANE p;
        if (length < 8 + sizeof(b) + sizeof(p))
        {
            return 0;
        }
        memcpy(&b, raw + 8, sizeof(b));
        memcpy(&p, raw + 8 + sizeof(b), sizeof(p));
        if (VmediaQueueIndex(b.Type) < 0 || b.Length != 1 || b.Memory != VMEDIA_MEMORY_USERPTR ||
            p.DataOffset > p.BytesUsed || p.BytesUsed > p.Length || !VmediaTimeToUs(b.Timestamp, &out->TimestampUs))
        {
            return 0;
        }
        out->Type = b.Type;
        out->Index = b.Index;
        out->BytesUsed = p.BytesUsed - p.DataOffset;
        out->Flags = b.Flags;
        out->Sequence = b.Sequence;
        *offset = p.DataOffset;
        return 1;
    }
    return 0;
}
/* A DQBUF returns ownership exactly once; stale/duplicate/bounds errors poison the session. */
static inline int VgpuVideoCompleteBuffer(VGPU_BUFFER_TRACKER *b, const VGPU_VIDEO_EVENT *e, uint32_t offset)
{
    if (!b || !e || b->State != VgpuBufferQueued || offset > b->Capacity || e->BytesUsed > b->Capacity - offset)
    {
        return 0;
    }
    b->State = VgpuBufferReturned;
    b->DataOffset = offset;
    b->BytesUsed = e->BytesUsed;
    b->Flags = e->Flags;
    b->TimestampUs = e->TimestampUs;
    return 1;
}
#endif
