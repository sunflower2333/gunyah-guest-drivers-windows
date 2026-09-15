/* SPDX-License-Identifier: BSD-3-Clause
 * Explicit video backend exported by viogpud3d.dll. This is not a DXVA/D3D Video DDI.
 * The caller supplies complete codec access units (e.g. Annex B AVC), not DXVA slice structures.
 * All functions return HRESULTs. CONTROL/BUFFERS/STREAM also return LinuxErrno.
 */
#ifndef VIOGPU_VIDEO_API_H
#define VIOGPU_VIDEO_API_H
#include <windows.h>
#include "video_ioctl.h"
#ifdef __cplusplus
extern "C"
{
#endif
    /* Open the Nth present VioGPU media function. Probe formats to determine encoder vs decoder. */
    HRESULT WINAPI VioGpuVideoOpen(UINT index, HANDLE *session, VGPU_VIDEO_OPEN *information);
    /* Enumerate/configure pointer-free V4L2 controls through the media command queue. */
    HRESULT WINAPI VioGpuVideoControl(HANDLE session, VGPU_VIDEO_CONTROL *control);
    /* Request/return driver-owned buffers; Count=0 releases a stopped queue. */
    HRESULT WINAPI VioGpuVideoAllocate(HANDLE session, VGPU_VIDEO_BUFFERS *buffers);
    /* Submit one complete bitstream AU or raw frame; CAPTURE uses data=NULL and BytesUsed=0. */
    HRESULT WINAPI VioGpuVideoQueue(HANDLE session, const VGPU_VIDEO_BUFFER *buffer, const void *data);
    /* Wait up to 1000ms for a sanitized event; S_FALSE means no event, not end-of-stream. */
    HRESULT WINAPI VioGpuVideoDequeue(HANDLE session, VGPU_VIDEO_EVENT *event);
    /* Copy a returned CAPTURE buffer; queue it again only after the consumer is finished. */
    HRESULT WINAPI VioGpuVideoCopy(HANDLE session, VGPU_VIDEO_BUFFER *buffer, void *data, UINT capacity);
    /* Start/stop one queue. STOP(OUTPUT) is not a substitute for codec drain. */
    HRESULT WINAPI VioGpuVideoStream(HANDLE session, VGPU_VIDEO_STREAM *stream);
    /* Close/revoke the session; check failure before considering hardware teardown successful. */
    HRESULT WINAPI VioGpuVideoClose(HANDLE session);
#ifdef __cplusplus
}
#endif
#endif
