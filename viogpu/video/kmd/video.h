/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef VIOGPU_VIDEO_KMD_H
#define VIOGPU_VIDEO_KMD_H
#include <ntddk.h>
#include <wdf.h>
#include "osdep.h"
#include "VirtIOWdf.h"
#include "../include/video_ioctl.h"
#define VV_TAG 'dVGV'
#define VV_EVENT_SLOTS 64u
#define VV_EVENT_RING 256u
#define VV_MEMORY_LIMIT (256ull*1024ull*1024ull)
typedef struct {
    PVOID va;
    PHYSICAL_ADDRESS pa;
    VV_BUFFER_STATE ownership;
} VV_ALLOCATION;
typedef struct {
    VIRTIO_WDF_DRIVER virtio;
    WDFDEVICE device;
    WDFINTERRUPT interrupt;
    WDFSPINLOCK lock;
    WDFWAITLOCK gate;
    struct virtqueue *queues[2];
    PVOID command_memory, event_memory;
    PHYSICAL_ADDRESS command_pa, event_pa;
    KEVENT command_done;
    ULONG command_used;
    BOOLEAN pending, initialized, queues_created, have_session;
    volatile LONG online, faulted;
    uint32_t session;
    VV_CONFIG config;
    VV_EVENT events[VV_EVENT_RING];
    ULONG read_at, event_count;
    VV_ALLOCATION buffers[2][VV_MAX_BUFFERS];
    uint32_t counts[2], queue_types[2];
    uint64_t allocated;
} VV_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VV_DEVICE, VvDevice);
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD VvAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE VvPrepare;
EVT_WDF_DEVICE_RELEASE_HARDWARE VvRelease;
EVT_WDF_DEVICE_D0_ENTRY VvD0Entry;
EVT_WDF_DEVICE_D0_EXIT VvD0Exit;
EVT_WDF_INTERRUPT_ISR VvIsr;
EVT_WDF_INTERRUPT_DPC VvDpc;
EVT_WDF_INTERRUPT_ENABLE VvInterruptEnable;
EVT_WDF_INTERRUPT_DISABLE VvInterruptDisable;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL VvControl;
EVT_WDF_FILE_CLEANUP VvCleanup;
/* Serialize a synchronous command at PASSIVE_LEVEL, with device-owned backing. */
NTSTATUS VvTransact(VV_DEVICE *d, const void *command, ULONG command_size,
                    void *reply, ULONG reply_capacity, ULONG *reply_size);
/* Execute a fixed-layout V4L2 command on the exclusively opened session. */
NTSTATUS VvIoctl(VV_DEVICE *d, uint32_t code, void *payload, ULONG input_size,
                 ULONG output_size, int32_t *error);
/* Release only allocations whose host access has ended by CLOSE/reset. */
void VvFreeBuffers(VV_DEVICE *d);
/* Close the host session; failure retains all backing until device reset. */
NTSTATUS VvClose(VV_DEVICE *d);
#endif
