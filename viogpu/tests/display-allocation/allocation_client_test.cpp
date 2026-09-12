#include "../../common/display_allocation_client.h"
#include "../../shared/dvsa_protocol.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#define SAME_TYPE(g, h) static_assert(sizeof(g) == sizeof(h) && alignof(g) == alignof(h), #g)
#define SAME_FIELD(g, h, f) static_assert(offsetof(g, f) == offsetof(h, f) && \
    sizeof(((g *)0)->f) == sizeof(((h *)0)->f), #f)
SAME_TYPE(VIOGPU_DVSA_ALLOCATE_REQUEST, dvsa_allocate);
SAME_TYPE(VIOGPU_DVSA_ALLOCATED, dvsa_allocated);
SAME_TYPE(VIOGPU_DVSA_DESCRIPTION, dvsa_allocation_description);
SAME_TYPE(VIOGPU_DVSA_ACK, dvsa_ack_mapping);
SAME_FIELD(VIOGPU_DVSA_ALLOCATE_REQUEST, dvsa_allocate, query);
SAME_FIELD(VIOGPU_DVSA_ALLOCATE_REQUEST, dvsa_allocate, width);
SAME_FIELD(VIOGPU_DVSA_ALLOCATE_REQUEST, dvsa_allocate, height);
SAME_FIELD(VIOGPU_DVSA_ALLOCATE_REQUEST, dvsa_allocate, fourcc);
SAME_FIELD(VIOGPU_DVSA_ALLOCATE_REQUEST, dvsa_allocate, flags);
SAME_FIELD(VIOGPU_DVSA_ALLOCATED, dvsa_allocated, query);
SAME_FIELD(VIOGPU_DVSA_ALLOCATED, dvsa_allocated, description);
SAME_FIELD(VIOGPU_DVSA_DESCRIPTION, dvsa_allocation_description, buffer_id);
SAME_FIELD(VIOGPU_DVSA_DESCRIPTION, dvsa_allocation_description, allocation_size);
SAME_FIELD(VIOGPU_DVSA_DESCRIPTION, dvsa_allocation_description, modifier);
SAME_FIELD(VIOGPU_DVSA_DESCRIPTION, dvsa_allocation_description, plane_offset);
SAME_FIELD(VIOGPU_DVSA_DESCRIPTION, dvsa_allocation_description, plane_stride);
SAME_FIELD(VIOGPU_DVSA_DESCRIPTION, dvsa_allocation_description, width);
SAME_FIELD(VIOGPU_DVSA_DESCRIPTION, dvsa_allocation_description, height);
SAME_FIELD(VIOGPU_DVSA_DESCRIPTION, dvsa_allocation_description, fourcc);
SAME_FIELD(VIOGPU_DVSA_DESCRIPTION, dvsa_allocation_description, plane_count);
SAME_FIELD(VIOGPU_DVSA_DESCRIPTION, dvsa_allocation_description, layout_flags);
SAME_FIELD(VIOGPU_DVSA_DESCRIPTION, dvsa_allocation_description, reserved);
SAME_FIELD(VIOGPU_DVSA_ACK, dvsa_ack_mapping, query);
SAME_FIELD(VIOGPU_DVSA_ACK, dvsa_ack_mapping, bar_offset);
SAME_FIELD(VIOGPU_DVSA_ACK, dvsa_ack_mapping, mapped_size);
static_assert(VIOGPU_DVSA_ALLOCATE == DVSA_CMD_ALLOCATE && VIOGPU_DVSA_DESTROY == DVSA_CMD_DESTROY &&
    VIOGPU_DVSA_ACK_MAPPING == DVSA_CMD_ACK_MAPPING && VIOGPU_DVSA_ALLOCATED_REPLY == DVSA_RESP_ALLOCATE &&
    VIOGPU_DVSA_ABGR8888 == DVSA_FORMAT_ABGR8888 && VIOGPU_DVSA_LINEAR == DVSA_LAYOUT_VERIFIED_LINEAR, "canonical constants");

static unsigned checks;
static void require(bool value, const char *name)
{
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL %s\n", name); std::exit(1); }
}

