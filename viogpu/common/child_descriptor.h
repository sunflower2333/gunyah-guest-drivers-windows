#pragma once

/* Video output child descriptor selection, free of WDK headers so the table
 * is testable. Values equal D3DKMDT_VIDEO_OUTPUT_TECHNOLOGY and
 * DXGK_CHILD_DEVICE_HPD_AWARENESS; the driver static_asserts them.
 *
 * Under the WIN8 DDI table dxgkrnl accepts an internal, always-connected
 * output. Registered as WDDM 2.0 the same descriptor fails adapter start
 * (StartAdapter_DpiFdoEnumChildDevicesFailed, STATUS_NOT_SUPPORTED) straight
 * after DxgkDdiQueryChildRelations. The registry value
 * VioGpuChildDescriptorMode selects the descriptor without a rebuild. */
enum VioGpuChildDescriptorMode : unsigned int
{
    VioGpuChildInternalAlwaysConnected = 0,
    VioGpuChildDisplayPortAlwaysConnected = 1,
    VioGpuChildDisplayPortInterruptible = 2,
    VioGpuChildHdmiInterruptible = 3,
    VioGpuChildInternalInterruptible = 4,
    VioGpuChildDescriptorModeCount = 5,
};

constexpr unsigned int VioGpuVotHdmi = 5;
constexpr unsigned int VioGpuVotDisplayPortExternal = 10;
constexpr unsigned int VioGpuVotInternal = 0x80000000U;
constexpr unsigned int VioGpuHpdAlwaysConnected = 1;
constexpr unsigned int VioGpuHpdInterruptible = 4;

struct VioGpuChildDescriptor
{
    unsigned int InterfaceTechnology;
    unsigned int HpdAwareness;
};

inline VioGpuChildDescriptorMode VioGpuDefaultChildDescriptorMode(bool wddm2Interface)
{
    return wddm2Interface ? VioGpuChildDisplayPortInterruptible : VioGpuChildInternalAlwaysConnected;
}

/* Missing or out-of-range registry data keeps the interface default. */
inline VioGpuChildDescriptorMode VioGpuSelectChildDescriptorMode(bool found, unsigned int value, bool wddm2Interface)
{
    return found && value < VioGpuChildDescriptorModeCount ? static_cast<VioGpuChildDescriptorMode>(value)
                                                           : VioGpuDefaultChildDescriptorMode(wddm2Interface);
}

inline VioGpuChildDescriptor VioGpuChildDescriptorFor(VioGpuChildDescriptorMode mode)
{
    switch (mode)
    {
        case VioGpuChildDisplayPortAlwaysConnected:
            return {VioGpuVotDisplayPortExternal, VioGpuHpdAlwaysConnected};
        case VioGpuChildDisplayPortInterruptible:
            return {VioGpuVotDisplayPortExternal, VioGpuHpdInterruptible};
        case VioGpuChildHdmiInterruptible:
            return {VioGpuVotHdmi, VioGpuHpdInterruptible};
        case VioGpuChildInternalInterruptible:
            return {VioGpuVotInternal, VioGpuHpdInterruptible};
        case VioGpuChildInternalAlwaysConnected:
        default:
            return {VioGpuVotInternal, VioGpuHpdAlwaysConnected};
    }
}

/* Child DDIs share the activation trace with QueryAdapterInfo. Their entry
 * type sets this bit, so a decoder cannot mistake one for a query type. */
constexpr unsigned int VioGpuActivationChildDdiFlag = 0x10000U;
constexpr unsigned int VioGpuActivationChildRelations = VioGpuActivationChildDdiFlag | 1U;
constexpr unsigned int VioGpuActivationChildStatus = VioGpuActivationChildDdiFlag | 2U;
constexpr unsigned int VioGpuActivationChildDescriptor = VioGpuActivationChildDdiFlag | 3U;
