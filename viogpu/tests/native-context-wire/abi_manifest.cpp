#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#if defined(_MSC_VER)
#include <fcntl.h>
#include <io.h>
#endif

#if defined(ABI_ENDPOINT_WINDOWS)
#define VIOGPU_WIRE_U8  uint8_t
#define VIOGPU_WIRE_I8  int8_t
#define VIOGPU_WIRE_U32 uint32_t
#define VIOGPU_WIRE_I32 int32_t
#define VIOGPU_WIRE_U64 uint64_t
#include "viogpu_3d_wire.h"
#define ABI_CAPSET                struct virgl_renderer_capset_drm
#define ABI_CAPSET_MSM            msm
#define ABI_CAPSET_PADDING        padding
#define ABI_CAPSET_ID             VIRTIO_GPU_CAPSET_DRM
#define ABI_GET_PARAM_REQ         struct msm_ccmd_ioctl_simple_get_param_req
#define ABI_GET_PARAM_RSP         struct msm_ccmd_ioctl_simple_get_param_rsp
#define ABI_SUBMITQUEUE_NEW_REQ   struct msm_ccmd_ioctl_simple_submitqueue_new_req
#define ABI_SUBMITQUEUE_NEW_RSP   struct msm_ccmd_ioctl_simple_submitqueue_new_rsp
#define ABI_SUBMITQUEUE_CLOSE_REQ struct msm_ccmd_ioctl_simple_submitqueue_close_req
#define ABI_SUBMITQUEUE_CLOSE_RSP struct msm_ccmd_ioctl_simple_submitqueue_close_rsp
#else
#include <linux/virtio_gpu.h>

#if defined(__cplusplus)
extern "C"
{
#endif
#include "drm_hw.h"
#include "msm_drm.h"
#include "msm_proto.h"
#include "virtgpu_drm.h"
#if defined(__cplusplus)
}
#endif

#define ABI_CAPSET         struct virgl_renderer_capset_drm
#define ABI_CAPSET_MSM     u.msm
#define ABI_CAPSET_PADDING pad
#define ABI_CAPSET_ID      VIRTGPU_DRM_CAPSET_DRM

#pragma pack(push, 4)
struct abi_msm_ccmd_ioctl_simple_get_param_req
{
    struct vdrm_ccmd_req hdr;
    uint32_t ioctl_cmd;
    struct drm_msm_param param;
};

struct abi_msm_ccmd_ioctl_simple_get_param_rsp
{
    struct vdrm_ccmd_rsp hdr;
    int32_t ret;
    struct drm_msm_param param;
};

struct abi_msm_ccmd_ioctl_simple_submitqueue_new_req
{
    struct vdrm_ccmd_req hdr;
    uint32_t ioctl_cmd;
    struct drm_msm_submitqueue submitqueue;
};

struct abi_msm_ccmd_ioctl_simple_submitqueue_new_rsp
{
    struct vdrm_ccmd_rsp hdr;
    int32_t ret;
    struct drm_msm_submitqueue submitqueue;
};

struct abi_msm_ccmd_ioctl_simple_submitqueue_close_req
{
    struct vdrm_ccmd_req hdr;
    uint32_t ioctl_cmd;
    uint32_t queue_id;
};

struct abi_msm_ccmd_ioctl_simple_submitqueue_close_rsp
{
    struct vdrm_ccmd_rsp hdr;
    int32_t ret;
};
#pragma pack(pop)

#define ABI_GET_PARAM_REQ         struct abi_msm_ccmd_ioctl_simple_get_param_req
#define ABI_GET_PARAM_RSP         struct abi_msm_ccmd_ioctl_simple_get_param_rsp
#define ABI_SUBMITQUEUE_NEW_REQ   struct abi_msm_ccmd_ioctl_simple_submitqueue_new_req
#define ABI_SUBMITQUEUE_NEW_RSP   struct abi_msm_ccmd_ioctl_simple_submitqueue_new_rsp
#define ABI_SUBMITQUEUE_CLOSE_REQ struct abi_msm_ccmd_ioctl_simple_submitqueue_close_req
#define ABI_SUBMITQUEUE_CLOSE_RSP struct abi_msm_ccmd_ioctl_simple_submitqueue_close_rsp
#endif

/*
 * CREATE_GUEST_HANDLE is a DroidVM virtio-gpu extension.  Older Linux UAPI
 * snapshots used by the host and guest fixture endpoints do not declare its
 * names, while the Windows wire header does.  Keep this compatibility block
 * local to the fixture and prefer an endpoint-provided VIRTGPU alias when one
 * is available; the canonical wire values remain part of the manifest below.
 */