static VIOGPU_DVSA_DISCOVERY discovery()
{
    VIOGPU_DVSA_DISCOVERY d = {};
    d.query.hdr.type = DVSA_RESP_DISCOVER; d.query.magic = DVSA_MAGIC;
    d.query.version = DVSA_VERSION; d.query.size = 128; d.query.surface_generation = 17;
    d.feature_bits = DVSA_FEATURE_ALLOCATION_MAPPING;
    d.unavailable_reasons = DVSA_UNAVAILABLE_PRODUCER_BRIDGE | DVSA_UNAVAILABLE_CONSUMER_BRIDGE;
    d.bar_size = 128ULL << 20; d.reserved_prefix = 8ULL << 20;
    d.dynamic_capacity = d.bar_size - d.reserved_prefix; d.mapping_alignment = 16384;
    d.min_allocations = d.max_allocations = 3; d.triple_bytes_lower_bound = 69500928;
    return d;
}

// Deterministic transport/host fault boundary. The client is included directly
// from production; these controls are not claimed as native device fixtures.
// Responses use canonical host structs and actual unfenced encoder semantics.
struct Host
{
    enum Failure { None, NotSubmitted, LostReply, ErrorWithOwner, BadDescription, BadMapCache };
    struct Owner { dvsa_allocated reply; unsigned state; uint64_t offset; };
    std::map<unsigned, Owner> owners;
    std::vector<unsigned> operations;
    unsigned calls = 0, fail_call = 0, context = 0, cache = 1;
    Failure failure = None;
    bool poisoned = false;
    uint64_t token = 100, generation = 17;

