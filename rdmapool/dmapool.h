/*
 * Restricted DMA Pool - Bitmap Page Allocator Header
 *
 * Page-granularity bitmap allocator for the restricted DMA pool region.
 *
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#pragma once

#include <ntddk.h>

struct _RDMAPOOL_FILE_CONTEXT;

/*
 * Initialize the DMA pool allocator.
 *
 * @param PhysicalBase  Physical base address of the pool region.
 * @param VirtualBase   Kernel virtual address of the mapped pool region.
 * @param TotalSize     Total pool size in bytes (must be page-aligned).
 * @return STATUS_SUCCESS or STATUS_INSUFFICIENT_RESOURCES.
 */
NTSTATUS
DmaPoolInit(_In_ PHYSICAL_ADDRESS PhysicalBase, _In_ PVOID VirtualBase, _In_ SIZE_T TotalSize);

/*
 * Destroy the DMA pool allocator and free internal resources.
 */
VOID DmaPoolDestroy(VOID);

/*
 * Allocate contiguous pages from the restricted DMA pool.
 *
 * @param Owner              File object that owns the allocation.
 * @param NumPages           Number of pages to allocate.
 * @param VirtualAddress     Receives the kernel VA of the allocation.
 * @param PhysicalAddress    Receives the physical address of the allocation.
 * @param AllocationToken    Receives the opaque allocation identity.
 * @return STATUS_SUCCESS or STATUS_INSUFFICIENT_RESOURCES.
 */
NTSTATUS
DmaPoolAllocatePages(_In_ struct _RDMAPOOL_FILE_CONTEXT *Owner,
                     _In_ ULONG NumPages,
                     _Out_ PVOID *VirtualAddress,
                     _Out_ PHYSICAL_ADDRESS *PhysicalAddress,
                     _Out_ ULONG64 *AllocationToken);

/*
 * Free pages previously allocated from the restricted DMA pool.
 *
 * @param Owner              File object that owns the allocation.
 * @param VirtualAddress     Exact VA returned by DmaPoolAllocatePages.
 * @param NumPages           Exact page count passed to DmaPoolAllocatePages.
 * @param AllocationToken    Exact token returned by DmaPoolAllocatePages.
 * @return STATUS_SUCCESS only when the exact live allocation was consumed.
 */
NTSTATUS
DmaPoolFreePages(_In_ struct _RDMAPOOL_FILE_CONTEXT *Owner,
                 _In_ PVOID VirtualAddress,
                 _In_ ULONG NumPages,
                 _In_ ULONG64 AllocationToken);

/* Reclaim every allocation owned by a closing file object. */
ULONG DmaPoolCloseOwner(_In_ struct _RDMAPOOL_FILE_CONTEXT *Owner);

/*
 * Query pool base addresses and total size.
 */
VOID DmaPoolQueryInfo(_Out_ PVOID *BaseVirtualAddress,
                      _Out_ PHYSICAL_ADDRESS *BasePhysicalAddress,
                      _Out_ ULONG64 *TotalSize);

VOID DmaPoolQueryAllocation(_Out_ ULONG *FreePages, _Out_ ULONG *LargestFreeRunPages);
