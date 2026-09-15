/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef VIOGPU_VIDEO_CORE_H
#define VIOGPU_VIDEO_CORE_H
#include "virtio_media_wire.h"
#ifdef __cplusplus
extern "C" {
#endif
#define VV_MAX_BUFFER_BYTES (32u*1024u*1024u)
#define VV_COMMAND_BYTES 512u
#define VV_EVENT_BYTES 608u
/* No pointer, GPA, or host address is ever returned in this normalized event. */
typedef struct {
    uint32_t event, session, queue, index, flags, bytesused, offset, sequence;
    int64_t timestamp_us;
    uint32_t detail, reserved;
} VV_EVENT;
typedef struct { uint32_t capacity, state; } VV_BUFFER_STATE;
enum { VV_OWNED=0, VV_HOST=1, VV_RETURNED=2 };
/* CLOSE succeeds with zero used bytes; other replies require a response header. */
int vv_reply_length_valid(uint32_t command, size_t used, size_t capacity);
/* Validate one of the two video queue directions, including its planar form. */
int vv_queue_valid(uint32_t queue);
/* Return whether this queue uses the multiplanar wire representation. */
int vv_multiplanar(uint32_t queue);
/* Translate a fixed-payload ioctl into its request size and reply payload size. */
int vv_ioctl_shape(uint32_t code, uint32_t *input, uint32_t *output);
/* Build one-plane, driver-owned USERPTR QBUF; physical address is kernel-only. */
int vv_build_qbuf(
    void *destination, size_t capacity, uint32_t session, uint32_t queue,
    uint32_t index, uint64_t address, uint32_t length, uint32_t bytesused,
    int64_t timestamp_us, size_t *written, size_t *reply_size);
/* Parse an untrusted event using copies, so unaligned descriptor bytes are safe. */
int vv_parse_event(const void *source, size_t length, VV_EVENT *event);
/* Change ownership before publishing QBUF; double-queueing is rejected. */
int vv_buffer_submit(VV_BUFFER_STATE *buffer);
/* Return ownership only on a validated DQBUF, never on a QBUF command reply. */
int vv_buffer_return(VV_BUFFER_STATE *buffer, const VV_EVENT *event);
/* Validate codec format and obtain the one-plane sizeimage supported by v1. */
int vv_format_size(const VV_FORMAT *format, uint32_t *size);
#ifdef __cplusplus
}
#endif
#endif
