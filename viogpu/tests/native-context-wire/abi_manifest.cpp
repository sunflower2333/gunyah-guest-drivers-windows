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

#ifndef VIRTIO_GPU_MAP_INFO_POOL
#define VIRTIO_GPU_MAP_INFO_POOL 0x80000000U
#endif

static void Emit(const char *name, unsigned long long value)
{
    printf("%s=%llu\n", name, value);
}

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
    return 0;
}
