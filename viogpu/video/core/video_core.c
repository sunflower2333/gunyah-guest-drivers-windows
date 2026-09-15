/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 DroidVM contributors.
 */
#include "../include/video_core.h"
#include <string.h>
#if !defined(VV_KERNEL)
#include <limits.h>
#endif

/* Accept empty completion only for CLOSE, as the pinned upstream runner writes no reply. */
int vv_reply_length_valid(uint32_t command, size_t used, size_t capacity)
{
    if (capacity<sizeof(VV_RESPONSE) || capacity>VV_COMMAND_BYTES || used>capacity) return 0;
    if (command==VV_CMD_CLOSE) return used==0 || used==sizeof(VV_RESPONSE);
    return used>=sizeof(VV_RESPONSE);
}

/* Accept only codec video queues, never overlay or pointer-bearing window types. */
int vv_queue_valid(uint32_t queue)
{
    return queue==VV_CAPTURE || queue==VV_OUTPUT ||
           queue==VV_CAPTURE_MPLANE || queue==VV_OUTPUT_MPLANE;
}

/* Identify the two V4L2 multiplanar codec queues. */
int vv_multiplanar(uint32_t queue)
{
    return queue==VV_CAPTURE_MPLANE || queue==VV_OUTPUT_MPLANE;
}

/* Limit generic ioctl forwarding to fixed-size payloads without nested pointers. */
int vv_ioctl_shape(uint32_t code, uint32_t *input, uint32_t *output)
{
    uint32_t size;
    if (!input || !output) return 0;
    switch (code) {
    case VV_IOCTL_ENUM_FMT: size=64; break;
    case VV_IOCTL_G_FMT:
    case VV_IOCTL_S_FMT:
    case VV_IOCTL_TRY_FMT: size=208; break;
    case VV_IOCTL_G_PARM:
    case VV_IOCTL_S_PARM: size=204; break;
    case VV_IOCTL_G_CTRL:
    case VV_IOCTL_S_CTRL: size=8; break;
    case VV_IOCTL_ENUM_FRAMESIZES: size=44; break;
    case VV_IOCTL_ENUM_FRAMEINTERVALS: size=52; break;
    case VV_IOCTL_G_SELECTION: size=64; break;
    case VV_IOCTL_DECODER_CMD: size=72; break;
    case VV_IOCTL_ENCODER_CMD: size=40; break;
    case VV_IOCTL_SUBSCRIBE_EVENT:
        *input=32; *output=0; return 1;
    default: return 0;
    }
    *input=size; *output=size;
    return 1;
}

/* Serialize the arch64 USERPTR buffer followed by its one physical SG entry. */
int vv_build_qbuf(
    void *destination, size_t capacity, uint32_t session, uint32_t queue,
    uint32_t index, uint64_t address, uint32_t length, uint32_t bytesused,
    int64_t timestamp_us, size_t *written, size_t *reply_size)
{
    VV_IOCTL command;
    VV_BUFFER buffer;
    VV_PLANE plane;
    VV_SG sg;
    uint8_t *out=(uint8_t *)destination;
    size_t n=sizeof(command)+sizeof(buffer);
    if (!destination || !written || !reply_size || !vv_queue_valid(queue) ||
        !length || length>VV_MAX_BUFFER_BYTES || bytesused>length ||
        address>UINT64_MAX-length) return 0;
    if ((queue==VV_CAPTURE || queue==VV_CAPTURE_MPLANE) && bytesused) return 0;
    if (vv_multiplanar(queue)) n+=sizeof(plane);
    if (capacity<n+sizeof(sg)) return 0;
    memset(&command,0,sizeof(command));
    memset(&buffer,0,sizeof(buffer));
    memset(&plane,0,sizeof(plane));
    memset(&sg,0,sizeof(sg));
    command.header.command=VV_CMD_IOCTL;
    command.session=session;
    command.code=VV_IOCTL_QBUF;
    buffer.index=index; buffer.type=queue; buffer.memory=VV_USERPTR;
    buffer.field=1; /* V4L2_FIELD_NONE */
    buffer.timestamp.seconds=timestamp_us/1000000;
    buffer.timestamp.microseconds=timestamp_us%1000000;
    if (buffer.timestamp.microseconds<0) {
        --buffer.timestamp.seconds;
        buffer.timestamp.microseconds+=1000000;
    }
    if (vv_multiplanar(queue)) {
        buffer.length=1;
        plane.length=length; plane.bytesused=bytesused;
    } else {
        buffer.length=length; buffer.bytesused=bytesused;
    }
    /* m/userptr values are opaque on this wire, not addresses; SG carries GPA. */
    sg.address=address; sg.length=length;
    memcpy(out,&command,sizeof(command));
    memcpy(out+sizeof(command),&buffer,sizeof(buffer));
    if (vv_multiplanar(queue)) memcpy(out+sizeof(command)+sizeof(buffer),&plane,sizeof(plane));
    memcpy(out+n,&sg,sizeof(sg));
    *written=n+sizeof(sg);
    *reply_size=sizeof(VV_RESPONSE)+sizeof(VV_BUFFER)+
                (vv_multiplanar(queue)?sizeof(VV_PLANE):0);
    return 1;
}

