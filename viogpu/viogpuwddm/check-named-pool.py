#!/usr/bin/env python3

import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


PROJECT_DIR = Path(__file__).resolve().parent
REPO_DIR = PROJECT_DIR.parent.parent
SOURCE_PATH = PROJECT_DIR.parent / "common" / "viogpu_named_pool.cpp"
HEADER_PATH = PROJECT_DIR.parent / "common" / "viogpu_named_pool.h"
ADAPTER_SOURCE_PATH = PROJECT_DIR.parent / "viogpudo" / "viogpudo.cpp"
ADAPTER_HEADER_PATH = PROJECT_DIR.parent / "viogpudo" / "viogpudo.h"
INTERFACE_PATH = REPO_DIR / "droidvmpool" / "droidvmpool_interface.h"
PROJECT_PATH = PROJECT_DIR / "viogpuwddm.vcxproj"
WORKFLOW_PATH = REPO_DIR / ".github" / "workflows" / "viogpuwddm-arm64-ci.yml"
NAMESPACE = {"msbuild": "http://schemas.microsoft.com/developer/msbuild/2003"}


def fail(message: str) -> None:
    print(f"viogpu named-pool contract failure: {message}", file=sys.stderr)
    raise SystemExit(1)


def strip_comments_and_literals(source: str) -> str:
    result = list(source)

    def blank(start: int, end: int) -> None:
        for offset in range(start, end):
            if result[offset] not in "\r\n":
                result[offset] = " "

    offset = 0
    while offset < len(source):
        if source.startswith("//", offset):
            end = source.find("\n", offset + 2)
            end = len(source) if end == -1 else end
            blank(offset, end)
            offset = end
            continue
        if source.startswith("/*", offset):
            end = source.find("*/", offset + 2)
            end = len(source) if end == -1 else end + 2
            blank(offset, end)
            offset = end
            continue
        if source[offset] in "\"'":
            quote = source[offset]
            end = offset + 1
            while end < len(source):
                if source[end] == "\\":
                    end = min(end + 2, len(source))
                    continue
                if source[end] == quote:
                    end += 1
                    break
                end += 1
            blank(offset, end)
            offset = end
            continue
        offset += 1
    return "".join(result)


def compact(source: str) -> str:
    return re.sub(r"\s+", "", source)


def function_body(name: str, source: str) -> str:
    matches = list(re.finditer(rf"\b{re.escape(name)}\s*\([^;{{}}]*?\)\s*(?:const\s*)?{{", source, re.DOTALL))
    if len(matches) != 1:
        fail(f"expected one definition of {name}, found {len(matches)}")

    start = matches[0].end() - 1
    depth = 0
    for offset in range(start, len(source)):
        if source[offset] == "{":
            depth += 1
        elif source[offset] == "}":
            depth -= 1
            if depth == 0:
                return source[start + 1 : offset]
    fail(f"unterminated function {name}")
    return ""


def require_once(source: str, token: str, message: str) -> None:
    if source.count(token) != 1:
        fail(message)


