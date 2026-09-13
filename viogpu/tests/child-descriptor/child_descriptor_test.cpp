#include "../../common/child_descriptor.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>

static unsigned checks;
static unsigned failures;
#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        ++checks;                                                                                                      \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            ++failures;                                                                                                \
            std::fprintf(stderr, "child descriptor check %u line %d: %s\n", checks, __LINE__, #c);                   \
        }                                                                                                              \
    } while (0)

int main()
{
    // Public WDK enumerator values the driver static_asserts against.
    CHECK(VioGpuVotInternal == 0x80000000U);
    CHECK(VioGpuVotHdmi == 5U && VioGpuVotDisplayPortExternal == 10U);
    CHECK(VioGpuHpdAlwaysConnected == 1U && VioGpuHpdInterruptible == 4U);

    // The WIN8 table keeps the exact descriptor it has always published.
    CHECK(VioGpuDefaultChildDescriptorMode(false) == VioGpuChildInternalAlwaysConnected);
    VioGpuChildDescriptor legacy = VioGpuChildDescriptorFor(VioGpuDefaultChildDescriptorMode(false));
    CHECK(legacy.InterfaceTechnology == VioGpuVotInternal && legacy.HpdAwareness == VioGpuHpdAlwaysConnected);

    // WDDM 2.0 must not default to the descriptor dxgkrnl refused on target.
    VioGpuChildDescriptorMode wddm2 = VioGpuDefaultChildDescriptorMode(true);
    VioGpuChildDescriptor current = VioGpuChildDescriptorFor(wddm2);
    CHECK(!(current.InterfaceTechnology == VioGpuVotInternal && current.HpdAwareness == VioGpuHpdAlwaysConnected));
    CHECK(current.InterfaceTechnology == VioGpuVotDisplayPortExternal &&
          current.HpdAwareness == VioGpuHpdInterruptible);

    // Every mode is distinct and uses only documented, non-reserved values.
    for (unsigned a = 0; a < VioGpuChildDescriptorModeCount; ++a)
    {
        VioGpuChildDescriptor d = VioGpuChildDescriptorFor(static_cast<VioGpuChildDescriptorMode>(a));
        CHECK(d.InterfaceTechnology == VioGpuVotInternal || d.InterfaceTechnology == VioGpuVotHdmi ||
              d.InterfaceTechnology == VioGpuVotDisplayPortExternal);
        CHECK(d.HpdAwareness == VioGpuHpdAlwaysConnected || d.HpdAwareness == VioGpuHpdInterruptible);
        for (unsigned b = a + 1; b < VioGpuChildDescriptorModeCount; ++b)
        {
            VioGpuChildDescriptor e = VioGpuChildDescriptorFor(static_cast<VioGpuChildDescriptorMode>(b));
            CHECK(d.InterfaceTechnology != e.InterfaceTechnology || d.HpdAwareness != e.HpdAwareness);
        }
        CHECK(VioGpuSelectChildDescriptorMode(true, a, true) == a);
        CHECK(VioGpuSelectChildDescriptorMode(true, a, false) == a);
    }

    // Absent or invalid registry data falls back to the interface default.
    for (bool wddm2Interface : {false, true})
    {
        const VioGpuChildDescriptorMode fallback = VioGpuDefaultChildDescriptorMode(wddm2Interface);
        CHECK(VioGpuSelectChildDescriptorMode(false, 0, wddm2Interface) == fallback);
        CHECK(VioGpuSelectChildDescriptorMode(false, 3, wddm2Interface) == fallback);
        CHECK(VioGpuSelectChildDescriptorMode(true, VioGpuChildDescriptorModeCount, wddm2Interface) == fallback);
        CHECK(VioGpuSelectChildDescriptorMode(true, 0xFFFFFFFFU, wddm2Interface) == fallback);
    }
    CHECK(VioGpuChildDescriptorFor(static_cast<VioGpuChildDescriptorMode>(99)).HpdAwareness ==
          VioGpuHpdAlwaysConnected);

    // Child DDI trace entries can never collide with DXGKQAITYPE values.
    for (unsigned type : {VioGpuActivationChildRelations, VioGpuActivationChildStatus, VioGpuActivationChildDescriptor})
    {
        CHECK((type & VioGpuActivationChildDdiFlag) != 0 && type > 0xFFFFU);
    }
    CHECK(VioGpuActivationChildRelations != VioGpuActivationChildStatus &&
          VioGpuActivationChildStatus != VioGpuActivationChildDescriptor);

    std::printf("child descriptor: %u checks, %u failures\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
