/* SPDX-License-Identifier: BSD-3-Clause
 * Production adapter-owned AHB allocation/mapping client. Caller serializes
 * this object with the adapter lifecycle mutex. No CPU address, GPU submit,
 * scanout binding or producer/presentation lease is exposed by this client.
 */
#pragma once
#include "../shared/viogpu_display_allocation.h"

enum VIOGPU_DVSA_EXCHANGE_RESULT
{
    VioGpuDvsaNotSubmitted,
    VioGpuDvsaCompleted,
    VioGpuDvsaUncertain
};

#pragma pack(push, 8)
struct VIOGPU_DVSA_CREATE_CONTEXT
{
    VIOGPU_DVSA_CTRL_HEADER hdr;
    VIOGPU_DVSA_U32 name_length, context_init;
    VIOGPU_DVSA_U8 name[64];
};
struct VIOGPU_DVSA_MAP
{
    VIOGPU_DVSA_CTRL_HEADER hdr;
    VIOGPU_DVSA_U32 resource_id, padding;
    VIOGPU_DVSA_U64 offset;
};
struct VIOGPU_DVSA_UNMAP
{
    VIOGPU_DVSA_CTRL_HEADER hdr;
    VIOGPU_DVSA_U32 resource_id, padding;
};
struct VIOGPU_DVSA_MAP_REPLY
{
    VIOGPU_DVSA_CTRL_HEADER hdr;
    VIOGPU_DVSA_U32 map_info, padding;
};
#pragma pack(pop)
static_assert(sizeof(VIOGPU_DVSA_CREATE_CONTEXT) == 96, "virtio context request");
static_assert(sizeof(VIOGPU_DVSA_MAP) == 40, "virtio map request");
static_assert(sizeof(VIOGPU_DVSA_UNMAP) == 32, "virtio unmap request");
static_assert(sizeof(VIOGPU_DVSA_MAP_REPLY) == 32, "virtio map reply");

static inline bool VioGpuDvsaPlainHeader(const VIOGPU_DVSA_CTRL_HEADER &hdr, VIOGPU_DVSA_U32 type)
{
    return hdr.type == type && hdr.flags == 0 && hdr.fence_id == 0 && hdr.context_id == 0 &&
           hdr.ring_idx == 0 && hdr.padding[0] == 0 && hdr.padding[1] == 0 && hdr.padding[2] == 0;
}

// All wire input is copied by bytes after checking its complete length.
template<class T> static bool VioGpuDvsaRead(const void *bytes, unsigned size, T &value)
{
    if (bytes == NULL || size != sizeof(T)) return false;
    for (size_t i = 0; i < sizeof(T); ++i)
        reinterpret_cast<VIOGPU_DVSA_U8 *>(&value)[i] = static_cast<const VIOGPU_DVSA_U8 *>(bytes)[i];
    return true;
}

enum VIOGPU_DVSA_OWNER_STATE
{
    VioGpuDvsaEmpty,
    VioGpuDvsaUnknownAllocation,
    VioGpuDvsaAllocated,
    VioGpuDvsaMappingAttempted,
    VioGpuDvsaMapped,
    VioGpuDvsaAcknowledged,
    VioGpuDvsaUnmapped
};

struct VIOGPU_DVSA_OWNER
{
    VIOGPU_DVSA_OWNER_STATE state;
    VIOGPU_DVSA_HEADER identity;
    VIOGPU_DVSA_DESCRIPTION description;
    VIOGPU_DVSA_U64 bar_offset;
    VIOGPU_DVSA_U32 map_info;
};

struct VioGpuDisplayAllocationPool
{
    VIOGPU_DVSA_DISCOVERY discovery;
    VIOGPU_DVSA_U64 reset_generation;
    VIOGPU_DVSA_U32 context_id, width, height;
    bool context_confirmed, transport_retired;
    VIOGPU_DVSA_OWNER owners[3];

    bool Busy() const { return context_id != 0; }
    bool Ready() const
    {
        return Busy() && context_confirmed && !transport_retired &&
               owners[0].state == VioGpuDvsaAcknowledged &&
               owners[1].state == VioGpuDvsaAcknowledged &&
               owners[2].state == VioGpuDvsaAcknowledged;
    }
    void InvalidateTransport() { if (Busy()) transport_retired = true; }
    VIOGPU_DVSA_U64 ControlLimit(VIOGPU_DVSA_U64 region_size) const
    {
        return Busy() && discovery.reserved_prefix < region_size ? discovery.reserved_prefix : region_size;
    }

    static VIOGPU_DVSA_HEADER Mutation(VIOGPU_DVSA_U32 type, unsigned size,
        VIOGPU_DVSA_U64 generation, VIOGPU_DVSA_U32 context, VIOGPU_DVSA_U32 resource,
        VIOGPU_DVSA_U64 token = 0)
    {
        VIOGPU_DVSA_HEADER header = {};
        header.hdr.type = type; header.hdr.context_id = context;
        header.magic = VIOGPU_DVSA_MAGIC; header.version = VIOGPU_DVSA_VERSION;
        header.size = size; header.surface_generation = generation;
        header.token = token; header.resource_id = resource;
        return header;
    }

