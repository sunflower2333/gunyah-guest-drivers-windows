/*
 * VirtIO-GPU DRM native-context wire ABI shared by the Windows driver and
 * standalone ABI fixtures. Keep this header independent of WDK declarations.
 */
#pragma once

#include <stddef.h>

#ifndef VIOGPU_WIRE_U8
#define VIOGPU_WIRE_U8  UCHAR
#define VIOGPU_WIRE_I8  CHAR
#define VIOGPU_WIRE_U32 ULONG
#define VIOGPU_WIRE_I32 LONG
#define VIOGPU_WIRE_U64 ULONGLONG
#define VIOGPU_WIRE_UNDEFINE_TYPES
#endif

#if defined(__cplusplus)
#define VIOGPU_WIRE_STATIC_ASSERT static_assert
#else
#define VIOGPU_WIRE_STATIC_ASSERT _Static_assert
#endif

enum virtio_gpu_ctrl_type
{
    VIRTIO_GPU_UNDEFINED = 0,

    VIRTIO_GPU_CMD_GET_DISPLAY_INFO = 0x0100,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_2D,
    VIRTIO_GPU_CMD_RESOURCE_UNREF,
    VIRTIO_GPU_CMD_SET_SCANOUT,
    VIRTIO_GPU_CMD_RESOURCE_FLUSH,
    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D,
    VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING,
    VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING,
    VIRTIO_GPU_CMD_GET_CAPSET_INFO,
    VIRTIO_GPU_CMD_GET_CAPSET,
    VIRTIO_GPU_CMD_GET_EDID,
    VIRTIO_GPU_CMD_RESOURCE_ASSIGN_UUID,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB,
    VIRTIO_GPU_CMD_SET_SCANOUT_BLOB,

    VIRTIO_GPU_CMD_CTX_CREATE = 0x0200,
    VIRTIO_GPU_CMD_CTX_DESTROY,
    VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE,
    VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_3D,
    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D,
    VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D,
    VIRTIO_GPU_CMD_SUBMIT_3D,
    VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB,
    VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB,

    VIRTIO_GPU_CMD_UPDATE_CURSOR = 0x0300,
    VIRTIO_GPU_CMD_MOVE_CURSOR,

    VIRTIO_GPU_RESP_OK_NODATA = 0x1100,
    VIRTIO_GPU_RESP_OK_DISPLAY_INFO,
    VIRTIO_GPU_RESP_OK_CAPSET_INFO,
    VIRTIO_GPU_RESP_OK_CAPSET,
    VIRTIO_GPU_RESP_OK_EDID,
    VIRTIO_GPU_RESP_OK_RESOURCE_UUID,
    VIRTIO_GPU_RESP_OK_MAP_INFO,

    VIRTIO_GPU_RESP_ERR_UNSPEC = 0x1200,
    VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY,
    VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID,
    VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID,
    VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID,
    VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER,
};

#define VIRTIO_GPU_F_VIRGL                       0
#define VIRTIO_GPU_F_EDID                        1
#define VIRTIO_GPU_F_RESOURCE_UUID               2
#define VIRTIO_GPU_F_RESOURCE_BLOB               3
#define VIRTIO_GPU_F_CONTEXT_INIT                4
#define VIRTIO_GPU_F_CREATE_GUEST_HANDLE         6

#define VIRTIO_GPU_FLAG_FENCE                    (1U << 0)
#define VIRTIO_GPU_FLAG_INFO_RING_IDX            (1U << 1)

#define VIRTIO_GPU_BLOB_MEM_GUEST                0x0001U
#define VIRTIO_GPU_BLOB_MEM_HOST3D               0x0002U
#define VIRTIO_GPU_BLOB_MEM_HOST3D_GUEST         0x0003U

#define VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE        0x0001U
#define VIRTIO_GPU_BLOB_FLAG_USE_SHAREABLE       0x0002U
#define VIRTIO_GPU_BLOB_FLAG_USE_CROSS_DEVICE    0x0004U
#define VIRTIO_GPU_BLOB_FLAG_CREATE_GUEST_HANDLE 0x0008U

#define VIRTIO_GPU_MAP_CACHE_CACHED              0x0001U
#define VIRTIO_GPU_MAP_INFO_POOL                 0x80000000U

