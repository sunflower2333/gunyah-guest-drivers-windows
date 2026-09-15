/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef VIOGPU_VIDEO_IOCTL_H
#define VIOGPU_VIDEO_IOCTL_H
#include "video_core.h"
/* FILE_DEVICE_UNKNOWN, function 0x900..902, READ|WRITE, METHOD_BUFFERED. */
#define IOCTL_VV_EXEC 0x0022e400u
#define IOCTL_VV_WRITE 0x0022e404u
#define IOCTL_VV_READ 0x0022e408u
#define VV_API_VERSION 1u
#define VV_OP_CONFIG 0u
#define VV_OP_OPEN 1u
#define VV_OP_CLOSE 2u
#define VV_OP_IOCTL 3u
#define VV_OP_ALLOC 4u
#define VV_OP_QBUF 5u
#define VV_OP_STREAMON 6u
#define VV_OP_STREAMOFF 7u
#define VV_OP_EVENT 8u
#define VV_MAX_BUFFERS 64u
#define VV_COPY_LIMIT (4u*1024u*1024u)
#define VV_INTERFACE_GUID_INIT {0x740b41e7,0x840e,0x43b3,{0xa4,0x73,0x75,0x9a,0x1f,0xe4,0x76,0x51}}
#pragma pack(push,8)
typedef struct {
    uint32_t version, operation, queue, index, count, bytesused;
    int64_t timestamp_us;
    uint32_t code, payload_size;
    uint8_t payload[208];
} VV_REQUEST;
typedef struct {
    uint32_t version;
    int32_t error; /* Positive Linux errno; NTSTATUS reports transport errors. */
    uint32_t count, buffer_bytes;
    VV_CONFIG config;
    VV_EVENT event;
    uint32_t payload_size, reserved;
    uint8_t payload[208];
} VV_RESULT;
typedef struct { uint32_t queue, index, offset, length; } VV_COPY;
#pragma pack(pop)
VV_ASSERT(sizeof(VV_EVENT)==48);
VV_ASSERT(sizeof(VV_REQUEST)==248);
VV_ASSERT(sizeof(VV_RESULT)==320);
VV_ASSERT(sizeof(VV_COPY)==16);
#endif