    bool DecodeAllocation(const void *bytes, unsigned size, VIOGPU_DVSA_U32 resource,
                          VIOGPU_DVSA_ALLOCATED &output) const
    {
        VIOGPU_DVSA_ALLOCATED reply = {};
        if (width == 0 || width > 8192 || height == 0 || height > 8192 ||
            discovery.mapping_alignment == 0 || discovery.dynamic_capacity == 0) return false;
        if (!VioGpuDvsaRead(bytes, size, reply)) return false;
        const VIOGPU_DVSA_HEADER &q = reply.query;
        const VIOGPU_DVSA_DESCRIPTION &d = reply.description;
        // The real unfenced virtio response envelope zeroes context_id. The
        // dedicated request context remains locally authoritative for cleanup.
        if (!VioGpuDvsaPlainHeader(q.hdr, VIOGPU_DVSA_ALLOCATED_REPLY) ||
            q.magic != VIOGPU_DVSA_MAGIC || q.version != 1 || q.size != sizeof(reply) ||
            q.scanout_id != 0 || q.surface_generation != discovery.query.surface_generation ||
            q.token == 0 || q.resource_id != resource || q.reserved != 0 ||
            d.buffer_id == 0 || d.width != width || d.height != height ||
            d.fourcc != VIOGPU_DVSA_ABGR8888 || d.modifier != 0 || d.plane_count != 1 ||
            d.layout_flags != VIOGPU_DVSA_LINEAR || d.reserved != 0 ||
            d.plane_stride % 4 != 0 || d.plane_stride < static_cast<VIOGPU_DVSA_U64>(width) * 4 ||
            d.allocation_size == 0 || d.allocation_size > 256ULL * 1024 * 1024 ||
            d.allocation_size % discovery.mapping_alignment != 0 ||
            d.allocation_size > discovery.dynamic_capacity / 3 ||
            d.plane_offset > d.allocation_size)
            return false;
        const VIOGPU_DVSA_U64 last_row = static_cast<VIOGPU_DVSA_U64>(height - 1) * d.plane_stride;
        const VIOGPU_DVSA_U64 row_bytes = static_cast<VIOGPU_DVSA_U64>(width) * 4;
        if (last_row > d.allocation_size - d.plane_offset ||
            row_bytes > d.allocation_size - d.plane_offset - last_row) return false;
        for (unsigned i = 0; i < 3; ++i)
            if (owners[i].state != VioGpuDvsaEmpty &&
                (owners[i].identity.token == q.token || owners[i].description.buffer_id == d.buffer_id))
                return false;
        output = reply;
        return true;
    }

    template<class Queue, class Request>
    static bool NoData(Queue &queue, const Request &request)
    {
        VIOGPU_DVSA_U8 bytes[128] = {};
        unsigned used = 0;
        VIOGPU_DVSA_CTRL_HEADER reply = {};
        return queue.ExchangeDisplayAllocationCommand(&request, sizeof(request), bytes, &used) == VioGpuDvsaCompleted &&
               VioGpuDvsaRead(bytes, used, reply) && VioGpuDvsaPlainHeader(reply, 0x1100U);
    }

    template<class Queue> bool Release(Queue &queue, VIOGPU_DVSA_U64 current_reset)
    {
        if (!Busy()) return true;
        if (transport_retired || reset_generation != current_reset) return false;
        bool retained = false;
        for (unsigned i = 0; i < 3; ++i)
        {
            VIOGPU_DVSA_OWNER &owner = owners[i];
            if (owner.state == VioGpuDvsaEmpty) continue;
            if (owner.state == VioGpuDvsaUnknownAllocation) { retained = true; continue; }
            if (owner.state == VioGpuDvsaMappingAttempted || owner.state == VioGpuDvsaMapped ||
                owner.state == VioGpuDvsaAcknowledged)
            {
                VIOGPU_DVSA_UNMAP unmap = {};
                unmap.hdr.type = 0x209U; unmap.resource_id = owner.identity.resource_id;
                // Errors, timeout and even INVALID_RESOURCE are not receipts.
                if (!NoData(queue, unmap)) { retained = true; continue; }
                owner.state = VioGpuDvsaUnmapped;
            }
            VIOGPU_DVSA_HEADER destroy = Mutation(VIOGPU_DVSA_DESTROY, 64,
                owner.identity.surface_generation, context_id, owner.identity.resource_id, owner.identity.token);
            if (!NoData(queue, destroy)) { retained = true; continue; }
            owner = VIOGPU_DVSA_OWNER();
        }
        if (retained) return false;
        VIOGPU_DVSA_CTRL_HEADER destroy_context = {};
        destroy_context.type = 0x201U; destroy_context.context_id = context_id;
        if (!NoData(queue, destroy_context)) return false;
        *this = VioGpuDisplayAllocationPool();
        return true;
    }