#define VIRTGPU_DRM_CAPSET_DRM                   6
#define VIRTGPU_DRM_CONTEXT_MSM                  1
#define VIRTGPU_DRM_WIRE_FORMAT_VERSION          2
#define VIRTIO_GPU_CAPSET_DRM                    VIRTGPU_DRM_CAPSET_DRM
#define VIRTIO_GPU_DRM_CONTEXT_MSM               VIRTGPU_DRM_CONTEXT_MSM
#define VIRTIO_GPU_DRM_WIRE_FORMAT_VERSION       VIRTGPU_DRM_WIRE_FORMAT_VERSION
#define VIRTIO_GPU_CONTEXT_INIT_CAPSET_ID_MASK   0x00ffU

#define MSM_BO_SCANOUT                           0x00000001U
#define MSM_BO_GPU_READONLY                      0x00000002U
#define MSM_BO_CACHED_COHERENT                   0x00080000U
#define MSM_BO_GUEST_ALLOC                       0x80000000U
#define MSM_PIPE_3D0                             0x10U
#define MSM_PARAM_VA_START                       0x0eU
#define MSM_PARAM_VA_SIZE                        0x0fU
#define DRM_IOCTL_MSM_GET_PARAM                  0xc0186440U

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4200)
#endif

#pragma pack(push, 4)

typedef struct virgl_renderer_capset_drm_msm
{
    VIOGPU_WIRE_U32 has_cached_coherent;
    VIOGPU_WIRE_U32 priorities;
    VIOGPU_WIRE_U64 va_start;
    VIOGPU_WIRE_U64 va_size;
    VIOGPU_WIRE_U32 gpu_id;
    VIOGPU_WIRE_U32 gmem_size;
    VIOGPU_WIRE_U64 gmem_base;
    VIOGPU_WIRE_U64 chip_id;
    VIOGPU_WIRE_U32 max_freq;
    VIOGPU_WIRE_U32 highest_bank_bit;
    VIOGPU_WIRE_U64 ubwc_swizzle;
    VIOGPU_WIRE_U64 macrotile_mode;
    VIOGPU_WIRE_U32 has_raytracing;
    VIOGPU_WIRE_U32 has_preemption;
    VIOGPU_WIRE_U64 uche_trap_base;
} GPU_CAPSET_DRM_MSM, *PGPU_CAPSET_DRM_MSM;

typedef struct virgl_renderer_capset_drm
{
    VIOGPU_WIRE_U32 wire_format_version;
    VIOGPU_WIRE_U32 version_major;
    VIOGPU_WIRE_U32 version_minor;
    VIOGPU_WIRE_U32 version_patchlevel;
    VIOGPU_WIRE_U32 context_type;
    VIOGPU_WIRE_U32 padding;
    GPU_CAPSET_DRM_MSM msm;
} GPU_CAPSET_DRM, *PGPU_CAPSET_DRM;

typedef struct vdrm_shmem
{
    VIOGPU_WIRE_U32 seqno;
    VIOGPU_WIRE_U32 rsp_mem_offset;
} VDRM_SHMEM, *PVDRM_SHMEM;

typedef struct msm_shmem
{
    VDRM_SHMEM base;
    VIOGPU_WIRE_U32 async_error;
    VIOGPU_WIRE_U32 global_faults;
} MSM_SHMEM, *PMSM_SHMEM;

typedef struct vdrm_ccmd_req
{
    VIOGPU_WIRE_U32 cmd;
    VIOGPU_WIRE_U32 len;
    VIOGPU_WIRE_U32 seqno;
    VIOGPU_WIRE_U32 rsp_off;
} VDRM_CCMD_REQ, *PVDRM_CCMD_REQ;

typedef struct vdrm_ccmd_rsp
{
    VIOGPU_WIRE_U32 len;
} VDRM_CCMD_RSP, *PVDRM_CCMD_RSP;

enum msm_ccmd
{
    MSM_CCMD_NOP = 1,
    MSM_CCMD_IOCTL_SIMPLE,
    MSM_CCMD_GEM_NEW,
    MSM_CCMD_GEM_SET_IOVA,
    MSM_CCMD_GEM_CPU_PREP,
    MSM_CCMD_GEM_SET_NAME,
    MSM_CCMD_GEM_SUBMIT,
    MSM_CCMD_GEM_UPLOAD,
    MSM_CCMD_SUBMITQUEUE_QUERY,
    MSM_CCMD_WAIT_FENCE,
    MSM_CCMD_SET_DEBUGINFO,
    MSM_CCMD_LAST,
};

