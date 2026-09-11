/* Production packet builders and response classification, with a fault-injected
 * queue boundary. The wire oracle below uses fixed protocol offsets/values,
 * not the driver's field names. Kernel wait/reset implementations have their
 * own production fixtures; this test checks their returned ownership contract. */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using UINT = uint32_t;
using ULONG = uint32_t;
using LONG = int32_t;
using UCHAR = uint8_t;
using CHAR = int8_t;
using ULONGLONG = uint64_t;
using SIZE_T = size_t;
using BOOLEAN = bool;
using PBOOLEAN = BOOLEAN *;
#define TRUE         true
#define FALSE        false
#define MAXULONGLONG UINT64_MAX
#define PAGED_CODE() ((void)0)
#include "viogpu_primary_scanout.h"

// INSERT_DECLARATIONS

static unsigned checks = 0;
#define CHECK(x)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        ++checks;                                                                                                      \
        if (!(x))                                                                                                      \
        {                                                                                                              \
            std::fprintf(stderr, "CHECK FAILED: line %d: %s\n", __LINE__, #x);                                         \
            std::exit(1);                                                                                              \
        }                                                                                                              \
    } while (0)

struct GPU_VBUFFER
{
    void *buf = nullptr;
    UINT buf_size = 0;
    void *data_buf = nullptr;
    UINT data_size = 0;
    GPU_CTRL_HDR response = {};
    void *resp_buf = &response;
    UINT response_size = 0;
};
using PGPU_VBUFFER = GPU_VBUFFER *;

struct Pool
{
    bool fail = false;
    void *AllocateMemory(SIZE_T size)
    {
        return fail ? nullptr : std::malloc(size);
    }
};

class CtrlQueue
{
  public:
    enum Transport
    {
        Complete,
        QueueRejected,
        Timeout,
        EpochChanged
    } transport = Complete;
    Pool pool;
    Pool *m_pBuf = &pool;
    bool gate = true, failCommand = false, poisoned = false;
    unsigned begins = 0, ends = 0, submits = 0, releases = 0;
    GPU_CTRL_HDR response = {0x1100, 0, 0, 0, 0, {0, 0, 0}};
    UINT responseSize = 24;
    PGPU_VBUFFER retained = nullptr;
    const GPU_MEM_ENTRY *callerEntries = nullptr;
    std::vector<uint8_t> packet, backing;

    CtrlQueue() = default;
    CtrlQueue(const CtrlQueue &) = delete;
    ~CtrlQueue()
    {
        if (retained)
        {
            ReleaseBuffer(retained);
        }
    }

    bool BeginSynchronousRequest()
    {
        if (!gate || poisoned)
        {
            return false;
        }
        ++begins;
        return true;
    }
    void EndSynchronousRequest()
    {
        ++ends;
    }
    void PoisonSynchronousRequests()
    {
        poisoned = true;
    }
    void *AllocCmd(PGPU_VBUFFER *output, SIZE_T size)
    {
        if (failCommand)
        {
            return nullptr;
        }
        CHECK(retained == nullptr);
        retained = new GPU_VBUFFER;
        retained->buf = std::malloc(size);
        CHECK(retained->buf != nullptr);
        std::memset(retained->buf, 0xcd, size); // reserved bytes must be cleared.
        retained->buf_size = static_cast<UINT>(size);
        *output = retained;
        return retained->buf;
    }
    void ReleaseBuffer(PGPU_VBUFFER buffer)
    {
        CHECK(buffer == retained);
        ++releases;
        std::free(buffer->data_buf);
        std::free(buffer->buf);
        delete buffer;
        retained = nullptr;
    }
    bool SubmitSynchronousLocked(PGPU_VBUFFER buffer, PBOOLEAN release, PBOOLEAN submitted)
    {
        ++submits;
        const auto *bytes = static_cast<const uint8_t *>(buffer->buf);
        packet.assign(bytes, bytes + buffer->buf_size);
        if (buffer->data_buf)
        {
            CHECK(buffer->data_buf != callerEntries);
            bytes = static_cast<const uint8_t *>(buffer->data_buf);
            backing.assign(bytes, bytes + buffer->data_size);
        }
        buffer->response = response;
        buffer->response_size = responseSize;
        *submitted = transport != QueueRejected;
        *release = transport == Complete || transport == QueueRejected;
        return transport == Complete;
    }
    VIOGPU_HOST_CONTEXT_RESULT SubmitSynchronousNoDataLocked(PGPU_VBUFFER);
    VIOGPU_HOST_CONTEXT_RESULT CreateGuestBlobSynchronous(UINT, ULONGLONG, const GPU_MEM_ENTRY *, UINT);
    VIOGPU_HOST_CONTEXT_RESULT SetScanoutBlobSynchronous(UINT, UINT, const VIOGPU_PRIMARY_SCANOUT_LAYOUT *);
};

