/*
 * DroidVM Pre-Shared Pool Provider - Public Interface
 *
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#pragma once

/*
 * Include <initguid.h> before this header in exactly one source file per
 * module to define the GUID. Other source files receive an extern reference.
 *
 * Device interface: {3FFC3B30-CD8F-419E-8C7B-5065C6395F25}
 */
DEFINE_GUID(GUID_DEVINTERFACE_DROIDVMPOOL, 0x3ffc3b30, 0xcd8f, 0x419e, 0x8c, 0x7b, 0x50, 0x65, 0xc6, 0x39, 0x5f, 0x25);

/* Direct-call mapping interface: {E514E4A9-445A-4C8A-AD7C-5FB99E2A0E86} */
DEFINE_GUID(GUID_DROIDVMPOOL_DIRECT_INTERFACE,
            0xe514e4a9,
            0x445a,
            0x4c8a,
            0xad,
            0x7c,
            0x5f,
            0xb9,
            0x9e,
            0x2a,
            0x0e,
            0x86);

#define FILE_DEVICE_DROIDVMPOOL          0x8001
#define DROIDVMPOOL_INTERFACE_VERSION_V1 1U
#define DROIDVMPOOL_DIRECT_VERSION_V1    1U
#ifndef DROIDVMPOOL_NAME_CAPACITY
#define DROIDVMPOOL_NAME_CAPACITY 64U
#endif

/*
 * Query the immutable identity and physical extent of one ACPI\DRVM0001 pool.
 * This is discovery metadata only. A client must acquire the direct-call
 * mapping interface before dereferencing the provider-owned kernel VA.
 */
#define IOCTL_DROIDVMPOOL_QUERY CTL_CODE(FILE_DEVICE_DROIDVMPOOL, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

#pragma pack(push, 8)

typedef struct _DROIDVMPOOL_QUERY_OUTPUT
{
    ULONG InterfaceVersion;
    ULONG StructureSize;
    ULONG PoolNameLength; /* ASCII bytes excluding the trailing NUL. */
    ULONG PageSize;
    PHYSICAL_ADDRESS BasePhysicalAddress;
    ULONG64 TotalSize;
    CHAR PoolName[DROIDVMPOOL_NAME_CAPACITY];
} DROIDVMPOOL_QUERY_OUTPUT, *PDROIDVMPOOL_QUERY_OUTPUT;

#pragma pack(pop)

#pragma pack(push, 8)

typedef struct _DROIDVMPOOL_MAPPING
{
    PVOID BaseVirtualAddress;
    PHYSICAL_ADDRESS BasePhysicalAddress;
    ULONG64 TotalSize;
} DROIDVMPOOL_MAPPING, *PDROIDVMPOOL_MAPPING;

/*
 * AcquireMapping and ReleaseMapping may be called at IRQL <= DISPATCH_LEVEL.
 * A successful acquire pins the provider mapping until the matching release.
 * The mapping must not be retained or dereferenced outside that interval.
 */
typedef BOOLEAN (*PDROIDVMPOOL_ACQUIRE_MAPPING)(_In_ PVOID context, _Out_ PDROIDVMPOOL_MAPPING mapping);
typedef VOID (*PDROIDVMPOOL_RELEASE_MAPPING)(_In_ PVOID context);

typedef struct _DROIDVMPOOL_DIRECT_INTERFACE
{
    INTERFACE InterfaceHeader;
    PDROIDVMPOOL_ACQUIRE_MAPPING AcquireMapping;
    PDROIDVMPOOL_RELEASE_MAPPING ReleaseMapping;
} DROIDVMPOOL_DIRECT_INTERFACE, *PDROIDVMPOOL_DIRECT_INTERFACE;

#pragma pack(pop)