typedef struct msm_ccmd_nop_req
{
    VDRM_CCMD_REQ hdr;
} MSM_CCMD_NOP_REQ, *PMSM_CCMD_NOP_REQ;

typedef struct msm_ccmd_ioctl_simple_req
{
    VDRM_CCMD_REQ hdr;
    VIOGPU_WIRE_U32 cmd;
    VIOGPU_WIRE_U8 payload[0];
} MSM_CCMD_IOCTL_SIMPLE_REQ, *PMSM_CCMD_IOCTL_SIMPLE_REQ;

typedef struct msm_ccmd_ioctl_simple_rsp
{
    VDRM_CCMD_RSP hdr;
    VIOGPU_WIRE_I32 ret;
    VIOGPU_WIRE_U8 payload[0];
} MSM_CCMD_IOCTL_SIMPLE_RSP, *PMSM_CCMD_IOCTL_SIMPLE_RSP;

typedef struct drm_msm_param
{
    VIOGPU_WIRE_U32 pipe;
    VIOGPU_WIRE_U32 param;
    VIOGPU_WIRE_U64 value;
    VIOGPU_WIRE_U32 len;
    VIOGPU_WIRE_U32 pad;
} DRM_MSM_PARAM, *PDRM_MSM_PARAM;

typedef struct msm_ccmd_ioctl_simple_get_param_req
{
    VDRM_CCMD_REQ hdr;
    VIOGPU_WIRE_U32 ioctl_cmd;
    DRM_MSM_PARAM param;
} MSM_CCMD_IOCTL_SIMPLE_GET_PARAM_REQ, *PMSM_CCMD_IOCTL_SIMPLE_GET_PARAM_REQ;

typedef struct msm_ccmd_ioctl_simple_get_param_rsp
{
    VDRM_CCMD_RSP hdr;
    VIOGPU_WIRE_I32 ret;
    DRM_MSM_PARAM param;
} MSM_CCMD_IOCTL_SIMPLE_GET_PARAM_RSP, *PMSM_CCMD_IOCTL_SIMPLE_GET_PARAM_RSP;

typedef struct msm_ccmd_gem_new_req
{
    VDRM_CCMD_REQ hdr;
    VIOGPU_WIRE_U64 iova;
    VIOGPU_WIRE_U64 size;
    VIOGPU_WIRE_U32 flags;
    VIOGPU_WIRE_U32 blob_id;
} MSM_CCMD_GEM_NEW_REQ, *PMSM_CCMD_GEM_NEW_REQ;

typedef struct msm_gem_new_run
{
    VIOGPU_WIRE_U64 arena_off;
    VIOGPU_WIRE_U64 len;
} MSM_GEM_NEW_RUN, *PMSM_GEM_NEW_RUN;

typedef struct msm_ccmd_gem_new_run_list
{
    VIOGPU_WIRE_U32 nr_runs;
    VIOGPU_WIRE_U32 pad;
    MSM_GEM_NEW_RUN runs[0];
} MSM_CCMD_GEM_NEW_RUN_LIST, *PMSM_CCMD_GEM_NEW_RUN_LIST;

typedef struct msm_ccmd_gem_set_iova_req
{
    VDRM_CCMD_REQ hdr;
    VIOGPU_WIRE_U64 iova;
    VIOGPU_WIRE_U32 res_id;
    VIOGPU_WIRE_U32 padding;
} MSM_CCMD_GEM_SET_IOVA_REQ, *PMSM_CCMD_GEM_SET_IOVA_REQ;

typedef struct msm_ccmd_gem_cpu_prep_req
{
    VDRM_CCMD_REQ hdr;
    VIOGPU_WIRE_U32 res_id;
    VIOGPU_WIRE_U32 op;
} MSM_CCMD_GEM_CPU_PREP_REQ, *PMSM_CCMD_GEM_CPU_PREP_REQ;

typedef struct msm_ccmd_gem_cpu_prep_rsp
{
    VDRM_CCMD_RSP hdr;
    VIOGPU_WIRE_I32 ret;
} MSM_CCMD_GEM_CPU_PREP_RSP, *PMSM_CCMD_GEM_CPU_PREP_RSP;

typedef struct msm_ccmd_gem_set_name_req
{
    VDRM_CCMD_REQ hdr;
    VIOGPU_WIRE_U32 res_id;
    VIOGPU_WIRE_U32 len;
    VIOGPU_WIRE_U8 payload[0];
} MSM_CCMD_GEM_SET_NAME_REQ, *PMSM_CCMD_GEM_SET_NAME_REQ;