    template<class Queue> bool Prepare(Queue &queue, const VIOGPU_DVSA_DISCOVERY &candidate,
        VIOGPU_DVSA_U64 pci_size, VIOGPU_DVSA_U32 requested_width, VIOGPU_DVSA_U32 requested_height,
        VIOGPU_DVSA_U32 dedicated_context, const VIOGPU_DVSA_U32 (&resources)[3],
        VIOGPU_DVSA_U64 current_reset)
    {
        if (Busy() || current_reset == 0 || dedicated_context == 0 ||
            requested_width == 0 || requested_width > 8192 || requested_height == 0 || requested_height > 8192 ||
            !VioGpuValidateDisplayAllocationDiscovery(&candidate, sizeof(candidate), pci_size) ||
            candidate.feature_bits != VIOGPU_DVSA_FEATURE_MAPPING)
            return false;
        for (unsigned i = 0; i < 3; ++i)
        {
            if (resources[i] == 0) return false;
            for (unsigned j = 0; j < i; ++j) if (resources[i] == resources[j]) return false;
        }
        discovery = candidate; reset_generation = current_reset;
        context_id = dedicated_context; width = requested_width; height = requested_height;
        VIOGPU_DVSA_CREATE_CONTEXT create = {};
        create.hdr.type = 0x200U; create.hdr.context_id = context_id;
        create.context_init = 6; // DRM capset; no WDDM render registration or submit queue.
        VIOGPU_DVSA_U8 bytes[128] = {};
        unsigned used = 0;
        VIOGPU_DVSA_EXCHANGE_RESULT exchanged = queue.ExchangeDisplayAllocationCommand(&create, sizeof(create), bytes, &used);
        VIOGPU_DVSA_CTRL_HEADER created = {};
        if (exchanged == VioGpuDvsaNotSubmitted) { *this = VioGpuDisplayAllocationPool(); return false; }
        if (exchanged != VioGpuDvsaCompleted || !VioGpuDvsaRead(bytes, used, created) ||
            !VioGpuDvsaPlainHeader(created, 0x1100U)) return false;
        context_confirmed = true;
        VIOGPU_DVSA_U64 next_offset = discovery.reserved_prefix;
        for (unsigned i = 0; i < 3; ++i)
        {
            VIOGPU_DVSA_OWNER &owner = owners[i];
            VIOGPU_DVSA_ALLOCATE_REQUEST allocate = {};
            allocate.query = Mutation(VIOGPU_DVSA_ALLOCATE, sizeof(allocate), discovery.query.surface_generation,
                                      context_id, resources[i]);
            allocate.width = width; allocate.height = height; allocate.fourcc = VIOGPU_DVSA_ABGR8888;
            // Publish potential ownership before any transport submission.
            owner.identity = allocate.query; owner.state = VioGpuDvsaUnknownAllocation;
            used = 0;
            exchanged = queue.ExchangeDisplayAllocationCommand(&allocate, sizeof(allocate), bytes, &used);
            if (exchanged == VioGpuDvsaNotSubmitted) { owner = VIOGPU_DVSA_OWNER(); break; }
            VIOGPU_DVSA_ALLOCATED allocated = {};
            if (exchanged != VioGpuDvsaCompleted || !DecodeAllocation(bytes, used, resources[i], allocated)) break;
            owner.identity = allocated.query; owner.identity.hdr.context_id = context_id;
            owner.description = allocated.description; owner.state = VioGpuDvsaAllocated;
            const VIOGPU_DVSA_U64 size = owner.description.allocation_size;
            if (next_offset > pci_size || size > pci_size - next_offset) break;
            owner.bar_offset = next_offset; next_offset += size;
            VIOGPU_DVSA_MAP map = {};
            map.hdr.type = 0x208U; map.resource_id = resources[i]; map.offset = owner.bar_offset;
            owner.state = VioGpuDvsaMappingAttempted;
            used = 0;
            exchanged = queue.ExchangeDisplayAllocationCommand(&map, sizeof(map), bytes, &used);
            if (exchanged == VioGpuDvsaNotSubmitted) { owner.state = VioGpuDvsaAllocated; break; }
            VIOGPU_DVSA_MAP_REPLY mapped = {};
            if (exchanged != VioGpuDvsaCompleted || !VioGpuDvsaRead(bytes, used, mapped) ||
                !VioGpuDvsaPlainHeader(mapped.hdr, 0x1106U) || mapped.padding != 0 ||
                (mapped.map_info != 1 && mapped.map_info != 2)) break;
            owner.map_info = mapped.map_info; owner.state = VioGpuDvsaMapped;
            VIOGPU_DVSA_ACK ack = {};
            ack.query = Mutation(VIOGPU_DVSA_ACK_MAPPING, sizeof(ack), owner.identity.surface_generation,
                                 context_id, resources[i], owner.identity.token);
            ack.bar_offset = owner.bar_offset; ack.mapped_size = size;
            if (!NoData(queue, ack)) break;
            owner.state = VioGpuDvsaAcknowledged;
        }
        if (Ready()) return true;
        Release(queue, current_reset);
        return false;
    }
};
