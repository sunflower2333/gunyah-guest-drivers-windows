/*
 * DroidVM Pre-Shared Pool Provider
 *
 * One instance binds each ACPI\DRVM0001 device emitted by DroidVM firmware.
 * The ACPI _UID is the pool's wire name. This provider maps the corresponding
 * _CRS memory resource and exposes immutable metadata to kernel clients. It
 * deliberately has no allocator: ownership remains with the side named by the
 * pool contract (for example, drm2kgsl_host is host-owned).
 *
 * Copyright (c) 2026
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#include <ntddk.h>
#include <wdf.h>
#include <acpiioct.h>
#include <initguid.h>

#include "droidvmpool_interface.h"
#include "droidvmpool.h"

#define DROIDVMPOOL_ACPI_UID_NAME    0x4449555FUL /* "_UID" in little-endian byte order. */
#define DROIDVMPOOL_ACPI_OUTPUT_SIZE 256U

C_ASSERT(sizeof(DROIDVMPOOL_QUERY_OUTPUT) == 96);
C_ASSERT(FIELD_OFFSET(DROIDVMPOOL_QUERY_OUTPUT, PoolName) == 32);
C_ASSERT(sizeof(DROIDVMPOOL_MAPPING) == 24);
C_ASSERT(sizeof(DROIDVMPOOL_DIRECT_INTERFACE) == 48);
C_ASSERT(FIELD_OFFSET(DROIDVMPOOL_DIRECT_INTERFACE, AcquireMapping) == 32);
C_ASSERT(FIELD_OFFSET(DROIDVMPOOL_DIRECT_INTERFACE, ReleaseMapping) == 40);

static BOOLEAN DroidVmPoolNameCharacterIsValid(UCHAR character)
{
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '_' || character == '-' || character == '.';
}