typedef struct msm_ccmd_gem_submit_req
{
    VDRM_CCMD_REQ hdr;
    VIOGPU_WIRE_U32 flags;
    VIOGPU_WIRE_U32 queue_id;
    VIOGPU_WIRE_U32 nr_bos;
    VIOGPU_WIRE_U32 nr_cmds;
    VIOGPU_WIRE_U32 fence;
    VIOGPU_WIRE_I8 payload[0];
} MSM_CCMD_GEM_SUBMIT_REQ, *PMSM_CCMD_GEM_SUBMIT_REQ;

typedef struct msm_ccmd_gem_upload_req
{
    VDRM_CCMD_REQ hdr;
    VIOGPU_WIRE_U32 res_id;
    VIOGPU_WIRE_U32 pad;
    VIOGPU_WIRE_U32 off;
    VIOGPU_WIRE_U32 len;
    VIOGPU_WIRE_U8 payload[0];
} MSM_CCMD_GEM_UPLOAD_REQ, *PMSM_CCMD_GEM_UPLOAD_REQ;

typedef struct msm_ccmd_submitqueue_query_req
{
    VDRM_CCMD_REQ hdr;
    VIOGPU_WIRE_U32 queue_id;
    VIOGPU_WIRE_U32 param;
    VIOGPU_WIRE_U32 len;
} MSM_CCMD_SUBMITQUEUE_QUERY_REQ, *PMSM_CCMD_SUBMITQUEUE_QUERY_REQ;

typedef struct msm_ccmd_submitqueue_query_rsp
{
    VDRM_CCMD_RSP hdr;
    VIOGPU_WIRE_I32 ret;
    VIOGPU_WIRE_U32 out_len;
    VIOGPU_WIRE_U8 payload[0];
} MSM_CCMD_SUBMITQUEUE_QUERY_RSP, *PMSM_CCMD_SUBMITQUEUE_QUERY_RSP;

typedef struct msm_ccmd_wait_fence_req
{
    VDRM_CCMD_REQ hdr;
    VIOGPU_WIRE_U32 queue_id;
    VIOGPU_WIRE_U32 fence;
} MSM_CCMD_WAIT_FENCE_REQ, *PMSM_CCMD_WAIT_FENCE_REQ;

typedef struct msm_ccmd_wait_fence_rsp
{
    VDRM_CCMD_RSP hdr;
    VIOGPU_WIRE_I32 ret;
} MSM_CCMD_WAIT_FENCE_RSP, *PMSM_CCMD_WAIT_FENCE_RSP;

typedef struct msm_ccmd_set_debuginfo_req
{
    VDRM_CCMD_REQ hdr;
    VIOGPU_WIRE_U32 comm_len;
    VIOGPU_WIRE_U32 cmdline_len;
    VIOGPU_WIRE_I8 payload[0];
} MSM_CCMD_SET_DEBUGINFO_REQ, *PMSM_CCMD_SET_DEBUGINFO_REQ;

#pragma pack(pop)

