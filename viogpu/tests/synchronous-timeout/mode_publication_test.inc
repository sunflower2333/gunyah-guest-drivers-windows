/* The actual modeset flush wrapper, adapter leaf, failure publisher and registry
 * recorder are compiled below. Only hardware/rundown/registry peers are modeled. */
using VOID = void;
using DWORD = uint32_t;
using HANDLE = void*;
using PCWSTR = const wchar_t*;
constexpr UINT MAXUINT = UINT32_MAX;
constexpr int PLUGPLAY_REGKEY_DRIVER = 1, KEY_SET_VALUE = 2;
#define NT_SUCCESS(status) ((status) >= 0)
#define DbgPrintEx(...) ((void)0)
#define ARRAYSIZE(array) (sizeof(array) / sizeof((array)[0]))
struct ModeRundown { bool available = true; unsigned owners = 0; };
static bool ExAcquireRundownProtection(ModeRundown* state) {
    if (!state->available) return false;
    ++state->owners; return true;
}
static void ExReleaseRundownProtection(ModeRundown* state) {
    check(state->owners != 0, "release only acquired hardware rundown"); --state->owners;
}
static VIOGPU_HOST_CONTEXT_RESULT flushResult = VioGpuHostContextConfirmed;
static unsigned flushCalls;
VIOGPU_HOST_CONTEXT_RESULT CtrlQueue::FlushResourceSynchronous(UINT id, UINT width, UINT height, UINT x, UINT y) {
    check(id == 71 && width == 1920 && height == 1080 && !x && !y, "exact primary forwarded to control flush");
    ++flushCalls;
    return flushResult;
}
struct VioGpuDod;
struct VioGpuAdapter {
    CtrlQueue m_CtrlQueue;
    VioGpuDod* m_pVioGpuDod = nullptr;
    volatile LONG64 m_NativeContextResetGeneration = 9;
    BOOLEAN Reconcile2DResourceAfterReset(VIOGPU_2D_RESOURCE_STATE*, ULONGLONG*, BOOLEAN* retired) {
        *retired = FALSE; return TRUE;
    }
    VIOGPU_HOST_CONTEXT_RESULT Flush2DResource(UINT, UINT, UINT, VIOGPU_2D_RESOURCE_STATE*, ULONGLONG*);
    void RecordSynchronousFailureDiagnostic(void);
};
struct VioGpuDod {
    VioGpuAdapter* m_pHWDevice = nullptr;
    ModeRundown m_HardwareOperations;
    void* m_pPhysicalDevice = this;
    bool submitAllowed = true, detachOnRelease = false;
    unsigned submitOwners = 0, acquireCalls = 0;
    unsigned opens = 0, closes = 0, writes = 0;
    unsigned failWrite = 0;
    bool failOpen = false;
    std::map<std::wstring, DWORD> registry;
    BOOLEAN AcquireNativeSubmissionOperation() {
        ++acquireCalls;
        if (!submitAllowed || !ExAcquireRundownProtection(&m_HardwareOperations)) return FALSE;
        ++submitOwners; return TRUE;
    }
    void ReleaseNativeSubmissionOperation() {
        check(submitOwners == 1, "mode wrapper releases one submission owner");
        --submitOwners; ExReleaseRundownProtection(&m_HardwareOperations);
        if (detachOnRelease) m_pHWDevice = nullptr;
    }
    VIOGPU_HOST_CONTEXT_RESULT Flush2DResource(UINT, UINT, UINT, VIOGPU_2D_RESOURCE_STATE*, ULONGLONG*);
    VOID RecordNativeSynchronousFailureDiagnostic(void);
    VOID RecordNativeSynchronousPoisonDiagnostic(ULONG, ULONG, ULONG, const VIOGPU_SYNCHRONOUS_TIMEOUT_DIAGNOSTIC*);
    NTSTATUS WriteRegistryDWORD(HANDLE key, PCWSTR name, DWORD* value) {
        check(key == this && m_HardwareOperations.owners == 1 && !submitOwners && fixtureIrql == PASSIVE_LEVEL,
              "mode registry write owns only independent passive hardware rundown");
        ++writes;
        if (failWrite && writes == failWrite) return static_cast<NTSTATUS>(0xc0000022U);
        registry[name] = *value; return STATUS_SUCCESS;
    }
};
static NTSTATUS IoOpenDeviceRegistryKey(void* physical, int kind, int access, HANDLE* key) {
    auto* dod = static_cast<VioGpuDod*>(physical);
    check(kind == PLUGPLAY_REGKEY_DRIVER && access == KEY_SET_VALUE &&
          dod->m_HardwareOperations.owners == 1 && !dod->submitOwners && fixtureIrql == PASSIVE_LEVEL,
          "open exact driver key under passive hardware lifetime");
    ++dod->opens;
    if (dod->failOpen) return static_cast<NTSTATUS>(0xc0000022U);
    *key = dod; return STATUS_SUCCESS;
}
static void ZwClose(HANDLE key) { ++static_cast<VioGpuDod*>(key)->closes; }
// INSERT_MODE_PRODUCTION

