#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>

using SIZE_T = size_t;
using UINT = uint32_t;
using ULONG = uint32_t;
using PVOID = void *;
using PUCHAR = unsigned char *;
using PUINT = UINT *;
using PFN_NUMBER = uint64_t;
using ULONGLONG = uint64_t;
using UCHAR = uint8_t;
using NTSTATUS = int32_t;
using BOOLEAN = bool;
using KIRQL = unsigned;
#ifndef _In_
#define _In_
#endif
#ifndef _In_reads_
#define _In_reads_(x)
#endif
#ifndef _Out_writes_
#define _Out_writes_(x)
#endif
#ifndef _Out_
#define _Out_
#endif
#ifndef _Outptr_result_buffer_
#define _Outptr_result_buffer_(x)
#endif
#define TRUE           true
#define FALSE          false
#define NT_SUCCESS(x)  ((x) >= 0)
#define PAGE_SHIFT     12
#define PAGE_SIZE      4096
#define MAXULONGLONG   UINT64_MAX
#define MAXULONG       UINT32_MAX
#define MAXUINT        UINT32_MAX
#define BYTE_OFFSET(p) (reinterpret_cast<uintptr_t>(p) & (PAGE_SIZE - 1))
#define DbgPrint(...)
constexpr KIRQL DISPATCH_LEVEL = 2;
static KIRQL testIrql;
[[maybe_unused]] static KIRQL KeGetCurrentIrql()
{
    return testIrql;
}
static const int VIOGPU_QUEUE_ERROR = -1;
using std::min;
struct PHYSICAL_ADDRESS
{
    int64_t QuadPart;
};
struct VirtIOBufferDescriptor
{
    PHYSICAL_ADDRESS physAddr;
    ULONG length;
};
struct GPU_VBUFFER
{
    char *buf;
    int size;
    void *data_buf;
    ULONG data_size;
    char *resp_buf;
    int resp_size;
    UINT response_size;
};
using PGPU_VBUFFER = GPU_VBUFFER *;
class CtrlQueue
{
  public:
    void *m_pBuf = this;
    UINT queueCapacity = 2048, kicks = 0, calls = 0, outputCount = 0, inputCount = 0;
    std::vector<VirtIOBufferDescriptor> retained;
    void Lock(KIRQL *saved)
    {
        *saved = testIrql;
    }
    void Unlock(KIRQL)
    {
    }
    void Kick()
    {
        ++kicks;
    }
    int AddBuf(VirtIOBufferDescriptor *sg, UINT out, UINT in, void *, void *, uint64_t)
    {
        ++calls;
        if (out + in > queueCapacity)
        {
            return -28;
        }
        retained.assign(sg, sg + out + in);
        outputCount = out;
        inputCount = in;
        return 0;
    }
    int QueueBuffer(PGPU_VBUFFER buf);
};
static bool MmIsAddressValid(PVOID p)
{
    return p != nullptr;
}
static PHYSICAL_ADDRESS MmGetPhysicalAddress(PVOID p)
{
    uintptr_t address = reinterpret_cast<uintptr_t>(p);
    return {static_cast<int64_t>((address & ~(static_cast<uintptr_t>(PAGE_SIZE) - 1)) * 2 + BYTE_OFFSET(p))};
}
constexpr NTSTATUS STATUS_SUCCESS = 0, STATUS_INVALID_PARAMETER = -1;
constexpr NTSTATUS STATUS_INSUFFICIENT_RESOURCES = -2, STATUS_INVALID_DEVICE_STATE = -3, STATUS_NO_MEMORY = -4;
constexpr unsigned NonPagedPoolNx = 0, VioGpuWddmAperturePageMapped = 1, VioGpuWddmAperturePageDummy = 2;
struct GPU_MEM_ENTRY
{
    uint64_t addr;
    uint32_t length, padding;
};
struct VIOGPU_WDDM_ALLOCATION
{
    PFN_NUMBER *AperturePfns;
    UCHAR *ApertureMappedPages;
    SIZE_T AperturePageCount, ApertureMappedPageCount;
};
static unsigned outstanding, checks, failures;
static bool failPool;
static void *ExAllocatePoolUninitialized(unsigned, SIZE_T bytes, unsigned)
{
    if (failPool)
    {
        return nullptr;
    }
    void *result = std::malloc(bytes);
    if (result)
    {
        ++outstanding;
    }
    return result;
}
static void ExFreePoolWithTag(void *p, unsigned)
{
    if (p)
    {
        --outstanding;
    }
    std::free(p);
}