#define VIOGPU_WIRE_ASSERT_SIZE(tag, expected)                                                                         \
    VIOGPU_WIRE_STATIC_ASSERT(sizeof(struct tag) == (expected), #tag " wire size")
#define VIOGPU_WIRE_ASSERT_OFFSET(tag, field, expected)                                                                \
    VIOGPU_WIRE_STATIC_ASSERT(offsetof(struct tag, field) == (expected), #tag "." #field " wire offset")
#define VIOGPU_WIRE_ASSERT_VALUE(name, expected)                                                                       \
    VIOGPU_WIRE_STATIC_ASSERT((unsigned long long)(name) == (unsigned long long)(expected), #name " wire value")

VIOGPU_WIRE_ASSERT_VALUE(VIRTIO_GPU_CMD_GET_CAPSET_INFO, 0x108);
VIOGPU_WIRE_ASSERT_VALUE(VIRTIO_GPU_CMD_GET_CAPSET, 0x109);
VIOGPU_WIRE_ASSERT_VALUE(VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB, 0x10c);
VIOGPU_WIRE_ASSERT_VALUE(VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB, 0x208);
VIOGPU_WIRE_ASSERT_VALUE(VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB, 0x209);
VIOGPU_WIRE_ASSERT_VALUE(VIRTIO_GPU_CMD_CTX_CREATE, 0x200);
VIOGPU_WIRE_ASSERT_VALUE(VIRTIO_GPU_CMD_CTX_DESTROY, 0x201);
VIOGPU_WIRE_ASSERT_VALUE(VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, 0x202);
VIOGPU_WIRE_ASSERT_VALUE(VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE, 0x203);
VIOGPU_WIRE_ASSERT_VALUE(VIRTIO_GPU_CMD_SUBMIT_3D, 0x207);
VIOGPU_WIRE_ASSERT_VALUE(VIRTIO_GPU_RESP_OK_NODATA, 0x1100);
VIOGPU_WIRE_ASSERT_VALUE(VIRTIO_GPU_RESP_OK_CAPSET_INFO, 0x1102);
VIOGPU_WIRE_ASSERT_VALUE(VIRTIO_GPU_RESP_OK_CAPSET, 0x1103);
VIOGPU_WIRE_ASSERT_VALUE(VIRTIO_GPU_RESP_OK_MAP_INFO, 0x1106);
VIOGPU_WIRE_ASSERT_VALUE(VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID, 0x1204);
VIOGPU_WIRE_ASSERT_VALUE(VIRTIO_GPU_CONTEXT_INIT_CAPSET_ID_MASK, 0xff);
VIOGPU_WIRE_ASSERT_VALUE(VIRTGPU_DRM_CAPSET_DRM, 6);
VIOGPU_WIRE_ASSERT_VALUE(VIRTGPU_DRM_CONTEXT_MSM, 1);
VIOGPU_WIRE_ASSERT_VALUE(VIRTGPU_DRM_WIRE_FORMAT_VERSION, 2);
VIOGPU_WIRE_ASSERT_VALUE(MSM_BO_GUEST_ALLOC, 0x80000000ULL);
VIOGPU_WIRE_ASSERT_VALUE(MSM_BO_SCANOUT, 0x00000001ULL);
VIOGPU_WIRE_ASSERT_VALUE(MSM_BO_GPU_READONLY, 0x00000002ULL);
VIOGPU_WIRE_ASSERT_VALUE(MSM_BO_CACHED_COHERENT, 0x00080000ULL);
VIOGPU_WIRE_ASSERT_VALUE(VIRTIO_GPU_MAP_CACHE_CACHED, 1);
VIOGPU_WIRE_ASSERT_VALUE(VIRTIO_GPU_MAP_INFO_POOL, 0x80000000ULL);
VIOGPU_WIRE_ASSERT_VALUE(MSM_PIPE_3D0, 0x10);
VIOGPU_WIRE_ASSERT_VALUE(MSM_PARAM_VA_START, 0x0e);
VIOGPU_WIRE_ASSERT_VALUE(MSM_PARAM_VA_SIZE, 0x0f);
VIOGPU_WIRE_ASSERT_VALUE(DRM_IOCTL_MSM_GET_PARAM, 0xc0186440ULL);
VIOGPU_WIRE_ASSERT_VALUE(MSM_CCMD_NOP, 1);
VIOGPU_WIRE_ASSERT_VALUE(MSM_CCMD_IOCTL_SIMPLE, 2);
VIOGPU_WIRE_ASSERT_VALUE(MSM_CCMD_GEM_NEW, 3);
VIOGPU_WIRE_ASSERT_VALUE(MSM_CCMD_GEM_SET_IOVA, 4);
VIOGPU_WIRE_ASSERT_VALUE(MSM_CCMD_GEM_CPU_PREP, 5);
VIOGPU_WIRE_ASSERT_VALUE(MSM_CCMD_GEM_SET_NAME, 6);
VIOGPU_WIRE_ASSERT_VALUE(MSM_CCMD_GEM_SUBMIT, 7);
VIOGPU_WIRE_ASSERT_VALUE(MSM_CCMD_GEM_UPLOAD, 8);
VIOGPU_WIRE_ASSERT_VALUE(MSM_CCMD_SUBMITQUEUE_QUERY, 9);
VIOGPU_WIRE_ASSERT_VALUE(MSM_CCMD_WAIT_FENCE, 10);
VIOGPU_WIRE_ASSERT_VALUE(MSM_CCMD_SET_DEBUGINFO, 11);
VIOGPU_WIRE_ASSERT_VALUE(MSM_CCMD_LAST, 12);

VIOGPU_WIRE_ASSERT_SIZE(virgl_renderer_capset_drm_msm, 88);
VIOGPU_WIRE_ASSERT_OFFSET(virgl_renderer_capset_drm_msm, va_start, 8);
VIOGPU_WIRE_ASSERT_OFFSET(virgl_renderer_capset_drm_msm, va_size, 16);
VIOGPU_WIRE_ASSERT_OFFSET(virgl_renderer_capset_drm_msm, gpu_id, 24);
VIOGPU_WIRE_ASSERT_OFFSET(virgl_renderer_capset_drm_msm, gmem_size, 28);
VIOGPU_WIRE_ASSERT_OFFSET(virgl_renderer_capset_drm_msm, gmem_base, 32);
VIOGPU_WIRE_ASSERT_OFFSET(virgl_renderer_capset_drm_msm, chip_id, 40);
VIOGPU_WIRE_ASSERT_OFFSET(virgl_renderer_capset_drm_msm, max_freq, 48);
VIOGPU_WIRE_ASSERT_OFFSET(virgl_renderer_capset_drm_msm, highest_bank_bit, 52);
VIOGPU_WIRE_ASSERT_OFFSET(virgl_renderer_capset_drm_msm, ubwc_swizzle, 56);
VIOGPU_WIRE_ASSERT_OFFSET(virgl_renderer_capset_drm_msm, macrotile_mode, 64);
VIOGPU_WIRE_ASSERT_OFFSET(virgl_renderer_capset_drm_msm, has_raytracing, 72);
VIOGPU_WIRE_ASSERT_OFFSET(virgl_renderer_capset_drm_msm, has_preemption, 76);
VIOGPU_WIRE_ASSERT_OFFSET(virgl_renderer_capset_drm_msm, uche_trap_base, 80);
VIOGPU_WIRE_ASSERT_SIZE(virgl_renderer_capset_drm, 112);
VIOGPU_WIRE_ASSERT_OFFSET(virgl_renderer_capset_drm, context_type, 16);
VIOGPU_WIRE_ASSERT_OFFSET(virgl_renderer_capset_drm, msm, 24);

VIOGPU_WIRE_ASSERT_SIZE(vdrm_shmem, 8);
VIOGPU_WIRE_ASSERT_OFFSET(vdrm_shmem, rsp_mem_offset, 4);
VIOGPU_WIRE_ASSERT_SIZE(msm_shmem, 16);
VIOGPU_WIRE_ASSERT_OFFSET(msm_shmem, async_error, 8);
VIOGPU_WIRE_ASSERT_OFFSET(msm_shmem, global_faults, 12);
VIOGPU_WIRE_ASSERT_SIZE(vdrm_ccmd_req, 16);
VIOGPU_WIRE_ASSERT_OFFSET(vdrm_ccmd_req, len, 4);
VIOGPU_WIRE_ASSERT_OFFSET(vdrm_ccmd_req, seqno, 8);
VIOGPU_WIRE_ASSERT_OFFSET(vdrm_ccmd_req, rsp_off, 12);
VIOGPU_WIRE_ASSERT_SIZE(vdrm_ccmd_rsp, 4);

VIOGPU_WIRE_ASSERT_SIZE(msm_ccmd_nop_req, 16);
VIOGPU_WIRE_ASSERT_SIZE(msm_ccmd_ioctl_simple_req, 20);
VIOGPU_WIRE_ASSERT_OFFSET(msm_ccmd_ioctl_simple_req, payload, 20);
VIOGPU_WIRE_ASSERT_SIZE(msm_ccmd_ioctl_simple_rsp, 8);
VIOGPU_WIRE_ASSERT_OFFSET(msm_ccmd_ioctl_simple_rsp, payload, 8);
VIOGPU_WIRE_ASSERT_SIZE(drm_msm_param, 24);
VIOGPU_WIRE_ASSERT_OFFSET(drm_msm_param, value, 8);
VIOGPU_WIRE_ASSERT_OFFSET(drm_msm_param, len, 16);
VIOGPU_WIRE_ASSERT_OFFSET(drm_msm_param, pad, 20);
VIOGPU_WIRE_ASSERT_SIZE(msm_ccmd_ioctl_simple_get_param_req, 44);
VIOGPU_WIRE_ASSERT_OFFSET(msm_ccmd_ioctl_simple_get_param_req, ioctl_cmd, 16);
VIOGPU_WIRE_ASSERT_OFFSET(msm_ccmd_ioctl_simple_get_param_req, param, 20);
VIOGPU_WIRE_ASSERT_SIZE(msm_ccmd_ioctl_simple_get_param_rsp, 32);
VIOGPU_WIRE_ASSERT_OFFSET(msm_ccmd_ioctl_simple_get_param_rsp, ret, 4);
VIOGPU_WIRE_ASSERT_OFFSET(msm_ccmd_ioctl_simple_get_param_rsp, param, 8);
VIOGPU_WIRE_ASSERT_SIZE(msm_ccmd_gem_new_req, 40);
VIOGPU_WIRE_ASSERT_OFFSET(msm_ccmd_gem_new_req, iova, 16);
VIOGPU_WIRE_ASSERT_OFFSET(msm_ccmd_gem_new_req, size, 24);
VIOGPU_WIRE_ASSERT_OFFSET(msm_ccmd_gem_new_req, flags, 32);
VIOGPU_WIRE_ASSERT_OFFSET(msm_ccmd_gem_new_req, blob_id, 36);
VIOGPU_WIRE_ASSERT_SIZE(msm_gem_new_run, 16);
VIOGPU_WIRE_ASSERT_OFFSET(msm_gem_new_run, len, 8);
VIOGPU_WIRE_ASSERT_SIZE(msm_ccmd_gem_new_run_list, 8);
VIOGPU_WIRE_ASSERT_OFFSET(msm_ccmd_gem_new_run_list, runs, 8);
VIOGPU_WIRE_ASSERT_SIZE(msm_ccmd_gem_set_iova_req, 32);
VIOGPU_WIRE_ASSERT_OFFSET(msm_ccmd_gem_set_iova_req, iova, 16);
VIOGPU_WIRE_ASSERT_OFFSET(msm_ccmd_gem_set_iova_req, res_id, 24);
VIOGPU_WIRE_ASSERT_SIZE(msm_ccmd_gem_cpu_prep_req, 24);
VIOGPU_WIRE_ASSERT_SIZE(msm_ccmd_gem_cpu_prep_rsp, 8);
VIOGPU_WIRE_ASSERT_SIZE(msm_ccmd_gem_set_name_req, 24);
VIOGPU_WIRE_ASSERT_OFFSET(msm_ccmd_gem_set_name_req, payload, 24);
VIOGPU_WIRE_ASSERT_SIZE(msm_ccmd_gem_submit_req, 36);
VIOGPU_WIRE_ASSERT_OFFSET(msm_ccmd_gem_submit_req, fence, 32);
VIOGPU_WIRE_ASSERT_OFFSET(msm_ccmd_gem_submit_req, payload, 36);
VIOGPU_WIRE_ASSERT_SIZE(msm_ccmd_gem_upload_req, 32);
VIOGPU_WIRE_ASSERT_OFFSET(msm_ccmd_gem_upload_req, payload, 32);
VIOGPU_WIRE_ASSERT_SIZE(msm_ccmd_submitqueue_query_req, 28);
VIOGPU_WIRE_ASSERT_SIZE(msm_ccmd_submitqueue_query_rsp, 12);
VIOGPU_WIRE_ASSERT_OFFSET(msm_ccmd_submitqueue_query_rsp, payload, 12);
VIOGPU_WIRE_ASSERT_SIZE(msm_ccmd_wait_fence_req, 24);
VIOGPU_WIRE_ASSERT_SIZE(msm_ccmd_wait_fence_rsp, 8);
VIOGPU_WIRE_ASSERT_SIZE(msm_ccmd_set_debuginfo_req, 24);
VIOGPU_WIRE_ASSERT_OFFSET(msm_ccmd_set_debuginfo_req, payload, 24);

#undef VIOGPU_WIRE_ASSERT_OFFSET
#undef VIOGPU_WIRE_ASSERT_SIZE
#undef VIOGPU_WIRE_ASSERT_VALUE
#undef VIOGPU_WIRE_STATIC_ASSERT

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#ifdef VIOGPU_WIRE_UNDEFINE_TYPES
#undef VIOGPU_WIRE_UNDEFINE_TYPES
#undef VIOGPU_WIRE_U64
#undef VIOGPU_WIRE_I32
#undef VIOGPU_WIRE_U32
#undef VIOGPU_WIRE_I8
#undef VIOGPU_WIRE_U8
#endif