#ifndef VIRTIO_GPU_F_CREATE_GUEST_HANDLE
#if defined(VIRTGPU_F_CREATE_GUEST_HANDLE)
#define VIRTIO_GPU_F_CREATE_GUEST_HANDLE VIRTGPU_F_CREATE_GUEST_HANDLE
#else
#define VIRTIO_GPU_F_CREATE_GUEST_HANDLE 6
#endif
#endif

#ifndef VIRTIO_GPU_BLOB_FLAG_CREATE_GUEST_HANDLE
#if defined(VIRTGPU_BLOB_FLAG_CREATE_GUEST_HANDLE)
#define VIRTIO_GPU_BLOB_FLAG_CREATE_GUEST_HANDLE VIRTGPU_BLOB_FLAG_CREATE_GUEST_HANDLE
#else
#define VIRTIO_GPU_BLOB_FLAG_CREATE_GUEST_HANDLE 0x0008U
#endif
#endif

static void Emit(const char *name, unsigned long long value)
{
    printf("%s=%llu\n", name, value);
}

#if defined(ABI_ENDPOINT_WINDOWS)
static enum viogpu_host_context_response_validation ValidateWindowsControlResponse(uint32_t response_size,
                                                                                   uint8_t submitted,
                                                                                   uint8_t completed,
                                                                                   uint32_t type,
                                                                                   uint32_t flags = 0,
                                                                                   uint64_t fence_id = 0,
                                                                                   uint32_t context_id = 0,
                                                                                   uint8_t ring_index = 0,
                                                                                   uint8_t padding0 = 0,
                                                                                   uint8_t padding1 = 0,
                                                                                   uint8_t padding2 = 0)
{
    return VioGpuValidatePlainControlResponse(response_size,
                                              submitted,
                                              completed,
                                              type,
                                              flags,
                                              fence_id,
                                              context_id,
                                              ring_index,
                                              padding0,
                                              padding1,
                                              padding2,
                                              VIRTIO_GPU_RESP_OK_NODATA);
}

static int CheckWindowsControlResponseCase(size_t index,
                                           enum viogpu_host_context_response_validation expected,
                                           enum viogpu_host_context_response_validation actual)
{
    if (actual != expected)
    {
        fprintf(stderr,
                "control-response case %zu: expected %u, got %u\n",
                index,
                (unsigned)expected,
                (unsigned)actual);
        return 1;
    }
    return 0;
}

static int CheckWindowsControlResponseValidation()
{
    size_t index = 0;
    int failed = 0;
    failed |= CheckWindowsControlResponseCase(index++,
                                              VioGpuHostResponseNotSubmitted,
                                              ValidateWindowsControlResponse(24, 0, 0, 0));
    failed |= CheckWindowsControlResponseCase(index++,
                                              VioGpuHostResponseNotCompleted,
                                              ValidateWindowsControlResponse(24, 1, 0, 0));
    failed |= CheckWindowsControlResponseCase(index++,
                                              VioGpuHostResponseTooShort,
                                              ValidateWindowsControlResponse(23, 1, 1, 0));
    failed |= CheckWindowsControlResponseCase(index++,
                                              VioGpuHostResponseWrongSize,
                                              ValidateWindowsControlResponse(25, 1, 1, 0));
    failed |= CheckWindowsControlResponseCase(index++,
                                              VioGpuHostResponseConfirmed,
                                              ValidateWindowsControlResponse(24, 1, 1, VIRTIO_GPU_RESP_OK_NODATA));
    failed |= CheckWindowsControlResponseCase(index++,
                                              VioGpuHostResponseRejected,
                                              ValidateWindowsControlResponse(24,
                                                                             1,
                                                                             1,
                                                                             VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID));
    failed |= CheckWindowsControlResponseCase(index++,
                                              VioGpuHostResponseMalformed,
                                              ValidateWindowsControlResponse(24, 1, 1, VIRTIO_GPU_RESP_OK_NODATA, 1));
    failed |= CheckWindowsControlResponseCase(index++,
                                              VioGpuHostResponseMalformed,
                                              ValidateWindowsControlResponse(24, 1, 1, VIRTIO_GPU_CMD_CTX_CREATE));
    return failed != 0;
}

static enum viogpu_host_context_response_validation
ValidateWindowsMapResponse(uint32_t response_size,
                           uint8_t submitted,
                           uint8_t completed,
                           uint32_t type,
                           uint32_t flags = 0,
                           uint64_t fence_id = 0,
                           uint32_t context_id = 0,
                           uint8_t ring_index = 0,
                           uint8_t padding0 = 0,
                           uint8_t padding1 = 0,
                           uint8_t padding2 = 0,
                           uint32_t map_info = VIRTIO_GPU_MAP_CACHE_CACHED,
                           uint32_t map_padding = 0)
{
    return VioGpuValidateMapInfoResponse(response_size,
                                         submitted,
                                         completed,
                                         type,
                                         flags,
                                         fence_id,
                                         context_id,
                                         ring_index,
                                         padding0,
                                         padding1,
                                         padding2,
                                         map_info,
                                         map_padding);
}

