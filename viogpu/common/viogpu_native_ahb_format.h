#pragma once

/*
 * Native AHB replies carry a DRM fourcc for the authenticated Android
 * allocation.  Keep the conversion table explicit: a fourcc describes byte
 * order, while a generic 32-bit format value does not.
 */
enum
{
    VIOGPU_NATIVE_AHB_FOURCC_AR24 = ((unsigned int)'A' | ((unsigned int)'R' << 8) | ((unsigned int)'2' << 16) |
                                    ((unsigned int)'4' << 24)),
    VIOGPU_NATIVE_AHB_FOURCC_AB24 = ((unsigned int)'A' | ((unsigned int)'B' << 8) | ((unsigned int)'2' << 16) |
                                    ((unsigned int)'4' << 24)),
    VIOGPU_NATIVE_AHB_FOURCC_XR24 = ((unsigned int)'X' | ((unsigned int)'R' << 8) | ((unsigned int)'2' << 16) |
                                    ((unsigned int)'4' << 24)),
    VIOGPU_NATIVE_AHB_FOURCC_XB24 = ((unsigned int)'X' | ((unsigned int)'B' << 8) | ((unsigned int)'2' << 16) |
                                    ((unsigned int)'4' << 24)),
    VIOGPU_NATIVE_AHB_VIRTIO_FORMAT_B8G8R8A8_UNORM = 1,
    VIOGPU_NATIVE_AHB_VIRTIO_FORMAT_B8G8R8X8_UNORM = 2,
    VIOGPU_NATIVE_AHB_VIRTIO_FORMAT_R8G8B8A8_UNORM = 67,
    VIOGPU_NATIVE_AHB_VIRTIO_FORMAT_R8G8B8X8_UNORM = 134,
};

static inline bool VioGpuNativeAhbFourccToVirtioFormat(unsigned int fourcc, unsigned int *format)
{
    if (format == nullptr)
    {
        return false;
    }

    switch (fourcc)
    {
    case VIOGPU_NATIVE_AHB_FOURCC_AR24:
        *format = VIOGPU_NATIVE_AHB_VIRTIO_FORMAT_B8G8R8A8_UNORM;
        return true;
    case VIOGPU_NATIVE_AHB_FOURCC_AB24:
        *format = VIOGPU_NATIVE_AHB_VIRTIO_FORMAT_R8G8B8A8_UNORM;
        return true;
    case VIOGPU_NATIVE_AHB_FOURCC_XR24:
        *format = VIOGPU_NATIVE_AHB_VIRTIO_FORMAT_B8G8R8X8_UNORM;
        return true;
    case VIOGPU_NATIVE_AHB_FOURCC_XB24:
        *format = VIOGPU_NATIVE_AHB_VIRTIO_FORMAT_R8G8B8X8_UNORM;
        return true;
    default:
        return false;
    }
}

static inline bool VioGpuNativeAhbVirtioFormatToFourcc(unsigned int format, unsigned int *fourcc)
{
    if (fourcc == nullptr)
    {
        return false;
    }

    switch (format)
    {
    case VIOGPU_NATIVE_AHB_VIRTIO_FORMAT_B8G8R8A8_UNORM:
        *fourcc = VIOGPU_NATIVE_AHB_FOURCC_AR24;
        return true;
    case VIOGPU_NATIVE_AHB_VIRTIO_FORMAT_R8G8B8A8_UNORM:
        *fourcc = VIOGPU_NATIVE_AHB_FOURCC_AB24;
        return true;
    case VIOGPU_NATIVE_AHB_VIRTIO_FORMAT_B8G8R8X8_UNORM:
        *fourcc = VIOGPU_NATIVE_AHB_FOURCC_XR24;
        return true;
    case VIOGPU_NATIVE_AHB_VIRTIO_FORMAT_R8G8B8X8_UNORM:
        *fourcc = VIOGPU_NATIVE_AHB_FOURCC_XB24;
        return true;
    default:
        return false;
    }
}

/* Match the authenticated allocation against the format requested by WDDM. */
static inline bool VioGpuNativeAhbFormatMatches(unsigned int fourcc, unsigned int requestedFormat)
{
    unsigned int authenticatedFormat = 0;
    return VioGpuNativeAhbFourccToVirtioFormat(fourcc, &authenticatedFormat) &&
           authenticatedFormat == requestedFormat;
}
