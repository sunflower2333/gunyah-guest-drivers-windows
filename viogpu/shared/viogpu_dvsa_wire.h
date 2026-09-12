/* SPDX-License-Identifier: BSD-3-Clause
 * Freestanding discovery subset of canonical DVSA v1. Do not include the host
 * dvsa_protocol.h here: its user-mode stdint.h conflicts with the WDK kernel
 * CRT. Tests compare every field and constant against that immutable header.
 */
#pragma once
#include <stddef.h>

typedef unsigned char VIOGPU_DVSA_U8;
typedef unsigned int VIOGPU_DVSA_U32;
typedef unsigned long long VIOGPU_DVSA_U64;
static_assert(sizeof(VIOGPU_DVSA_U8) == 1, "DVSA requires 8-bit bytes");
static_assert(sizeof(VIOGPU_DVSA_U32) == 4, "DVSA requires 32-bit words");
static_assert(sizeof(VIOGPU_DVSA_U64) == 8, "DVSA requires 64-bit words");

#define VIOGPU_DVSA_DISCOVER 0xd110U
#define VIOGPU_DVSA_DISCOVERY_REPLY 0xd210U
#define VIOGPU_DVSA_MAGIC 0x41535644U
#define VIOGPU_DVSA_VERSION 1U
#define VIOGPU_DVSA_ALLOCATE 0xd111U
#define VIOGPU_DVSA_DESTROY 0xd114U
#define VIOGPU_DVSA_ACK_MAPPING 0xd115U
#define VIOGPU_DVSA_ALLOCATED_REPLY 0xd211U
#define VIOGPU_DVSA_ABGR8888 0x34324241U
#define VIOGPU_DVSA_LINEAR 1U
#define VIOGPU_DVSA_FEATURE_MAPPING (1ULL << 1)
#define VIOGPU_DVSA_NO_MAPPER (1ULL << 0)
#define VIOGPU_DVSA_NO_SUFFIX (1ULL << 1)
#define VIOGPU_DVSA_NO_TRIPLE_CAPACITY (1ULL << 2)
#define VIOGPU_DVSA_NO_SURFACE (1ULL << 3)
#define VIOGPU_DVSA_NO_ALLOCATOR (1ULL << 4)
#define VIOGPU_DVSA_NO_RENDERER_IMPORT (1ULL << 5)
#define VIOGPU_DVSA_NO_PRODUCER_BRIDGE (1ULL << 6)
#define VIOGPU_DVSA_NO_CONSUMER_BRIDGE (1ULL << 7)
#define VIOGPU_DVSA_NO_GUEST_IDENTITY (1ULL << 8)
#define VIOGPU_DVSA_KNOWN_REASONS 0x1ffULL
#define VIOGPU_DVSA_MAPPING_PREREQUISITES (VIOGPU_DVSA_NO_MAPPER | VIOGPU_DVSA_NO_SUFFIX | \
    VIOGPU_DVSA_NO_TRIPLE_CAPACITY | VIOGPU_DVSA_NO_SURFACE | VIOGPU_DVSA_NO_ALLOCATOR | \
    VIOGPU_DVSA_NO_RENDERER_IMPORT | VIOGPU_DVSA_NO_GUEST_IDENTITY)

#pragma pack(push, 8)
struct VIOGPU_DVSA_CTRL_HEADER
{
    VIOGPU_DVSA_U32 type, flags;
    VIOGPU_DVSA_U64 fence_id;
    VIOGPU_DVSA_U32 context_id;
    VIOGPU_DVSA_U8 ring_idx, padding[3];
};
struct VIOGPU_DVSA_HEADER
{
    VIOGPU_DVSA_CTRL_HEADER hdr;
    VIOGPU_DVSA_U32 magic, version, size, scanout_id;
    VIOGPU_DVSA_U64 surface_generation, token;
    VIOGPU_DVSA_U32 resource_id, reserved;
};
struct VIOGPU_DVSA_DISCOVERY
{
    VIOGPU_DVSA_HEADER query;
    VIOGPU_DVSA_U64 feature_bits, unavailable_reasons;
    VIOGPU_DVSA_U64 bar_size, reserved_prefix, dynamic_capacity, mapping_alignment;
    VIOGPU_DVSA_U32 min_allocations, max_allocations;
    VIOGPU_DVSA_U64 triple_bytes_lower_bound;
};
struct VIOGPU_DVSA_ALLOCATE_REQUEST
{
    VIOGPU_DVSA_HEADER query;
    VIOGPU_DVSA_U32 width, height, fourcc, flags;
};
struct VIOGPU_DVSA_DESCRIPTION
{
    VIOGPU_DVSA_U64 buffer_id, allocation_size, modifier, plane_offset;
    VIOGPU_DVSA_U32 plane_stride, width, height, fourcc, plane_count, layout_flags;
    VIOGPU_DVSA_U64 reserved;
};
struct VIOGPU_DVSA_ALLOCATED
{
    VIOGPU_DVSA_HEADER query;
    VIOGPU_DVSA_DESCRIPTION description;
};
struct VIOGPU_DVSA_ACK
{
    VIOGPU_DVSA_HEADER query;
    VIOGPU_DVSA_U64 bar_offset, mapped_size;
};
#pragma pack(pop)

static_assert(sizeof(VIOGPU_DVSA_CTRL_HEADER) == 24, "DVSA control header size");
static_assert(sizeof(VIOGPU_DVSA_HEADER) == 64, "DVSA common header size");
static_assert(sizeof(VIOGPU_DVSA_DISCOVERY) == 128, "DVSA discovery size");
static_assert(sizeof(VIOGPU_DVSA_ALLOCATE_REQUEST) == 80, "DVSA allocate size");
static_assert(sizeof(VIOGPU_DVSA_DESCRIPTION) == 64, "DVSA description size");
static_assert(sizeof(VIOGPU_DVSA_ALLOCATED) == 128, "DVSA allocated size");
static_assert(sizeof(VIOGPU_DVSA_ACK) == 80, "DVSA ACK size");
