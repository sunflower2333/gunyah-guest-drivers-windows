/* SPDX-License-Identifier: BSD-3-Clause */
#include "../../video/media_state.h"
#include <assert.h>
#include <stdio.h>
#include <initializer_list>
int main()
{
    VGPU_BUFFER_TRACKER b = {};
    b.Capacity = 4096;
    assert(VgpuVideoBeginQueue(&b));
    assert(b.State == VgpuBufferQueued); // command ACK must not change ownership
    assert(!VgpuVideoBeginQueue(&b));    // neither a second QBUF nor a timeout releases it
    VMEDIA_EVENT wire = {};
    wire.Event = VMEDIA_EVT_DQBUF;
    wire.SessionId = 0;
    wire.Buffer.Type = VMEDIA_CAPTURE;
    wire.Buffer.Length = 1;
    wire.Buffer.Memory = VMEDIA_MEMORY_USERPTR;
    wire.Buffer.PointerOrOffset = UINT64_MAX; // ignored, never exposed to user mode
    wire.Buffer.Timestamp = VmediaTimeFromUs(-1000001);
    wire.Planes[0].Length = 4096;
    wire.Planes[0].BytesUsed = 1040;
    wire.Planes[0].DataOffset = 16;
    VGPU_VIDEO_EVENT e = {};
    uint32_t offset = 0;
    assert(VgpuVideoParseEvent(&wire, sizeof(wire), 0, &e, &offset));
    assert(e.BytesUsed == 1024 && offset == 16 && e.TimestampUs == -1000001);
    assert(!VgpuVideoParseEvent(&wire, 159, 0, &e, &offset));
    assert(!VgpuVideoParseEvent(&wire, sizeof(wire), 1, &e, &offset));
    assert(VgpuVideoParseEvent(&wire, sizeof(wire), 0, &e, &offset));
    assert(VgpuVideoCompleteBuffer(&b, &e, offset));
    assert(!VgpuVideoCompleteBuffer(&b, &e, offset));
    assert(VgpuVideoBeginQueue(&b));
    e.BytesUsed = 4096;
    assert(!VgpuVideoCompleteBuffer(&b, &e, 16));
    assert(b.State == VgpuBufferQueued); // malformed completion cannot free storage
    wire.Planes[0].DataOffset = 1041;
    assert(!VgpuVideoParseEvent(&wire, sizeof(wire), 0, &e, &offset));
    wire.Planes[0].DataOffset = 0;
    wire.Buffer.Length = 9;
    assert(!VgpuVideoParseEvent(&wire, sizeof(wire), 0, &e, &offset));
    VGPU_VIDEO_HEADER h = {1, sizeof(h), 7};
    assert(VgpuVideoHeaderValid(&h, sizeof(h), sizeof(h), 7));
    assert(!VgpuVideoHeaderValid(&h, sizeof(h), sizeof(h), 8));
    assert(!VgpuVideoHeaderValid(&h, sizeof(h) - 1, sizeof(h), 7));
    for (int64_t pts : {INT64_MIN, INT64_MIN + 1, INT64_C(-1), INT64_C(0), INT64_MAX})
    {
        int64_t decoded = 0;
        assert(VmediaTimeToUs(VmediaTimeFromUs(pts), &decoded) && decoded == pts);
    }
    puts("PASS: production buffer/event validation, ownership, generation, truncation, duplicate DQBUF and PTS "
         "boundaries");
}
