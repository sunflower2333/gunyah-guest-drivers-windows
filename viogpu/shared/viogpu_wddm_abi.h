#ifndef VIOGPU_WDDM_ABI_H
#define VIOGPU_WDDM_ABI_H

#include <stdint.h>

/*
 * Experimental pre-v1 snapshot. Version 1 must not be published until the
 * context VA and allocation IOVA control contracts are implemented.
 */
#define VIOGPU_WDDM_ABI_MAGIC              0x504D5644U
#define VIOGPU_WDDM_ABI_VERSION            0U

#define VIOGPU_WDDM_CAPABILITIES_NONE      0ULL

#define VIOGPU_WDDM_ALLOCATION_PRIMARY     0x00000001U
#define VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE 0x00000002U

#define VIOGPU_WDDM_CONTEXT_FLAGS_NONE     0U
#define VIOGPU_WDDM_RENDER_FLAGS_NONE      0U

#define VIOGPU_WDDM_REFERENCE_READ         0x00000001U
#define VIOGPU_WDDM_REFERENCE_WRITE        0x00000002U

typedef enum VIOGPU_WDDM_FORMAT
{
    VIOGPU_WDDM_FORMAT_NONE = 0,
    VIOGPU_WDDM_FORMAT_B8G8R8A8_UNORM = 1,
    VIOGPU_WDDM_FORMAT_B8G8R8X8_UNORM = 2,
} VIOGPU_WDDM_FORMAT;

typedef enum VIOGPU_WDDM_RENDER_OPCODE
{
    VIOGPU_WDDM_RENDER_NATIVE_SUBMIT = 1,
} VIOGPU_WDDM_RENDER_OPCODE;

#pragma pack(push, 4)

typedef struct VIOGPU_WDDM_ABI_HEADER
{
    uint32_t Magic;
    uint32_t Version;
    uint32_t Size;
    uint32_t Reserved;
} VIOGPU_WDDM_ABI_HEADER;

typedef struct VIOGPU_WDDM_ADAPTER_INFO
{
    VIOGPU_WDDM_ABI_HEADER Header;
    uint64_t Capabilities;
    uint64_t ResetGeneration;
    uint32_t MsmMajorVersion;
    uint32_t MsmMinorVersion;
    uint32_t MsmPatchVersion;
    uint32_t GpuId;
    uint64_t ChipId;
    uint32_t GmemSize;
    uint32_t PriorityCount;
    uint64_t GmemBase;
    uint32_t HighestBankBit;
    uint32_t HasCachedCoherentMemory;
    uint64_t UbwcSwizzle;
    uint64_t MacrotileMode;
    uint64_t UcheTrapBase;
    uint32_t HasRayTracing;
    uint32_t MaxFrequency;
    uint64_t Reserved[2];
} VIOGPU_WDDM_ADAPTER_INFO;

typedef struct VIOGPU_WDDM_ALLOCATION_INFO
{
    VIOGPU_WDDM_ABI_HEADER Header;
    uint64_t Size;
    uint64_t Alignment;
    uint32_t Flags;
    uint32_t Format;
    uint32_t Width;
    uint32_t Height;
    uint32_t Pitch;
    uint32_t RefreshRateNumerator;
    uint32_t RefreshRateDenominator;
    uint32_t Reserved;
} VIOGPU_WDDM_ALLOCATION_INFO;

typedef struct VIOGPU_WDDM_CONTEXT_CREATE
{
    VIOGPU_WDDM_ABI_HEADER Header;
    uint64_t ExpectedResetGeneration;
    uint32_t Flags;
    uint32_t Reserved;
} VIOGPU_WDDM_CONTEXT_CREATE;

typedef struct VIOGPU_WDDM_RENDER_COMMAND
{
    VIOGPU_WDDM_ABI_HEADER Header;
    uint32_t Opcode;
    uint32_t Flags;
    uint64_t ExpectedResetGeneration;
    uint32_t AllocationReferencesOffset;
    uint32_t AllocationReferenceCount;
    uint32_t CommandStreamOffset;
    uint32_t CommandStreamSize;
    uint32_t Reserved[4];
} VIOGPU_WDDM_RENDER_COMMAND;

typedef struct VIOGPU_WDDM_ALLOCATION_REFERENCE
{
    uint32_t AllocationIndex;
    uint32_t Flags;
    uint64_t AllocationOffset;
    uint64_t Length;
    uint32_t PatchOffset;
    uint32_t Reserved;
} VIOGPU_WDDM_ALLOCATION_REFERENCE;

#pragma pack(pop)

#endif
