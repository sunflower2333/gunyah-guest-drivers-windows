/* SPDX-License-Identifier: BSD-3-Clause
 * VioGPU video companion ABI. All handles refer to driver-owned ordinary RAM.
 * No user pointers, guest physical addresses, pool handles or restricted allocations.
 */
#ifndef VIOGPU_VIDEO_IOCTL_H
#define VIOGPU_VIDEO_IOCTL_H
#include "media_wire.h"
#define VGPU_VIDEO_VERSION          1u
#define VGPU_VIDEO_MAX_BUFFERS      32u
#define VGPU_VIDEO_MAX_BUFFER_BYTES (32u * 1024u * 1024u)
#define VGPU_VIDEO_MAX_TOTAL_BYTES  (512u * 1024u * 1024u)
#define VGPU_VIDEO_CONTROL_BYTES    256u
/* FILE_DEVICE_UNKNOWN, read/write access, METHOD_BUFFERED. */
#define VGPU_VIDEO_IOCTL(n)         ((0x22u << 16) | (3u << 14) | ((0x800u + (n)) << 2))
#define IOCTL_VGPU_VIDEO_OPEN       VGPU_VIDEO_IOCTL(0)
#define IOCTL_VGPU_VIDEO_CONTROL    VGPU_VIDEO_IOCTL(1)
#define IOCTL_VGPU_VIDEO_BUFFERS    VGPU_VIDEO_IOCTL(2)
#define IOCTL_VGPU_VIDEO_QUEUE      VGPU_VIDEO_IOCTL(3)
#define IOCTL_VGPU_VIDEO_EVENT      VGPU_VIDEO_IOCTL(4)
#define IOCTL_VGPU_VIDEO_COPY       VGPU_VIDEO_IOCTL(5)
#define IOCTL_VGPU_VIDEO_STREAM     VGPU_VIDEO_IOCTL(6)
#define IOCTL_VGPU_VIDEO_CLOSE      VGPU_VIDEO_IOCTL(7)
#define VGPU_VIDEO_INTERFACE_GUID_INIT                                                                                 \
    {                                                                                                                  \
        0xd4f38a6c, 0xa175, 0x4d6f,                                                                                    \
        {                                                                                                              \
            0x94, 0xeb, 0x73, 0x62, 0x20, 0xc8, 0x3e, 0x51                                                             \
        }                                                                                                              \
    }
#pragma pack(push, 8)
typedef struct VGPU_VIDEO_HEADER
{
    uint32_t Version, Size;
    uint64_t Generation;
} VGPU_VIDEO_HEADER;
typedef struct VGPU_VIDEO_OPEN
{
    VGPU_VIDEO_HEADER Header;
    VMEDIA_CONFIG Config;
} VGPU_VIDEO_OPEN;
typedef struct VGPU_VIDEO_CONTROL
{
    VGPU_VIDEO_HEADER Header;
    uint32_t Code, PayloadBytes, LinuxErrno, Reserved;
    uint8_t Payload[VGPU_VIDEO_CONTROL_BYTES];
} VGPU_VIDEO_CONTROL;
typedef struct VGPU_VIDEO_BUFFERS
{
    VGPU_VIDEO_HEADER Header;
    uint32_t Type, Count, BufferBytes, LinuxErrno;
} VGPU_VIDEO_BUFFERS;
typedef struct VGPU_VIDEO_BUFFER
{
    VGPU_VIDEO_HEADER Header;
    uint32_t Type, Index, BytesUsed, Flags;
    int64_t TimestampUs;
    /* QUEUE(OUTPUT): BytesUsed data bytes follow.
     * COPY(CAPTURE): BytesUsed data bytes are returned after this header. */
} VGPU_VIDEO_BUFFER;
typedef struct VGPU_VIDEO_STREAM
{
    VGPU_VIDEO_HEADER Header;
    uint32_t Type, On, LinuxErrno, Reserved;
} VGPU_VIDEO_STREAM;
typedef struct VGPU_VIDEO_EVENT
{
    VGPU_VIDEO_HEADER Header;
    uint32_t TimeoutMs, Event, Type, Index;
    uint32_t BytesUsed, Flags, Sequence, V4l2Type;
    int64_t TimestampUs;
    uint32_t LinuxErrno, Changes;
} VGPU_VIDEO_EVENT;
#pragma pack(pop)
VMEDIA_ASSERT(sizeof(VGPU_VIDEO_HEADER) == 16);
VMEDIA_ASSERT(sizeof(VGPU_VIDEO_CONTROL) == 288);
VMEDIA_ASSERT(sizeof(VGPU_VIDEO_BUFFER) == 40);
VMEDIA_ASSERT(sizeof(VGPU_VIDEO_EVENT) == 64);
#endif