// INSERT_PRODUCTION

static void check(bool good, const char *label)
{
    ++checks;
    if (!good)
    {
        ++failures;
        std::printf("FAIL: %s\n", label);
    }
}
static void allocation(SIZE_T pages, SIZE_T contiguousRun, bool success)
{
    std::vector<PFN_NUMBER> pfns(pages);
    std::vector<UCHAR> states(pages, VioGpuWddmAperturePageMapped);
    for (SIZE_T i = 0; i < pages; ++i)
    {
        pfns[i] = 0x10000 + i + i / contiguousRun;
    }
    VIOGPU_WDDM_ALLOCATION value = {pfns.data(), states.data(), pages, pages};
    GPU_MEM_ENTRY *entries = nullptr;
    UINT count = 0;
    NTSTATUS result = AllocateApertureBackingEntries(&value, &entries, &count);
    std::printf("pages=%zu runs=%zu status=%d entries=%u\n", pages, contiguousRun, result, count);
    check(success ? result == STATUS_SUCCESS : result == STATUS_INSUFFICIENT_RESOURCES,
          "fragmented backing must fit the supported request");
    if (NT_SUCCESS(result))
    {
        SIZE_T page = 0;
        bool exact = entries && count && count <= VIOGPU_MAX_BACKING_ENTRIES;
        for (UINT i = 0; i < count; ++i)
        {
            exact &= entries[i].padding == 0 && entries[i].length != 0 && entries[i].length % PAGE_SIZE == 0;
            for (uint64_t offset = 0; offset < entries[i].length; offset += PAGE_SIZE)
            {
                if (page >= pages || entries[i].addr + offset != pfns[page] * PAGE_SIZE)
                {
                    exact = false;
                }
                ++page;
            }
        }
        check(exact && page == pages, "wire entries preserve every PFN in order, exactly once");
        ExFreePoolWithTag(entries, 0);
    }
    else
    {
        check(entries == nullptr && count == 0, "failure publishes no partial backing");
    }
    check(outstanding == 0, "temporary backing entries are released");
}
static void controlPacket()
{
    const ULONG bytes = static_cast<ULONG>(VIOGPU_MAX_BACKING_ENTRIES * sizeof(GPU_MEM_ENTRY));
    std::vector<UCHAR> storage(static_cast<SIZE_T>(bytes) + PAGE_SIZE * 2);
    uintptr_t aligned = (reinterpret_cast<uintptr_t>(storage.data()) + PAGE_SIZE - 1) &
                        ~(static_cast<uintptr_t>(PAGE_SIZE) - 1);
    PVOID unaligned = reinterpret_cast<PVOID>(aligned + PAGE_SIZE - 1);
    VirtIOBufferDescriptor sg[VIOGPU_CONTROL_SG_CAPACITY + 1] = {};
    sg[VIOGPU_CONTROL_SG_CAPACITY].length = 0xabcdef;
    UINT used = BuildSGElements(sg, VIOGPU_CONTROL_SG_CAPACITY, unaligned, 96);
    check(used == 2, "unaligned command consumes two descriptors");
    UINT payload = BuildSGElements(sg + used, VIOGPU_CONTROL_SG_CAPACITY - used, unaligned, bytes);
    check(payload != 0, "largest backing list fits the production SG builder");
    used += payload;
    UINT response = BuildSGElements(sg + used, VIOGPU_CONTROL_SG_CAPACITY - used, unaligned, 24);
    check(response == 2 && used + response <= VIOGPU_CONTROL_SG_CAPACITY,
          "largest backing packet reserves the complete response");
    uint64_t observed = 0;
    for (UINT i = 2; i < used; ++i)
    {
        observed += sg[i].length;
    }
    check(observed == bytes, "DMA descriptors cover the entire entry payload");
    check(sg[VIOGPU_CONTROL_SG_CAPACITY].length == 0xabcdef, "control descriptor array stays bounded");
    check(VIOGPU_MAX_BACKING_ENTRIES <= 262144, "backing list stays within paired Host udmabuf limit");
}
static void queuePacket(SIZE_T pages)
{
    const ULONG bytes = static_cast<ULONG>(pages * sizeof(GPU_MEM_ENTRY));
    std::vector<UCHAR> storage(static_cast<SIZE_T>(bytes) + 3 * PAGE_SIZE);
    uintptr_t aligned = (reinterpret_cast<uintptr_t>(storage.data()) + PAGE_SIZE - 1) &
                        ~(static_cast<uintptr_t>(PAGE_SIZE) - 1);
    char *unaligned = reinterpret_cast<char *>(aligned + PAGE_SIZE - 1);
    GPU_VBUFFER packet = {unaligned, 96, unaligned, bytes, unaligned, 24, 123};
    CtrlQueue queue;
    int result = queue.QueueBuffer(&packet);
    check(result == 0 && queue.kicks == 1 && packet.response_size == 0,
          "complete large packet reaches the real QueueBuffer enqueue path");
    uint64_t outputBytes = 0, inputBytes = 0;
    for (UINT i = 0; i < queue.retained.size(); ++i)
    {
        (i < queue.outputCount ? outputBytes : inputBytes) += queue.retained[i].length;
    }
    check(result == 0 && outputBytes == bytes + 96 && inputBytes == 24,
          "enqueued descriptors cover command, complete payload and writable response");
    check(outstanding == 0, "successful enqueue releases only the temporary SG array");
    if (pages < 65536)
    {
        return;
    }

    queue.queueCapacity = 16;
    UINT previousKicks = queue.kicks;
    result = queue.QueueBuffer(&packet);
    check(result < 0 && queue.kicks == previousKicks && outstanding == 0,
          "full or smaller negotiated queue refuses without kick or SG leak");
    failPool = true;
    UINT previousCalls = queue.calls;
    result = queue.QueueBuffer(&packet);
    check(result < 0 && queue.calls == previousCalls && outstanding == 0,
          "SG allocation failure publishes no descriptor and leaks nothing");
    failPool = false;
    testIrql = DISPATCH_LEVEL + 1;
    result = queue.QueueBuffer(&packet);
    check(result < 0 && queue.calls == previousCalls && outstanding == 0,
          "large packet never allocates from pool above DISPATCH_LEVEL");
    testIrql = 0;
    packet.data_size = UINT32_MAX;
    result = queue.QueueBuffer(&packet);
    check(result < 0 && queue.calls == previousCalls && outstanding == 0,
          "oversized packet is refused before any enqueue or wrapped allocation");
}
int main(int argc, char **)
{
    bool old = argc > 1;
    allocation(16384, 1, true);
    allocation(16385, 1, !old);
    allocation(32768, 1, !old);
    allocation(32768, 4, true);
    allocation(140070, 1, !old);
    allocation(262144, 1, !old);
    allocation(VIOGPU_MAX_BACKING_ENTRIES, 1, true);
    allocation(static_cast<SIZE_T>(VIOGPU_MAX_BACKING_ENTRIES) + 1, 1, false);
    controlPacket();
    queuePacket(16);
    queuePacket(32768);
    if (!old)
    {
        queuePacket(140070);
        queuePacket(262144);
    }
    PFN_NUMBER pfns[] = {0x100, 0x101, 0x200, 0x202};
    GPU_MEM_ENTRY entries[3] = {};
    entries[2] = {0xabcdef, 0x1234, 0x5678};
    UINT count = 0;
    check(BuildPfnEntries(pfns, 4, entries, 2, &count) == STATUS_INSUFFICIENT_RESOURCES,
          "packing refuses insufficient output capacity");
    check(entries[2].addr == 0xabcdef && entries[2].length == 0x1234 && entries[2].padding == 0x5678,
          "packing does not overwrite the output boundary");
    pfns[0] = (UINT64_MAX >> PAGE_SHIFT) + 1;
    check(BuildPfnEntries(pfns, 1, entries, 2, &count) == STATUS_INVALID_PARAMETER, "reject PFN shift overflow");
    pfns[0] = 0x100;
    UCHAR states[] = {VioGpuWddmAperturePageMapped};
    VIOGPU_WDDM_ALLOCATION value = {pfns, states, 1, 1};
    GPU_MEM_ENTRY *output = nullptr;
    failPool = true;
    check(AllocateApertureBackingEntries(&value, &output, &count) == STATUS_NO_MEMORY && output == nullptr,
          "pool allocation failure stays distinguishable from capacity");
    failPool = false;
    value.ApertureMappedPageCount = 0;
    check(AllocateApertureBackingEntries(&value, &output, &count) == STATUS_INVALID_DEVICE_STATE,
          "incomplete aperture cannot publish host backing");
    check(outstanding == 0, "no leaked entry arrays");
    std::printf("backing entries: %u/%u PASS%s\n",
                checks - failures,
                checks,
                old ? " (old limit characterization)" : "");
    return failures ? 1 : 0;
}
