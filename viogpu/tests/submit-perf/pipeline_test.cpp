// SPDX-License-Identifier: BSD-3-Clause
// Kernel primitives are mocked; scheduler and Render worker bodies are production code.
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <unordered_set>
#include <vector>
using UINT = unsigned int;
using LONG = int;
using ULONG = unsigned long;
using ULONGLONG = unsigned long long;
using BOOLEAN = bool;
using VOID = void;
using PVOID = void*;
using KIRQL = int;
using NTSTATUS = int;
#define _In_
#define _Inout_
#define _Use_decl_annotations_
#define TRUE true
#define FALSE false
#define MAXUINT UINT32_MAX
#define NT_ASSERT assert
#define NT_SUCCESS(x) ((x) >= 0)
constexpr int STATUS_DEVICE_NOT_READY=-1, STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE=-2;
constexpr int PASSIVE_LEVEL=0, DISPATCH_LEVEL=2, IO_NO_INCREMENT=0, DelayedWorkQueue=0;
constexpr int DPFLTR_DEFAULT_ID=0, DPFLTR_INFO_LEVEL=0;
struct LIST_ENTRY { LIST_ENTRY *Flink, *Blink; };
using PLIST_ENTRY=LIST_ENTRY*;
#define CONTAINING_RECORD(p,t,f) reinterpret_cast<t*>(reinterpret_cast<char*>(p)-offsetof(t,f))
void InitializeListHead(PLIST_ENTRY p) { p->Flink=p->Blink=p; }
bool IsListEmpty(PLIST_ENTRY p) { return p->Flink==p; }
void InsertTailList(PLIST_ENTRY h,PLIST_ENTRY e) { e->Blink=h->Blink;e->Flink=h;h->Blink->Flink=e;h->Blink=e; }
void RemoveEntryList(PLIST_ENTRY e) { e->Blink->Flink=e->Flink;e->Flink->Blink=e->Blink; }
PLIST_ENTRY RemoveHeadList(PLIST_ENTRY h) { auto e=h->Flink;RemoveEntryList(e);return e; }
using KSPIN_LOCK=std::mutex;
thread_local KIRQL irql=PASSIVE_LEVEL;
void KeAcquireSpinLock(KSPIN_LOCK *m,KIRQL *old) { *old=irql;m->lock();irql=DISPATCH_LEVEL; }
void KeReleaseSpinLock(KSPIN_LOCK *m,KIRQL old) { irql=old;m->unlock(); }
KIRQL KeGetCurrentIrql() { return irql; }
LONG InterlockedExchange(volatile LONG *p,LONG n) { LONG o=*p;*p=n;return o; }
LONG InterlockedCompareExchange(volatile LONG *p,LONG n,LONG e) { LONG o=*p;if(o==e)*p=n;return o; }
struct KEVENT { bool set=false; };
void KeClearEvent(KEVENT *p) { p->set=false; }
void KeSetEvent(KEVENT *p,int,bool) { p->set=true; }
ULONGLONG clock100ns=100;
ULONGLONG KeQueryInterruptTime() { return clock100ns++; }
struct RUNDOWN { int references=0;bool allow=true; };
bool ExAcquireRundownProtection(RUNDOWN *p) { if(!p->allow)return false;++p->references;return true; }
void ExReleaseRundownProtection(RUNDOWN *p) { assert(p->references>0);--p->references; }
struct WORK_QUEUE_ITEM { void (*routine)(PVOID);PVOID context; };
std::deque<WORK_QUEUE_ITEM*> tasks;
void ExQueueWorkItem(WORK_QUEUE_ITEM *p,int) { assert(std::find(tasks.begin(),tasks.end(),p)==tasks.end());tasks.push_back(p); }
unsigned reportCount=0;
template<class... T> void DbgPrintEx(int,int,const char*,T...) { ++reportCount; }
// INSERT_RECORDS
struct VIOGPU_WDDM_SUBMISSION;
struct GPU_VBUFFER { VIOGPU_WDDM_SUBMISSION *owner; };
using PGPU_VBUFFER=GPU_VBUFFER*;
struct VIOGPU_WDDM_CONTEXT { UINT NodeOrdinal=0; };
struct VioGpuDod {
    KSPIN_LOCK m_NativePassiveLock;
    LIST_ENTRY m_NativePassiveQueue, m_NativePassiveHostPending;
    UINT m_NativePassivePendingCount=0, m_NativePassiveHostPendingCount=0;
    BOOLEAN m_NativePassiveWorkerQueued=false, m_NativePassiveWorkerRunning=false;
    VIOGPU_NATIVE_PASSIVE_WORK *m_NativePassiveActiveWork=nullptr;
    volatile LONG m_NativePassiveClosing=0;
    KEVENT m_NativePassiveIdleEvent;
    VIOGPU_NATIVE_SUBMIT_PERF m_NativeSubmitPerf{};
    ULONGLONG m_NativeSubmitPerfReported=0;
    RUNDOWN m_HardwareOperations;
    WORK_QUEUE_ITEM m_NativePassiveWorkItem;
    bool reset=false, fast=false, failQueue=false;
    UINT lastFence=0, faults=0;
    std::vector<UINT> issued, barriers;
    VioGpuDod() {
        InitializeListHead(&m_NativePassiveQueue);InitializeListHead(&m_NativePassiveHostPending);
        m_NativePassiveWorkItem={[](PVOID p){static_cast<VioGpuDod*>(p)->RunNativePassiveWorker();},this};
    }
    ~VioGpuDod() { assert(NativePassiveIdleLocked());assert(m_HardwareOperations.references==0); }
    BOOLEAN IsHardwareResetRequested() { return reset; }
    BOOLEAN RecordNativeSubmissionFence(UINT fence) { if(fence<=lastFence)return false;lastFence=fence;return true; }
    BOOLEAN QueueNativePassiveWork(VIOGPU_NATIVE_PASSIVE_WORK*,UINT);
    BOOLEAN NativePassiveDispatchReadyLocked(VIOGPU_NATIVE_PASSIVE_WORK *incoming=nullptr);
    BOOLEAN NativePassiveIdleLocked();
    VOID ReleaseNativePassiveDispatch(VIOGPU_NATIVE_PASSIVE_WORK*);
    VOID CompleteNativePassiveWork(VIOGPU_NATIVE_PASSIVE_WORK*);
    VIOGPU_NATIVE_PASSIVE_WORK_OWNERSHIP CancelNativePassiveWork(VIOGPU_NATIVE_PASSIVE_WORK*);
    VOID CloseNativePassiveQueue();
    VOID RunNativePassiveWorker();
    VOID ReportNativeSubmitPerf();
    bool AcquireNativeSubmissionOperation() { return !reset; }
    void ReleaseNativeSubmissionOperation() {}
    int QueueNativeSubmit(PGPU_VBUFFER,UINT);
    void RequestHardwareResetAtAnyIrql() { reset=true; }
    void NotifyNativeSubmissionFault(UINT,NTSTATUS,UINT,UINT,bool) { ++faults;reset=true; }
};
constexpr UINT VIOGPU_WDDM_SUBMISSION_SIGNATURE=0xabcdef;
enum { VioGpuWddmSubmissionPrepared, VioGpuWddmSubmissionEngineQueued, VioGpuWddmSubmissionHostIssued };
struct VIOGPU_WDDM_SUBMISSION {
    UINT Signature=VIOGPU_WDDM_SUBMISSION_SIGNATURE;
    VioGpuDod *Adapter;
    GPU_VBUFFER buffer{this};
    PGPU_VBUFFER VirtioBuffer=&buffer;
    UINT FenceId;
    volatile LONG WorkReferenceHeld=1, CancelRequested=0, State=VioGpuWddmSubmissionEngineQueued;
    VIOGPU_WDDM_CONTEXT context;
    VIOGPU_WDDM_CONTEXT *Context=&context;
    VIOGPU_NATIVE_PASSIVE_WORK Work{};
    int refs=2;
    bool registry=true;
};
std::unordered_set<VIOGPU_WDDM_SUBMISSION*> live;
bool ReferenceRenderSubmission(VIOGPU_WDDM_SUBMISSION *s) { assert(live.count(s));assert(s->refs>0);++s->refs;return true; }
void DereferenceRenderSubmission(VIOGPU_WDDM_SUBMISSION *s) { assert(s->refs>0);if(--s->refs==0){assert(live.erase(s)==1);delete s;} }
void ReleaseRenderWorkReference(VIOGPU_WDDM_SUBMISSION *s) { if(InterlockedCompareExchange(&s->WorkReferenceHeld,0,1)==1)DereferenceRenderSubmission(s); }
void InvalidateContextUmdFenceTracker(VIOGPU_WDDM_CONTEXT*) {}
void QuarantineSubmission(VIOGPU_WDDM_SUBMISSION *s,int,bool) { if(s->registry){s->registry=false;s->VirtioBuffer=nullptr;DereferenceRenderSubmission(s);} }
NTSTATUS ValidateNativeRenderBindings(VIOGPU_WDDM_SUBMISSION*) { return 0; }
void terminal(VIOGPU_WDDM_SUBMISSION *s) {
    // Model terminal ownership in the same order as the production callbacks.
    ReferenceRenderSubmission(s);
    QuarantineSubmission(s,0,false);
    s->Adapter->CompleteNativePassiveWork(&s->Work);
    ReleaseRenderWorkReference(s);
    DereferenceRenderSubmission(s);
}
int VioGpuDod::QueueNativeSubmit(PGPU_VBUFFER b,UINT fence) {
    if(failQueue)return -1;
    issued.push_back(fence);
    if(fast)terminal(b->owner);
    return 0;
}
// INSERT_PRODUCTION
void pump() {
    unsigned budget=100000;
    while(!tasks.empty()) { assert(budget--);auto p=tasks.front();tasks.pop_front();p->routine(p->context); }
}
void barrier(PVOID p) {
    auto s=static_cast<VIOGPU_WDDM_SUBMISSION*>(p);
    assert(s->Adapter->m_NativePassiveHostPendingCount==0);
    s->Adapter->barriers.push_back(s->FenceId);terminal(s);
}
VIOGPU_WDDM_SUBMISSION *submit(VioGpuDod &a,UINT fence,bool render=true) {
    auto s=new VIOGPU_WDDM_SUBMISSION{};live.insert(s);s->Adapter=&a;s->FenceId=fence;
    InitializeListHead(&s->Work.Link);s->Work.Context=s;s->Work.CancelRequested=&s->CancelRequested;
    s->Work.Routine=render?NativeRenderDispatchWorker:barrier;
    s->Work.CancelRoutine=NativeRenderDispatchCancelled;
    s->Work.PipelineEligible=render;s->Work.PayloadBytes=128;s->Work.AllocationReferences=3;
    assert(a.QueueNativePassiveWork(&s->Work,fence));return s;
}
void finish(VioGpuDod &a) {
    pump();unsigned limit=100000;
    while(!a.NativePassiveIdleLocked()) {
        assert(limit--);
        if(!IsListEmpty(&a.m_NativePassiveHostPending)) {
            auto w=CONTAINING_RECORD(a.m_NativePassiveHostPending.Blink,VIOGPU_NATIVE_PASSIVE_WORK,Link);
            terminal(static_cast<VIOGPU_WDDM_SUBMISSION*>(w->Context));
        }
        pump();
    }
    assert(a.m_NativePassiveHostPendingCount==0 && a.m_NativePassivePendingCount==0);
}
int main() {
    {
        VioGpuDod a;
        const UINT n=VIOGPU_NATIVE_PIPELINE_WINDOW+3;
        for(UINT i=1;i<=n;++i)submit(a,i);
        pump();
        assert(a.issued.size()==VIOGPU_NATIVE_PIPELINE_WINDOW);
        assert(a.m_NativePassiveHostPendingCount==VIOGPU_NATIVE_PIPELINE_WINDOW);
        assert(a.m_NativePassivePendingCount==3);
        finish(a);
        assert(a.m_NativeSubmitPerf.Accepted==n && a.m_NativeSubmitPerf.Retired==n);
        assert(a.m_NativeSubmitPerf.RenderBytes==n*128ULL);
        assert(a.m_NativeSubmitPerf.RenderReferences==n*3ULL);
        assert(a.m_NativeSubmitPerf.HostPendingPeak==VIOGPU_NATIVE_PIPELINE_WINDOW);
        auto reports=reportCount;a.ReportNativeSubmitPerf();a.ReportNativeSubmitPerf();assert(reportCount==reports+1);
    }
    {
        VioGpuDod a;auto first=submit(a,1);submit(a,2,false);submit(a,3);pump();
        assert(a.issued.size()==1 && a.barriers.empty());
        terminal(first);pump();assert(a.barriers==std::vector<UINT>{2});assert(a.issued.size()==2);finish(a);
    }
    {
        VioGpuDod a;submit(a,1);auto blocked=submit(a,2,false);submit(a,3);pump();
        assert(a.CancelNativePassiveWork(&blocked->Work)==VioGpuNativePassiveWorkRemoved);
        NativeRenderDispatchCancelled(blocked);pump();
        assert(a.issued.size()==(VIOGPU_NATIVE_PIPELINE_WINDOW==1?1U:2U));finish(a);
        assert(a.m_NativeSubmitPerf.CancelledQueued==1);
    }
    {
        VioGpuDod a;a.fast=true;
        for(UINT i=1;i<=512;++i)submit(a,i);
        pump();assert(a.issued.size()==512 && live.empty());assert(a.m_NativeSubmitPerf.Retired==512);
    }
    {
        VioGpuDod a;a.failQueue=true;submit(a,1);pump();
        assert(a.faults==1 && live.empty());assert(a.m_NativeSubmitPerf.Retired==1);
    }
    {
        VioGpuDod a;for(UINT i=1;i<=VIOGPU_NATIVE_PIPELINE_WINDOW+2;++i)submit(a,i);pump();
        a.reset=true;a.CloseNativePassiveQueue();
        assert(a.m_NativeSubmitPerf.CancelledQueued==2 && !a.NativePassiveIdleLocked());
        for(auto p:live)assert(p->CancelRequested==1);
        finish(a);assert(a.issued.size()==VIOGPU_NATIVE_PIPELINE_WINDOW);
    }
    for(UINT seed=1;seed<=30;++seed) {
        VioGpuDod a;std::mt19937 rng(seed);
        for(UINT i=1;i<=200;++i) {
            submit(a,i,rng()%7!=0);
            if(rng()%3==0)pump();
            if(rng()%3==0&&!IsListEmpty(&a.m_NativePassiveHostPending)) {
                auto w=CONTAINING_RECORD(a.m_NativePassiveHostPending.Flink,VIOGPU_NATIVE_PASSIVE_WORK,Link);
                terminal(static_cast<VIOGPU_WDDM_SUBMISSION*>(w->Context));
            }
        }
        finish(a);assert(a.m_NativeSubmitPerf.Accepted==200);assert(a.m_NativeSubmitPerf.Retired==200);
        assert(a.m_NativeSubmitPerf.HostPendingPeak<=VIOGPU_NATIVE_PIPELINE_WINDOW);
    }
    assert(tasks.empty() && live.empty());
    std::printf("PASS production pipeline window=%u: bounds, out-of-order retirement, barriers, cancellation, fast callback, enqueue failure, reset, 30 randomized schedules\n",VIOGPU_NATIVE_PIPELINE_WINDOW);
}
