/* SPDX-License-Identifier: BSD-3-Clause */
#include "../../video/media_wire.h"
#include "../../video/video_ioctl.h"
#include <assert.h>
#include <stdio.h>
#if defined(__linux__)
#include <linux/videodev2.h>
#define SAME(a, b) static_assert(sizeof(a) == sizeof(b), #a " != " #b)
SAME(VMEDIA_BUFFER, v4l2_buffer);
SAME(VMEDIA_PLANE, v4l2_plane);
SAME(VMEDIA_FORMAT, v4l2_format);
SAME(VMEDIA_REQBUFS, v4l2_requestbuffers);
SAME(VMEDIA_V4L2_EVENT, v4l2_event);
static_assert(offsetof(VMEDIA_BUFFER, PointerOrOffset) == offsetof(v4l2_buffer, m), "buffer.m");
static_assert(offsetof(VMEDIA_BUFFER, Timestamp) == offsetof(v4l2_buffer, timestamp), "buffer.timestamp");
#endif
int main()
{
    uint8_t packet[184];
    memset(packet, 0xa5, sizeof(packet));
    assert(VmediaBuildQbuf(0, VMEDIA_OUTPUT, 3, 0x100010000ull, 65536, 1024, -1, packet, sizeof(packet)) == 184);
    VMEDIA_IOCTL cmd;
    VMEDIA_BUFFER b;
    VMEDIA_PLANE p;
    VMEDIA_SG sg;
    memcpy(&cmd, packet, 16);
    memcpy(&b, packet + 16, 88);
    memcpy(&p, packet + 104, 64);
    memcpy(&sg, packet + 168, 16);
    assert(cmd.Header.Command == 3 && cmd.SessionId == 0 && cmd.Code == 15);
    assert(b.Index == 3 && b.Memory == 2 && b.Length == 1 && b.PointerOrOffset == 0);
    assert(b.Timestamp.Seconds == -1 && b.Timestamp.Microseconds == 999999);
    assert(p.BytesUsed == 1024 && p.Length == 65536 && sg.Gpa == 0x100010000ull && !sg.Padding);
    for (uint32_t r : p.Reserved)
    {
        assert(r == 0);
    }
    int64_t us = 0;
    assert(VmediaTimeToUs(b.Timestamp, &us) && us == -1);
    assert(!VmediaBuildQbuf(1, VMEDIA_CAPTURE, 0, 0x10000, 4096, 1, 0, packet, sizeof(packet)));
    assert(!VmediaBuildQbuf(1, VMEDIA_OUTPUT, 0, UINT64_MAX - 2, 4096, 1, 0, packet, sizeof(packet)));
    assert(!VmediaBuildQbuf(1, VMEDIA_OUTPUT, 0, 0x10000, 4096, 4097, 0, packet, sizeof(packet)));
    assert(!VmediaBuildQbuf(1, VMEDIA_OUTPUT, 0, 0x10000, 4096, 4096, 0, packet, 183));
    VMEDIA_TIMEVAL bad = {INT64_MAX, 0};
    assert(!VmediaTimeToUs(bad, &us));
    bad.Seconds = 0;
    bad.Microseconds = 1000000;
    assert(!VmediaTimeToUs(bad, &us));
    assert(VmediaControlSize(8) == 0 && VmediaControlSize(15) == 0 && VmediaControlSize(17) == 0);
    assert(VmediaControlSize(71) == 0 && VmediaControlSize(72) == 0 && VmediaControlSize(73) == 0);
    assert(VmediaControlSize(0) == 0 && VmediaControlSize(0xc058560full) == 0);
#if defined(__linux__)
    assert(VmediaControlSize(2) == sizeof(v4l2_fmtdesc));
    assert(VmediaControlSize(21) == sizeof(v4l2_streamparm));
    assert(VmediaControlSize(36) == sizeof(v4l2_queryctrl));
    assert(VmediaControlSize(74) == sizeof(v4l2_frmsizeenum));
    assert(VmediaControlSize(75) == sizeof(v4l2_frmivalenum));
    assert(VmediaControlSize(77) == sizeof(v4l2_encoder_cmd));
    assert(VmediaControlSize(94) == sizeof(v4l2_selection));
    assert(VmediaControlSize(96) == sizeof(v4l2_decoder_cmd));
    assert(VmediaControlSize(103) == sizeof(v4l2_query_ext_ctrl));
#endif
    VMEDIA_FORMAT f = {};
    f.Type = VMEDIA_CAPTURE;
    f.Data[180] = 1;
    VmediaWrite32(f.Data + 20, 3110400);
    uint32_t size = 0;
    assert(VmediaFormatSize(&f, 1 << 24, &size) && size == 3110400);
    f.Data[180] = 2;
    assert(!VmediaFormatSize(&f, 1 << 24, &size));
    puts("PASS: wire layout, Linux ABI, QBUF/SG, negative PTS, overflow and control allowlist");
    return 0;
}
