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
 * {3FFC3B30-CD8F-419E-8C7B-5065C6395F25}
 */
DEFINE_GUID(GUID_DEVINTERFACE_DROIDVMPOOL, 0x3ffc3b30, 0xcd8f, 0x419e, 0x8c, 0x7b, 0x50, 0x65, 0xc6, 0x39, 0x5f, 0x25);

#define FILE_DEVICE_DROIDVMPOOL          0x8001
#define DROIDVMPOOL_INTERFACE_VERSION_V1 1U
#ifndef DROIDVMPOOL_NAME_CAPACITY
#define DROIDVMPOOL_NAME_CAPACITY 64U
#endif

/*
 * Query the immutable identity and mapping of one ACPI\DRVM0001 pool.
 * The returned kernel VA remains provider-owned and is valid only while the
 * client keeps its interface connection open and the provider remains ready.
 */
#define IOCTL_DROIDVMPOOL_QUERY CTL_CODE(FILE_DEVICE_DROIDVMPOOL, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

#pragma pack(push, 8)

typedef struct _DROIDVMPOOL_QUERY_OUTPUT
{
    ULONG InterfaceVersion;
    ULONG StructureSize;
    ULONG PoolNameLength; /* ASCII bytes excluding the trailing NUL. */
    ULONG PageSize;
    PVOID BaseVirtualAddress;
    PHYSICAL_ADDRESS BasePhysicalAddress;
    ULONG64 TotalSize;
    CHAR PoolName[DROIDVMPOOL_NAME_CAPACITY];
} DROIDVMPOOL_QUERY_OUTPUT, *PDROIDVMPOOL_QUERY_OUTPUT;

#pragma pack(pop)
