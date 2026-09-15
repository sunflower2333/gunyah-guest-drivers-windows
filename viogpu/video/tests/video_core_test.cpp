// SPDX-License-Identifier: BSD-3-Clause
#include "../include/video_core.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <limits>
#ifdef __linux__
#include <linux/videodev2.h>
static_assert(sizeof(VV_BUFFER)==sizeof(v4l2_buffer),"buffer ABI");
static_assert(offsetof(VV_BUFFER,timestamp)==offsetof(v4l2_buffer,timestamp),"time ABI");
static_assert(offsetof(VV_BUFFER,m)==offsetof(v4l2_buffer,m),"pointer ABI");
static_assert(sizeof(VV_PLANE)==sizeof(v4l2_plane),"plane ABI");
static_assert(sizeof(VV_FORMAT)==sizeof(v4l2_format),"format ABI");
static_assert(offsetof(VV_FORMAT,fmt)==offsetof(v4l2_format,fmt),"format alignment");
static_assert(sizeof(VV_PIX_MPLANE)==sizeof(v4l2_pix_format_mplane),"multi-format ABI");
static_assert(sizeof(VV_REQBUFS)==sizeof(v4l2_requestbuffers),"reqbufs ABI");
static_assert(sizeof(VV_FMTDESC)==sizeof(v4l2_fmtdesc),"fmtdesc ABI");
static_assert(sizeof(VV_V4L2_EVENT)==sizeof(v4l2_event),"event ABI");
static_assert(VV_IOCTL_QBUF==_IOC_NR(VIDIOC_QBUF),"ioctl number, not _IOC");
#endif
#define CHECK(e) do { if (!(e)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#e); std::exit(1); } } while (0)

// Compare the production serializer to independently populated Linux UAPI bytes.
static void test_wire()
{
    unsigned char wire[VV_COMMAND_BYTES]={};
    size_t n=0,r=0;
    CHECK(vv_build_qbuf(wire,sizeof(wire),7,VV_OUTPUT_MPLANE,3,0x80020000u,4096,122,-1,&n,&r));
    CHECK(n==184 && r==160);
    CHECK(wire[0]==3 && wire[8]==7 && wire[12]==15);
    VV_BUFFER b; VV_PLANE p; VV_SG s;
    std::memcpy(&b,wire+16,sizeof(b));
    std::memcpy(&p,wire+104,sizeof(p));
    std::memcpy(&s,wire+168,sizeof(s));
    CHECK(b.timestamp.seconds==-1 && b.timestamp.microseconds==999999);
    CHECK(b.memory==2 && b.length==1 && b.m==0 && b.padding0==0 && b.padding1==0);
    CHECK(p.length==4096 && p.bytesused==122 && p.data_offset==0 && p.m==0);
    CHECK(s.address==0x80020000u && s.length==4096 && s.padding==0);
#ifdef __linux__
    v4l2_buffer linux_b={}; v4l2_plane linux_p={};
    linux_b.index=3; linux_b.type=V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    linux_b.field=V4L2_FIELD_NONE; linux_b.memory=V4L2_MEMORY_USERPTR;
    linux_b.length=1; linux_b.timestamp.tv_sec=-1; linux_b.timestamp.tv_usec=999999;
    linux_p.length=4096; linux_p.bytesused=122;
    CHECK(std::memcmp(&b,&linux_b,sizeof(b))==0);
    CHECK(std::memcmp(&p,&linux_p,sizeof(p))==0);
#endif
    CHECK(!vv_build_qbuf(wire,183,7,VV_OUTPUT_MPLANE,0,1,4096,0,0,&n,&r));
    CHECK(!vv_build_qbuf(wire,sizeof(wire),7,VV_OUTPUT,0,UINT64_MAX-10,4096,0,0,&n,&r));
    CHECK(!vv_build_qbuf(wire,sizeof(wire),7,VV_CAPTURE,0,0,4096,1,0,&n,&r));
    CHECK(!vv_build_qbuf(wire,sizeof(wire),7,3,0,0,4096,0,0,&n,&r));
    CHECK(!vv_build_qbuf(wire,sizeof(wire),7,VV_OUTPUT,0,0,0,0,0,&n,&r));
    CHECK(!vv_build_qbuf(wire,sizeof(wire),7,VV_OUTPUT,0,0,1,2,0,&n,&r));
    CHECK(vv_build_qbuf(wire,sizeof(wire),7,VV_OUTPUT,0,4096,4096,8,42,&n,&r));
    CHECK(n==120 && r==96);
}

