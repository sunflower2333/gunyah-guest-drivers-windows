#include "display_timing.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#ifndef _In_
#define _In_
#endif
#define CONST                         const
#define PAGED_CODE()                  ((void)0)
#define DbgPrint(...)                 ((void)0)
#define UNREFERENCED_PARAMETER(value) ((void)(value))
// Record release errors through the return value, as in a free WDK build.
#define NT_ASSERT(value)              ((void)(value))
#define RtlZeroMemory(pointer, bytes) std::memset(pointer, 0, bytes)
using UINT = unsigned;
using VOID = void;
using NTSTATUS = int32_t;
constexpr NTSTATUS STATUS_SUCCESS = 0;
constexpr NTSTATUS STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET = -1;
constexpr NTSTATUS ADD_ERROR = -2;
constexpr NTSTATUS CREATE_ERROR = -3;
constexpr NTSTATUS RELEASE_ERROR = -4;
static bool NT_SUCCESS(NTSTATUS value)
{
    return value >= 0;
}
enum
{
    D3DKMDT_VSS_OTHER = 1,
    D3DDDI_VSSLO_PROGRESSIVE = 2,
    D3DKMDT_MCO_DRIVER = 3,
    D3DKMDT_MP_PREFERRED = 4,
    D3DKMDT_MP_NOTPREFERRED = 5,
    D3DKMDT_CB_SRGB = 6,
};
struct Size
{
    unsigned cx, cy;
};
struct Rational
{
    unsigned Numerator, Denominator;
};
struct D3DKMDT_VIDEO_SIGNAL_INFO
{
    unsigned VideoStandard;
    Size TotalSize, ActiveSize;
    Rational VSyncFreq, HSyncFreq;
    unsigned long long PixelRate;
    unsigned ScanLineOrdering;
};
struct D3DKMDT_MONITOR_SOURCE_MODE
{
    D3DKMDT_VIDEO_SIGNAL_INFO VideoSignalInfo;
    unsigned Origin, Preference, ColorBasis;
    struct
    {
        unsigned FirstChannel, SecondChannel, ThirdChannel, FourthChannel;
    } ColorCoeffDynamicRanges;
};
struct VIDEO_MODE_INFORMATION
{
    unsigned ModeIndex, VisScreenWidth, VisScreenHeight;
};
using PVIDEO_MODE_INFORMATION = VIDEO_MODE_INFORMATION *;
struct ModeSet;
struct MonitorInterface
{
    NTSTATUS (*pfnCreateNewModeInfo)(ModeSet *, D3DKMDT_MONITOR_SOURCE_MODE **);
    NTSTATUS (*pfnAddMode)(ModeSet *, D3DKMDT_MONITOR_SOURCE_MODE *);
    NTSTATUS (*pfnReleaseModeInfo)(ModeSet *, D3DKMDT_MONITOR_SOURCE_MODE *);
};
struct DXGKARG_RECOMMENDMONITORMODES
{
    ModeSet *hMonitorSourceModeSet;
    const MonitorInterface *pMonitorSourceModeSetInterface;
};
struct Hardware
{
    UINT current = 0;
    VIDEO_MODE_INFORMATION modes[3] = {{0, 3040, 1904}, {1, 3040, 1904}, {2, 1920, 1080}};
    VIOGPU_DISPLAY_TIMING timings[3] = {VioGpuVirtualTiming(3040, 1904, 60),
                                        VioGpuVirtualTiming(3040, 1904, 165),
                                        VioGpuVirtualTiming(1920, 1080, 60)};
    Hardware()
    {
        // Match the actual current crosvm DisplayID high-clock mode.
        timings[1].TotalWidth = 3600;
        timings[1].TotalHeight = 1954;
        timings[1].PixelClock = 1160680000ULL;
    }
    UINT GetModeCount()
    {
        return 3;
    }
    UINT GetCurrentModeIndex()
    {
        return current;
    }
    PVIDEO_MODE_INFORMATION GetModeInfo(UINT index)
    {
        return &modes[index];
    }
    const VIOGPU_DISPLAY_TIMING &GetModeTiming(UINT index)
    {
        return timings[index];
    }
};
struct VioGpuDod
{
    Hardware *m_pHWDevice;
    VOID BuildVideoSignalInfo(D3DKMDT_VIDEO_SIGNAL_INFO *, PVIDEO_MODE_INFORMATION);
    NTSTATUS AddSingleMonitorMode(const DXGKARG_RECOMMENDMONITORMODES *const);
};