static NTSTATUS DroidVmPoolReadUid(_In_ WDFDEVICE device,
                                   _Out_writes_z_(DROIDVMPOOL_NAME_CAPACITY) PCHAR poolName,
                                   _Out_ PULONG poolNameLength)
{
    ACPI_EVAL_INPUT_BUFFER input = {0};
    UCHAR outputStorage[DROIDVMPOOL_ACPI_OUTPUT_SIZE] = {0};
    PACPI_EVAL_OUTPUT_BUFFER output = (PACPI_EVAL_OUTPUT_BUFFER)outputStorage;
    WDF_MEMORY_DESCRIPTOR inputDescriptor;
    WDF_MEMORY_DESCRIPTOR outputDescriptor;
    ULONG_PTR bytesReturned = 0;
    ULONG nameLength;
    SIZE_T argumentOffset;
    SIZE_T dataOffset;
    PACPI_METHOD_ARGUMENT argument;
    NTSTATUS status;
    ULONG index;

    input.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE;
    input.MethodNameAsUlong = DROIDVMPOOL_ACPI_UID_NAME;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inputDescriptor, &input, sizeof(input));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&outputDescriptor, outputStorage, sizeof(outputStorage));

    status = WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(device),
                                               NULL,
                                               IOCTL_ACPI_EVAL_METHOD,
                                               &inputDescriptor,
                                               &outputDescriptor,
                                               NULL,
                                               &bytesReturned);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    argumentOffset = FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER, Argument);
    dataOffset = argumentOffset + FIELD_OFFSET(ACPI_METHOD_ARGUMENT, Data);
    if (bytesReturned < dataOffset || bytesReturned > sizeof(outputStorage) ||
        output->Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE || output->Count != 1 || output->Length < dataOffset ||
        output->Length > bytesReturned)
    {
        return STATUS_ACPI_INVALID_DATA;
    }

    argument = &output->Argument[0];
    if (argument->Type != ACPI_METHOD_ARGUMENT_STRING || argument->DataLength < 2 ||
        argument->DataLength > DROIDVMPOOL_NAME_CAPACITY ||
        ACPI_METHOD_ARGUMENT_LENGTH(argument->DataLength) > output->Length - argumentOffset ||
        dataOffset + argument->DataLength > bytesReturned || argument->Data[argument->DataLength - 1] != '\0')
    {
        return STATUS_ACPI_INVALID_DATA;
    }

    nameLength = argument->DataLength - 1;
    for (index = 0; index < nameLength; ++index)
    {
        if (argument->Data[index] == '\0' || !DroidVmPoolNameCharacterIsValid(argument->Data[index]))
        {
            return STATUS_ACPI_INVALID_DATA;
        }
    }

    RtlCopyMemory(poolName, argument->Data, nameLength);
    poolName[nameLength] = '\0';
    *poolNameLength = nameLength;
    return STATUS_SUCCESS;
}

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT driverObject, _In_ PUNICODE_STRING registryPath)
{
    WDF_DRIVER_CONFIG config;

    WDF_DRIVER_CONFIG_INIT(&config, DroidVmPoolEvtDeviceAdd);
    return WdfDriverCreate(driverObject, registryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}

NTSTATUS DroidVmPoolEvtDeviceAdd(_In_ WDFDRIVER driver, _Inout_ PWDFDEVICE_INIT deviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_QUERY_INTERFACE_CONFIG queryInterfaceConfig;
    DROIDVMPOOL_DIRECT_INTERFACE directInterface = {0};
    PDROIDVMPOOL_DEVICE_CONTEXT deviceContext;
    WDFDEVICE device;
    WDFQUEUE queue;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDevicePrepareHardware = DroidVmPoolEvtDevicePrepareHardware;
    pnpCallbacks.EvtDeviceReleaseHardware = DroidVmPoolEvtDeviceReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(deviceInit, &pnpCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DROIDVMPOOL_DEVICE_CONTEXT);
    deviceAttributes.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfDeviceCreate(&deviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    deviceContext = DroidVmPoolGetDeviceContext(device);
    ExInitializeRundownProtection(&deviceContext->MappingReferences);

    directInterface.InterfaceHeader.Size = sizeof(directInterface);
    directInterface.InterfaceHeader.Version = DROIDVMPOOL_DIRECT_VERSION_V1;
    directInterface.InterfaceHeader.Context = device;
    directInterface.InterfaceHeader.InterfaceReference = DroidVmPoolInterfaceReference;
    directInterface.InterfaceHeader.InterfaceDereference = DroidVmPoolInterfaceDereference;
    directInterface.AcquireMapping = DroidVmPoolAcquireMapping;
    directInterface.ReleaseMapping = DroidVmPoolReleaseMapping;
    WDF_QUERY_INTERFACE_CONFIG_INIT(&queryInterfaceConfig,
                                    (PINTERFACE)&directInterface,
                                    &GUID_DROIDVMPOOL_DIRECT_INTERFACE,
                                    NULL);
    status = WdfDeviceAddQueryInterface(device, &queryInterfaceConfig);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_DROIDVMPOOL, NULL);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = DroidVmPoolEvtIoDeviceControl;
    return WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &queue);
}

