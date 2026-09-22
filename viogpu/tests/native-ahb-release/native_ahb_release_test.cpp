#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>
#include <algorithm>
using UINT = uint32_t;
using ULONG = uint32_t;
using LONG = int32_t;
using LONG64 = int64_t;
using ULONGLONG = uint64_t;
using UCHAR = uint8_t;
using CHAR = int8_t;
using VOID = void;
using PVOID = void *;
using BOOLEAN = bool;
#include "viogpu_3d_wire.h"
#define _Inout_
#define TRUE true
#define FALSE false
#define RtlZeroMemory(p, n) std::memset(p, 0, n)
#define RtlCopyMemory(p, q, n) std::memcpy(p, q, n)
#define PAGED_CODE() ((void)0)
constexpr ULONGLONG MAXULONGLONG=~ULONGLONG(0);
constexpr int DISPATCH_LEVEL = 2, NonPagedPoolNx = 0, IO_NO_INCREMENT = 0;
static int currentIrql;
int KeGetCurrentIrql() { return currentIrql; }
void *operator new(std::size_t size, int) { return ::operator new(size); }
LONG InterlockedCompareExchange(volatile LONG *p, LONG desired, LONG expected) {
    LONG old = *p; if(old == expected) *p = desired; return old;
}
LONG64 InterlockedCompareExchange64(volatile LONG64 *p, LONG64 desired, LONG64 expected) {
    LONG64 old = *p; if(old == expected) *p = desired; return old;
}
void KeClearEvent(bool *e) { *e=false; }
void KeSetEvent(bool *e, int, bool) { *e=true; }
#pragma pack(push, 1)
// INSERT_WIRE
#pragma pack(pop)
enum VIOGPU_HOST_CONTEXT_RESULT { VioGpuHostContextNotSubmitted, VioGpuHostContextConfirmed,
    VioGpuHostContextRejected, VioGpuHostContextUnknown };
enum VIOGPU_SYNCHRONOUS_STATE { VioGpuSynchronousOffline, VioGpuSynchronousEnabled,
    VioGpuSynchronousQuiescing, VioGpuSynchronousPoisoned };
enum { VioGpuVbufferTerminalUnarmed, VioGpuVbufferTerminalArmed,
    VioGpuVbufferTerminalClaimed, VioGpuVbufferTerminalCompleted };
enum VIOGPU_VBUFFER_TERMINAL_CLAIM { VioGpuVbufferTerminalClaimUnarmed,
    VioGpuVbufferTerminalClaimWon, VioGpuVbufferTerminalClaimLost };