#define RtlZeroMemory(p, n)    std::memset(p, 0, n)
#define RtlCopyMemory(p, q, n) std::memcpy(p, q, n)
// INSERT_PRODUCTION

static void le32(std::vector<uint8_t> &bytes, size_t offset, uint32_t value)
{
    for (unsigned i = 0; i != 4; ++i)
    {
        bytes.at(offset + i) = static_cast<uint8_t>(value >> (i * 8));
    }
}

static void create_packet()
{
    CtrlQueue q;
    // A non-contiguous, >4GiB guest address verifies the high address half.
    GPU_MEM_ENTRY entries[] = {{0x12345678000ULL, 4096, 0}, {0x99887000, 8192, 0}};
    q.callerEntries = entries;
    CHECK(q.CreateGuestBlobSynchronous(7, 8192, entries, 2) == VioGpuHostContextConfirmed);
    std::vector<uint8_t> expected(56, 0);
    le32(expected, 0, 0x10c); // RESOURCE_CREATE_BLOB
    le32(expected, 24, 7);    // resource_id
    le32(expected, 28, 1);    // GUEST memory; ctx/blob_id/flags remain zero
    le32(expected, 36, 2);    // nr_entries
    le32(expected, 48, 8192);
    CHECK(q.packet == expected);
    expected.assign(32, 0);
    le32(expected, 0, 0x45678000);
    le32(expected, 4, 0x123);
    le32(expected, 8, 4096);
    le32(expected, 16, 0x99887000);
    le32(expected, 24, 8192);
    CHECK(q.backing == expected);
    CHECK(q.begins == 1 && q.ends == 1 && q.releases == 1 && !q.retained && !q.poisoned);
}

static void invalid_create_inputs()
{
    struct Case
    {
        UINT id;
        ULONGLONG size;
        GPU_MEM_ENTRY entry;
        UINT count;
    };
    const Case cases[] = {
                                                                                                        {0,
                                                                                                         4096,
                                                                                                         {0x1000,
                                                                                                          4096,
                                                                                                          0},
                                                                                                         1},
                                                                                                        {0x80000000,
                                                                                                         4096,
                                                                                                         {0x1000,
                                                                                                          4096,
                                                                                                          0},
                                                                                                         1},
                                                                                                        {7,
                                                                                                         0,
                                                                                                         {0x1000,
                                                                                                          4096,
                                                                                                          0},
                                                                                                         1},
                                                                                                        {7,
                                                                                                         4096,
                                                                                                         {0x1000,
                                                                                                          4096,
                                                                                                          0},
                                                                                                         0},
                                                                                                        {7,
                                                                                                         4096,
                                                                                                         {0x1000,
                                                                                                          4096,
                                                                                                          0},
                                                                                                         (1U
                                                                                                          << 18) + 1},
                                                                                                        {7,
                                                                                                         4096,
                                                                                                         {0, 4096, 0},
                                                                                                         1},
                                                                                                        {7,
                                                                                                         4096,
                                                                                                         {0x1000, 0, 0},
                                                                                                         1},
                                                                                                        {7,
                                                                                                         4096,
                                                                                                         {0x1000,
                                                                                                          4096,
                                                                                                          1},
                                                                                                         1},
                                                                                                        {7,
                                                                                                         4096,
                                                                                                         {UINT64_MAX - 4094,
                                                                                                          4096,
                                                                                                          0},
                                                                                                         1},
                                                                                                        {7,
                                                                                                         4097,
                                                                                                         {0x1000,
                                                                                                          4096,
                                                                                                          0},
                                                                                                         1},
    };
    for (const auto &c : cases)
    {
        CtrlQueue q;
        CHECK(q.CreateGuestBlobSynchronous(c.id, c.size, &c.entry, c.count) == VioGpuHostContextNotSubmitted);
        CHECK(q.begins == 0 && q.submits == 0 && !q.retained);
    }
    CtrlQueue q;
    CHECK(q.CreateGuestBlobSynchronous(7, 4096, nullptr, 1) == VioGpuHostContextNotSubmitted);
    CHECK(q.begins == 0);
}

