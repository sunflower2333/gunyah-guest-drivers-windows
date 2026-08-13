#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef void *PVOID;
typedef void VOID;
typedef uint8_t BOOLEAN;
typedef char CHAR;
typedef uint32_t ULONG;
typedef uint64_t ULONG64;
typedef int64_t LONGLONG;

typedef struct _PHYSICAL_ADDRESS
{
    LONGLONG QuadPart;
} PHYSICAL_ADDRESS;

typedef struct _INTERFACE
{
    uint16_t Size;
    uint16_t Version;
    PVOID Context;
    PVOID InterfaceReference;
    PVOID InterfaceDereference;
} INTERFACE;

#define DEFINE_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) static const ULONG name = (l)
#define _In_
#define _Out_

#include "../../droidvmpool_interface.h"

#define PRINT_SIZE(type)           printf("sizeof(" #type ")=%zu\n", sizeof(type))
#define PRINT_OFFSET(type, member) printf("offsetof(" #type "," #member ")=%zu\n", offsetof(type, member))
#define PRINT_VALUE(name)          printf(#name "=%llu\n", (unsigned long long)(name))

int main(void)
{
    PRINT_SIZE(DROIDVMPOOL_QUERY_OUTPUT);
    PRINT_OFFSET(DROIDVMPOOL_QUERY_OUTPUT, InterfaceVersion);
    PRINT_OFFSET(DROIDVMPOOL_QUERY_OUTPUT, BasePhysicalAddress);
    PRINT_OFFSET(DROIDVMPOOL_QUERY_OUTPUT, TotalSize);
    PRINT_OFFSET(DROIDVMPOOL_QUERY_OUTPUT, PoolName);
    PRINT_SIZE(DROIDVMPOOL_MAPPING);
    PRINT_OFFSET(DROIDVMPOOL_MAPPING, BaseVirtualAddress);
    PRINT_OFFSET(DROIDVMPOOL_MAPPING, BasePhysicalAddress);
    PRINT_OFFSET(DROIDVMPOOL_MAPPING, TotalSize);
    PRINT_SIZE(DROIDVMPOOL_DIRECT_INTERFACE);
    PRINT_OFFSET(DROIDVMPOOL_DIRECT_INTERFACE, InterfaceHeader);
    PRINT_OFFSET(DROIDVMPOOL_DIRECT_INTERFACE, AcquireMapping);
    PRINT_OFFSET(DROIDVMPOOL_DIRECT_INTERFACE, ReleaseMapping);
    PRINT_VALUE(DROIDVMPOOL_INTERFACE_VERSION_V1);
    PRINT_VALUE(DROIDVMPOOL_DIRECT_VERSION_V1);
    PRINT_VALUE(DROIDVMPOOL_NAME_CAPACITY);
    return 0;
}
