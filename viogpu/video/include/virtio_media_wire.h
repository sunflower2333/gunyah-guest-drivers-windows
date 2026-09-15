/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 DroidVM contributors.
 * Fixed little-endian, Linux arch64 wire ABI; never use Windows long/pointers.
 * Upstream: Droid-VM/virtio-media@6b6d2b3, Droid-VM/v4l2r@7eb3afa.
 */
#ifndef VIOGPU_VIDEO_WIRE_H
#define VIOGPU_VIDEO_WIRE_H
#if defined(VV_KERNEL)
/* Do not mix the user-mode MSVC CRT stdint.h into WDK km/crt headers. */
#include <ntddk.h>
typedef UINT8 uint8_t;
typedef UINT16 uint16_t;
typedef UINT32 uint32_t;
typedef UINT64 uint64_t;
typedef INT32 int32_t;
typedef INT64 int64_t;
#define UINT64_MAX ((uint64_t)~0ull)
#define INT64_MAX ((int64_t)0x7fffffffffffffffll)
#define INT64_MIN (-INT64_MAX-1ll)
#else
#include <stddef.h>
#include <stdint.h>
#endif
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error The current video transport requires a little-endian target
#endif
#define VV_JOIN_(a,b) a##b
#define VV_JOIN(a,b) VV_JOIN_(a,b)
#define VV_ASSERT(e) typedef char VV_JOIN(vv_assert_,__LINE__)[(e) ? 1 : -1]
#define VV_ID_MEDIA 48u
#define VV_QUEUE_COMMAND 0u
#define VV_QUEUE_EVENT 1u
#define VV_CMD_OPEN 1u
#define VV_CMD_CLOSE 2u
#define VV_CMD_IOCTL 3u
#define VV_CAPTURE 1u
#define VV_OUTPUT 2u
#define VV_CAPTURE_MPLANE 9u
#define VV_OUTPUT_MPLANE 10u
#define VV_USERPTR 2u
#define VV_MAX_PLANES 8u
#define VV_BUF_ERROR 0x0040u
#define VV_BUF_LAST 0x00100000u
#define VV_EVENT_ERROR 0u
#define VV_EVENT_DQBUF 1u
#define VV_EVENT_V4L2 2u
#define VV_V4L2_EOS 2u
#define VV_V4L2_SOURCE_CHANGE 5u
#define VV_IOCTL_ENUM_FMT 2u
#define VV_IOCTL_G_FMT 4u
#define VV_IOCTL_S_FMT 5u
#define VV_IOCTL_REQBUFS 8u
#define VV_IOCTL_QBUF 15u
#define VV_IOCTL_STREAMON 18u
#define VV_IOCTL_STREAMOFF 19u
#define VV_IOCTL_G_PARM 21u
#define VV_IOCTL_S_PARM 22u
#define VV_IOCTL_G_CTRL 27u
#define VV_IOCTL_S_CTRL 28u
#define VV_IOCTL_TRY_FMT 64u
#define VV_IOCTL_ENUM_FRAMESIZES 74u
#define VV_IOCTL_ENUM_FRAMEINTERVALS 75u
#define VV_IOCTL_ENCODER_CMD 77u
#define VV_IOCTL_SUBSCRIBE_EVENT 90u
#define VV_IOCTL_G_SELECTION 94u
#define VV_IOCTL_DECODER_CMD 96u
#define VV_FOURCC(a,b,c,d) ((uint32_t)(a)|((uint32_t)(b)<<8)|((uint32_t)(c)<<16)|((uint32_t)(d)<<24))
#define VV_NV12 VV_FOURCC('N','V','1','2')
#define VV_H264 VV_FOURCC('H','2','6','4')
#define VV_HEVC VV_FOURCC('H','E','V','C')
#pragma pack(push,8)
typedef struct { uint32_t command, padding; } VV_COMMAND;
typedef struct { int32_t error; uint32_t padding; } VV_RESPONSE;
typedef struct { VV_COMMAND header; uint32_t session, code; } VV_IOCTL;
typedef struct { VV_RESPONSE header; uint32_t session, padding; } VV_OPEN_RESPONSE;
typedef struct { uint32_t device_caps, device_type; uint8_t card[32]; } VV_CONFIG;
typedef struct { uint64_t address; uint32_t length, padding; } VV_SG;
typedef struct { int64_t seconds, microseconds; } VV_TIMEVAL;
typedef struct {
    uint32_t type, flags;
    uint8_t frames, seconds, minutes, hours, userbits[4];
} VV_TIMECODE;
typedef struct {
    uint32_t index, type, bytesused, flags, field, padding0;
    VV_TIMEVAL timestamp;
    VV_TIMECODE timecode;
    uint32_t sequence, memory;
    uint64_t m;
    uint32_t length, reserved2;
    int32_t request_fd;
    uint32_t padding1;
} VV_BUFFER;
typedef struct {
    uint32_t bytesused, length;
    uint64_t m;
    uint32_t data_offset, reserved[11];
} VV_PLANE;
typedef struct { uint32_t sizeimage, bytesperline; uint16_t reserved[6]; } VV_PLANE_FORMAT;
typedef struct {
    uint32_t width, height, fourcc, field, colorspace;
    VV_PLANE_FORMAT planes[8];
    uint8_t num_planes, flags, ycbcr, quantization, xfer, reserved[7];
} VV_PIX_MPLANE;
typedef struct {
    uint32_t width, height, fourcc, field, bytesperline, sizeimage;
    uint32_t colorspace, priv, flags, ycbcr, quantization, xfer;
} VV_PIX;
typedef struct {
    uint32_t type, padding;
    union { uint64_t align[25]; VV_PIX pix; VV_PIX_MPLANE mp; } fmt;
} VV_FORMAT;
typedef struct {
    uint32_t count, type, memory, capabilities;
    uint8_t flags, reserved[3];
} VV_REQBUFS;
typedef struct {
    uint32_t index, type, flags;
    uint8_t description[32];
    uint32_t fourcc, mbus_code, reserved[3];
} VV_FMTDESC;
typedef struct { uint32_t type, id, flags, reserved[5]; } VV_SUBSCRIPTION;
typedef struct { uint32_t type, padding; uint8_t data[64];
    uint32_t pending, sequence; int64_t seconds, nanoseconds;
    uint32_t id, reserved[8], padding1;
} VV_V4L2_EVENT;
typedef struct { uint32_t event, session; } VV_EVENT_HEADER;
/* Upstream sends ALL eight planes, including for a single-planar buffer. */
typedef struct { VV_EVENT_HEADER header; VV_BUFFER buffer; VV_PLANE planes[8]; } VV_DQBUF_EVENT;
#pragma pack(pop)
VV_ASSERT(sizeof(VV_COMMAND)==8);
VV_ASSERT(sizeof(VV_RESPONSE)==8);
VV_ASSERT(sizeof(VV_IOCTL)==16);
VV_ASSERT(sizeof(VV_OPEN_RESPONSE)==16);
VV_ASSERT(sizeof(VV_CONFIG)==40);
VV_ASSERT(sizeof(VV_SG)==16);
VV_ASSERT(sizeof(VV_BUFFER)==88);
VV_ASSERT(offsetof(VV_BUFFER,timestamp)==24);
VV_ASSERT(offsetof(VV_BUFFER,m)==64);
VV_ASSERT(sizeof(VV_PLANE)==64);
VV_ASSERT(sizeof(VV_PLANE_FORMAT)==20);
VV_ASSERT(sizeof(VV_PIX_MPLANE)==192);
VV_ASSERT(sizeof(VV_FORMAT)==208);
VV_ASSERT(offsetof(VV_FORMAT,fmt)==8);
VV_ASSERT(sizeof(VV_REQBUFS)==20);
VV_ASSERT(sizeof(VV_FMTDESC)==64);
VV_ASSERT(sizeof(VV_SUBSCRIPTION)==32);
VV_ASSERT(sizeof(VV_V4L2_EVENT)==136);
VV_ASSERT(sizeof(VV_DQBUF_EVENT)==608);
#endif