// INSERT_PRODUCTION

static unsigned checks = 0, failures = 0;
static void check(bool success, const char *message)
{
    ++checks;
    if (!success)
    {
        ++failures;
        std::printf("FAIL %s\n", message);
    }
}
struct ModeSet
{
    struct Allocation
    {
        std::unique_ptr<D3DKMDT_MONITOR_SOURCE_MODE> mode;
        enum State
        {
            Created,
            Transferred,
            Released,
            ReleaseFailed
        } state = Created;
    };
    std::vector<Allocation> allocations;
    std::vector<D3DKMDT_MONITOR_SOURCE_MODE> accepted;
    unsigned creates = 0, adds = 0, releases = 0;
    unsigned failCreate = 0;
    unsigned long long failAddClock = 0, failReleaseClock = 0;

    Allocation &allocation(D3DKMDT_MONITOR_SOURCE_MODE *mode)
    {
        return *std::find_if(allocations.begin(), allocations.end(), [mode](const Allocation &entry) {
            return entry.mode.get() == mode;
        });
    }
    static NTSTATUS create(ModeSet *set, D3DKMDT_MONITOR_SOURCE_MODE **mode)
    {
        *mode = nullptr;
        if (++set->creates == set->failCreate)
        {
            return CREATE_ERROR;
        }
        Allocation entry;
        entry.mode = std::make_unique<D3DKMDT_MONITOR_SOURCE_MODE>();
        *mode = entry.mode.get();
        set->allocations.push_back(std::move(entry));
        return STATUS_SUCCESS;
    }
    static NTSTATUS add(ModeSet *set, D3DKMDT_MONITOR_SOURCE_MODE *mode)
    {
        ++set->adds;
        check(set->allocation(mode).state == Allocation::Created, "only newly created mode is added");
        if (mode->VideoSignalInfo.PixelRate == set->failAddClock)
        {
            return ADD_ERROR;
        }
        if (set->contains(mode->VideoSignalInfo.PixelRate))
        {
            return STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET;
        }
        set->accepted.push_back(*mode);
        set->allocation(mode).state = Allocation::Transferred;
        return STATUS_SUCCESS;
    }
    static NTSTATUS release(ModeSet *set, D3DKMDT_MONITOR_SOURCE_MODE *mode)
    {
        ++set->releases;
        check(set->allocation(mode).state == Allocation::Created, "failed addition released exactly once");
        if (mode->VideoSignalInfo.PixelRate == set->failReleaseClock)
        {
            set->allocation(mode).state = Allocation::ReleaseFailed;
            return RELEASE_ERROR;
        }
        set->allocation(mode).state = Allocation::Released;
        return STATUS_SUCCESS;
    }
    bool contains(unsigned long long clock) const
    {
        return std::any_of(accepted.begin(), accepted.end(), [clock](const auto &mode) {
            return mode.VideoSignalInfo.PixelRate == clock;
        });
    }
    void seed(VioGpuDod &dod, UINT index)
    {
        D3DKMDT_MONITOR_SOURCE_MODE mode = {};
        dod.BuildVideoSignalInfo(&mode.VideoSignalInfo, dod.m_pHWDevice->GetModeInfo(index));
        mode.Preference = index == dod.m_pHWDevice->current ? D3DKMDT_MP_PREFERRED : D3DKMDT_MP_NOTPREFERRED;
        accepted.push_back(mode);
    }
    NTSTATUS run(VioGpuDod &dod)
    {
        static const MonitorInterface callbacks = {create, add, release};
        const DXGKARG_RECOMMENDMONITORMODES request = {this, &callbacks};
        const NTSTATUS status = dod.AddSingleMonitorMode(&request);
        for (const auto &entry : allocations)
        {
            check(entry.state != Allocation::Created, "no outstanding unhandled mode ownership");
        }
        return status;
    }
};