def check_client(source_text: str, header_text: str, interface_text: str) -> None:
    source = strip_comments_and_literals(source_text)
    code = compact(source)
    connect = compact(function_body("VioGpuDrmHostPool::Connect", source))
    open_connection = compact(function_body("OpenNamedPoolConnection", source))
    release_connection = compact(function_body("ReleaseNamedPoolConnection", source))
    duplicate_name = compact(function_body("DuplicateInterfaceName", source))
    validate = compact(function_body("ValidateNamedPoolQuery", source))
    validate_direct = compact(function_body("ValidateDirectInterface", source))
    validate_mapping = compact(function_body("ValidateNamedPoolMapping", source))
    query_direct = compact(function_body("NamedPoolQueryDirectInterface", source))
    disconnect = compact(function_body("VioGpuDrmHostPool::Disconnect", source))
    callback = compact(function_body("VioGpuDrmHostPool::PnpNotificationCallback", source))
    handle_pnp = compact(function_body("VioGpuDrmHostPool::HandlePnpNotification", source))
    query_remove = compact(function_body("VioGpuDrmHostPool::HandleQueryRemove", source))
    remove_cancelled = compact(function_body("VioGpuDrmHostPool::HandleRemoveCancelled", source))
    remove_complete = compact(function_body("VioGpuDrmHostPool::HandleRemoveComplete", source))
    has_owner = compact(function_body("VioGpuDrmHostPool::HasConnectionOwner", source))
    acquire = compact(function_body("VioGpuDrmHostPool::AcquireMapping", source))
    release_mapping = compact(function_body("VioGpuDrmHostPool::ReleaseMapping", source))

    if source_text.count('"drm2kgsl_host"') != 1 or "gpu_guest" in source_text:
        fail("client must select only the exact drm2kgsl_host wire name")
    for assertion in (
        "static_assert(sizeof(DROIDVMPOOL_QUERY_OUTPUT)==96",
        "static_assert(FIELD_OFFSET(DROIDVMPOOL_QUERY_OUTPUT,PoolName)==32",
        "static_assert(sizeof(DROIDVMPOOL_MAPPING)==24",
        "static_assert(sizeof(DROIDVMPOOL_DIRECT_INTERFACE)==48",
        "static_assert(FIELD_OFFSET(DROIDVMPOOL_DIRECT_INTERFACE,AcquireMapping)==32",
        "static_assert(FIELD_OFFSET(DROIDVMPOOL_DIRECT_INTERFACE,ReleaseMapping)==40",
    ):
        require_once(code, assertion, f"client must retain provider ABI assertion: {assertion}")
    for forbidden in ("IOCTL_DROIDVMPOOL_ALLOC", "IOCTL_DROIDVMPOOL_FREE", "RtlZeroMemory(m_BaseVA", "MmMapIoSpace"):
        if forbidden in source_text:
            fail(f"host-owned pool client must not allocate, free, clear, or remap storage: {forbidden}")

    for token in (
        "IoGetDeviceObjectPointer(deviceName,FILE_ALL_ACCESS,&fileObject,&deviceObject)",
        "NamedPoolQuery(deviceObject,fileObject,&query)",
        "ValidateNamedPoolQuery(&query,ioctlResult.Information,isDrmHost)",
        "if(!NT_SUCCESS(status)||!*isDrmHost){ObDereferenceObject(fileObject);returnstatus;}",
        "NamedPoolQueryDirectInterface(deviceObject,&directInterface)",
        "ValidateDirectInterface(&directInterface)",
        "directInterface.AcquireMapping(directInterface.InterfaceHeader.Context,&mapping)",
        "ValidateNamedPoolMapping(&mapping,&query)",
        "directInterface.ReleaseMapping(directInterface.InterfaceHeader.Context);",
        "connection->FileObject=fileObject;",
        "connection->DirectInterface=directInterface;",
        "connection->Query=query;",
    ):
        require_once(open_connection, token, f"target open must retain validated direct-interface step: {token}")

    release_direct = release_connection.find("DereferenceDirectInterface(&connection->DirectInterface);")
    release_file = release_connection.find("ObDereferenceObject(connection->FileObject);")
    if min(release_direct, release_file) < 0 or release_direct > release_file:
        fail("connection release must dereference the direct interface before the file object")
    for token in (
        "source->Length>MAXUSHORT-sizeof(WCHAR)",
        "ExAllocatePoolUninitialized(PagedPool,maximumLength,VIOGPU_NAMED_POOL_TAG)",
        "buffer[source->Length/sizeof(WCHAR)]=",
    ):
        require_once(duplicate_name, token, f"saved target symbolic link must remain bounded and terminated: {token}")

    for token in (
        "GetNamedPoolNotificationDriverObject()",
        "IoGetDeviceInterfaces(&GUID_DEVINTERFACE_DROIDVMPOOL,NULL,0,&interfaceList)",
        "for(PWSTRinterfaceName=interfaceList;",
        "interfaceName+=(deviceName.Length/sizeof(WCHAR))+1)",
        "status=OpenNamedPoolConnection(&deviceName,&connection,&isDrmHost);",
        "if(!isDrmHost){continue;}",
        "++matchCount;",
        "status=DuplicateInterfaceName(&deviceName,&selectedName);",
        "if(!NT_SUCCESS(firstFailure)||matchCount!=1)",
        "IoRegisterPlugPlayNotification(EventCategoryTargetDeviceChange,0,selectedConnection.FileObject,notificationDriverObject,PnpNotificationCallback,this,&notificationEntry)",
        "m_NotificationDriverObject=notificationDriverObject;",
        "m_NotificationEntry=notificationEntry;",
        "m_InterfaceName=selectedName;",
        "m_FileObject=selectedConnection.FileObject;",
        "m_DirectInterface=selectedConnection.DirectInterface;",
        "m_PnpState=VioGpuDrmHostPoolConnected;",
        "InterlockedExchange(&m_Ready,TRUE);",
    ):
        require_once(connect, token, f"enumeration must retain exact fail-closed selection/PnP step: {token}")
    if connect.count("ReleaseNamedPoolConnection(&selectedConnection);") != 2 or connect.count(
        "FreeInterfaceName(&selectedName);"
    ) != 2:
        fail("selection and registration failures must release both the provisional target and saved name")
    free_interfaces = connect.find("ExFreePool(interfaceList);")
    register_target = connect.find("IoRegisterPlugPlayNotification(EventCategoryTargetDeviceChange")
    publish_connection = connect.find("m_FileObject=selectedConnection.FileObject;")
    publish_ready = connect.find("InterlockedExchange(&m_Ready,TRUE);")
    if min(free_interfaces, register_target, publish_connection, publish_ready) < 0 or not (
        free_interfaces < register_target < publish_connection < publish_ready
    ):
        fail("client must free discovery storage, register target PnP, then publish the connection and readiness")

    for token in (
        "information!=sizeof(*query)",
        "query->InterfaceVersion!=DROIDVMPOOL_INTERFACE_VERSION_V1",
        "query->StructureSize!=sizeof(*query)",
        "query->PoolNameLength==0",
        "query->PoolNameLength>=DROIDVMPOOL_NAME_CAPACITY",
        "query->PoolName[query->PoolNameLength]!=",
        "query->PageSize!=PAGE_SIZE",
        "query->TotalSize>MAXULONG_PTR",
        "!NamedPoolCharacterIsValid(query->PoolName[index])",
        "for(ULONGindex=query->PoolNameLength+1;index<DROIDVMPOOL_NAME_CAPACITY;++index)",
        "if(query->PoolName[index]!=){returnSTATUS_DATA_ERROR;}",
        "query->PoolNameLength==sizeof(VIOGPU_DRM_HOST_POOL_NAME)-1",
        "RtlCompareMemory(query->PoolName,VIOGPU_DRM_HOST_POOL_NAME,sizeof(VIOGPU_DRM_HOST_POOL_NAME)-1)",
    ):
        require_once(validate, token, f"provider query must retain ABI/name/bounds validation: {token}")

    for token in (
        "directInterface->InterfaceHeader.Size!=sizeof(*directInterface)",
        "directInterface->InterfaceHeader.Version!=DROIDVMPOOL_DIRECT_VERSION_V1",
        "directInterface->InterfaceHeader.Context==NULL",
        "directInterface->InterfaceHeader.InterfaceReference==NULL",
        "directInterface->InterfaceHeader.InterfaceDereference==NULL",
        "directInterface->AcquireMapping==NULL",
        "directInterface->ReleaseMapping==NULL",
    ):
        require_once(validate_direct, token, f"direct interface must retain callback validation: {token}")
    for token in (
        "IoGetAttachedDeviceReference(deviceObject)",
        "IoBuildSynchronousFsdRequest(IRP_MJ_PNP,targetObject",
        "irpStack->MinorFunction=IRP_MN_QUERY_INTERFACE;",
        "irpStack->Parameters.QueryInterface.InterfaceType=(LPGUID)&GUID_DROIDVMPOOL_DIRECT_INTERFACE;",
        "irpStack->Parameters.QueryInterface.Size=sizeof(*directInterface);",
        "irpStack->Parameters.QueryInterface.Version=DROIDVMPOOL_DIRECT_VERSION_V1;",
        "irp->IoStatus.Status=STATUS_NOT_SUPPORTED;",
    ):
        require_once(query_direct, token, f"direct-interface query must retain synchronous PnP step: {token}")
    if query_direct.count("ObDereferenceObject(targetObject);") != 2:
        fail("direct-interface query must release the target reference on allocation failure and completion")
    for token in (
        "mapping->BaseVirtualAddress==NULL",
        "mapping->BasePhysicalAddress.QuadPart!=query->BasePhysicalAddress.QuadPart",
        "mapping->TotalSize!=query->TotalSize",
    ):
        require_once(validate_mapping, token, f"initial mapping must match discovery metadata: {token}")

    for token in (
        "ExAcquireRundownProtection(&pool->m_NotificationCallbacks)",
        "status=pool->HandlePnpNotification(notificationStructure);",
        "ExReleaseRundownProtection(&pool->m_NotificationCallbacks);",
    ):
        require_once(callback, token, f"PnP callback must hold its object-lifetime rundown: {token}")
    callback_acquire = callback.find("ExAcquireRundownProtection(&pool->m_NotificationCallbacks)")
    callback_dispatch = callback.find("pool->HandlePnpNotification(notificationStructure)")
    callback_release = callback.find("ExReleaseRundownProtection(&pool->m_NotificationCallbacks)")
    if not 0 <= callback_acquire < callback_dispatch < callback_release:
        fail("PnP callback must hold notification rundown across dispatch")

    for token in (
        "GUID_TARGET_DEVICE_QUERY_REMOVE",
        "HandleQueryRemove(removal->FileObject)",
        "GUID_TARGET_DEVICE_REMOVE_CANCELLED",
        "HandleRemoveCancelled()",
        "GUID_TARGET_DEVICE_REMOVE_COMPLETE",
        "HandleRemoveComplete()",
    ):
        require_once(handle_pnp, token, f"target-device callback must dispatch every removal event: {token}")

    query_withdraw = query_remove.find("InterlockedExchange(&m_Ready,FALSE);")
    query_drain = query_remove.find("ExWaitForRundownProtectionRelease(&m_Operations);")
    query_clear = query_remove.find("RtlZeroMemory(&m_DirectInterface,sizeof(m_DirectInterface));")
    query_state = query_remove.find("m_PnpState=VioGpuDrmHostPoolQueryRemove;")
    query_unlock = query_remove.find("UnlockState();", query_state)
    query_release = query_remove.find("ReleaseNamedPoolConnection(&connection);", query_unlock)
    if min(query_withdraw, query_drain, query_clear, query_state, query_unlock, query_release) < 0 or not (
        query_withdraw < query_drain < query_clear < query_state < query_unlock < query_release
    ):
        fail("query-remove must withdraw readiness, drain leases, detach the interface, and release the target")
    if "m_NotificationEntry=NULL" in query_remove or "IoUnregisterPlugPlayNotification" in query_remove:
        fail("query-remove must retain the old registration for remove-cancelled or remove-complete")
    if "m_BasePA.QuadPart=0" in query_remove or "m_Size=0" in query_remove:
        fail("query-remove must retain the selected pool identity for remove-cancelled validation")
    require_once(
        query_remove,
        "notificationFileObject!=m_FileObject",
        "query-remove must verify the notification belongs to the retained target file object",
    )

    cancel_detach = remove_cancelled.find("oldNotificationEntry=m_NotificationEntry;")
    cancel_unregister = remove_cancelled.find("IoUnregisterPlugPlayNotificationEx(oldNotificationEntry);")
    cancel_open = remove_cancelled.find("OpenNamedPoolConnection(&m_InterfaceName,&connection,&isDrmHost);")
    cancel_identity = remove_cancelled.find(
        "connection.Query.BasePhysicalAddress.QuadPart!=m_BasePA.QuadPart"
    )
    cancel_register = remove_cancelled.find("IoRegisterPlugPlayNotification(EventCategoryTargetDeviceChange")
    cancel_reinitialize = remove_cancelled.find("ExReInitializeRundownProtection(&m_Operations);")
    cancel_publish = remove_cancelled.find("m_FileObject=connection.FileObject;")
    cancel_ready = remove_cancelled.find("InterlockedExchange(&m_Ready,TRUE);")
    if min(
        cancel_detach,
        cancel_unregister,
        cancel_open,
        cancel_identity,
        cancel_register,
        cancel_reinitialize,
        cancel_publish,
        cancel_ready,
    ) < 0 or not (
        cancel_detach
        < cancel_unregister
        < cancel_open
        < cancel_identity
        < cancel_register
        < cancel_reinitialize
        < cancel_publish
        < cancel_ready
    ):
        fail("remove-cancelled must replace the old registration, requery the exact target, and publish a new lease epoch")
    for token in (
        "NT_ASSERT(m_NotificationEntry==NULL||m_NotificationEntry==oldNotificationEntry);",
        "if(m_NotificationEntry==NULL){m_NotificationEntry=oldNotificationEntry;}",
    ):
        require_once(
            remove_cancelled,
            token,
            "remove-cancelled must retain an unregister-failed entry even during concurrent disconnect",
        )

    complete_withdraw = remove_complete.find("InterlockedExchange(&m_Ready,FALSE);")
    complete_drain = remove_complete.find("ExWaitForRundownProtectionRelease(&m_Operations);")
    complete_detach = remove_complete.find("m_NotificationEntry=NULL;")
    complete_release = remove_complete.find("ReleaseNamedPoolConnection(&connection);")
    complete_unregister = remove_complete.find("IoUnregisterPlugPlayNotificationEx(notificationEntry);")
    if min(complete_withdraw, complete_drain, complete_detach, complete_release, complete_unregister) < 0 or not (
        complete_withdraw < complete_drain < complete_detach < complete_unregister < complete_release
    ):
        fail("remove-complete must unregister before releasing surprise-removed target references")
    for token in (
        "NT_ASSERT(m_NotificationEntry==NULL||m_NotificationEntry==notificationEntry);",
        "if(m_NotificationEntry==NULL){m_NotificationEntry=notificationEntry;}",
    ):
        require_once(
            remove_complete,
            token,
            "remove-complete must retain an unregister-failed entry even during concurrent disconnect",
        )
    for token in (
        "m_FileObject=connection.FileObject;",
        "m_DirectInterface=connection.DirectInterface;",
        "RtlZeroMemory(&connection,sizeof(connection));",
    ):
        require_once(
            remove_complete,
            token,
            "surprise-remove unregister failure must restore the complete target connection",
        )
    if "!m_ShuttingDown&&m_NotificationEntry==NULL" in remove_cancelled or (
        "!m_ShuttingDown&&m_NotificationEntry==NULL" in remove_complete
    ):
        fail("callback unregister failure must not hide a live registration from concurrent disconnect")

    withdraw = disconnect.find("InterlockedExchange(&m_Ready,FALSE);")
    drain = disconnect.find("ExWaitForRundownProtectionRelease(&m_Operations);")
    clear_direct = disconnect.find("RtlZeroMemory(&m_DirectInterface,sizeof(m_DirectInterface));")
    release_target = disconnect.find("ReleaseNamedPoolConnection(&connection);")
    unregister = disconnect.find("IoUnregisterPlugPlayNotificationEx(notificationEntry);")
    drain_callbacks = disconnect.find("ExWaitForRundownProtectionRelease(&m_NotificationCallbacks);")
    free_name = disconnect.find("FreeInterfaceName(&m_InterfaceName);")
    disconnected = disconnect.find("m_PnpState=VioGpuDrmHostPoolDisconnected;")
    if min(withdraw, drain, clear_direct, release_target, unregister, drain_callbacks, free_name, disconnected) < 0 or not (
        withdraw < drain < clear_direct < unregister < drain_callbacks < release_target < free_name < disconnected
    ):
        fail("disconnect must unregister and drain callbacks before releasing the target connection")
    if disconnect.count("IoUnregisterPlugPlayNotificationEx(notificationEntry)") != 2:
        fail("disconnect must unregister both the initial entry and any entry published by an in-flight callback")
    for token in (
        "if(m_DisconnectInProgress){UnlockState();returnSTATUS_DEVICE_BUSY;}",
        "m_DisconnectInProgress=TRUE;",
    ):
        require_once(disconnect, token, f"disconnect must serialize teardown and restore failed ownership: {token}")
    for token in ("m_FileObject=connection.FileObject;", "m_DirectInterface=connection.DirectInterface;"):
        if disconnect.count(token) != 2:
            fail(f"both unregister failure exits must restore the target connection: {token}")
    if disconnect.count("m_DisconnectInProgress=FALSE;") != 3:
        fail("every disconnect success/failure exit must release the single-flight owner")
    for token in (
        "m_PnpState!=VioGpuDrmHostPoolDisconnected",
        "m_NotificationDriverObject!=NULL",
        "m_NotificationEntry!=NULL",
        "m_InterfaceName.Buffer!=NULL",
        "m_FileObject!=NULL",
        "m_DirectInterface.InterfaceHeader.Context!=NULL",
    ):
        require_once(has_owner, token, f"connection ownership must include every PnP lifetime component: {token}")

    for token in (
        "mapping==NULL",
        "mapping->m_Owner!=NULL",
        "KeGetCurrentIrql()>DISPATCH_LEVEL",
        "!ExAcquireRundownProtection(&m_Operations)",
        "m_DirectInterface.AcquireMapping(m_DirectInterface.InterfaceHeader.Context,&mappingValue)",
        "mappingValue.BasePhysicalAddress.QuadPart!=m_BasePA.QuadPart",
        "mappingValue.TotalSize!=m_Size",
        "mapping->m_Owner=this;",
    ):
        require_once(acquire, token, f"client acquire must retain nested lease validation: {token}")
    release_direct = release_mapping.find(
        "m_DirectInterface.ReleaseMapping(m_DirectInterface.InterfaceHeader.Context);"
    )
    clear_owner = release_mapping.find("mapping->m_Owner=NULL;")
    release_local = release_mapping.find("ExReleaseRundownProtection(&m_Operations);")
    if min(release_direct, clear_owner, release_local) < 0 or not release_direct < clear_owner < release_local:
        fail("client release must drop provider lease before local rundown")

    if "Allocate" in header_text or re.search(r"\bFree\s*\(", header_text):
        fail("named host-pool client header must not expose allocation operations")
    if re.search(r"\bQuery\s*\(", header_text):
        fail("client must not expose a raw mapping query without a lease")
    if "VioGpuDrmHostPoolMapping(const VioGpuDrmHostPoolMapping &);" not in header_text or (
        "VioGpuDrmHostPoolMapping &operator=(const VioGpuDrmHostPoolMapping &);" not in header_text
    ):
        fail("mapping lease must remain non-copyable")
    for token in (
        "static DRIVER_NOTIFICATION_CALLBACK_ROUTINE PnpNotificationCallback;",
        "mutable KMUTEX m_StateLock;",
        "EX_RUNDOWN_REF m_NotificationCallbacks;",
        "BOOLEAN m_NotificationRundownCompleted;",
        "BOOLEAN m_DisconnectInProgress;",
        "PDRIVER_OBJECT m_NotificationDriverObject;",
        "PVOID m_NotificationEntry;",
        "UNICODE_STRING m_InterfaceName;",
    ):
        if header_text.count(token) != 1:
            fail(f"named-pool client must retain remote-PnP lifetime state: {token}")
    if "DROIDVMPOOL_INTERFACE_VERSION_V1" not in interface_text or "IOCTL_DROIDVMPOOL_QUERY" not in interface_text:
        fail("client must consume the versioned provider query ABI")