/* Normalize host events and reject malformed lengths, planes, and timestamps. */
int vv_parse_event(const void *source, size_t length, VV_EVENT *event)
{
    VV_EVENT_HEADER hdr;
    VV_BUFFER b;
    VV_PLANE p;
    VV_V4L2_EVENT v;
    int32_t error;
    const uint8_t *in=(const uint8_t *)source;
    if (!source || !event || length<sizeof(hdr) || length>VV_EVENT_BYTES) return 0;
    memcpy(&hdr,in,sizeof(hdr));
    memset(event,0,sizeof(*event));
    event->event=hdr.event; event->session=hdr.session;
    if (hdr.event==VV_EVENT_ERROR) {
        if (length!=16) return 0;
        memcpy(&error,in+8,sizeof(error));
        if (error<=0 || error>4095) return 0;
        event->detail=(uint32_t)error;
        return 1;
    }
    if (hdr.event==VV_EVENT_V4L2) {
        if (length!=8+sizeof(v)) return 0;
        memcpy(&v,in+8,sizeof(v));
        event->detail=v.type;
        memcpy(&event->flags,v.data,sizeof(event->flags));
        event->sequence=v.sequence;
        return 1;
    }
    if (hdr.event!=VV_EVENT_DQBUF || length!=sizeof(VV_DQBUF_EVENT)) return 0;
    memcpy(&b,in+8,sizeof(b));
    if (!vv_queue_valid(b.type) || b.memory!=VV_USERPTR ||
        b.timestamp.microseconds<0 || b.timestamp.microseconds>=1000000 ||
        b.timestamp.seconds>INT64_MAX/1000000 ||
        b.timestamp.seconds<INT64_MIN/1000000) return 0;
    event->timestamp_us=b.timestamp.seconds*1000000;
    if (event->timestamp_us>INT64_MAX-b.timestamp.microseconds) return 0;
    event->timestamp_us+=b.timestamp.microseconds;
    event->queue=b.type; event->index=b.index;
    event->flags=b.flags; event->sequence=b.sequence;
    if (vv_multiplanar(b.type)) {
        /* The v1 allocator deliberately admits only contiguous, one-plane formats. */
        if (b.length!=1) return 0;
        memcpy(&p,in+8+sizeof(b),sizeof(p));
        if (p.bytesused>p.length || p.data_offset>p.bytesused) return 0;
        event->bytesused=p.bytesused; event->offset=p.data_offset;
    } else {
        if (b.bytesused>b.length) return 0;
        event->bytesused=b.bytesused;
    }
    return 1;
}

/* Ownership moves to the device before the descriptor can be observed. */
int vv_buffer_submit(VV_BUFFER_STATE *buffer)
{
    if (!buffer || !buffer->capacity ||
        (buffer->state!=VV_OWNED && buffer->state!=VV_RETURNED)) return 0;
    buffer->state=VV_HOST;
    return 1;
}

/* A returned buffer remains inaccessible when any host metadata is invalid. */
int vv_buffer_return(VV_BUFFER_STATE *buffer, const VV_EVENT *event)
{
    if (!buffer || !event || event->event!=VV_EVENT_DQBUF ||
        buffer->state!=VV_HOST || event->bytesused>buffer->capacity ||
        event->offset>event->bytesused) return 0;
    buffer->state=VV_RETURNED;
    return 1;
}

/* Reject unsupported multi-plane layouts instead of silently truncating them. */
int vv_format_size(const VV_FORMAT *format, uint32_t *size)
{
    uint32_t bytes;
    if (!format || !size || !vv_queue_valid(format->type)) return 0;
    if (vv_multiplanar(format->type)) {
        if (format->fmt.mp.num_planes!=1) return 0;
        bytes=format->fmt.mp.planes[0].sizeimage;
    } else bytes=format->fmt.pix.sizeimage;
    if (!bytes || bytes>VV_MAX_BUFFER_BYTES) return 0;
    *size=bytes;
    return 1;
}