    template<class T> static T read(const void *bytes, unsigned size)
    {
        require(size == sizeof(T), "exact production command size");
        T value = {}; std::memcpy(&value, bytes, sizeof(value)); return value;
    }
    template<class T> static void reply(const T &value, void *bytes, unsigned *size)
    {
        std::memcpy(bytes, &value, sizeof(value)); *size = sizeof(value);
    }
    static void mutation(const dvsa_header &h, unsigned type, unsigned size, bool allocate)
    {
        require(h.hdr.type == type && h.hdr.flags == 0 && h.hdr.fence_id == 0 && h.hdr.context_id != 0 &&
            h.hdr.ring_idx == 0 && h.hdr.padding[0] == 0 && h.hdr.padding[1] == 0 && h.hdr.padding[2] == 0 &&
            h.magic == DVSA_MAGIC && h.version == 1 && h.size == size && h.scanout_id == 0 &&
            h.surface_generation != 0 && h.resource_id != 0 && h.reserved == 0 && (h.token == 0) == allocate,
            "canonical host mutation admission");
    }
    VIOGPU_DVSA_EXCHANGE_RESULT ExchangeDisplayAllocationCommand(const void *command, unsigned size,
                                                                void *bytes, unsigned *used)
    {
        *used = 0;
        if (poisoned) return VioGpuDvsaNotSubmitted;
        ++calls;
        dvsa_ctrl_header header = {}; std::memcpy(&header, command, sizeof(header));
        operations.push_back(header.type);
        if (calls == fail_call && failure == NotSubmitted) return VioGpuDvsaNotSubmitted;
        dvsa_ctrl_header status = {}; status.type = 0x1100;
        switch (header.type)
        {
        case 0x200: {
            const auto c = read<VIOGPU_DVSA_CREATE_CONTEXT>(command, size);
            require(context == 0 && c.context_init == 6 && c.hdr.context_id != 0 &&
                    c.name_length == 0 && c.hdr.flags == 0, "dedicated native context created once");
            context = c.hdr.context_id; break;
        }
        case DVSA_CMD_ALLOCATE: {
            const auto a = read<dvsa_allocate>(command, size);
            mutation(a.query, DVSA_CMD_ALLOCATE, 80, true);
            require(a.query.hdr.context_id == context && a.query.surface_generation == generation &&
                a.width == 3040 && a.height == 1904 && a.fourcc == DVSA_FORMAT_ABGR8888 && a.flags == 0 &&
                owners.count(a.query.resource_id) == 0 && owners.size() < 3, "host allocation identity and dimensions");
            Owner owner = {};
            owner.reply.query = a.query;
            owner.reply.query.hdr = {}; // Actual unfenced frontend envelope.
            owner.reply.query.hdr.type = DVSA_RESP_ALLOCATE;
            owner.reply.query.size = 128; owner.reply.query.token = ++token;
            auto &d = owner.reply.description;
            d.buffer_id = token + 1000; d.plane_offset = 256; d.plane_stride = 12288;
            d.width = a.width; d.height = a.height; d.fourcc = DVSA_FORMAT_ABGR8888;
            d.plane_count = 1; d.layout_flags = DVSA_LAYOUT_VERIFIED_LINEAR;
            d.allocation_size = ((uint64_t(d.height) * d.plane_stride + d.plane_offset + 16383) & ~uint64_t(16383)) + owners.size() * 16384;
            owners.emplace(a.query.resource_id, owner);
            if (calls == fail_call && failure == BadDescription) owner.reply.description.plane_offset = UINT64_MAX;
            reply(owner.reply, bytes, used); break;
        }
        case 0x208: {
            const auto m = read<VIOGPU_DVSA_MAP>(command, size);
            auto &o = owners.at(m.resource_id);
            require(m.hdr.context_id == 0 && m.padding == 0 && o.state == 0 && m.offset >= 8ULL << 20 &&
                    m.offset % 16384 == 0 && m.offset <= (128ULL << 20) - o.reply.description.allocation_size,
                    "independent aligned mapping within PCI suffix");
            for (const auto &entry : owners) if (entry.second.state == 1 || entry.second.state == 2)
                require(m.offset >= entry.second.offset + entry.second.reply.description.allocation_size ||
                        entry.second.offset >= m.offset + o.reply.description.allocation_size, "nonoverlapping actual allocation ranges");
            o.offset = m.offset; o.state = 1;
            VIOGPU_DVSA_MAP_REPLY mapped = {}; mapped.hdr.type = 0x1106; mapped.map_info = cache;
            if (calls == fail_call && failure == BadMapCache) mapped.map_info = 4;
            reply(mapped, bytes, used); break;
        }
        case DVSA_CMD_ACK_MAPPING: {
            const auto a = read<dvsa_ack_mapping>(command, size);
            mutation(a.query, DVSA_CMD_ACK_MAPPING, 80, false);
            auto &o = owners.at(a.query.resource_id);
            require(a.query.hdr.context_id == context && a.query.token == o.reply.query.token &&
                a.query.surface_generation == generation && o.state == 1 && a.bar_offset == o.offset &&
                a.mapped_size == o.reply.description.allocation_size, "ACK exact owner and actual mapped range");
            o.state = 2; break;
        }
        case 0x209: {
            const auto u = read<VIOGPU_DVSA_UNMAP>(command, size);
            auto &o = owners.at(u.resource_id);
            require(u.hdr.context_id == 0 && u.padding == 0, "unmap canonical resource identity");
            if (o.state != 1 && o.state != 2) status.type = 0x1205;
            else o.state = 3;
            break;
        }
        case DVSA_CMD_DESTROY: {
            const auto d = read<dvsa_header>(command, size);
            mutation(d, DVSA_CMD_DESTROY, 64, false);
            auto it = owners.find(d.resource_id);
            if (it == owners.end()) { status.type = 0x1203; break; }
            const auto &o = it->second;
            require(d.hdr.context_id == context && d.token == o.reply.query.token &&
                d.surface_generation == o.reply.query.surface_generation && (o.state == 0 || o.state == 3),
                "destroy binds old identity and confirmed unmap");
            owners.erase(it); break;
        }
        case 0x201: {
            const auto d = read<dvsa_ctrl_header>(command, size);
            require(owners.empty() && d.context_id == context, "dedicated context retained until every owner retired");
            context = 0; break;
        }
        default: require(false, "no rendering, presentation, CPU writes or foreign commands");
        }
        if (*used == 0) reply(status, bytes, used);
        if (calls == fail_call && failure == ErrorWithOwner)
        {
            status.type = 0x1200; reply(status, bytes, used);
        }
        if (calls == fail_call && failure == LostReply) { poisoned = true; return VioGpuDvsaUncertain; }
        return VioGpuDvsaCompleted;
    }
};

static bool prepare(VioGpuDisplayAllocationPool &pool, Host &host)
{
    const VIOGPU_DVSA_U32 resources[3] = {91, 92, 93};
    const auto d = discovery();
    return pool.Prepare(host, d, d.bar_size, 3040, 1904, 81, resources, 7);
}