def check_adapter(adapter_text: str, adapter_header_text: str) -> None:
    adapter = strip_comments_and_literals(adapter_text)
    start = compact(function_body("VioGpuAdapter::StartNativeContextTransport", adapter))
    begin = compact(function_body("VioGpuAdapter::BeginNativeContextInitialization", adapter))
    complete = compact(function_body("VioGpuAdapter::CompleteNativeContextInitialization", adapter))
    query_readiness = compact(function_body("VioGpuAdapter::QueryNativeContextReadiness", adapter))
    generation_current = compact(function_body("VioGpuAdapter::IsNativeContextGenerationCurrent", adapter))
    stop = compact(function_body("VioGpuAdapter::StopNativeContextTransportLocked", adapter))
    destructor = compact(function_body("VioGpuAdapter::~VioGpuAdapter", adapter))

    rdma = start.find("status=ConnectRestrictedDma();")
    named = start.find("status=ConnectDrmHostPool();")
    virtio = start.find("status=VioGpuAdapterInit(pDispInfo);")
    if min(rdma, named, virtio) < 0 or not rdma < named < virtio:
        fail("adapter must connect restricted DMA, then drm2kgsl_host, before VirtIO initialization")
    require_once(begin, "m_DrmHostPool.HasConnectionOwner()", "initialization must reject a stale named-pool owner")
    require_once(complete, "m_DrmHostPool.IsActive()", "Ready publication must require the named pool connection")
    if query_readiness.count("m_DrmHostPool.IsActive()") != 2:
        fail("readiness snapshots must check named-pool activity before and after copying published state")
    require_once(
        generation_current,
        "m_DrmHostPool.IsActive()",
        "context generation must become invalid while the remote pool is unavailable",
    )
    named_close = stop.find("status=m_DrmHostPool.Disconnect();")
    named_failure = stop.find("if(!NT_SUCCESS(status)){FailNativeContextAtAnyIrql();returnstatus;}", named_close)
    rdma_close = stop.find("status=m_RdmaPool.Disconnect();")
    offline = stop.find("InterlockedExchange(&m_NativeContextState,VioGpuNativeContextOffline);")
    if min(named_close, named_failure, rdma_close, offline) < 0 or not (
        named_close < named_failure < rdma_close < offline
    ):
        fail("teardown must propagate named-pool unregister failure before RDMA and Offline publication")
    if destructor.count("m_DrmHostPool.HasConnectionOwner()") != 2:
        fail("destructor must detect and then assert release of the named-pool owner")
    require_once(destructor, "NT_ASSERT(!m_DrmHostPool.HasConnectionOwner());", "destructor must require named-pool release")

    if adapter_text.count("AcquireDrmHostPoolMapping") != 1 or adapter_header_text.count(
        "AcquireDrmHostPoolMapping"
    ) != 1:
        fail("mapping access must remain unused until no-suspend and cache/coherency contracts are implemented")

    guarded_header = compact(adapter_header_text)
    for token in (
        '#ifdefined(VIOGPU_WDDM_CI_ONLY)#include"viogpu_named_pool.h"#endif',
        "#ifdefined(VIOGPU_WDDM_CI_ONLY)VioGpuDrmHostPoolm_DrmHostPool;#endif",
    ):
        require_once(guarded_header, token, "stable Display-Only target must not acquire the compile-only named-pool client")


