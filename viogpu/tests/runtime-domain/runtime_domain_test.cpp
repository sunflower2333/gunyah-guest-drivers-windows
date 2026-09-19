/* SPDX-License-Identifier: BSD-3-Clause */
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <atomic>
#include "viogpu/shared/viogpu_wddm_abi.h"
using ULONG = uint32_t;
using UINT = uint32_t;
using ULONGLONG = uint64_t;
using BOOLEAN = bool;
using NTSTATUS = int;
using VOID = void;
using KIRQL = int;
using KSPIN_LOCK = std::mutex;
constexpr bool TRUE = true, FALSE = false;
constexpr uint32_t MAXULONG = UINT32_MAX;
constexpr int STATUS_SUCCESS = 0, STATUS_INVALID_HANDLE = -1, STATUS_INVALID_PARAMETER = -2, STATUS_DEVICE_BUSY = -3;
constexpr int VioGpuWddmContextNative = 0;
#define NT_ASSERT assert
#define CONTAINING_RECORD(ptr, type, member) reinterpret_cast<type *>(reinterpret_cast<char *>(ptr) - offsetof(type, member))
struct LIST_ENTRY { LIST_ENTRY *Flink, *Blink; };
using PLIST_ENTRY = LIST_ENTRY *;
void InitializeListHead(LIST_ENTRY *p) { p->Flink = p->Blink = p; }
void InsertTailList(LIST_ENTRY *head, LIST_ENTRY *p) {
    p->Flink = head; p->Blink = head->Blink; head->Blink->Flink = p; head->Blink = p;
}
void RemoveEntryList(LIST_ENTRY *p) { p->Blink->Flink = p->Flink; p->Flink->Blink = p->Blink; }
void KeAcquireSpinLock(KSPIN_LOCK *lock, KIRQL *level) { *level = 0; lock->lock(); }
void KeReleaseSpinLock(KSPIN_LOCK *lock, KIRQL) { lock->unlock(); }
struct VIOGPU_NATIVE_CONTEXT_REGISTRATION {
    KSPIN_LOCK BindingLock;
    UINT ContextId = 0;
    ULONGLONG ResetGeneration = 0;
};
struct VIOGPU_WDDM_DEVICE {
    KSPIN_LOCK DomainLock;
    LIST_ENTRY NativeDomains;
    VIOGPU_WDDM_DEVICE() { InitializeListHead(&NativeDomains); }
};
struct VIOGPU_WDDM_CONTEXT {
    VIOGPU_WDDM_DEVICE *Device;
    int Type = VioGpuWddmContextNative;
    LIST_ENTRY DomainLink;
    VIOGPU_WDDM_CONTEXT *DomainOwner = nullptr;
    ULONG DomainChildren = 0;
    BOOLEAN DomainClosing = false, DomainPublished = false;
    UINT DomainContextId = 0;
    ULONGLONG DomainResetGeneration = 0;
    VIOGPU_NATIVE_CONTEXT_REGISTRATION NativeContext;
    explicit VIOGPU_WDDM_CONTEXT(VIOGPU_WDDM_DEVICE *device) : Device(device) { InitializeListHead(&DomainLink); }
};
// INSERT_PRODUCTION
int main() {
    static_assert(sizeof(VIOGPU_WDDM_CONTEXT_CREATE) == 32, "legacy ABI changed");
    static_assert(sizeof(VIOGPU_WDDM_CONTEXT_CREATE_SHARED) == 40, "shared-domain ABI changed");
    VIOGPU_WDDM_DEVICE device, foreign;
    VIOGPU_WDDM_CONTEXT owner(&device), a(&device), b(&device), alien(&foreign);
    owner.NativeContext.ContextId = 7; owner.NativeContext.ResetGeneration = 42;
    PublishNativeDomain(&owner);
    assert(AttachNativeDomain(&alien, 7, 42) == STATUS_INVALID_HANDLE);
    assert(AttachNativeDomain(&a, 7, 43) == STATUS_INVALID_HANDLE);
    assert(AttachNativeDomain(&a, 8, 42) == STATUS_INVALID_HANDLE);
    assert(AttachNativeDomain(&a, 0, 42) == STATUS_INVALID_PARAMETER);
    owner.DomainChildren = MAXULONG;
    assert(AttachNativeDomain(&a, 7, 42) == STATUS_INVALID_HANDLE);
    owner.DomainChildren = 0;
    assert(AttachNativeDomain(&a, 7, 42) == STATUS_SUCCESS);
    assert(AttachNativeDomain(&b, 7, 42) == STATUS_SUCCESS);
    assert(AttachNativeDomain(&a, 7, 42) == STATUS_INVALID_PARAMETER);
    assert(NativeRegistration(&a) == NativeRegistration(&owner));
    assert(NativeRegistration(&a) == NativeRegistration(&b));
    assert(owner.DomainChildren == 2 && a.DomainChildren == 0);
    assert(CloseNativeDomain(&a) == STATUS_SUCCESS && owner.DomainPublished);
    assert(CloseNativeDomain(&owner) == STATUS_DEVICE_BUSY);
    assert(owner.DomainClosing && !owner.DomainPublished && owner.DomainChildren == 2);
    VIOGPU_WDDM_CONTEXT late(&device);
    assert(AttachNativeDomain(&late, 7, 42) == STATUS_INVALID_HANDLE);
    DetachNativeDomain(&a);
    assert(CloseNativeDomain(&owner) == STATUS_DEVICE_BUSY);
    DetachNativeDomain(&b);
    assert(CloseNativeDomain(&owner) == STATUS_SUCCESS && !owner.DomainChildren);
    for (unsigned i = 0; i < 500; ++i) {
        VIOGPU_WDDM_CONTEXT raced_owner(&device), child(&device);
        raced_owner.NativeContext.ContextId = 9; raced_owner.NativeContext.ResetGeneration = 42;
        PublishNativeDomain(&raced_owner);
        std::atomic<bool> go{false};
        NTSTATUS attached = STATUS_INVALID_HANDLE;
        std::thread worker([&] { while (!go.load()) {} attached = AttachNativeDomain(&child, 9, 42); });
        go.store(true);
        const auto closed = CloseNativeDomain(&raced_owner);
        worker.join();
        if (attached == STATUS_SUCCESS) {
            assert(closed == STATUS_DEVICE_BUSY && raced_owner.DomainChildren == 1);
            DetachNativeDomain(&child);
        } else {
            assert(attached == STATUS_INVALID_HANDLE && closed == STATUS_SUCCESS);
        }
        assert(CloseNativeDomain(&raced_owner) == STATUS_SUCCESS);
    }
    puts("PASS production runtime-domain ownership, two queues, close ordering, identity and 500 creation/close races");
}
