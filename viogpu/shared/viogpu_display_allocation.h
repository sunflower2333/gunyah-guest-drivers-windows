/* SPDX-License-Identifier: BSD-3-Clause
 * DVSA v1 wire contract. Integer fields are little endian; Windows targets
 * consuming this header are little endian. This is not an upstream feature.
 */
#pragma once
#include "viogpu_dvsa_wire.h"

typedef char VIOGPU_DVSA_HEADER_SIZE_CHECK[(sizeof(VIOGPU_DVSA_HEADER) == 64) ? 1 : -1];
typedef char VIOGPU_DVSA_REPLY_SIZE_CHECK[(sizeof(VIOGPU_DVSA_DISCOVERY) == 128) ? 1 : -1];
typedef char VIOGPU_DVSA_MAGIC_OFFSET_CHECK[(offsetof(VIOGPU_DVSA_HEADER, magic) == 24) ? 1 : -1];
typedef char VIOGPU_DVSA_GENERATION_OFFSET_CHECK[(offsetof(VIOGPU_DVSA_HEADER, surface_generation) == 40) ? 1 : -1];
typedef char VIOGPU_DVSA_CAPACITY_OFFSET_CHECK[(offsetof(VIOGPU_DVSA_DISCOVERY, dynamic_capacity) == 96) ? 1 : -1];

/* The caller owns aligned storage. Initialize every byte, including padding. No
 * rendering context, producer fence, token or resource participates in discovery. */
static inline void VioGpuInitializeDisplayAllocationQuery(VIOGPU_DVSA_HEADER *query)
{
    size_t offset;
    if (query == NULL) return;
    for (offset = 0; offset < sizeof(*query); ++offset)
    {
        ((VIOGPU_DVSA_U8 *)query)[offset] = 0;
    }
    query->hdr.type = VIOGPU_DVSA_DISCOVER;
    query->magic = VIOGPU_DVSA_MAGIC;
    query->version = VIOGPU_DVSA_VERSION;
    query->size = (VIOGPU_DVSA_U32)sizeof(*query);
}

/* A coherent unavailable response is useful negotiation, not allocation or
 * presentation admission. The PCI region size is independently observed by
 * the guest; never trust host-provided offsets/capacity as a substitute. */
static inline int VioGpuValidateDisplayAllocationDiscovery(const VIOGPU_DVSA_DISCOVERY *reply,
                                                          size_t used_size,
                                                          VIOGPU_DVSA_U64 pci_region_size)
{
    const VIOGPU_DVSA_HEADER *header;
    VIOGPU_DVSA_U64 reasons;
    VIOGPU_DVSA_U64 alignment;
    int insufficient;
    if (reply == NULL || used_size != sizeof(*reply))
    {
        return 0;
    }
    header = &reply->query;
    if (header->hdr.type != VIOGPU_DVSA_DISCOVERY_REPLY || header->hdr.flags != 0 || header->hdr.fence_id != 0 ||
        header->hdr.context_id != 0 || header->hdr.ring_idx != 0 || header->hdr.padding[0] != 0 ||
        header->hdr.padding[1] != 0 || header->hdr.padding[2] != 0 || header->magic != VIOGPU_DVSA_MAGIC ||
        header->version != VIOGPU_DVSA_VERSION || header->size != sizeof(*reply) ||
        header->scanout_id != 0 || header->token != 0 || header->resource_id != 0 || header->reserved != 0)
    {
        return 0;
    }
    reasons = reply->unavailable_reasons;
    /* This consumer only understands allocation prerequisites. Full-SDR bit0
     * and unknown bits fail closed; no accepted reply grants an allocation,
     * GPU write, presentation lease or WDDM capability. */
    if ((reply->feature_bits & ~VIOGPU_DVSA_FEATURE_MAPPING) != 0 ||
        (reasons & ~VIOGPU_DVSA_KNOWN_REASONS) != 0 || reply->min_allocations != 3 ||
        ((header->surface_generation == 0) != ((reasons & VIOGPU_DVSA_NO_SURFACE) != 0)))
    {
        return 0;
    }
    if ((reasons & VIOGPU_DVSA_NO_MAPPER) != 0)
    {
        return reply->feature_bits == 0 && reply->max_allocations == 0 && reply->bar_size == 0 &&
               reply->reserved_prefix == 0 && reply->dynamic_capacity == 0 &&
               reply->mapping_alignment == 0 && reply->triple_bytes_lower_bound == 0;
    }
    alignment = reply->mapping_alignment;
    if (pci_region_size == 0 || reply->bar_size != pci_region_size ||
        reply->reserved_prefix > reply->bar_size ||
        reply->dynamic_capacity != reply->bar_size - reply->reserved_prefix ||
        alignment < 4096 || (alignment & (alignment - 1)) != 0 || alignment > reply->bar_size ||
        reply->bar_size % alignment != 0 || reply->reserved_prefix % alignment != 0 ||
        ((reply->dynamic_capacity == 0) != ((reasons & VIOGPU_DVSA_NO_SUFFIX) != 0)))
    {
        return 0;
    }
    if (reply->triple_bytes_lower_bound != 0 &&
        (reply->triple_bytes_lower_bound % 3 != 0 || (reply->triple_bytes_lower_bound / 3) % alignment != 0))
    {
        return 0;
    }
    insufficient = reply->triple_bytes_lower_bound == 0 || reply->triple_bytes_lower_bound > reply->dynamic_capacity;
    if (insufficient != ((reasons & VIOGPU_DVSA_NO_TRIPLE_CAPACITY) != 0))
    {
        return 0;
    }
    if (reply->feature_bits == 0)
    {
        return reply->max_allocations == 0;
    }
    return (reasons & VIOGPU_DVSA_MAPPING_PREREQUISITES) == 0 && reply->max_allocations == 3;
}

/* Only this byte parser receives transport payloads. It checks the exact
 * transport byte count before any read, tolerates unaligned input, and leaves
 * the caller's previous output untouched on every malformed response. */
static inline int VioGpuParseDisplayAllocationDiscovery(const void *bytes,
                                                       size_t used_size,
                                                       VIOGPU_DVSA_U64 pci_region_size,
                                                       VIOGPU_DVSA_DISCOVERY *output)
{
    VIOGPU_DVSA_DISCOVERY reply;
    size_t offset;
    if (bytes == NULL || output == NULL || used_size != sizeof(reply)) return 0;
    for (offset = 0; offset < sizeof(reply); ++offset)
    {
        ((VIOGPU_DVSA_U8 *)&reply)[offset] = ((const VIOGPU_DVSA_U8 *)bytes)[offset];
    }
    if (!VioGpuValidateDisplayAllocationDiscovery(&reply, sizeof(reply), pci_region_size)) return 0;
    *output = reply;
    return 1;
}