// Check DQBUF ownership and malformed host data against the real core functions.
static void test_events()
{
    VV_DQBUF_EVENT raw={}; VV_EVENT e={};
    raw.header.event=VV_EVENT_DQBUF; raw.header.session=9;
    raw.buffer.type=VV_CAPTURE_MPLANE; raw.buffer.memory=VV_USERPTR;
    raw.buffer.length=1; raw.buffer.flags=VV_BUF_LAST;
    raw.planes[0].length=4096; raw.planes[0].bytesused=100;
    raw.planes[0].data_offset=4;
    // Host pointers must never escape through the normalized event.
    raw.buffer.m=UINT64_MAX; raw.planes[0].m=UINT64_MAX;
    unsigned char unaligned[sizeof(raw)+1]={};
    std::memcpy(unaligned+1,&raw,sizeof(raw));
    CHECK(vv_parse_event(unaligned+1,sizeof(raw),&e));
    CHECK(e.bytesused==100 && e.offset==4 && e.flags==VV_BUF_LAST);
    VV_BUFFER_STATE state={4096,VV_OWNED};
    CHECK(!vv_buffer_return(&state,&e));
    CHECK(vv_buffer_submit(&state));
    CHECK(!vv_buffer_submit(&state));
    CHECK(state.state==VV_HOST); // QBUF ACK must not change this state.
    VV_EVENT bad=e; bad.bytesused=4097;
    CHECK(!vv_buffer_return(&state,&bad) && state.state==VV_HOST);
    CHECK(vv_buffer_return(&state,&e));
    CHECK(!vv_buffer_return(&state,&e));
    CHECK(vv_buffer_submit(&state));
    for (size_t n=0;n<sizeof(raw);++n) CHECK(!vv_parse_event(&raw,n,&e));
    raw.buffer.length=9; CHECK(!vv_parse_event(&raw,sizeof(raw),&e));
    raw.buffer.length=1; raw.planes[0].data_offset=101;
    CHECK(!vv_parse_event(&raw,sizeof(raw),&e));
    raw.planes[0].data_offset=0; raw.buffer.timestamp.microseconds=1000000;
    CHECK(!vv_parse_event(&raw,sizeof(raw),&e));
    raw.buffer.timestamp.microseconds=0; raw.buffer.timestamp.seconds=INT64_MAX;
    CHECK(!vv_parse_event(&raw,sizeof(raw),&e));
    raw.buffer.timestamp.seconds=0; raw.planes[0].bytesused=0;
    CHECK(vv_parse_event(&raw,sizeof(raw),&e) && e.bytesused==0); // Empty LAST is legal.
    raw.header.event=3; CHECK(!vv_parse_event(&raw,sizeof(raw),&e));
    struct { VV_EVENT_HEADER hdr; int32_t error; uint32_t pad; } error={{0,9},5,0};
    CHECK(vv_parse_event(&error,sizeof(error),&e) && e.detail==5);
    error.error=-5; CHECK(!vv_parse_event(&error,sizeof(error),&e));
    struct { VV_EVENT_HEADER hdr; VV_V4L2_EVENT ev; } change={{2,9},{}};
    change.ev.type=VV_V4L2_SOURCE_CHANGE;
    CHECK(vv_parse_event(&change,sizeof(change),&e) && e.detail==VV_V4L2_SOURCE_CHANGE);
}

// Verify the restricted ioctl list and format admission before allocating memory.
static void test_admission()
{
    uint32_t a=0,b=0;
    CHECK(vv_ioctl_shape(VV_IOCTL_G_FMT,&a,&b) && a==208 && b==208);
    CHECK(vv_ioctl_shape(VV_IOCTL_SUBSCRIBE_EVENT,&a,&b) && a==32 && b==0);
    CHECK(!vv_ioctl_shape(72,&a,&b)); // Nested G/S_EXT_CTRLS unsupported, not raw forwarded.
    CHECK(!vv_ioctl_shape(VV_IOCTL_REQBUFS,&a,&b)); // Driver owns allocation and SG.
    CHECK(!vv_ioctl_shape(VV_IOCTL_QBUF,&a,&b));
    CHECK(!vv_ioctl_shape(VV_IOCTL_STREAMOFF,&a,&b)); // Driver owns ownership barrier.
    VV_FORMAT f={}; f.type=VV_CAPTURE_MPLANE;
    CHECK(!vv_format_size(&f,&a));
    f.fmt.mp.num_planes=1; f.fmt.mp.planes[0].sizeimage=1920u*1080u*3u/2u;
    CHECK(vv_format_size(&f,&a) && a==3110400);
    f.fmt.mp.num_planes=2; CHECK(!vv_format_size(&f,&a));
    f.fmt.mp.num_planes=1; f.fmt.mp.planes[0].sizeimage=UINT32_MAX;
    CHECK(!vv_format_size(&f,&a));
}

// Execute the production core without a device or any synthesized success path.
int main()
{
    test_wire(); test_events(); test_admission();
    std::puts("PASS: wire ABI, Linux byte comparison, ownership, events, admission");
    return 0;
}