static void scanout_packet()
{
    // Padding on each line is deliberate; the crop is independent of pitch.
    const VIOGPU_PRIMARY_SCANOUT_LAYOUT layout = {17, 3, 67, 128, 324};
    CtrlQueue q;
    CHECK(q.SetScanoutBlobSynchronous(2, 7, &layout) == VioGpuHostContextConfirmed);
    std::vector<uint8_t> expected(96, 0);
    le32(expected, 0, 0x10d); // SET_SCANOUT_BLOB, not SET_SCANOUT
    le32(expected, 32, 17);
    le32(expected, 36, 3);
    le32(expected, 40, 2);
    le32(expected, 44, 7);
    le32(expected, 48, 17);
    le32(expected, 52, 3);
    le32(expected, 56, 67);
    le32(expected, 64, 128);
    CHECK(q.packet == expected); // other planes, offsets, crop origin/padding=0
    CHECK(q.backing.empty() && q.begins == 1 && q.ends == 1 && q.releases == 1);
    for (UINT format : {1U, 2U, 67U})
    {
        auto valid = layout;
        valid.Format = format;
        CHECK(q.SetScanoutBlobSynchronous(0, 0x7fffffff, &valid) == VioGpuHostContextConfirmed);
    }
    for (unsigned variant = 0; variant != 8; ++variant)
    {
        CtrlQueue invalid;
        auto bad = layout;
        if (variant == 0)
        {
            bad.Format = 134;
        }
        if (variant == 1)
        {
            bad.BackingSize = 323;
        }
        if (variant == 2)
        {
            bad.Width = 0;
        }
        if (variant == 3)
        {
            bad.Stride = 64;
        }
        CHECK(invalid.SetScanoutBlobSynchronous(variant == 4 ? 16 : 0,
                                                variant == 5   ? 0
                                                : variant == 6 ? 0x80000000
                                                               : 7,
                                                variant == 7 ? nullptr : &bad) == VioGpuHostContextNotSubmitted);
        CHECK(invalid.begins == 0 && invalid.submits == 0);
    }
}

static void allocation_failures()
{
    GPU_MEM_ENTRY entry = {0x1000, 4096, 0};
    for (unsigned variant = 0; variant != 3; ++variant)
    {
        CtrlQueue q;
        q.gate = variant != 0;
        q.failCommand = variant == 1;
        q.pool.fail = variant == 2;
        CHECK(q.CreateGuestBlobSynchronous(7, 4096, &entry, 1) == VioGpuHostContextNotSubmitted);
        CHECK(q.begins == q.ends && q.submits == 0 && !q.retained && !q.poisoned);
        CHECK(q.releases == (variant == 2 ? 1U : 0U));
    }
    for (bool gate : {false, true})
    {
        CtrlQueue q;
        q.gate = gate;
        q.failCommand = true;
        const VIOGPU_PRIMARY_SCANOUT_LAYOUT layout = {1, 1, 1, 4, 4096};
        CHECK(q.SetScanoutBlobSynchronous(0, 7, &layout) == VioGpuHostContextNotSubmitted);
        CHECK(q.begins == q.ends && q.submits == 0 && !q.retained);
    }
}

static void response_ownership()
{
    for (unsigned variant = 0; variant != 19; ++variant)
    {
        CtrlQueue q;
        GPU_MEM_ENTRY entry = {0x1000, 4096, 0};
        q.callerEntries = &entry;
        if (variant < 6)
        {
            q.response.type = 0x1200 + variant;
        }
        if (variant == 6)
        {
            q.response.flags = 1;
        }
        if (variant == 7)
        {
            q.response.fence_id = 1;
        }
        if (variant == 8)
        {
            q.response.ctx_id = 1;
        }
        if (variant == 9)
        {
            q.response.ring_idx = 1;
        }
        if (variant >= 10 && variant <= 12)
        {
            q.response.padding[variant - 10] = 1;
        }
        if (variant == 13)
        {
            q.responseSize = 23;
        }
        if (variant == 14)
        {
            q.responseSize = 25;
        }
        if (variant == 15)
        {
            q.response.type = 0x1206;
        }
        if (variant == 16)
        {
            q.transport = CtrlQueue::Timeout;
        }
        if (variant == 17)
        {
            q.transport = CtrlQueue::EpochChanged;
        }
        if (variant == 18)
        {
            q.transport = CtrlQueue::QueueRejected;
        }
        const auto expected = variant < 6     ? VioGpuHostContextRejected
                              : variant == 18 ? VioGpuHostContextNotSubmitted
                                              : VioGpuHostContextUnknown;
        CHECK(q.CreateGuestBlobSynchronous(7, 4096, &entry, 1) == expected);
        CHECK(q.begins == 1 && q.ends == 1 && q.submits == 1);
        const bool outstanding = variant == 16 || variant == 17;
        CHECK(q.releases == (outstanding ? 0U : 1U));
        CHECK((q.retained != nullptr) == outstanding);
        CHECK(q.poisoned == (expected == VioGpuHostContextUnknown));
        if (outstanding)
        {
            // The caller can release/change its temporary SG list now. The
            // outstanding device descriptor must still have the original bytes.
            entry.addr = 0;
            const auto *owned = static_cast<const GPU_MEM_ENTRY *>(q.retained->data_buf);
            CHECK(owned->addr == 0x1000 && owned->length == 4096);
        }
        if (q.poisoned)
        {
            entry.addr = 0x1000;
            CHECK(q.CreateGuestBlobSynchronous(8, 4096, &entry, 1) == VioGpuHostContextNotSubmitted);
            CHECK(q.begins == 1 && q.ends == 1 && q.submits == 1);
        }
    }
}

int main()
{
    create_packet();
    invalid_create_inputs();
    scanout_packet();
    allocation_failures();
    response_ownership();
    std::printf("Guest scanout packets: %u protocol/ownership checks passed\n", checks);
}
