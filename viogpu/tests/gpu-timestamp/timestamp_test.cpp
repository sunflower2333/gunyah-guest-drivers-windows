// SPDX-License-Identifier: MIT
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#ifndef _In_
#define _In_
#endif
#ifndef _Out_
#define _Out_
#endif
#define PAGED_CODE() ((void)0)
using ULONGLONG = unsigned long long;
using PULONGLONG = ULONGLONG *;
using LONG = int32_t;
using NTSTATUS = int32_t;
constexpr NTSTATUS STATUS_SUCCESS = 0;
constexpr NTSTATUS STATUS_INVALID_PARAMETER = -1;
constexpr NTSTATUS STATUS_DEVICE_NOT_READY = -2;
constexpr NTSTATUS STATUS_NOT_SUPPORTED = -3;
constexpr NTSTATUS STATUS_INSUFFICIENT_RESOURCES = -4;
constexpr int PASSIVE_LEVEL = 0;
constexpr int VioGpuNativeContextOwnerLive = 1;
constexpr int MSM_PARAM_TIMESTAMP = 5;
enum VIOGPU_HOST_CONTEXT_RESULT { VioGpuHostContextConfirmed, VioGpuHostContextRejected,
                                 VioGpuHostContextUnknown, VioGpuHostContextNotSubmitted };
struct VIOGPU_NATIVE_CONTEXT_OWNER {
   int State = 1, Registration = 1, ContextId = 42, Generation = 1;
   ULONGLONG ResetGeneration = 2;
};
class VioGpuAdapter;
struct VIOGPU_NATIVE_CONTEXT_SNAPSHOT {
   VioGpuAdapter *Adapter;
   VIOGPU_NATIVE_CONTEXT_OWNER *Owner;
   int Registration = 1, ContextId = 42, Generation = 1;
   ULONGLONG ResetGeneration = 2;
};
static int checks, failures;
static bool faults;
static int KeGetCurrentIrql() { return PASSIVE_LEVEL; }
static bool VioGpuNativeControlFaultsClear(VioGpuAdapter *, VIOGPU_NATIVE_CONTEXT_OWNER *) { return !faults; }
class VioGpuAdapter {
public:
   bool current = true, reset_on_read = false, fault_on_read = false;
   LONG error = 0;
   ULONGLONG sample = 0x123456789ab;
   VIOGPU_HOST_CONTEXT_RESULT injectedResult = VioGpuHostContextConfirmed;
   bool IsNativeContextGenerationCurrent(int, ULONGLONG) { return current; }
   VIOGPU_HOST_CONTEXT_RESULT QueryNativeContextParameterLocked(VIOGPU_NATIVE_CONTEXT_OWNER *, int param,
                                                               PULONGLONG ticks, LONG *hostError) {
      if (param != MSM_PARAM_TIMESTAMP) failures++;
      *ticks = sample; *hostError = error;
      if (reset_on_read) current = false;
      if (fault_on_read) faults = true;
      return injectedResult;
   }
   NTSTATUS QueryNativeGpuTimestamp(const VIOGPU_NATIVE_CONTEXT_SNAPSHOT *, PULONGLONG);
};
// INSERT_PRODUCTION
static void check(bool value, const char *name) {
   checks++;
   if (!value) { printf("FAIL %s\n", name); failures++; }
}
int main() {
   VioGpuAdapter adapter;
   VIOGPU_NATIVE_CONTEXT_OWNER owner;
   VIOGPU_NATIVE_CONTEXT_SNAPSHOT snapshot{&adapter, &owner};
   ULONGLONG sample = 99;
   auto run = [&]() { return adapter.QueryNativeGpuTimestamp(&snapshot, &sample); };
   check(run() == 0 && sample == adapter.sample, "physical sample preserved");
   adapter.sample = 0;
   check(run() == STATUS_NOT_SUPPORTED && sample == 0, "legacy fabricated zero rejected");
   adapter.sample = 0x123456789ab;
   adapter.injectedResult = VioGpuHostContextRejected;
   for (LONG error : {-25, -95, -38}) {
      adapter.error = error;
      check(run() == STATUS_NOT_SUPPORTED && sample == 0, "unsupported ioctl preserved");
   }
   adapter.error = -12;
   check(run() == STATUS_INSUFFICIENT_RESOURCES && sample == 0, "host allocation failure preserved");
   adapter.error = -5;
   check(run() == STATUS_DEVICE_NOT_READY && sample == 0, "host IO failure rejected");
   adapter.injectedResult = VioGpuHostContextUnknown;
   check(run() == STATUS_DEVICE_NOT_READY && sample == 0, "ambiguous completion rejected");
   adapter.injectedResult = VioGpuHostContextConfirmed;
   adapter.current = false;
   check(run() == STATUS_DEVICE_NOT_READY && sample == 0, "old reset epoch rejected");
   adapter.current = true; adapter.reset_on_read = true;
   check(run() == STATUS_DEVICE_NOT_READY && sample == 0, "reset during timestamp read");
   adapter.current = true; adapter.reset_on_read = false; adapter.fault_on_read = true;
   check(run() == STATUS_DEVICE_NOT_READY && sample == 0, "host fault during read rejected");
   adapter.fault_on_read = false; faults = false;
   owner.ContextId++;
   check(run() == STATUS_DEVICE_NOT_READY && sample == 0, "wrong context identity rejected");
   owner.ContextId--;
   owner.State = 2;
   check(run() == STATUS_DEVICE_NOT_READY && sample == 0, "destroying context rejected");
   printf("KMD_TIMESTAMP checks=%d failures=%d\n", checks, failures);
   return failures ? 1 : 0;
}
