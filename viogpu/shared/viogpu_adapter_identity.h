#pragma once

#include "viogpu_wddm_abi.h"

#pragma pack(push, 4)
/* Optional trailer for the native D3D runtime callback.  The v0 prefix and
 * its Header.Size remain 128; only callers with a full 160-byte output
 * buffer receive this independently versioned identity. */
#define VIOGPU_WDDM_ADAPTER_IDENTITY_MAGIC   0x44494C56U
#define VIOGPU_WDDM_ADAPTER_IDENTITY_VERSION 1U
#define VIOGPU_WDDM_ADAPTER_IDENTITY_VALID   1U
typedef struct VIOGPU_WDDM_ADAPTER_IDENTITY
{
    VIOGPU_WDDM_UINT32 Magic;
    VIOGPU_WDDM_UINT32 Version;
    VIOGPU_WDDM_UINT32 Size;
    VIOGPU_WDDM_UINT32 Flags;
    VIOGPU_WDDM_UINT32 AdapterLuidLowPart;
    VIOGPU_WDDM_UINT32 AdapterLuidHighPart;
    VIOGPU_WDDM_UINT32 NodeMask;
    VIOGPU_WDDM_UINT32 Reserved;
} VIOGPU_WDDM_ADAPTER_IDENTITY;

typedef struct VIOGPU_WDDM_ADAPTER_INFO_WITH_IDENTITY
{
    VIOGPU_WDDM_ADAPTER_INFO AdapterInfo;
    VIOGPU_WDDM_ADAPTER_IDENTITY Identity;
} VIOGPU_WDDM_ADAPTER_INFO_WITH_IDENTITY;
#pragma pack(pop)