int main()
{
    for (unsigned cache : {1U, 2U})
    {
        Host host; host.cache = cache; VioGpuDisplayAllocationPool pool = {};
        require(prepare(pool, host) && pool.Ready() && host.calls == 10, "actual client creates/maps/ACKs three owners");
        require(pool.ControlLimit(128ULL << 20) == 8ULL << 20, "legacy control slots exclude retained dynamic suffix");
        require(pool.ControlLimit(4ULL << 20) == 4ULL << 20, "smaller PCI observation never enlarged");
        const unsigned before = host.calls;
        require(!prepare(pool, host) && before == host.calls, "busy pool cannot replace identities");
        for (unsigned i = 0; i < 3; ++i)
        {
            require(pool.owners[i].identity.hdr.context_id == 81 && pool.owners[i].map_info == cache,
                    "owner restores local context and preserves cache receipt");
            if (i) require(pool.owners[i].bar_offset == pool.owners[i-1].bar_offset +
                           pool.owners[i-1].description.allocation_size, "range follows authoritative padded size");
        }
        host.generation = 18; // Surface replacement must still allow old cleanup.
        require(pool.Release(host, 7) && !pool.Busy() && host.calls == 17 && host.owners.empty() && host.context == 0,
                "old generation cleanup completes UNMAP/DESTROY before context release");
    }
    for (unsigned call = 1; call <= 10; ++call)
    {
        Host host; host.fail_call = call; host.failure = Host::NotSubmitted;
        VioGpuDisplayAllocationPool pool = {};
        require(!prepare(pool, host) && !pool.Busy() && host.owners.empty() && host.context == 0,
                "every non-submission rolls back only confirmed prior owners");
    }
    for (unsigned call = 1; call <= 10; ++call)
    {
        Host host; host.fail_call = call; host.failure = Host::LostReply;
        VioGpuDisplayAllocationPool pool = {};
        require(!prepare(pool, host) && pool.Busy() && !pool.Ready(), "every lost create/allocate/map/ACK reply retains owner");
        pool.InvalidateTransport(); host.poisoned = false;
        const auto retained = pool; const unsigned before = host.calls;
        require(!pool.Release(host, 8) && !pool.Release(host, 7) && host.calls == before &&
                std::memcmp(&retained, &pool, sizeof(pool)) == 0, "reset never retires owner or submits stale cleanup");
        require(pool.ControlLimit(128ULL << 20) == 8ULL << 20, "reset keeps external range reserved");
    }
    for (unsigned call : {2U, 5U, 8U})
    {
        for (auto failure : {Host::ErrorWithOwner, Host::BadDescription})
        {
            Host host; host.fail_call = call; host.failure = failure;
            VioGpuDisplayAllocationPool pool = {};
            require(!prepare(pool, host) && pool.Busy() && host.owners.size() == 1 && host.context == 81,
                    "allocation error/malformed reply can retain an owner without a trustworthy token");
            const unsigned before = host.calls;
            require(!pool.Release(host, 7) && host.calls == before, "unknown allocation never guessed destroyed");
        }
    }
    for (unsigned call = 11; call <= 17; ++call)
    {
        Host host; VioGpuDisplayAllocationPool pool = {};
        require(prepare(pool, host), "cleanup fault setup");
        host.fail_call = call; host.failure = Host::NotSubmitted;
        require(!pool.Release(host, 7) && pool.Busy(), "unmap/destroy/context non-submission retains identity");
        require(pool.Release(host, 7) && !pool.Busy() && host.owners.empty(), "same-epoch exact cleanup retry succeeds");
    }
    for (unsigned call = 11; call <= 17; ++call)
    {
        Host host; VioGpuDisplayAllocationPool pool = {};
        require(prepare(pool, host), "lost cleanup setup");
        host.fail_call = call; host.failure = Host::LostReply;
        require(!pool.Release(host, 7) && pool.Busy(), "lost unmap/destroy/context receipt retains ownership");
        pool.InvalidateTransport(); const auto before = host.calls;
        require(!pool.Release(host, 8) && before == host.calls, "lost cleanup not erased by reset");
    }
    for (unsigned call : {3U, 6U, 9U})
    {
        Host host; host.fail_call = call; host.failure = Host::BadMapCache;
        VioGpuDisplayAllocationPool pool = {};
        require(!prepare(pool, host) && !pool.Busy() && host.owners.empty(), "unsupported cache never ACKed and checked cleanup succeeds");
    }
    {
        Host host; VioGpuDisplayAllocationPool pool = {};
        auto d = discovery(); const VIOGPU_DVSA_U32 ids[3] = {91, 92, 93};
        for (uint64_t feature : {UINT64_C(0), UINT64_C(1), UINT64_C(3), UINT64_C(4)})
        {
            d.feature_bits = feature;
            require(!pool.Prepare(host, d, d.bar_size, 3040, 1904, 81, ids, 7) && host.calls == 0, "no allocation without bit1 only");
        }
        d = discovery();
        const VIOGPU_DVSA_U32 repeated[3] = {91, 91, 93};
        require(!pool.Prepare(host, d, d.bar_size, 3040, 1904, 81, repeated, 7) && host.calls == 0, "resource IDs cannot alias");
        require(!pool.Prepare(host, d, d.bar_size, 0, 1904, 81, ids, 7) && host.calls == 0, "invalid dimensions before ownership");
        require(!pool.Prepare(host, d, d.bar_size - 4096, 3040, 1904, 81, ids, 7) && host.calls == 0, "independent PCI bound required");
    }
    {
        Host host; VioGpuDisplayAllocationPool pool = {};
        require(prepare(pool, host), "allocation parser setup");
        auto canonical = host.owners.at(91).reply;
        // Use an empty owner table but preserve independently validated input.
        for (auto &owner : pool.owners) owner = VIOGPU_DVSA_OWNER();
        VIOGPU_DVSA_ALLOCATED output = {};
        require(pool.DecodeAllocation(&canonical, 128, 91, output), "canonical unfenced allocation reply accepted");
        for (unsigned size = 0; size < 128; ++size)
        {
            std::vector<unsigned char> prefix(size);
            if (size) std::memcpy(prefix.data(), &canonical, size);
            std::memset(&output, 0xa5, sizeof(output)); const auto before = output;
            require(!pool.DecodeAllocation(prefix.data(), size, 91, output) && std::memcmp(&before, &output, sizeof(output)) == 0,
                    "every truncated allocation reply bounded and output unchanged");
        }
        require(!pool.DecodeAllocation(&canonical, UINT32_MAX, 91, output), "oversized response rejected before read");
        std::vector<unsigned char> unaligned(129); std::memcpy(unaligned.data() + 1, &canonical, 128);
        require(pool.DecodeAllocation(unaligned.data() + 1, 128, 91, output), "unaligned allocation reply accepted");
        for (unsigned offset = 0; offset < 128; ++offset)
        {
            // Semantically variable token/buffer/size/offset/stride fields get
            // explicit arithmetic/alias checks below; fixed fields reject every bit.
            if ((offset >= 48 && offset < 56) || (offset >= 64 && offset < 80) ||
                (offset >= 88 && offset < 100)) continue;
            for (unsigned bit = 0; bit < 8; ++bit)
            {
                auto bad = canonical; reinterpret_cast<unsigned char *>(&bad)[offset] ^= static_cast<unsigned char>(1U << bit);
                require(!pool.DecodeAllocation(&bad, 128, 91, output), "mutated allocation identity/layout fixed field rejected");
            }
        }
        for (uint64_t value : {UINT64_C(0), UINT64_C(1), UINT64_MAX})
        {
            auto bad = canonical; bad.description.allocation_size = value;
            require(!pool.DecodeAllocation(&bad, 128, 91, output), "invalid allocation size/alignment bounds");
        }
        auto bad = canonical; bad.description.plane_offset = UINT64_MAX;
        require(!pool.DecodeAllocation(&bad, 128, 91, output), "plane offset overflow");
        bad = canonical; bad.description.plane_stride = UINT32_MAX;
        require(!pool.DecodeAllocation(&bad, 128, 91, output), "stride overflow");
        bad = canonical; bad.query.token = 0;
        require(!pool.DecodeAllocation(&bad, 128, 91, output), "zero token forbidden");
        pool.owners[0].state = VioGpuDvsaAllocated; pool.owners[0].identity.token = canonical.query.token;
        require(!pool.DecodeAllocation(&canonical, 128, 91, output), "token cannot alias a retained owner");
        pool.owners[0].identity.token = 0; pool.owners[0].description.buffer_id = canonical.description.buffer_id;
        require(!pool.DecodeAllocation(&canonical, 128, 91, output), "AHB identity cannot alias a retained owner");
    }
    std::printf("DVSA production allocation client: %u checks, lifecycle/identity/capacity/fault boundaries PASS\n", checks);
}