static void mode_publication() {
    VioGpuDod dod;
    VioGpuAdapter adapter;
    dod.m_pHWDevice = &adapter; adapter.m_pVioGpuDod = &dod;
    VIOGPU_2D_RESOURCE_STATE state = VioGpu2DResourceBackingAttached;
    ULONGLONG reset = 9;
    auto flush = [&] { return dod.Flush2DResource(71, 1920, 1080, &state, &reset); };
    enable(adapter.m_CtrlQueue);
    check(flush() == VioGpuHostContextConfirmed && !dod.opens && !dod.m_HardwareOperations.owners,
          "successful mode flush performs no diagnostic IO");
    flushResult = VioGpuHostContextRejected;
    check(flush() == flushResult && !dod.opens, "healthy host rejection invents no timeout");

    // A real accepted queue wait records the first flush timeout; no context
    // destruction occurs anywhere in this fixture's production mode path.
    GPU_RES_FLUSH command = {};
    command.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH; command.resource_id = 71;
    GPU_VBUFFER buffer = {}; buffer.buf = reinterpret_cast<char*>(&command); buffer.size = sizeof(command);
    BOOLEAN release = TRUE, submitted = FALSE;
    waitResult = STATUS_TIMEOUT;
    check(!adapter.m_CtrlQueue.SubmitSynchronousLocked(&buffer, &release, &submitted) && submitted && !release,
          "mode timeout preserves host descriptor ownership");
    flushResult = VioGpuHostContextUnknown;
    check(flush() == VioGpuHostContextUnknown && dod.registry[L"NativeSynchronousTimeoutValid"] == 1,
          "mode flush failure publishes first timeout without context destruction");
    check(dod.registry[L"NativeSynchronousTimeoutType"] == VIRTIO_GPU_CMD_RESOURCE_FLUSH &&
          dod.registry[L"NativeSynchronousTimeoutResourceId"] == 71 &&
          dod.registry[L"NativeSynchronousTimeoutWaitStatus"] == static_cast<DWORD>(STATUS_TIMEOUT) &&
          dod.registry[L"NativeSynchronousTimeoutCallerRva"] == 0x4321 &&
          dod.registry[L"NativeSynchronousTimeoutEpochGeneration"] == 1,
          "mode failure registry identifies original submitted command");
    check(!adapter.m_CtrlQueue.IsSynchronousRequestsHealthy() && !release && buffer.complete_cb && !buffer.auto_release,
          "diagnostic publication never heals poison or releases quarantined descriptor");
    const auto first = dod.registry;
    const auto beforeFlushCalls = flushCalls, beforeAcquire = dod.acquireCalls;
    dod.submitAllowed = false;
    check(flush() == VioGpuHostContextNotSubmitted && flushCalls == beforeFlushCalls &&
          dod.acquireCalls == beforeAcquire + 1 && dod.registry == first,
          "closed submission gate still publishes immutable timeout without resubmitting");
    check(!dod.m_HardwareOperations.owners && !dod.submitOwners && dod.opens == dod.closes,
          "failure publication balances independent hardware and registry ownership");

    unsigned oldOpens = dod.opens;
    fixtureIrql = 2;
    check(flush() == VioGpuHostContextNotSubmitted && dod.opens == oldOpens,
          "nonpassive mode refusal performs no registry IO");
    fixtureIrql = PASSIVE_LEVEL;
    dod.m_HardwareOperations.available = false;
    check(flush() == VioGpuHostContextNotSubmitted && dod.opens == oldOpens,
          "closed hardware rundown forbids diagnostic dereference");
    dod.m_HardwareOperations.available = true;
    dod.m_pHWDevice = nullptr;
    check(flush() == VioGpuHostContextNotSubmitted && dod.opens == oldOpens && !dod.m_HardwareOperations.owners,
          "missing hardware adapter balances diagnostic rundown");
    dod.m_pHWDevice = &adapter; dod.submitAllowed = true; dod.detachOnRelease = true;
    check(flush() == VioGpuHostContextUnknown && dod.opens == oldOpens,
          "adapter is reacquired after submission release instead of retaining a stale pointer");
    dod.m_pHWDevice = &adapter; dod.submitAllowed = false; dod.detachOnRelease = false;

    dod.failOpen = true;
    check(flush() == VioGpuHostContextNotSubmitted && !dod.m_HardwareOperations.owners,
          "registry open failure leaves mode result and lifetime unchanged");
    dod.failOpen = false;
    for (unsigned failure = 4; failure <= 11; ++failure) {
        dod.registry.clear(); dod.writes = 0; dod.failWrite = failure;
        check(flush() == VioGpuHostContextNotSubmitted && dod.registry[L"NativeSynchronousTimeoutValid"] == 0,
              "partial mode timeout writes never publish a valid snapshot");
        dod.failWrite = 0;
        check(flush() == VioGpuHostContextNotSubmitted && dod.registry == first,
              "next failure retries registry publication without changing first timeout");
    }
    check(!dod.m_HardwareOperations.owners && !dod.submitOwners, "mode fixture leaves no operation owner");
    waitResult = STATUS_SUCCESS; flushResult = VioGpuHostContextConfirmed;
    std::puts("PASS actual mode flush failure entry, registry publication, closed-gate lifetime and negative boundaries");
}
