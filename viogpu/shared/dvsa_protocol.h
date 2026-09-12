/* Copyright 2026 The ChromiumOS Authors. BSD-style license, see LICENSE. */
#ifndef CROSVM_DVSA_PROTOCOL_H
#define CROSVM_DVSA_PROTOCOL_H
#include <stdint.h>
#include <stddef.h>

/* All fields little endian. Wire v1, never native handles or fd numbers. */
#define DVSA_MAGIC UINT32_C(0x41535644)
#define DVSA_VERSION UINT32_C(1)
#define DVSA_CMD_DISCOVER UINT32_C(0xd110)
#define DVSA_CMD_ALLOCATE UINT32_C(0xd111)
#define DVSA_CMD_DESTROY UINT32_C(0xd114)
#define DVSA_CMD_ACK_MAPPING UINT32_C(0xd115)
#define DVSA_RESP_DISCOVER UINT32_C(0xd210)
#define DVSA_RESP_ALLOCATE UINT32_C(0xd211)
#define DVSA_FEATURE_SDR_ZERO_COPY (UINT64_C(1) << 0)
/* Reserved primitive admission only; never a render/present/write lease. */
#define DVSA_FEATURE_ALLOCATION_MAPPING (UINT64_C(1) << 1)
#define DVSA_FORMAT_ABGR8888 UINT32_C(0x34324241)
#define DVSA_LAYOUT_VERIFIED_LINEAR UINT32_C(1)
#define DVSA_UNAVAILABLE_MAPPER (UINT64_C(1) << 0)
#define DVSA_UNAVAILABLE_NO_SUFFIX (UINT64_C(1) << 1)
#define DVSA_UNAVAILABLE_TRIPLE_CAPACITY (UINT64_C(1) << 2)
#define DVSA_UNAVAILABLE_NATIVE_SURFACE (UINT64_C(1) << 3)
#define DVSA_UNAVAILABLE_ALLOCATOR (UINT64_C(1) << 4)
#define DVSA_UNAVAILABLE_RENDERER_IMPORT (UINT64_C(1) << 5)
#define DVSA_UNAVAILABLE_PRODUCER_BRIDGE (UINT64_C(1) << 6)
#define DVSA_UNAVAILABLE_CONSUMER_BRIDGE (UINT64_C(1) << 7)
#define DVSA_UNAVAILABLE_GUEST_IDENTITY (UINT64_C(1) << 8)

#pragma pack(push, 8)
struct dvsa_ctrl_header {
    uint32_t type, flags;
    uint64_t fence_id;
    uint32_t context_id;
    uint8_t ring_idx, padding[3];
};
struct dvsa_header {
    struct dvsa_ctrl_header hdr;
    uint32_t magic, version, size, scanout_id;
    uint64_t surface_generation, token;
    uint32_t resource_id, reserved;
};
struct dvsa_discovery {
    struct dvsa_header query;
    uint64_t feature_bits, unavailable_reasons;
    uint64_t bar_size, reserved_prefix, dynamic_capacity, mapping_alignment;
    uint32_t min_allocations, max_allocations;
    uint64_t triple_bytes_lower_bound;
};
struct dvsa_allocate {
    struct dvsa_header query;
    uint32_t width, height, fourcc, flags;
};
struct dvsa_allocation_description {
    uint64_t buffer_id, allocation_size, modifier, plane_offset;
    uint32_t plane_stride, width, height, fourcc, plane_count, layout_flags;
    uint64_t reserved;
};
struct dvsa_allocated {
    struct dvsa_header query;
    struct dvsa_allocation_description description;
};
struct dvsa_ack_mapping {
    struct dvsa_header query;
    uint64_t bar_offset, mapped_size;
};
#pragma pack(pop)

/* DISCOVER: exactly64bytes, scanout/context/ring/padding/reserved0, identity0;
 * only FENCEbit0 allowed; fence_id0 unless FENCE. Response128bytes.
 * ALLOCATE: exactly80bytes; flags0; ABGR8888; current generation; token0;
 * unused resource_id and existing native DRM context_id both nonzero.
 * ACK_MAPPING: exactly80bytes; exact returned identity and actual mapped range.
 * DESTROY: exactly64bytes; exact retained identity, old generation allowed
 * for cleanup only. Requires successful UNMAP if mapping was attempted.
 * All mutations: hdr flags/fence/ring/padding0, scanout0; no trailing bytes.
 * Ordinary MAP_BLOB/UNMAP_BLOB identify a retained host owner by resource_id.
 * UNMAP is terminal for a token: DESTROY then ALLOCATE before another MAP.
 * New ALLOCATE requires primitive admission. Known-owner cleanup remains
 * available after Surface/capacity changes. A dedicated native diagnostic
 * context is required: holding an allocation blocks every ordinary SUBMIT in
 * that context until checked destruction. No CPU map/ACK grants a GPU lease.
 * ACQUIRE0xd112/PRESENT0xd113 are unfrozen proposals and remain unsupported.
 */
#if defined(__cplusplus)
#define DVSA_STATIC_ASSERT(c) static_assert(c, #c)
#else
#define DVSA_STATIC_ASSERT(c) _Static_assert(c, #c)
#endif
DVSA_STATIC_ASSERT(sizeof(struct dvsa_ctrl_header) == 24);
DVSA_STATIC_ASSERT(sizeof(struct dvsa_header) == 64);
DVSA_STATIC_ASSERT(sizeof(struct dvsa_discovery) == 128);
DVSA_STATIC_ASSERT(sizeof(struct dvsa_allocate) == 80);
DVSA_STATIC_ASSERT(sizeof(struct dvsa_allocation_description) == 64);
DVSA_STATIC_ASSERT(sizeof(struct dvsa_allocated) == 128);
DVSA_STATIC_ASSERT(sizeof(struct dvsa_ack_mapping) == 80);
DVSA_STATIC_ASSERT(offsetof(struct dvsa_header, surface_generation) == 40);
DVSA_STATIC_ASSERT(offsetof(struct dvsa_discovery, feature_bits) == 64);
DVSA_STATIC_ASSERT(offsetof(struct dvsa_discovery, triple_bytes_lower_bound) == 120);
#undef DVSA_STATIC_ASSERT
#endif
