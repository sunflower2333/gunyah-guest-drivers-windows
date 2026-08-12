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
#define ABI_CAPSET         struct virgl_renderer_capset_drm
#define ABI_CAPSET_MSM     msm
#define ABI_CAPSET_PADDING padding
#define ABI_CAPSET_ID      VIRTIO_GPU_CAPSET_DRM
#else
#include <linux/virtio_gpu.h>

#if defined(__cplusplus)
extern "C"
{
#endif
#include "drm_hw.h"
#include "msm_proto.h"
#include "virtgpu_drm.h"
#if defined(__cplusplus)
}
#endif

#define ABI_CAPSET         struct virgl_renderer_capset_drm
#define ABI_CAPSET_MSM     u.msm
#define ABI_CAPSET_PADDING pad
#define ABI_CAPSET_ID      VIRTGPU_DRM_CAPSET_DRM
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