def check_project_and_workflow(workflow: str) -> None:
    root = ET.parse(PROJECT_PATH).getroot()
    compile_inputs = [
        element.attrib["Include"].replace("\\", "/")
        for element in root.findall(".//msbuild:ClCompile[@Include]", NAMESPACE)
    ]
    header_inputs = [
        element.attrib["Include"].replace("\\", "/")
        for element in root.findall(".//msbuild:ClInclude[@Include]", NAMESPACE)
    ]
    if compile_inputs.count("../common/viogpu_named_pool.cpp") != 1:
        fail("full WDDM project must compile the named-pool client exactly once")
    for required in ("../common/viogpu_named_pool.h", "../../droidvmpool/droidvmpool_interface.h"):
        if header_inputs.count(required) != 1:
            fail(f"full WDDM project must track {required} exactly once")
    require_once(
        workflow,
        "python viogpu/viogpuwddm/check-named-pool.py",
        "ARM64 workflow must run the named-pool checker exactly once",
    )
    if "droidvmpool.sys" in workflow:
        fail("compile-only workflow must not upload or install the provider")


def main() -> None:
    workflow = WORKFLOW_PATH.read_text(encoding="utf-8")
    check_client(
        SOURCE_PATH.read_text(encoding="utf-8"),
        HEADER_PATH.read_text(encoding="utf-8"),
        INTERFACE_PATH.read_text(encoding="utf-8"),
    )
    check_adapter(
        ADAPTER_SOURCE_PATH.read_text(encoding="utf-8"),
        ADAPTER_HEADER_PATH.read_text(encoding="utf-8"),
    )
    check_project_and_workflow(workflow)
    print("viogpu named-pool client contract: PASS")


if __name__ == "__main__":
    main()