using VIOGPU_NATIVE_AHB_COMPLETION = VOID (*)(PVOID, VIOGPU_HOST_CONTEXT_RESULT, UINT, ULONGLONG);
struct GPU_VBUFFER {
    GPU_NATIVE_AHB_OPERATION command{};
    GPU_NATIVE_AHB_PAGING paging{};
    PVOID data_buf{}; UINT data_size{},resp_size{};
    void *resp_buf{};
    UINT response_size{};
    void (*complete_cb)(void *){}; void *complete_ctx{};
    void (*cancel_cb)(void *){}; void *cancel_ctx{};
    void (*queue_error_cb)(void *){}; void *queue_error_ctx{};
    LONG terminal_callback_state{}; bool terminal_callback_event{}, auto_release{};
};
using PGPU_VBUFFER = GPU_VBUFFER *;
BOOLEAN VioGpuArmVbufferTerminalCallbacks(PGPU_VBUFFER);
VIOGPU_VBUFFER_TERMINAL_CLAIM VioGpuClaimVbufferTerminalCallbacks(PGPU_VBUFFER);
VOID VioGpuDetachVbufferTerminalCallbacks(PGPU_VBUFFER);
VOID VioGpuCompleteVbufferTerminalCallbacks(PGPU_VBUFFER);
struct Pool {
    bool fail{}; int live{};
    void *AllocateMemory(std::size_t size) { if(fail) return nullptr; ++live; return std::malloc(size); }
    void *AllocateMemoryUninitialized(std::size_t size) { return AllocateMemory(size); }
    void FreeMemory(void *p) { assert(p && live > 0); --live; std::free(p); }
};
class CtrlQueue {
public:
    Pool pool; Pool *m_pBuf = &pool;
    LONG64 m_SynchronousEpochState = (1LL << 32) | VioGpuSynchronousEnabled;
    bool full{}, failCommand{}, immediate{};
    bool synchronous{},poisoned{};
    UINT pagingVariant{};
    std::vector<UCHAR> pixels=std::vector<UCHAR>(65536,0x3a);
    UINT liveBuffers{}, freedBuffers{};
    std::vector<PGPU_VBUFFER> pending;
    static bool IsStandard2DResourceId(UINT id) { return id && id < 0x80000000U; }
    bool QueueNativeAhbOperation(UINT, ULONGLONG, BOOLEAN, VIOGPU_NATIVE_AHB_COMPLETION, PVOID);
    static VOID CompleteNativeAhbOperation(PVOID);
    static VOID CancelNativeAhbOperation(PVOID);
    VIOGPU_HOST_CONTEXT_RESULT PageNativeAhbSynchronous(UINT,UINT,ULONGLONG,UINT,UINT,PVOID);
    bool BeginSynchronousRequest() { assert(!synchronous); synchronous=true; return true; }
    void EndSynchronousRequest() { assert(synchronous); synchronous=false; }
    void PoisonSynchronousRequests() { poisoned=true; }
    bool SubmitSynchronousLocked(PGPU_VBUFFER b,BOOLEAN *release,BOOLEAN *submitted) {
        assert(synchronous); *release=true; *submitted=true;
        auto r=static_cast<PGPU_NATIVE_AHB_PAGING>(b->resp_buf); *r=b->paging;
        assert(r->hdr.type==VIRTIO_GPU_CMD_PAGE_NATIVE_AHB);
        r->hdr.type=VIRTIO_GPU_RESP_OK_NATIVE_AHB_PAGING; b->response_size=b->resp_size;
        assert(r->offset+r->length<=pixels.size());
        if(r->operation==VIRTIO_GPU_NATIVE_AHB_PAGE_WRITE) {
            assert(b->data_size==r->length); std::memcpy(pixels.data()+r->offset,b->data_buf,r->length);
        } else if(r->operation==VIRTIO_GPU_NATIVE_AHB_PAGE_READ) {
            assert(!b->data_buf); std::memcpy(r+1,pixels.data()+r->offset,r->length);
        } else {
            assert(!b->data_buf);
            for(UINT i=0;i<r->length;i+=4) std::memcpy(pixels.data()+r->offset+i,&r->pattern,4);
        }
        if(pagingVariant==1) ++r->offset;
        if(pagingVariant==2) b->response_size=48;
        if(pagingVariant==3) { r->hdr.type=VIRTIO_GPU_RESP_ERR_UNSPEC; b->response_size=24; }
        return true;
    }
    PVOID AllocCmdResp(PGPU_VBUFFER *out, int size, PVOID response, int responseSize) {
        assert((size==40 && responseSize==40) || (size==48 && responseSize>=48));
        if(failCommand) return nullptr;
        *out = new GPU_VBUFFER; ++liveBuffers; (*out)->resp_buf = response; (*out)->resp_size=responseSize;
        return size==40?static_cast<PVOID>(&(*out)->command):static_cast<PVOID>(&(*out)->paging);
    }
    void ReleaseBuffer(PGPU_VBUFFER b) {
        assert(liveBuffers); --liveBuffers; ++freedBuffers;
        VioGpuDetachVbufferTerminalCallbacks(b);
        if(b->data_buf) pool.FreeMemory(b->data_buf);
        pool.FreeMemory(b->resp_buf); VioGpuCompleteVbufferTerminalCallbacks(b); delete b;
    }
    int QueueBuffer(PGPU_VBUFFER b) {
        if(full) return -1;
        assert(b->command.hdr.flags==0 && b->command.hdr.fence_id==0 && b->command.hdr.ctx_id==0 &&
               b->command.hdr.ring_idx==0 && b->command.hdr.padding[0]==0 && b->command.hdr.padding[1]==0 &&
               b->command.hdr.padding[2]==0 && b->command.reserved==0);
        assert(b->command.hdr.type==VIRTIO_GPU_CMD_PRESENT_NATIVE_AHB ||
               b->command.hdr.type==VIRTIO_GPU_CMD_WAIT_NATIVE_AHB_RELEASE);
        pending.push_back(b);
        if(immediate) reply(b, accepted(b, b->command.sequence));
        return 0;
    }
    static GPU_NATIVE_AHB_OPERATION accepted(PGPU_VBUFFER b, ULONGLONG sequence) {
        GPU_NATIVE_AHB_OPERATION r{}; r.hdr.type=VIRTIO_GPU_RESP_OK_NATIVE_AHB_OPERATION;
        r.resource_id=b->command.resource_id; r.sequence=sequence; return r;
    }
    void reply(PGPU_VBUFFER b, const GPU_NATIVE_AHB_OPERATION &r, UINT size=40) {
        auto found=std::find(pending.begin(),pending.end(),b); assert(found!=pending.end()); pending.erase(found);
        *static_cast<PGPU_NATIVE_AHB_OPERATION>(b->resp_buf)=r; b->response_size=size;
        assert(VioGpuClaimVbufferTerminalCallbacks(b)==VioGpuVbufferTerminalClaimWon);
        assert(VioGpuClaimVbufferTerminalCallbacks(b)==VioGpuVbufferTerminalClaimLost);
        auto cb=b->complete_cb; auto ctx=b->complete_ctx; VioGpuDetachVbufferTerminalCallbacks(b); cb(ctx);
    }
    void reset() {
        m_SynchronousEpochState += (1LL << 32);
        for(auto b : pending) {
            assert(VioGpuClaimVbufferTerminalCallbacks(b)==VioGpuVbufferTerminalClaimWon);
            assert(VioGpuClaimVbufferTerminalCallbacks(b)==VioGpuVbufferTerminalClaimLost);
            auto cb=b->cancel_cb; auto ctx=b->cancel_ctx; VioGpuDetachVbufferTerminalCallbacks(b); cb(ctx);
            // Cancel must leave storage to reset; exactly one release follows.
            ReleaseBuffer(b);
        }
        pending.clear();
    }
    ~CtrlQueue() { assert(pending.empty() && liveBuffers==0 && pool.live==0); }
};
// INSERT_PRODUCTION
struct Result {
    UINT calls{}, resource{}; ULONGLONG sequence{};
    VIOGPU_HOST_CONTEXT_RESULT result=VioGpuHostContextNotSubmitted;
    static void done(void *p, VIOGPU_HOST_CONTEXT_RESULT r, UINT id, ULONGLONG seq) {
        auto self=static_cast<Result *>(p); ++self->calls; self->resource=id; self->sequence=seq; self->result=r;
    }
};
int main() {
    static_assert(sizeof(GPU_CTRL_HDR)==24 && sizeof(GPU_NATIVE_AHB_OPERATION)==40);
    CtrlQueue q; Result a,b;
    assert(q.QueueNativeAhbOperation(1,41,false,Result::done,&a));
    auto wait=q.pending.back();
    assert(q.QueueNativeAhbOperation(2,0,true,Result::done,&b));
    auto present=q.pending.back();
    q.reply(present,CtrlQueue::accepted(present,42));
    assert(b.calls==1 && b.result==VioGpuHostContextConfirmed && b.sequence==42 && a.calls==0);
    q.reply(wait,CtrlQueue::accepted(wait,41));
    assert(a.calls==1 && a.result==VioGpuHostContextConfirmed && a.sequence==41);

    for(UINT variant=0;variant<15;++variant) {
        Result r; bool isPresent=variant==0;
        assert(q.QueueNativeAhbOperation(3,isPresent?0:77,isPresent,Result::done,&r));
        auto p=q.pending.back(); auto response=CtrlQueue::accepted(p,isPresent?0:77); UINT length=40;
        switch(variant) {
        case 0: break; // PRESENT sequence zero is invalid.
        case 1: ++response.sequence; break;
        case 2: ++response.resource_id; break;
        case 3: ++response.reserved; break;
        case 4: ++response.hdr.flags; break;
        case 5: ++response.hdr.fence_id; break;
        case 6: ++response.hdr.ctx_id; break;
        case 7: ++response.hdr.ring_idx; break;
        case 8: ++response.hdr.padding[0]; break;
        case 9: ++response.hdr.padding[1]; break;
        case 10: ++response.hdr.padding[2]; break;
        case 11: length=24; break;
        case 12: length=41; break;
        case 13: response.hdr.type=VIRTIO_GPU_RESP_OK_NODATA; length=24; break;
        case 14: q.m_SynchronousEpochState += (1LL << 32); break;
        }
        q.reply(p,response,length);
        assert(r.calls==1 && r.result==VioGpuHostContextUnknown);
    }
    Result error;
    assert(q.QueueNativeAhbOperation(3,7,false,Result::done,&error));
    auto p=q.pending.back(); auto response=CtrlQueue::accepted(p,7);
    response.hdr.type=VIRTIO_GPU_RESP_ERR_UNSPEC; q.reply(p,response,24);
    assert(error.calls==1 && error.result==VioGpuHostContextRejected);

    Result cancelled;
    assert(q.QueueNativeAhbOperation(3,7,false,Result::done,&cancelled)); q.reset();
    assert(cancelled.calls==1 && cancelled.result==VioGpuHostContextUnknown);
    Result instant; q.immediate=true;
    assert(q.QueueNativeAhbOperation(3,0,false,Result::done,&instant)); q.immediate=false;
    assert(instant.calls==1 && instant.result==VioGpuHostContextConfirmed && instant.sequence==0);

    Result unqueued;
    q.full=true; assert(!q.QueueNativeAhbOperation(1,0,false,Result::done,&unqueued)); q.full=false;
    q.pool.fail=true; assert(!q.QueueNativeAhbOperation(1,0,false,Result::done,&unqueued)); q.pool.fail=false;
    q.failCommand=true; assert(!q.QueueNativeAhbOperation(1,0,false,Result::done,&unqueued)); q.failCommand=false;
    assert(!q.QueueNativeAhbOperation(0,0,false,Result::done,&unqueued));
    assert(!q.QueueNativeAhbOperation(0x80000000U,0,false,Result::done,&unqueued));
    assert(!q.QueueNativeAhbOperation(1,5,true,Result::done,&unqueued));
    assert(!q.QueueNativeAhbOperation(1,0,false,nullptr,&unqueued));
    currentIrql=3; assert(!q.QueueNativeAhbOperation(1,0,false,Result::done,&unqueued)); currentIrql=0;
    q.m_SynchronousEpochState=VioGpuSynchronousPoisoned;
    assert(!q.QueueNativeAhbOperation(1,0,false,Result::done,&unqueued));
    assert(unqueued.calls==0);

    static_assert(sizeof(GPU_NATIVE_AHB_PAGING)==48);
    std::vector<UCHAR> data(65536),restored(65536);
    for(UINT i=0;i<data.size();++i) data[i]=static_cast<UCHAR>(i*13);
    assert(q.PageNativeAhbSynchronous(1,2,0,data.size(),0,data.data())==VioGpuHostContextConfirmed);
    assert(q.PageNativeAhbSynchronous(1,1,0,data.size(),0,restored.data())==VioGpuHostContextConfirmed);
    assert(data==restored);
    assert(q.PageNativeAhbSynchronous(1,3,0,65536,0x75ab1234,nullptr)==VioGpuHostContextConfirmed);
    assert(q.pixels[0]==0x34 && q.pixels[65535]==0x75);
    for(UINT variant=1;variant<=3;++variant) {
        q.pagingVariant=variant; q.poisoned=false; std::fill(restored.begin(),restored.end(),0);
        const auto expected=variant==3?VioGpuHostContextRejected:VioGpuHostContextUnknown;
        assert(q.PageNativeAhbSynchronous(1,1,0,65536,0,restored.data())==expected);
        assert(q.poisoned==(variant!=3));
        assert(std::all_of(restored.begin(),restored.end(),[](UCHAR c){return c==0;}));
    }
    assert(q.PageNativeAhbSynchronous(1,1,0,65537,0,restored.data())==VioGpuHostContextNotSubmitted);
    assert(q.PageNativeAhbSynchronous(1,3,1,4,0,nullptr)==VioGpuHostContextNotSubmitted);
    assert(q.PageNativeAhbSynchronous(1,1,0,4,1,restored.data())==VioGpuHostContextNotSubmitted);
    assert(q.PageNativeAhbSynchronous(1,1,MAXULONGLONG,4,0,restored.data())==VioGpuHostContextNotSubmitted);
}
