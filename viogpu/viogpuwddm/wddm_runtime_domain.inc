/* SPDX-License-Identifier: BSD-3-Clause */
/* Included in the resident WDDM bridge. Device-local numeric identities never
 * cross a device boundary, and no caller-supplied pointer is dereferenced. */
static VIOGPU_NATIVE_CONTEXT_REGISTRATION *NativeRegistration(const VIOGPU_WDDM_CONTEXT *context)
{
    VIOGPU_WDDM_CONTEXT *owner = context->DomainOwner;
    return owner != NULL ? &owner->NativeContext
                         : const_cast<VIOGPU_NATIVE_CONTEXT_REGISTRATION *>(&context->NativeContext);
}

static VOID PublishNativeDomain(VIOGPU_WDDM_CONTEXT *context)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&context->NativeContext.BindingLock, &oldIrql);
    context->DomainContextId = context->NativeContext.ContextId;
    context->DomainResetGeneration = context->NativeContext.ResetGeneration;
    KeReleaseSpinLock(&context->NativeContext.BindingLock, oldIrql);
    KeAcquireSpinLock(&context->Device->DomainLock, &oldIrql);
    InsertTailList(&context->Device->NativeDomains, &context->DomainLink);
    context->DomainPublished = TRUE;
    KeReleaseSpinLock(&context->Device->DomainLock, oldIrql);
}

static NTSTATUS AttachNativeDomain(VIOGPU_WDDM_CONTEXT *child, UINT id, ULONGLONG generation)
{
    if (child->DomainOwner != NULL || id == 0 || generation == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }
    VIOGPU_WDDM_DEVICE *device = child->Device;
    NTSTATUS status = STATUS_INVALID_HANDLE;
    KIRQL oldIrql;
    KeAcquireSpinLock(&device->DomainLock, &oldIrql);
    for (PLIST_ENTRY link = device->NativeDomains.Flink; link != &device->NativeDomains; link = link->Flink)
    {
        VIOGPU_WDDM_CONTEXT *owner = CONTAINING_RECORD(link, VIOGPU_WDDM_CONTEXT, DomainLink);
        if (owner->Device != device || owner->DomainContextId != id ||
            owner->DomainResetGeneration != generation || owner->DomainClosing || !owner->DomainPublished ||
            owner->DomainOwner != NULL || owner->DomainChildren == MAXULONG)
        {
            continue;
        }
        ++owner->DomainChildren;
        child->DomainOwner = owner;
        status = STATUS_SUCCESS;
        break;
    }
    KeReleaseSpinLock(&device->DomainLock, oldIrql);
    return status;
}

static VOID DetachNativeDomain(VIOGPU_WDDM_CONTEXT *child)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&child->Device->DomainLock, &oldIrql);
    VIOGPU_WDDM_CONTEXT *owner = child->DomainOwner;
    NT_ASSERT(owner != NULL && owner->Device == child->Device && owner->DomainChildren != 0);
    --owner->DomainChildren;
    child->DomainOwner = NULL;
    KeReleaseSpinLock(&child->Device->DomainLock, oldIrql);
    /* No owner access after dropping the lock: its blocked destroy may retry. */
}

static NTSTATUS CloseNativeDomain(VIOGPU_WDDM_CONTEXT *context)
{
    if (context->Device == NULL)
    {
        return STATUS_INVALID_HANDLE;
    }
    if (context->Type != VioGpuWddmContextNative || context->DomainOwner != NULL)
    {
        return STATUS_SUCCESS;
    }
    KIRQL oldIrql;
    KeAcquireSpinLock(&context->Device->DomainLock, &oldIrql);
    context->DomainClosing = TRUE;
    if (context->DomainPublished)
    {
        RemoveEntryList(&context->DomainLink);
        InitializeListHead(&context->DomainLink);
        context->DomainPublished = FALSE;
    }
    NTSTATUS status = context->DomainChildren == 0 ? STATUS_SUCCESS : STATUS_DEVICE_BUSY;
    KeReleaseSpinLock(&context->Device->DomainLock, oldIrql);
    return status;
}