int main()
{
    Hardware hardware;
    VioGpuDod dod = {&hardware};
    const auto clock60 = hardware.timings[0].PixelClock;
    const auto clock165 = hardware.timings[1].PixelClock;
    const auto clock1920 = hardware.timings[2].PixelClock;
    {
        ModeSet set;
        check(set.run(dod) == STATUS_SUCCESS && set.accepted.size() == 3, "empty modeset receives all distinct modes");
        for (const auto &mode : set.accepted)
        {
            const auto &signal = mode.VideoSignalInfo;
            check(mode.Preference == (signal.PixelRate == clock60 ? D3DKMDT_MP_PREFERRED : D3DKMDT_MP_NOTPREFERRED),
                  "current preference preserved without forcing165Hz");
            check(mode.Origin == D3DKMDT_MCO_DRIVER && mode.ColorBasis == D3DKMDT_CB_SRGB && mode.ColorCoeffDynamicRanges.FirstChannel == 8 &&
                                                                                                                      mode.ColorCoeffDynamicRanges.SecondChannel == 8 &&
                                                                                                                      mode.ColorCoeffDynamicRanges.ThirdChannel == 8 &&
                                                                                                                      mode.ColorCoeffDynamicRanges.FourthChannel == 8,
                  "monitor origin color and dynamic range preserved");
            check(signal.VideoStandard == D3DKMDT_VSS_OTHER && signal.ScanLineOrdering == D3DDDI_VSSLO_PROGRESSIVE,
                  "video standard and scan ordering preserved");
            if (signal.PixelRate == clock165)
            {
                check(signal.ActiveSize.cx == 3040 && signal.ActiveSize.cy == 1904 && signal.TotalSize.cx == 3600 && signal.TotalSize.cy == 1954 &&
                                                                                                                          signal.VSyncFreq.Numerator == 1450850 &&
                                                                                                                          signal.VSyncFreq.Denominator == 8793 &&
                                                                                                                          signal.HSyncFreq.Numerator == 2901700 &&
                                                                                                                          signal.HSyncFreq.Denominator == 9,
                      "production signal conversion retains full165Hz clock and exact rationals");
            }
        }
    }
    {
        ModeSet set;
        set.seed(dod, 0);
        check(set.run(dod) == STATUS_SUCCESS && set.contains(clock165) && set.contains(clock1920),
              "existing preferred mode still admits missing165Hz");
    }
    {
        ModeSet set;
        set.seed(dod, 1);
        check(set.run(dod) == STATUS_SUCCESS && set.contains(clock1920),
              "later duplicate does not stop remaining modes");
    }
    {
        ModeSet set;
        set.failAddClock = clock60;
        check(set.run(dod) == ADD_ERROR && set.releases == 1 && set.accepted.empty(),
              "first add error is preserved and temporary released");
    }
    {
        ModeSet set;
        set.failAddClock = clock165;
        check(set.run(dod) == ADD_ERROR && !set.contains(clock1920), "later add error survives successful cleanup");
    }
    {
        ModeSet set;
        set.failCreate = 1;
        check(set.run(dod) == CREATE_ERROR && set.releases == 0 && set.adds == 0,
              "first create failure owns no temporary");
    }
    {
        ModeSet set;
        set.failCreate = 2;
        check(set.run(dod) == CREATE_ERROR && set.releases == 0 && set.accepted.size() == 1,
              "later create failure does not release transferred mode");
    }
    {
        ModeSet set;
        set.seed(dod, 0);
        set.failReleaseClock = clock60;
        check(set.run(dod) == RELEASE_ERROR && set.releases == 1, "first duplicate release failure is propagated");
    }
    {
        ModeSet set;
        set.seed(dod, 1);
        set.failReleaseClock = clock165;
        check(set.run(dod) == RELEASE_ERROR && !set.contains(clock1920),
              "later duplicate release failure is propagated");
    }
    for (auto clock : {clock60, clock165})
    {
        ModeSet set;
        set.failAddClock = clock;
        set.failReleaseClock = clock;
        check(set.run(dod) == ADD_ERROR, "initiating add failure retained when release also fails");
    }
    {
        ModeSet set;
        hardware.current = 1;
        check(set.run(dod) == STATUS_SUCCESS && set.accepted.size() == 3 && set.accepted[0].Preference == D3DKMDT_MP_PREFERRED &&
                                                                                                                  set.accepted[0].VideoSignalInfo.PixelRate == clock165,
              "current165Hz is added first and remains preferred");
    }
    std::printf("%s actual monitor callback ownership enumeration and signals: %u checks, %u failures\n",
                failures ? "FAIL" : "PASS",
                checks,
                failures);
    return failures ? 1 : 0;
}