NTSTATUS DroidVmPoolEvtDevicePrepareHardware(_In_ WDFDEVICE device,
                                             _In_ WDFCMRESLIST resourcesRaw,
                                             _In_ WDFCMRESLIST resourcesTranslated)
{
    PDROIDVMPOOL_DEVICE_CONTEXT deviceContext = DroidVmPoolGetDeviceContext(device);
    PHYSICAL_ADDRESS poolPhysicalBase = {0};
    SIZE_T poolSize = 0;
    BOOLEAN memoryFound = FALSE;
    NTSTATUS status;
    ULONG resourceCount;
    ULONG index;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR descriptor;
    PVOID poolVirtualBase;

    UNREFERENCED_PARAMETER(resourcesRaw);

    if (deviceContext->PoolVirtualBase != NULL)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (deviceContext->MappingRundownCompleted)
    {
        ExReInitializeRundownProtection(&deviceContext->MappingReferences);
        deviceContext->MappingRundownCompleted = FALSE;
    }
    InterlockedExchange(&deviceContext->PoolReady, FALSE);
    status = DroidVmPoolReadUid(device, deviceContext->PoolName, &deviceContext->PoolNameLength);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    resourceCount = WdfCmResourceListGetCount(resourcesTranslated);
    for (index = 0; index < resourceCount; ++index)
    {
        descriptor = WdfCmResourceListGetDescriptor(resourcesTranslated, index);
        if (descriptor == NULL || descriptor->Type != CmResourceTypeMemory)
        {
            continue;
        }
        if (memoryFound)
        {
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }

        memoryFound = TRUE;
        poolPhysicalBase = descriptor->u.Memory.Start;
        poolSize = descriptor->u.Memory.Length;
    }

    if (!memoryFound || poolPhysicalBase.QuadPart < 0 || poolSize == 0 ||
        ((ULONG64)poolPhysicalBase.QuadPart & (PAGE_SIZE - 1)) != 0 || (poolSize & (PAGE_SIZE - 1)) != 0)
    {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if ((ULONG64)poolPhysicalBase.QuadPart > MAXULONGLONG - ((ULONG64)poolSize - 1))
    {
        return STATUS_INTEGER_OVERFLOW;
    }

#if defined(NTDDI_WINTHRESHOLD) && (NTDDI_VERSION >= NTDDI_WINTHRESHOLD)
    poolVirtualBase = MmMapIoSpaceEx(poolPhysicalBase, poolSize, PAGE_READWRITE);
#else
    poolVirtualBase = MmMapIoSpace(poolPhysicalBase, poolSize, MmCached);
#endif
    if (poolVirtualBase == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    deviceContext->PoolPhysicalBase = poolPhysicalBase;
    deviceContext->PoolSize = poolSize;
    deviceContext->PoolVirtualBase = poolVirtualBase;
    InterlockedExchange(&deviceContext->PoolReady, TRUE);

    DbgPrintEx(DPFLTR_DEFAULT_ID,
               DPFLTR_INFO_LEVEL,
               "droidvmpool: %s PA=0x%llx VA=%p size=0x%Ix\n",
               deviceContext->PoolName,
               deviceContext->PoolPhysicalBase.QuadPart,
               deviceContext->PoolVirtualBase,
               deviceContext->PoolSize);
    return STATUS_SUCCESS;
}

NTSTATUS DroidVmPoolEvtDeviceReleaseHardware(_In_ WDFDEVICE device, _In_ WDFCMRESLIST resourcesTranslated)
{
    PDROIDVMPOOL_DEVICE_CONTEXT deviceContext = DroidVmPoolGetDeviceContext(device);

    UNREFERENCED_PARAMETER(resourcesTranslated);

    InterlockedExchange(&deviceContext->PoolReady, FALSE);
    if (!deviceContext->MappingRundownCompleted)
    {
        ExWaitForRundownProtectionRelease(&deviceContext->MappingReferences);
        deviceContext->MappingRundownCompleted = TRUE;
    }
    if (deviceContext->PoolVirtualBase != NULL)
    {
        MmUnmapIoSpace(deviceContext->PoolVirtualBase, deviceContext->PoolSize);
        deviceContext->PoolVirtualBase = NULL;
    }
    deviceContext->PoolPhysicalBase.QuadPart = 0;
    deviceContext->PoolSize = 0;
    return STATUS_SUCCESS;
}

VOID DroidVmPoolInterfaceReference(_In_ PVOID context)
{
    WdfObjectReference((WDFDEVICE)context);
}

VOID DroidVmPoolInterfaceDereference(_In_ PVOID context)
{
    WdfObjectDereference((WDFDEVICE)context);
}

BOOLEAN DroidVmPoolAcquireMapping(_In_ PVOID context, _Out_ PDROIDVMPOOL_MAPPING mapping)
{
    PDROIDVMPOOL_DEVICE_CONTEXT deviceContext = DroidVmPoolGetDeviceContext((WDFDEVICE)context);
    DROIDVMPOOL_MAPPING mappingValue = {0};

    if (mapping == NULL || KeGetCurrentIrql() > DISPATCH_LEVEL ||
        !ExAcquireRundownProtection(&deviceContext->MappingReferences))
    {
        return FALSE;
    }

    if (InterlockedCompareExchange(&deviceContext->PoolReady, FALSE, FALSE) == FALSE ||
        deviceContext->PoolVirtualBase == NULL || deviceContext->PoolSize == 0)
    {
        ExReleaseRundownProtection(&deviceContext->MappingReferences);
        return FALSE;
    }

    mappingValue.BaseVirtualAddress = deviceContext->PoolVirtualBase;
    mappingValue.BasePhysicalAddress = deviceContext->PoolPhysicalBase;
    mappingValue.TotalSize = deviceContext->PoolSize;
    *mapping = mappingValue;
    return TRUE;
}

VOID DroidVmPoolReleaseMapping(_In_ PVOID context)
{
    PDROIDVMPOOL_DEVICE_CONTEXT deviceContext = DroidVmPoolGetDeviceContext((WDFDEVICE)context);

    NT_ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);
    ExReleaseRundownProtection(&deviceContext->MappingReferences);
}

VOID DroidVmPoolEvtIoDeviceControl(_In_ WDFQUEUE queue,
                                   _In_ WDFREQUEST request,
                                   _In_ size_t outputBufferLength,
                                   _In_ size_t inputBufferLength,
                                   _In_ ULONG ioControlCode)
{
    WDFDEVICE device = WdfIoQueueGetDevice(queue);
    PDROIDVMPOOL_DEVICE_CONTEXT deviceContext = DroidVmPoolGetDeviceContext(device);
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    size_t bytesReturned = 0;

    if (WdfRequestGetRequestorMode(request) != KernelMode)
    {
        WdfRequestComplete(request, STATUS_ACCESS_DENIED);
        return;
    }
    if (!ExAcquireRundownProtection(&deviceContext->MappingReferences))
    {
        WdfRequestComplete(request, STATUS_DEVICE_NOT_READY);
        return;
    }
    if (InterlockedCompareExchange(&deviceContext->PoolReady, FALSE, FALSE) == FALSE)
    {
        ExReleaseRundownProtection(&deviceContext->MappingReferences);
        WdfRequestComplete(request, STATUS_DEVICE_NOT_READY);
        return;
    }

    if (ioControlCode == IOCTL_DROIDVMPOOL_QUERY)
    {
        PDROIDVMPOOL_QUERY_OUTPUT output;

        if (inputBufferLength != 0 || outputBufferLength != sizeof(*output))
        {
            status = STATUS_INFO_LENGTH_MISMATCH;
        }
        else
        {
            status = WdfRequestRetrieveOutputBuffer(request, sizeof(*output), (PVOID *)&output, NULL);
            if (NT_SUCCESS(status))
            {
                DROIDVMPOOL_QUERY_OUTPUT outputValue = {0};
                outputValue.InterfaceVersion = DROIDVMPOOL_INTERFACE_VERSION_V1;
                outputValue.StructureSize = sizeof(outputValue);
                outputValue.PoolNameLength = deviceContext->PoolNameLength;
                outputValue.PageSize = PAGE_SIZE;
                outputValue.BasePhysicalAddress = deviceContext->PoolPhysicalBase;
                outputValue.TotalSize = deviceContext->PoolSize;
                RtlCopyMemory(outputValue.PoolName, deviceContext->PoolName, deviceContext->PoolNameLength + 1);
                *output = outputValue;
                bytesReturned = sizeof(*output);
            }
        }
    }

    ExReleaseRundownProtection(&deviceContext->MappingReferences);
    WdfRequestCompleteWithInformation(request, status, bytesReturned);
}
