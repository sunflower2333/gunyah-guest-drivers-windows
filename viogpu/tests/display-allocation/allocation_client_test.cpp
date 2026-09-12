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
SAME_TYPE(VIOGPU_DVSA_OWNER_REQUEST, dvsa_owner_request);
SAME_TYPE(VIOGPU_DVSA_RECOVERABLE_ALLOCATE, dvsa_allocate_recoverable);
SAME_TYPE(VIOGPU_DVSA_OWNER_RESPONSE, dvsa_owner_response);
SAME_FIELD(VIOGPU_DVSA_OWNER_REQUEST, dvsa_owner_request, query);
SAME_FIELD(VIOGPU_DVSA_OWNER_REQUEST, dvsa_owner_request, host_epoch);
SAME_FIELD(VIOGPU_DVSA_RECOVERABLE_ALLOCATE, dvsa_allocate_recoverable, allocation);
SAME_FIELD(VIOGPU_DVSA_RECOVERABLE_ALLOCATE, dvsa_allocate_recoverable, host_epoch);
SAME_FIELD(VIOGPU_DVSA_OWNER_RESPONSE, dvsa_owner_response, query);
SAME_FIELD(VIOGPU_DVSA_OWNER_RESPONSE, dvsa_owner_response, host_epoch);
SAME_FIELD(VIOGPU_DVSA_OWNER_RESPONSE, dvsa_owner_response, state);
SAME_FIELD(VIOGPU_DVSA_OWNER_RESPONSE, dvsa_owner_response, owner_context);
SAME_FIELD(VIOGPU_DVSA_OWNER_RESPONSE, dvsa_owner_response, allocation_size);
SAME_FIELD(VIOGPU_DVSA_OWNER_RESPONSE, dvsa_owner_response, bar_offset);
SAME_FIELD(VIOGPU_DVSA_OWNER_RESPONSE, dvsa_owner_response, mapped_size);
SAME_FIELD(VIOGPU_DVSA_OWNER_RESPONSE, dvsa_owner_response, reserved);
static_assert(VIOGPU_DVSA_QUERY_OWNER == DVSA_CMD_QUERY_OWNER &&
    VIOGPU_DVSA_ALLOCATE_RECOVERABLE == DVSA_CMD_ALLOCATE_RECOVERABLE &&
    VIOGPU_DVSA_CLEANUP_OWNER == DVSA_CMD_CLEANUP_OWNER && VIOGPU_DVSA_OWNER_REPLY == DVSA_RESP_OWNER &&
    VIOGPU_DVSA_OWNER_SESSION == DVSA_OWNER_SESSION && VIOGPU_DVSA_OWNER_RETAINED == DVSA_OWNER_RETAINED &&
    VIOGPU_DVSA_OWNER_RELEASED == DVSA_OWNER_RELEASED && VIOGPU_DVSA_OWNER_UNKNOWN == DVSA_OWNER_UNKNOWN,
    "recovery canonical constants");
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
    enum Failure { None, NotSubmitted, LostReply, ErrorWithOwner, ErrorBeforeOwner, BadDescription, BadMapCache };
    struct Owner { dvsa_allocated reply; unsigned state; uint64_t offset; };
    std::map<unsigned, Owner> owners;
    std::map<unsigned, dvsa_header> terminal;
    std::vector<unsigned> operations;
    unsigned calls = 0, fail_call = 0, context = 0, cache = 1;
    Failure failure = None;
    bool poisoned = false;
    bool release_failed = false;
    unsigned release_calls = 0;
    uint64_t host_epoch[2] = {0x1200340056007800ULL, 0xab00cd00ef001234ULL};
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
        case DVSA_CMD_QUERY_OWNER:
        case DVSA_CMD_CLEANUP_OWNER: {
            const auto q = read<dvsa_owner_request>(command, size);
            dvsa_owner_response response = {};
            response.query = q.query; response.query.hdr = {};
            response.query.hdr.type = DVSA_RESP_OWNER; response.query.size = 128;
            response.host_epoch[0] = host_epoch[0]; response.host_epoch[1] = host_epoch[1];
            response.owner_context = q.query.hdr.context_id;
            const bool expected = q.host_epoch[0] != 0 || q.host_epoch[1] != 0;
            if (expected && (q.host_epoch[0] != host_epoch[0] || q.host_epoch[1] != host_epoch[1]))
            { status.type = 0x1205; break; }
            if (q.query.resource_id == 0)
            {
                require(header.type == DVSA_CMD_QUERY_OWNER && q.query.hdr.context_id == 0 &&
                    q.query.surface_generation == 0 && q.query.token == 0 && q.query.size == 80,
                    "recovery handshake has no owner or mutation");
                response.state = DVSA_OWNER_SESSION;
            }
            else
            {
                mutation(q.query, header.type, 80, q.query.token == 0);
                require(expected, "owner recovery requires previously observed host lifetime");
                const auto live = owners.find(q.query.resource_id);
                const auto retired = terminal.find(q.query.resource_id);
                if (live != owners.end())
                {
                    auto &o = live->second;
                    require(q.query.hdr.context_id == context &&
                        q.query.surface_generation == o.reply.query.surface_generation &&
                        (q.query.token == 0 || q.query.token == o.reply.query.token),
                        "cleanup exact old owner identity");
                    response.query.token = o.reply.query.token;
                    response.state = DVSA_OWNER_RETAINED;
                    if (header.type == DVSA_CMD_CLEANUP_OWNER)
                    {
                        ++release_calls;
                        if (!release_failed)
                        {
                            auto identity = o.reply.query; identity.hdr.context_id = context;
                            terminal.emplace(q.query.resource_id, identity);
                            owners.erase(live); response.state = DVSA_OWNER_RELEASED;
                        }
                    }
                }
                else if (retired != terminal.end())
                {
                    require(q.query.hdr.context_id == retired->second.hdr.context_id &&
                        q.query.surface_generation == retired->second.surface_generation &&
                        (q.query.token == 0 || q.query.token == retired->second.token), "duplicate terminal identity");
                    response.query.token = retired->second.token; response.state = DVSA_OWNER_RELEASED;
                }
                else
                {
                    require(q.query.token == 0, "unknown request cannot invent token");
                    response.state = DVSA_OWNER_UNKNOWN;
                    if (header.type == DVSA_CMD_CLEANUP_OWNER)
                    { terminal.emplace(q.query.resource_id, q.query); response.state = DVSA_OWNER_RELEASED; }
                }
            }
            reply(response, bytes, used); break;
        }
        case 0x200: {
            const auto c = read<VIOGPU_DVSA_CREATE_CONTEXT>(command, size);
            require(context == 0 && c.context_init == 6 && c.hdr.context_id != 0 &&
                    c.name_length == 0 && c.hdr.flags == 0, "dedicated native context created once");
            context = c.hdr.context_id; break;
        }
        case DVSA_CMD_ALLOCATE_RECOVERABLE: {
            const auto tracked = read<dvsa_allocate_recoverable>(command, size);
            const auto &a = tracked.allocation;
            mutation(a.query, DVSA_CMD_ALLOCATE_RECOVERABLE, 96, true);
            require(tracked.host_epoch[0] == host_epoch[0] && tracked.host_epoch[1] == host_epoch[1] &&
                    terminal.count(a.query.resource_id) == 0, "allocation bound to fresh tracked owner identity");
            if (calls == fail_call && failure == ErrorBeforeOwner) { status.type = 0x1200; break; }
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
        require(prepare(pool, host) && pool.Ready() && host.calls == 11, "actual client handshakes/creates/maps/ACKs three owners");
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
        require(pool.Release(host, 7) && !pool.Busy() && host.calls == 19 && host.owners.empty() && host.context == 0,
                "old generation cleanup has terminal owner receipts before context release");
    }
    for (unsigned call = 1; call <= 11; ++call)
    {
        Host host; host.fail_call = call; host.failure = Host::NotSubmitted;
        VioGpuDisplayAllocationPool pool = {};
        require(!prepare(pool, host) && !pool.Busy() && host.owners.empty() && host.context == 0,
                "every non-submission rolls back only confirmed prior owners");
    }
    for (unsigned call = 2; call <= 11; ++call)
    {
        Host host; host.fail_call = call; host.failure = Host::LostReply;
        VioGpuDisplayAllocationPool pool = {};
        require(!prepare(pool, host) && pool.Busy() && !pool.Ready(), "every lost create/allocate/map/ACK reply retains owner");
        pool.InvalidateTransport(); host.poisoned = false;
        require(pool.ControlLimit(128ULL << 20) == 8ULL << 20, "reset keeps external range reserved");
        require(pool.Release(host, 8) && !pool.Busy() && host.owners.empty() && host.context == 0,
                "new transport verifies same host incarnation and recovers each lost reply");
    }
    for (unsigned call : {3U, 6U, 9U})
    {
        for (auto failure : {Host::ErrorWithOwner, Host::ErrorBeforeOwner, Host::BadDescription})
        {
            Host host; host.fail_call = call; host.failure = failure;
            VioGpuDisplayAllocationPool pool = {};
            require(!prepare(pool, host) && !pool.Busy() && host.owners.empty() && host.context == 0,
                    "tokenless allocation failure reconciled only through explicit release or seal");
            require(!host.terminal.empty(), "cleanup preserved authoritative terminal identity");
        }
    }
    for (unsigned call = 12; call <= 19; ++call)
    {
        Host host; VioGpuDisplayAllocationPool pool = {};
        require(prepare(pool, host), "cleanup fault setup");
        host.fail_call = call; host.failure = Host::NotSubmitted;
        require(!pool.Release(host, 7) && pool.Busy(), "unmap/destroy/context non-submission retains identity");
        require(pool.Release(host, 7) && !pool.Busy() && host.owners.empty(), "same-epoch exact cleanup retry succeeds");
    }
    for (unsigned call = 12; call <= 19; ++call)
    {
        Host host; VioGpuDisplayAllocationPool pool = {};
        require(prepare(pool, host), "lost cleanup setup");
        host.fail_call = call; host.failure = Host::LostReply;
        require(!pool.Release(host, 7) && pool.Busy(), "lost unmap/destroy/context receipt retains ownership");
        pool.InvalidateTransport(); host.poisoned = false;
        if (call == 19)
        {
            const auto before = host.calls;
            require(!pool.Release(host, 8) && before == host.calls && pool.context_cleanup_uncertain,
                    "legacy context lost destruction remains quarantined without a retirement contract");
        }
        else require(pool.Release(host, 8) && !pool.Busy() && host.release_calls == 3,
                     "lost owner cleanup receipt recovers terminal journal without duplicate release");
    }
    {
        Host host; VioGpuDisplayAllocationPool pool = {};
        require(prepare(pool, host), "cleanup-only transition setup");
        host.poisoned = true;
        require(!pool.Release(host, 7) && pool.Busy() && !pool.Ready(),
                "closing pool never reports ready even if every unmap was not submitted");
        host.poisoned = false;
        require(pool.Release(host, 7), "closing state allows exact same-epoch cleanup retry only");
    }
    for (unsigned call : {4U, 7U, 10U})
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
    for (unsigned call : {3U, 6U, 9U})
    {
        Host host; host.fail_call = call; host.failure = Host::ErrorWithOwner; host.release_failed = true;
        VioGpuDisplayAllocationPool pool = {};
        require(!prepare(pool, host) && pool.Busy() && !host.owners.empty(),
                "KGSL release failure retains tokenless owner and reserved BAR");
        host.generation = 18; host.release_failed = false; pool.InvalidateTransport();
        require(pool.Release(host, 8) && !pool.Busy() && host.owners.empty(),
                "failed native release retries after Surface and guest transport changes");
    }
    {
        Host host; host.fail_call = 3; host.failure = Host::LostReply;
        VioGpuDisplayAllocationPool pool = {};
        require(!prepare(pool, host) && pool.Busy(), "host replacement fault setup");
        pool.InvalidateTransport(); host.poisoned = false; ++host.host_epoch[1];
        const auto saved0 = pool.host_epoch[0], saved1 = pool.host_epoch[1];
        const unsigned before = host.calls;
        require(!pool.Release(host, 8) && pool.Busy() && host.calls == before + 1 &&
                pool.host_epoch[0] == saved0 && pool.host_epoch[1] == saved1 && host.release_calls == 0 &&
                pool.ControlLimit(128ULL << 20) == 8ULL << 20,
                "new host empty journal cannot erase old external ownership");
    }
    {
        Host host; VioGpuDisplayAllocationPool pool = {};
        require(prepare(pool, host), "recovery parser setup");
        const auto request = pool.OwnerRequest(VIOGPU_DVSA_QUERY_OWNER, &pool.owners[0].identity);
        VIOGPU_DVSA_OWNER_RESPONSE canonical = {}, output = {};
        require(pool.OwnerExchange(host, request, canonical), "production recovery reply");
        for (unsigned size = 0; size < 128; ++size)
        {
            std::memset(&output, 0xa5, sizeof(output)); const auto before = output;
            require(!pool.DecodeOwner(&canonical, size, request, output) &&
                    std::memcmp(&before, &output, sizeof(output)) == 0,
                    "every truncated recovery response leaves ownership unchanged");
        }
        for (unsigned offset = 0; offset < 128; ++offset)
        {
            if (offset >= 80 && offset < 84) continue; // Explicit states tested separately.
            if (offset >= 88 && offset < 112) continue; // Retained metadata cannot grant release.
            for (unsigned bit = 0; bit < 8; ++bit)
            {
                auto bad = canonical; reinterpret_cast<unsigned char *>(&bad)[offset] ^= static_cast<unsigned char>(1U << bit);
                require(!pool.DecodeOwner(&bad, 128, request, output), "recovery fixed identity bit mutation rejected");
            }
        }
        for (unsigned state : {0U, DVSA_OWNER_SESSION, 5U, UINT32_MAX})
        {
            auto bad = canonical; bad.state = state;
            require(!pool.DecodeOwner(&bad, 128, request, output), "invalid owner response state rejected");
        }
        auto bad = canonical; bad.state = DVSA_OWNER_RELEASED; bad.mapped_size = 16384;
        require(!pool.DecodeOwner(&bad, 128, request, output), "terminal receipt cannot retain mapped bytes");
        std::vector<unsigned char> unaligned(129); std::memcpy(unaligned.data() + 1, &canonical, 128);
        require(pool.DecodeOwner(unaligned.data() + 1, 128, request, output), "unaligned recovery reply bounded");
        require(pool.Release(host, 7), "parser fixture cleanup");
    }
    std::printf("DVSA production allocation client: %u checks, lifecycle/identity/capacity/recovery boundaries PASS\n", checks);
}
