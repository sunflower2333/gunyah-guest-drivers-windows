#include "viogpu_native_diagnostic_mode.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

using VOID = void; using UINT = unsigned; using ULONG = unsigned;
using LONG = int; using ULONG_PTR = uintptr_t; using NTSTATUS = int;
using BOOLEAN = bool; using HANDLE = void*; using KMUTEX = std::timed_mutex;
constexpr bool FALSE = false;
constexpr int STATUS_SUCCESS = 0, Executive = 0, KernelMode = 0;
constexpr int PLUGPLAY_REGKEY_DRIVER = 0, KEY_SET_VALUE = 0, REG_BINARY = 0;
#define PAGED_CODE() ((void)0)
#define NT_SUCCESS(x) ((x) >= 0)
#define ARRAYSIZE(x) (sizeof(x) / sizeof((x)[0]))
#define RtlZeroMemory(p, n) std::memset(p, 0, n)
void check(bool result, const char *message)
{
    if (!result) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
struct Region { UINT cx, cy; };
struct Rational { UINT Numerator, Denominator; };
struct D3DKMDT_VIDPN_SOURCE_MODE {
    struct { struct { Region PrimSurfSize, VisibleRegionSize; UINT Stride, PixelFormat; } Graphics; } Format;
};
struct D3DKMDT_VIDPN_PRESENT_PATH {
    UINT VidPnSourceId, VidPnTargetId;
    struct { UINT Rotation, Scaling; } ContentTransformation;
};
struct D3DKMDT_VIDEO_SIGNAL_INFO {
    Region ActiveSize, TotalSize; unsigned long long PixelRate; Rational HSyncFreq, VSyncFreq;
};
struct CursorFlags { UINT Value, Visible; };
struct DXGKARG_SETPOINTERPOSITION { int X, Y; CursorFlags Flags; };
struct DXGKARG_SETPOINTERSHAPE { UINT Width, Height, Pitch, XHot, YHot; CursorFlags Flags; };
struct UNICODE_STRING { const wchar_t *value; };

std::atomic<unsigned> waits{}, writes{};
bool failWait{}, failOpen{};
thread_local std::function<void()> beforeWait, afterRead, onExchange, onWrite;
void once(std::function<void()> &hook)
{
    auto callback = std::move(hook);
    hook = {};
    if (callback) callback();
}
LONG InterlockedCompareExchange(volatile LONG *value, LONG replacement, LONG expected)
{
    __atomic_compare_exchange_n(value, &expected, replacement, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    once(afterRead);
    return expected;
}
LONG InterlockedExchange(volatile LONG *value, LONG replacement)
{
    once(onExchange);
    return __atomic_exchange_n(value, replacement, __ATOMIC_SEQ_CST);
}
LONG InterlockedOr(volatile LONG *value, LONG bits)
{
    return __atomic_fetch_or(value, bits, __ATOMIC_SEQ_CST);
}
void KeInitializeMutex(KMUTEX*, int) {}
NTSTATUS KeWaitForSingleObject(KMUTEX *mutex, int, int, bool, void*)
{
    ++waits;
    once(beforeWait);
    if (failWait) return -1;
    check(mutex->try_lock_for(std::chrono::seconds(2)), "unexpected capture mutex wait");
    return STATUS_SUCCESS;
}
void KeReleaseMutex(KMUTEX *mutex, bool) { mutex->unlock(); }
size_t RtlCompareMemory(const void *a, const void *b, size_t size)
{
    return std::memcmp(a, b, size) == 0 ? size : 0;
}
NTSTATUS IoOpenDeviceRegistryKey(void *device, int, int, HANDLE *key)
{
    *key = device;
    return failOpen ? -1 : STATUS_SUCCESS;
}
void RtlInitUnicodeString(UNICODE_STRING *name, const wchar_t *value) { name->value = value; }
NTSTATUS ZwSetValueKey(HANDLE, UNICODE_STRING*, int, int, void*, ULONG)
{
    ++writes;
    once(onWrite);
    return STATUS_SUCCESS;
}
void ZwClose(HANDLE) {}

struct VioGpuDod {
// INSERT_MEMBERS
    bool enabled = true;
    void *m_pPhysicalDevice = this;
    VioGpuDod() {
// INSERT_INIT
    }
    bool NativeScanoutDiagnosticEnabled() const { return enabled; }
    bool QueryReservedNativeScanoutMode(VIOGPU_NATIVE_SCANOUT_MODE*, BOOLEAN*) { return false; }
    VOID RecordNativeDiagnosticDdi(const D3DKMDT_VIDPN_SOURCE_MODE*, const D3DKMDT_VIDPN_PRESENT_PATH*,
        const D3DKMDT_VIDEO_SIGNAL_INFO*, HANDLE, UINT, NTSTATUS, const VIOGPU_NATIVE_SCANOUT_MODE*);
    VOID RecordNativeDiagnosticCursor(const DXGKARG_SETPOINTERPOSITION*, const DXGKARG_SETPOINTERSHAPE*);
    void mode(const VIOGPU_NATIVE_SCANOUT_MODE &value) {
        RecordNativeDiagnosticDdi(nullptr, nullptr, nullptr, nullptr, 2, STATUS_SUCCESS, &value);
    }
};
// INSERT_PRODUCTION

int main()
{
    VioGpuDod device;
    DXGKARG_SETPOINTERPOSITION position{-7, 900, {17, 1}};
    DXGKARG_SETPOINTERSHAPE shape{64, 32, 256, 3, 4, {5, 0}};
    device.enabled = false;
    device.RecordNativeDiagnosticCursor(&position, nullptr);
    device.enabled = true;
    device.RecordNativeDiagnosticCursor(nullptr, nullptr);
    device.RecordNativeDiagnosticCursor(&position, &shape);
    check(waits == 0 && writes == 0, "disabled or invalid capture waited");

    VIOGPU_NATIVE_SCANOUT_MODE mode{};
    mode.LocalResetGeneration = 1;
    device.mode(mode);
    device.RecordNativeDiagnosticCursor(&position, nullptr);
    auto before = waits.load(), savedWrites = writes.load();
    const auto first = device.m_NativeDiagnosticCursorCapture[1];
    check(first.Stage == 2 && first.X == static_cast<UINT>(-7) && first.Y == 900 && first.Visible == 1,
          "position capture changed values");
    position.X = 300;
    device.RecordNativeDiagnosticCursor(&position, nullptr);
    check(waits == before && writes == savedWrites, "duplicate position waited");
    check(std::memcmp(&first, &device.m_NativeDiagnosticCursorCapture[1], sizeof(first)) == 0,
          "first cursor record overwritten");
    device.RecordNativeDiagnosticCursor(nullptr, &shape);
    const auto capturedShape = device.m_NativeDiagnosticCursorCapture[0];
    check(capturedShape.Stage == 1 && capturedShape.Width == 64 && capturedShape.HotX == 3,
          "shape capture changed values");
    before = waits; savedWrites = writes;
    device.RecordNativeDiagnosticCursor(nullptr, &shape);
    check(waits == before && writes == savedWrites, "duplicate shape waited");
    device.mode(mode);
    before = waits;
    device.RecordNativeDiagnosticCursor(&position, nullptr);
    check(waits == before, "same mode spuriously rearmed capture");

    // Reset domains independently rearm both one-shot records, with records
    // cleared before the completion mask becomes observable as empty.
    for (int generation = 0; generation < 3; ++generation) {
        if (generation == 0) ++mode.LocalResetGeneration;
        if (generation == 1) ++mode.Geometry.ModeGeneration;
        if (generation == 2) ++mode.Profile.ProfileGeneration;
        onExchange = [&] {
            const VIOGPU_NATIVE_DIAGNOSTIC_CURSOR_RECORD empty[2]{};
            check(std::memcmp(empty, device.m_NativeDiagnosticCursorCapture, sizeof(empty)) == 0,
                  "rearm published before records cleared");
            check(std::memcmp(&mode, &device.m_NativeDiagnosticCaptureMode, sizeof(mode)) == 0,
                  "rearm published before mode installed");
        };
        device.mode(mode);
        onExchange = {};
        before = waits;
        device.RecordNativeDiagnosticCursor(&position, nullptr);
        check(waits == before + 1 && device.m_NativeDiagnosticCursorCapture[1].Stage == 2,
              "new generation did not rearm");
        check(std::memcmp(&mode, &device.m_NativeDiagnosticCursorCapture[1].Mode, sizeof(mode)) == 0,
              "new cursor captured wrong generation");
        device.RecordNativeDiagnosticCursor(nullptr, &shape);
    }

    // Both callers saw empty before acquiring the mutex: only the winner
    // retains its sample, including an old waiter spanning a mode reset.
    ++mode.LocalResetGeneration;
    device.mode(mode);
    DXGKARG_SETPOINTERPOSITION winner{42, 71, {0, 1}};
    beforeWait = [&] { device.RecordNativeDiagnosticCursor(&winner, nullptr); };
    device.RecordNativeDiagnosticCursor(&position, nullptr);
    check(device.m_NativeDiagnosticCursorCapture[1].X == 42, "first cursor record overwritten");
    ++mode.LocalResetGeneration;
    device.mode(mode);
    beforeWait = [&] { ++mode.LocalResetGeneration; device.mode(mode); };
    device.RecordNativeDiagnosticCursor(&position, nullptr);
    check(device.m_NativeDiagnosticCursorCapture[1].Mode.LocalResetGeneration == mode.LocalResetGeneration,
          "waiter captured stale generation");

    // An old-generation fast read may linearize before a concurrent rearm;
    // it must not set any bit that suppresses the next new-generation event.
    afterRead = [&] { ++mode.LocalResetGeneration; device.mode(mode); };
    device.RecordNativeDiagnosticCursor(&position, nullptr);
    check(device.m_NativeDiagnosticCursorCapture[1].Stage == 0, "old fast read poisoned new generation");
    device.RecordNativeDiagnosticCursor(&position, nullptr);
    check(device.m_NativeDiagnosticCursorCapture[1].Stage == 2, "post-reset first cursor missing");

    // A captured position is independent of slow registry publication.
    ++mode.LocalResetGeneration;
    device.mode(mode);
    onWrite = [&] {
        const auto count = waits.load();
        device.RecordNativeDiagnosticCursor(&winner, nullptr);
        check(waits == count, "completed record still waited on registry writer");
    };
    device.RecordNativeDiagnosticCursor(&position, nullptr);
    check(device.m_NativeDiagnosticCursorCapture[1].X == static_cast<UINT>(position.X),
          "registry callback overwrote first cursor");

    ++mode.LocalResetGeneration;
    device.mode(mode);
    failWait = true;
    device.RecordNativeDiagnosticCursor(&position, nullptr);
    failWait = false;
    check(device.m_NativeDiagnosticCursorCapture[1].Stage == 0, "failed wait consumed capture");
    failOpen = true;
    savedWrites = writes;
    device.RecordNativeDiagnosticCursor(&position, nullptr);
    failOpen = false;
    before = waits;
    device.RecordNativeDiagnosticCursor(&position, nullptr);
    check(waits == before && writes == savedWrites, "registry failure changed one-shot policy");

    ++mode.LocalResetGeneration;
    device.mode(mode);
    savedWrites = writes;
    std::vector<std::thread> callers;
    for (int i = 0; i < 16; ++i) callers.emplace_back([&] {
        for (int repeat = 0; repeat < 100; ++repeat) device.RecordNativeDiagnosticCursor(&position, nullptr);
    });
    for (auto &caller : callers) caller.join();
    check(writes == savedWrites + 1, "concurrent callers published duplicate records");
    std::puts("PASS production cursor capture: duplicate bypass, shape/position independence, generation rearm, reset races, registry/wait failures, 1600 concurrent calls");
}