static int CheckWindowsMapResponseCase(size_t index,
                                       enum viogpu_host_context_response_validation expected,
                                       enum viogpu_host_context_response_validation actual)
{
    if (actual != expected)
    {
        fprintf(stderr, "map-response case %zu: expected %u, got %u\n", index, (unsigned)expected, (unsigned)actual);
        return 1;
    }
    return 0;
}

static int CheckWindowsMapResponseValidation()
{
    size_t index = 0;
    int failed = 0;
    failed |= CheckWindowsMapResponseCase(index++,
                                          VioGpuHostResponseNotSubmitted,
                                          ValidateWindowsMapResponse(32, 0, 0, 0));
    failed |= CheckWindowsMapResponseCase(index++,
                                          VioGpuHostResponseNotCompleted,
                                          ValidateWindowsMapResponse(32, 1, 0, 0));
    failed |= CheckWindowsMapResponseCase(index++, VioGpuHostResponseTooShort, ValidateWindowsMapResponse(23, 1, 1, 0));
    failed |= CheckWindowsMapResponseCase(index++,
                                          VioGpuHostResponseWrongSize,
                                          ValidateWindowsMapResponse(31, 1, 1, VIRTIO_GPU_RESP_OK_MAP_INFO));
    failed |= CheckWindowsMapResponseCase(index++,
                                          VioGpuHostResponseWrongSize,
                                          ValidateWindowsMapResponse(33, 1, 1, VIRTIO_GPU_RESP_OK_MAP_INFO));
    failed |= CheckWindowsMapResponseCase(index++,
                                          VioGpuHostResponseConfirmed,
                                          ValidateWindowsMapResponse(32, 1, 1, VIRTIO_GPU_RESP_OK_MAP_INFO));
    failed |= CheckWindowsMapResponseCase(index++,
                                          VioGpuHostResponseRejected,
                                          ValidateWindowsMapResponse(24, 1, 1, VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID));
    failed |= CheckWindowsMapResponseCase(index++,
                                          VioGpuHostResponseMalformed,
                                          ValidateWindowsMapResponse(32, 1, 1, VIRTIO_GPU_RESP_OK_MAP_INFO, 1));
    failed |= CheckWindowsMapResponseCase(index++,
                                          VioGpuHostResponseMalformed,
                                          ValidateWindowsMapResponse(32, 1, 1, VIRTIO_GPU_RESP_OK_NODATA));
    failed |= CheckWindowsMapResponseCase(index++,
                                          VioGpuHostResponseMalformed,
                                          ValidateWindowsMapResponse(32,
                                                                     1,
                                                                     1,
                                                                     VIRTIO_GPU_RESP_OK_MAP_INFO,
                                                                     0,
                                                                     0,
                                                                     0,
                                                                     0,
                                                                     0,
                                                                     0,
                                                                     0,
                                                                     0,
                                                                     1));
    failed |= CheckWindowsMapResponseCase(index++,
                                          VioGpuHostResponseMalformed,
                                          ValidateWindowsMapResponse(32,
                                                                     1,
                                                                     1,
                                                                     VIRTIO_GPU_RESP_OK_MAP_INFO,
                                                                     0,
                                                                     0,
                                                                     0,
                                                                     0,
                                                                     0,
                                                                     0,
                                                                     0,
                                                                     0));
    return failed != 0;
}
#endif

int main()
{
#if defined(_MSC_VER)
    _setmode(_fileno(stdout), _O_BINARY);
#endif
#if defined(__cplusplus)
#define ABI_STATIC_ASSERT static_assert
#else
#define ABI_STATIC_ASSERT _Static_assert
#endif
#define ABI_VALUE(label, expression, expected)                                                                         \
    ABI_STATIC_ASSERT((unsigned long long)(expression) == (unsigned long long)(expected), #label " value");            \
    Emit("value." #label, (unsigned long long)(expression))
#define ABI_EXPR(label, expression, expected)    ABI_VALUE(label, expression, expected)
#define ABI_SIZE(label, type, expected)          ABI_VALUE(label, sizeof(type), expected)
#define ABI_OFFSET(label, type, field, expected) ABI_VALUE(label, offsetof(type, field), expected)

#include "abi_manifest_entries.h"

#undef ABI_OFFSET
#undef ABI_SIZE
#undef ABI_EXPR
#undef ABI_VALUE
#undef ABI_STATIC_ASSERT
#if defined(ABI_ENDPOINT_WINDOWS)
    if (CheckWindowsControlResponseValidation() != 0)
    {
        return 1;
    }
    if (CheckWindowsMapResponseValidation() != 0)
    {
        return 1;
    }
#endif
    return 0;
}
