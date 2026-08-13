/*
 * NetKVM Restricted DMA Pool support
 *
 * Lets NetKVM allocate device-visible (DMA) memory from the rdmapool
 * restricted DMA pool driver when running inside a Gunyah protected VM,
 * where normal guest memory is not accessible to the virtio backend.
 *
 * Mirrors the approach already used by VirtIO/WDF (VirtIOWdf.c / Dma.c)
 * and viostor (viostor_bounce.c).
 *
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct _PARANDIS_ADAPTER *PPARANDIS_ADAPTER_FWD;

    /*
     * Try to connect to the rdmapool device. On success sets
     * pContext->RdmaPoolActive = TRUE and records the pool base VA/PA/size.
     * STATUS_NOT_FOUND means rdmapool is absent and permits normal NDIS DMA.
     * Any other failure is fatal to adapter initialization. Must be called at
     * PASSIVE_LEVEL.
     */
    NTSTATUS ParaNdis_RdmaPoolConnect(PPARANDIS_ADAPTER_FWD pContext);

    /* Freeze allocation and destroy provider allocations. On the first FREE
     * failure, close the file owner so provider cleanup reclaims the rest. */
    NTSTATUS ParaNdis_RdmaPoolReleaseAllocations(PPARANDIS_ADAPTER_FWD pContext);

    /*
     * Release the file owner after ReleaseAllocations succeeds. A FREE that
     * reached IoCallDriver is never retried; terminal failure recovery closes
     * the owner in ReleaseAllocations and publishes local tombstones.
     */
    NTSTATUS ParaNdis_RdmaPoolDisconnect(PPARANDIS_ADAPTER_FWD pContext);

    /*
     * Allocate 'size' bytes (rounded up to pages) from the restricted DMA
     * pool. Returns the kernel VA and fills *pPa with the physical address.
     * Returns NULL on failure. Must be called at PASSIVE_LEVEL.
     */
    PVOID ParaNdis_RdmaPoolAllocate(PPARANDIS_ADAPTER_FWD pContext, ULONG size, PHYSICAL_ADDRESS *pPa);

    /*
     * Free the exact base VA returned by ParaNdis_RdmaPoolAllocate. The stored
     * provider token/page count, not caller-derived data, is sent to rdmapool.
     * Ownership is retained unless FREE succeeds. Must be at PASSIVE_LEVEL.
     */
    NTSTATUS ParaNdis_RdmaPoolFree(PPARANDIS_ADAPTER_FWD pContext, PVOID va, ULONG size);

#ifdef __cplusplus
}
#endif
