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


def function_body_with_parameters(name: str, parameters: str, source: str) -> str:
    expected = compact(parameters)
    matches = [
        match
        for match in re.finditer(
            rf"\b{re.escape(name)}\s*\((?P<parameters>[^;{{}}]*?)\)\s*(?:const\s*)?{{", source, re.DOTALL
        )
        if compact(match.group("parameters")) == expected
    ]
    if len(matches) != 1:
        fail(f"expected one definition of {name}({parameters}), found {len(matches)}")

    start = matches[0].end() - 1
    depth = 0
    for offset in range(start, len(source)):
        if source[offset] == "{":
            depth += 1
        elif source[offset] == "}":
            depth -= 1
            if depth == 0:
                return source[start + 1 : offset]
    fail(f"unterminated function {name}({parameters})")
    return ""


def require_once(source: str, token: str, message: str) -> None:
    if source.count(token) != 1:
        fail(message)


def check_client(source_text: str, header_text: str, interface_text: str) -> None:
    source = strip_comments_and_literals(source_text)
    code = compact(source)
    header_code = compact(header_text)
    connect = compact(function_body("VioGpuNamedPool::Connect", source))
    open_connection = compact(function_body("OpenNamedPoolConnection", source))
    release_connection = compact(function_body("ReleaseNamedPoolConnection", source))
    duplicate_name = compact(function_body("DuplicateInterfaceName", source))
    validate = compact(function_body("ValidateNamedPoolQuery", source))
    validate_direct = compact(function_body("ValidateDirectInterface", source))
    validate_mapping = compact(function_body("ValidateNamedPoolMapping", source))
    query_direct = compact(function_body("NamedPoolQueryDirectInterface", source))
    disconnect = compact(function_body("VioGpuNamedPool::Disconnect", source))
    disconnect_internal = compact(function_body("VioGpuNamedPool::DisconnectInternal", source))
    queue_cleanup = compact(function_body("VioGpuNamedPool::QueuePnpCleanup", source))
    cleanup_worker = compact(function_body("VioGpuNamedPool::PnpCleanupWorker", source))
    callback = compact(function_body("VioGpuNamedPool::PnpNotificationCallback", source))
    handle_pnp = compact(function_body("VioGpuNamedPool::HandlePnpNotification", source))
    query_remove = compact(function_body("VioGpuNamedPool::HandleQueryRemove", source))
    remove_complete = compact(function_body("VioGpuNamedPool::HandleRemoveComplete", source))
    has_owner = compact(function_body("VioGpuNamedPool::HasConnectionOwner", source))
    query_range = compact(function_body("VioGpuNamedPool::QueryPhysicalRange", source))
    acquire = compact(function_body("VioGpuNamedPool::AcquireMapping", source))
    release_mapping = compact(function_body("VioGpuNamedPool::ReleaseMapping", source))

    if source_text.count('"drm2kgsl_host"') != 1 or source_text.count('"gpu_guest"') != 1:
        fail("client must define each exact product pool name once")
    for token in (
        "VioGpuNamedPoolConnecting",
        "volatileLONGm_RemovalLatched",
        "volatileLONGm_PnpState",
    ):
        if token not in header_code:
            fail(f"callback-visible registration state must be atomic: {token}")
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
        "ValidateNamedPoolQuery(&query,ioctlResult.Information,expectedName,expectedNameLength,isMatch)",
        "if(!NT_SUCCESS(status)||!*isMatch){ObDereferenceObject(fileObject);returnstatus;}",
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
    probe_guard_enter = open_connection.find("KeEnterGuardedRegion();")
    probe_acquire = open_connection.find("directInterface.AcquireMapping(")
    probe_release = open_connection.find("directInterface.ReleaseMapping(")
    probe_guard_leave = open_connection.find("KeLeaveGuardedRegion();")
    if min(probe_guard_enter, probe_acquire, probe_release, probe_guard_leave) < 0 or not (
        probe_guard_enter < probe_acquire < probe_release < probe_guard_leave
    ):
        fail("connection probe must hold a non-suspendable lease while validating the mapping")

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
        "cleanupWait=WaitForPnpCleanupIdle();",
        "GetNamedPoolNotificationDriverObject()",
        "IoGetDeviceInterfaces(&GUID_DEVINTERFACE_DROIDVMPOOL,NULL,0,&interfaceList)",
        "for(PWSTRinterfaceName=interfaceList;",
        "interfaceName+=(deviceName.Length/sizeof(WCHAR))+1)",
        "status=OpenNamedPoolConnection(&deviceName,m_ExpectedName,m_ExpectedNameLength,&connection,&isMatch);",
        "if(!isMatch){continue;}",
        "++matchCount;",
        "status=DuplicateInterfaceName(&deviceName,&selectedName);",
        "if(!NT_SUCCESS(firstFailure)||matchCount!=1)",
        "LONG64registrationGeneration=InterlockedIncrement64(&m_Generation);",
        "InterlockedExchange(&m_PnpState,VioGpuNamedPoolConnecting);",
        "InterlockedExchangePointer((PVOIDvolatile*)&m_FileObject,selectedConnection.FileObject);",
        "IoRegisterPlugPlayNotification(EventCategoryTargetDeviceChange,0,selectedConnection.FileObject,notificationDriverObject,PnpNotificationCallback,this,&notificationEntry)",
        "m_NotificationDriverObject=notificationDriverObject;",
        "m_NotificationEntry=notificationEntry;",
        "m_InterfaceName=selectedName;",
        "m_DirectInterface=selectedConnection.DirectInterface;",
        "InterlockedExchange(&m_Ready,TRUE);",
        "LONGpreviousState=InterlockedCompareExchange(&m_PnpState,VioGpuNamedPoolConnected,VioGpuNamedPoolConnecting);",
        "LONG64publishedGeneration=InterlockedCompareExchange64(&m_Generation,0,0);",
        "BOOLEANcommitValid=previousState==VioGpuNamedPoolConnecting&&!removalLatched&&publishedGeneration==registrationGeneration;",
        "pnpState=InterlockedCompareExchange(&m_PnpState,0,0)",
        "fileObject=reinterpret_cast<PFILE_OBJECT>(InterlockedCompareExchangePointer((PVOIDvolatile*)&m_FileObject,NULL,NULL))",
    ):
        require_once(connect, token, f"enumeration must retain exact fail-closed selection/PnP step: {token}")
    connect_state = connect.find("pnpState=InterlockedCompareExchange(&m_PnpState,0,0)")
    connect_file = connect.find(
        "fileObject=reinterpret_cast<PFILE_OBJECT>(InterlockedCompareExchangePointer((PVOIDvolatile*)&m_FileObject,NULL,NULL))"
    )
    connect_stale = connect.find("if(InterlockedCompareExchange(&m_PnpCleanupQueued,FALSE,FALSE)!=FALSE")
    if min(connect_state, connect_file, connect_stale) < 0 or not (connect_state < connect_stale and connect_file < connect_stale):
        fail("connect must snapshot callback-visible PnP fields atomically before stale-owner inspection")
    if connect.count("InterlockedExchange(&m_RemovalLatched,FALSE);") < 2:
        fail("connect must clear the removal latch before each new registration attempt")
    require_once(
        connect,
        "BOOLEANcommitValid=previousState==VioGpuNamedPoolConnecting&&!removalLatched&&publishedGeneration==registrationGeneration;",
        "Connect must fail closed when the callback wins the Connecting commit race",
    )
    require_once(
        connect,
        "commitValid=InterlockedCompareExchange(&m_PnpState,0,0)==VioGpuNamedPoolConnected&&InterlockedCompareExchange(&m_RemovalLatched,FALSE,FALSE)==FALSE&&InterlockedCompareExchange64(&m_Generation,0,0)==registrationGeneration;",
        "Connect must recheck state, removal latch, and generation after publishing readiness",
    )
    cleanup_join = connect.find("cleanupWait=WaitForPnpCleanupIdle();")
    lock_state = connect.find("LockState();")
    if min(cleanup_join, lock_state) < 0 or cleanup_join > lock_state:
        fail("connect must join the previous PnP worker before inspecting reusable pool state")
    if connect.count("ReleaseNamedPoolConnection(&selectedConnection);") != 2 or connect.count(
        "FreeInterfaceName(&selectedName);"
    ) != 2:
        fail("selection and registration failures must release both the provisional target and saved name")
    free_interfaces = connect.find("ExFreePool(interfaceList);")
    registration_generation = connect.find("LONG64registrationGeneration=InterlockedIncrement64(&m_Generation);")
    pre_register_latch = connect.find("InterlockedExchange(&m_RemovalLatched,FALSE);", registration_generation)
    publish_connecting = connect.find("InterlockedExchange(&m_PnpState,VioGpuNamedPoolConnecting);")
    register_target = connect.find("IoRegisterPlugPlayNotification(EventCategoryTargetDeviceChange")
    publish_connection = connect.find(
        "InterlockedExchangePointer((PVOIDvolatile*)&m_FileObject,selectedConnection.FileObject);"
    )
    publish_ready = connect.find("InterlockedExchange(&m_Ready,TRUE);")
    publish_state = connect.find(
        "LONGpreviousState=InterlockedCompareExchange(&m_PnpState,VioGpuNamedPoolConnected,VioGpuNamedPoolConnecting);"
    )
    publish_entry = connect.find("m_NotificationEntry=notificationEntry;")
    publish_name = connect.find("m_InterfaceName=selectedName;")
    publish_direct = connect.find("m_DirectInterface=selectedConnection.DirectInterface;")
    published_generation = connect.find("LONG64publishedGeneration=InterlockedCompareExchange64(&m_Generation,0,0);")
    if min(
        free_interfaces,
        registration_generation,
        pre_register_latch,
        publish_connecting,
        publish_connection,
        register_target,
        publish_state,
        published_generation,
        publish_ready,
        publish_entry,
        publish_name,
        publish_direct,
    ) < 0 or not (
        free_interfaces
        < registration_generation
        < pre_register_latch
        < publish_connecting
        < publish_connection
        < register_target
        < publish_entry
        < publish_name
        < publish_direct
        < publish_state
        < published_generation
        < publish_ready
    ):
        fail("client must seed generation and publish Connecting identity before PnP registration, then commit after owner transfer")
    registration_failure = connect.find("InterlockedExchange(&m_PnpState,VioGpuNamedPoolFailed);")
    failure_wait = connect.find("ExWaitForRundownProtectionRelease(&m_NotificationCallbacks);", registration_failure)
    failure_complete = connect.find("ExRundownCompleted(&m_NotificationCallbacks);", failure_wait)
    failure_release = connect.find("ReleaseNamedPoolConnection(&selectedConnection);", failure_wait)
    failure_name_release = connect.find("FreeInterfaceName(&selectedName);", failure_release)
    failure_mark_rundown = connect.find("m_NotificationRundownCompleted=TRUE;", failure_name_release)
    failure_disconnected = connect.find("InterlockedExchange(&m_PnpState,VioGpuNamedPoolDisconnected);", failure_mark_rundown)
    failure_unlock = connect.find("UnlockState();", failure_disconnected)
    failure_first_unlock = connect.find("UnlockState();", registration_failure)
    if min(
        registration_failure,
        failure_wait,
        failure_complete,
        failure_release,
        failure_name_release,
        failure_mark_rundown,
        failure_disconnected,
        failure_unlock,
    ) < 0 or not (
        registration_failure
        < failure_wait
        < failure_complete
        < failure_release
        < failure_name_release
        < failure_mark_rundown
        < failure_disconnected
        < failure_unlock
    ) or failure_first_unlock != failure_unlock:
        fail("failed registration must retain the state lock while joining callback rundown and releasing provisional ownership")

    for token in (
        "information!=sizeof(*query)",
        "query->InterfaceVersion!=DROIDVMPOOL_INTERFACE_VERSION",
        "query->StructureSize!=sizeof(*query)",
        "query->PoolNameLength==0",
        "query->PoolNameLength>=DROIDVMPOOL_NAME_CAPACITY",
        "query->PoolName[query->PoolNameLength]!=",
        "query->PageSize!=PAGE_SIZE",
        "query->TotalSize>MAXULONG_PTR",
        "!NamedPoolCharacterIsValid(query->PoolName[index])",
        "for(ULONGindex=query->PoolNameLength+1;index<DROIDVMPOOL_NAME_CAPACITY;++index)",
        "if(query->PoolName[index]!=){returnSTATUS_DATA_ERROR;}",
        "expectedName==NULL",
        "expectedNameLength==0",
        "expectedNameLength>=DROIDVMPOOL_NAME_CAPACITY",
        "expectedName[expectedNameLength]!=",
        "query->PoolNameLength==expectedNameLength",
        "RtlCompareMemory(query->PoolName,expectedName,expectedNameLength)==expectedNameLength",
    ):
        require_once(validate, token, f"provider query must retain ABI/name/bounds validation: {token}")

    for token in (
        "directInterface->InterfaceHeader.Size!=sizeof(*directInterface)",
        "directInterface->InterfaceHeader.Version!=DROIDVMPOOL_DIRECT_VERSION",
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
        "irpStack->Parameters.QueryInterface.Version=DROIDVMPOOL_DIRECT_VERSION;",
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
        "if(IsEqualGUID(header->Event,GUID_TARGET_DEVICE_REMOVE_CANCELLED)){returnSTATUS_SUCCESS;}",
        "GUID_TARGET_DEVICE_REMOVE_COMPLETE",
        "HandleRemoveComplete()",
    ):
        require_once(handle_pnp, token, f"target-device callback must dispatch every removal event: {token}")

    callback_tree = (callback, handle_pnp, query_remove, remove_complete)
    for forbidden in (
        "LockState(",
        "KeWaitForSingleObject(",
        "ExWaitForRundownProtectionRelease(",
        "IoUnregisterPlugPlayNotification",
        "OpenNamedPoolConnection(",
        "ReleaseNamedPoolConnection(",
        "DisconnectInternal(",
    ):
        if any(forbidden in function for function in callback_tree):
            fail(f"PnP notification callback tree must not block or perform synchronous target teardown: {forbidden}")

    for token in (
        "InterlockedCompareExchange(&m_ShuttingDown,FALSE,FALSE)",
        "pnpState=InterlockedCompareExchange(&m_PnpState,0,0)",
        "if(pnpState==VioGpuNamedPoolConnecting){returnSTATUS_UNSUCCESSFUL;}",
        "InterlockedCompareExchangePointer((PVOIDvolatile*)&m_FileObject,NULL,NULL)",
        "notificationFileObject==NULL",
        "notificationFileObject==currentFileObject",
        "targetOwned=notificationFileObject==NULL||notificationFileObject==currentFileObject",
        "returntargetOwned&&pnpState==VioGpuNamedPoolConnected?STATUS_UNSUCCESSFUL:STATUS_SUCCESS",
    ):
        require_once(query_remove, token, f"active named pools must veto query-remove without blocking: {token}")
    query_state = query_remove.find("pnpState=InterlockedCompareExchange(&m_PnpState,0,0)")
    query_shutdown = query_remove.find("InterlockedCompareExchange(&m_ShuttingDown,FALSE,FALSE)")
    query_connecting = query_remove.find("if(pnpState==VioGpuNamedPoolConnecting){returnSTATUS_UNSUCCESSFUL;}")
    query_file = query_remove.find("InterlockedCompareExchangePointer((PVOIDvolatile*)&m_FileObject,NULL,NULL)")
    if min(query_shutdown, query_state, query_connecting, query_file) < 0 or not (
        query_shutdown < query_state < query_connecting < query_file
    ):
        fail("query-remove must honor shutdown and veto Connecting before inspecting a possibly invalid notification file object")

    complete_shutdown = remove_complete.find("InterlockedCompareExchange(&m_ShuttingDown,FALSE,FALSE)")
    complete_connecting = remove_complete.find(
        "InterlockedCompareExchange(&m_PnpState,VioGpuNamedPoolFailed,VioGpuNamedPoolConnecting)"
    )
    complete_connected = remove_complete.find(
        "InterlockedCompareExchange(&m_PnpState,VioGpuNamedPoolFailed,VioGpuNamedPoolConnected)"
    )
    complete_latch = remove_complete.find("InterlockedExchange(&m_RemovalLatched,TRUE);")
    complete_withdraw = remove_complete.find("InterlockedExchange(&m_Ready,FALSE);")
    complete_generation = remove_complete.find("InterlockedIncrement64(&m_Generation);")
    complete_fail = remove_complete.find("m_FailureCallback(m_FailureContext);")
    complete_queue = remove_complete.find("QueuePnpCleanup();")
    if min(
        complete_shutdown,
        complete_connecting,
        complete_connected,
        complete_latch,
        complete_withdraw,
        complete_generation,
        complete_fail,
        complete_queue,
    ) < 0 or not (
        complete_shutdown
        < complete_connecting
        < complete_connected
        < complete_latch
        < complete_withdraw
        < complete_generation
        < complete_fail
        < complete_queue
    ):
        fail("surprise removal must atomically fail Connecting/Connected, latch removal, withdraw readiness, and queue external cleanup")
    require_once(
        remove_complete,
        "elseif(previousState!=VioGpuNamedPoolConnecting){returnSTATUS_SUCCESS;}",
        "duplicate or stale remove-complete callbacks must not republish adapter failure",
    )
    require_once(
        remove_complete,
        "if(queueCleanup){QueuePnpCleanup();}",
        "Connecting removal must defer cleanup until Connect transfers ownership",
    )

    constructor = compact(function_body("VioGpuNamedPool::VioGpuNamedPool", source))
    queue_lock = queue_cleanup.find("KeAcquireSpinLock(&m_PnpCleanupLock,&oldIrql);")
    queue_claim = queue_cleanup.find(
        "InterlockedCompareExchange(&m_PnpCleanupQueued,0,0)!=VioGpuNamedPoolCleanupIdle"
    )
    queue_worker_idle = queue_cleanup.find(
        "InterlockedCompareExchange(&m_PnpCleanupWorkerState,0,0)!=VioGpuNamedPoolWorkerIdle"
    )
    queue_rundown = queue_cleanup.find("ExAcquireRundownProtection(&m_PnpCleanupWorkerReferences)")
    queue_publish = queue_cleanup.find(
        "InterlockedExchange(&m_PnpCleanupQueued,VioGpuNamedPoolCleanupPublishing);"
    )
    queue_worker_running = queue_cleanup.find(
        "InterlockedExchange(&m_PnpCleanupWorkerState,VioGpuNamedPoolWorkerRunning);"
    )
    queue_pending = queue_cleanup.find("InterlockedExchange(&m_PnpCleanupStatus,STATUS_PENDING);")
    queue_clear = queue_cleanup.find("KeClearEvent(&m_PnpCleanupComplete);")
    queue_generation = queue_cleanup.find("InterlockedIncrement(&m_PnpCleanupGeneration);")
    queue_queued = queue_cleanup.find(
        "InterlockedExchange(&m_PnpCleanupQueued,VioGpuNamedPoolCleanupQueued);"
    )
    queue_submit = queue_cleanup.find("ExQueueWorkItem(&m_PnpCleanupWorkItem,DelayedWorkQueue);")
    queue_unlock = queue_cleanup.rfind("KeReleaseSpinLock(&m_PnpCleanupLock,oldIrql);")
    if min(
        queue_lock,
        queue_claim,
        queue_worker_idle,
        queue_rundown,
        queue_publish,
        queue_worker_running,
        queue_pending,
        queue_clear,
        queue_generation,
        queue_queued,
        queue_submit,
        queue_unlock,
    ) < 0 or not (
        queue_lock
        < queue_claim
        < queue_worker_idle
        < queue_rundown
        < queue_publish
        < queue_worker_running
        < queue_pending
        < queue_clear
        < queue_generation
        < queue_queued
        < queue_submit
        < queue_unlock
    ):
        fail("cleanup publication must serialize worker ownership, state, event, and generation under one spin lock")
    if queue_cleanup.count("KeReleaseSpinLock(&m_PnpCleanupLock,oldIrql);") != 3:
        fail("cleanup publication must release its spin lock on duplicate, rundown-failure, and queue paths")
    if constructor.count("ExInitializeWorkItem(&m_PnpCleanupWorkItem,PnpCleanupWorker,this);") != 1:
        fail("the embedded cleanup work item must be initialized exactly once with the pool lifetime")
    if "ExInitializeWorkItem" in queue_cleanup:
        fail("cleanup publication must not reinitialize an embedded work item")

    worker_claim = cleanup_worker.find("pool->ClaimQueuedPnpCleanup()")
    worker_cleanup = cleanup_worker.find("status=pool->DisconnectInternal();")
    worker_complete = cleanup_worker.find("pool->CompletePnpCleanupTeardown(status,TRUE);")
    worker_release = cleanup_worker.find(
        "ExReleaseRundownProtection(&pool->m_PnpCleanupWorkerReferences);", worker_complete
    )
    if min(worker_claim, worker_cleanup, worker_complete, worker_release) < 0 or not (
        worker_claim < worker_cleanup < worker_complete < worker_release
    ):
        fail("PnP worker must publish teardown completion before releasing its work-item lifetime reference")
    if cleanup_worker.count("ExReleaseRundownProtection(&pool->m_PnpCleanupWorkerReferences);") != 2:
        fail("all PnP worker exits must release the work-item lifetime reference exactly once")
    if not cleanup_worker.endswith("ExReleaseRundownProtection(&pool->m_PnpCleanupWorkerReferences);"):
        fail("the successful PnP worker path must perform no pool access after its lifetime release")

    complete_cleanup = compact(function_body("VioGpuNamedPool::CompletePnpCleanupTeardown", source))
    external_idle = (
        "if(!worker){NT_ASSERT(InterlockedCompareExchange(&m_PnpCleanupWorkerState,0,0)=="
        "VioGpuNamedPoolWorkerIdle);InterlockedExchange(&m_PnpCleanupQueued,VioGpuNamedPoolCleanupIdle);}"
    )
    if complete_cleanup.count(external_idle) != 1:
        fail("worker completion must keep Teardown published until a waiter joins the last-object-access barrier")

    cleanup_waiter = compact(function_body("VioGpuNamedPool::WaitForPnpCleanupIdle", source))
    worker_rundown_reinitialize = cleanup_waiter.find(
        "ExReInitializeRundownProtection(&m_PnpCleanupWorkerReferences);"
    )
    finalize_lock = cleanup_waiter.find(
        "KeAcquireSpinLock(&m_PnpCleanupLock,&oldIrql);", worker_rundown_reinitialize + 1
    )
    finalize_sequence = (
        cleanup_waiter.find("cleanupState==VioGpuNamedPoolCleanupTeardown&&workerState==VioGpuNamedPoolWorkerRunning"),
        cleanup_waiter.find("InterlockedExchange(&m_PnpCleanupWorkerState,VioGpuNamedPoolWorkerFinalizing);"),
        cleanup_waiter.find("KeClearEvent(&m_PnpCleanupComplete);"),
        cleanup_waiter.find("ExWaitForRundownProtectionRelease(&m_PnpCleanupWorkerReferences);"),
        cleanup_waiter.find("ExRundownCompleted(&m_PnpCleanupWorkerReferences);"),
        worker_rundown_reinitialize,
        finalize_lock,
        cleanup_waiter.find("InterlockedExchange(&m_PnpCleanupWorkerState,VioGpuNamedPoolWorkerIdle);"),
        cleanup_waiter.find("InterlockedExchange(&m_PnpCleanupQueued,VioGpuNamedPoolCleanupIdle);"),
        cleanup_waiter.find("KeSetEvent(&m_PnpCleanupComplete,IO_NO_INCREMENT,FALSE);"),
    )
    if min(finalize_sequence) < 0 or list(finalize_sequence) != sorted(finalize_sequence):
        fail("cleanup waiter must join and reinitialize worker rundown before publishing reusable Idle state")
    if cleanup_waiter.count("ExReInitializeRundownProtection(&m_PnpCleanupWorkerReferences);") != 1:
        fail("cleanup waiter must reinitialize worker rundown exactly once per completed lifetime barrier")
    if "ExReInitializeRundownProtection" in queue_cleanup:
        fail("cleanup publication must not reinitialize rundown protection while holding its spin lock")

    wrapper_first_wait = disconnect.find("waitStatus=BeginPnpCleanupTeardown();")
    wrapper_generation = disconnect.find("cleanupGeneration=InterlockedCompareExchange(&m_PnpCleanupGeneration,0,0);")
    wrapper_cleanup = disconnect.find("status=DisconnectInternal();")
    wrapper_complete = disconnect.find("CompletePnpCleanupTeardown(status,FALSE);")
    wrapper_second_wait = disconnect.find(
        "waitStatus=KeWaitForSingleObject(&m_PnpCleanupComplete", wrapper_first_wait + 1
    )
    wrapper_final_generation = disconnect.find(
        "finalCleanupGeneration=InterlockedCompareExchange(&m_PnpCleanupGeneration,0,0);"
    )
    wrapper_status = disconnect.find("InterlockedCompareExchange(&m_PnpCleanupStatus,0,0)")
    if min(
        wrapper_first_wait,
        wrapper_generation,
        wrapper_cleanup,
        wrapper_complete,
        wrapper_second_wait,
        wrapper_final_generation,
        wrapper_status,
    ) < 0 or not (
        wrapper_first_wait
        < wrapper_generation
        < wrapper_cleanup
        < wrapper_complete
        < wrapper_second_wait
        < wrapper_final_generation
        < wrapper_status
    ):
        fail("external disconnect must join PnP cleanup both before teardown and before object destruction")
    if disconnect.count("KeWaitForSingleObject(&m_PnpCleanupComplete") != 1:
        fail("external disconnect must use the claimed cleanup state before its final completion wait")
    require_once(
        disconnect,
        "status==STATUS_DEVICE_BUSY&&cleanupGeneration!=finalCleanupGeneration",
        "an unrelated external disconnect owner must still report STATUS_DEVICE_BUSY",
    )

    withdraw = disconnect_internal.find("InterlockedExchange(&m_Ready,FALSE);")
    drain = disconnect_internal.find("ExWaitForRundownProtectionRelease(&m_Operations);")
    complete_operations = disconnect_internal.find("ExRundownCompleted(&m_Operations);")
    clear_direct = disconnect_internal.find("RtlZeroMemory(&m_DirectInterface,sizeof(m_DirectInterface));")
    release_target = disconnect_internal.find("ReleaseNamedPoolConnection(&connection);")
    unregister = disconnect_internal.find("IoUnregisterPlugPlayNotificationEx(notificationEntry);")
    drain_callbacks = disconnect_internal.find("ExWaitForRundownProtectionRelease(&m_NotificationCallbacks);")
    complete_callbacks = disconnect_internal.find("ExRundownCompleted(&m_NotificationCallbacks);")
    free_name = disconnect_internal.find("FreeInterfaceName(&m_InterfaceName);")
    disconnected = disconnect_internal.find("InterlockedExchange(&m_PnpState,VioGpuNamedPoolDisconnected);")
    if min(
        withdraw,
        drain,
        complete_operations,
        clear_direct,
        release_target,
        unregister,
        drain_callbacks,
        complete_callbacks,
        free_name,
        disconnected,
    ) < 0 or not (
        withdraw
        < drain
        < complete_operations
        < clear_direct
        < unregister
        < drain_callbacks
        < complete_callbacks
        < release_target
        < free_name
        < disconnected
    ):
        fail("worker/external teardown must complete both rundown objects before releasing the target connection")
    if disconnect_internal.count("IoUnregisterPlugPlayNotificationEx(notificationEntry)") != 1:
        fail("callback-external teardown must synchronously unregister exactly one target notification")
    if "IoUnregisterPlugPlayNotification(notificationEntry)" in disconnect_internal:
        fail("callback-external teardown must use synchronous unregister")
    for token in (
        "if(m_DisconnectInProgress){UnlockState();returnSTATUS_DEVICE_BUSY;}",
        "m_DisconnectInProgress=TRUE;",
    ):
        require_once(disconnect_internal, token, f"disconnect must serialize teardown and restore failed ownership: {token}")
    for token in (
        "InterlockedExchangePointer((PVOIDvolatile*)&m_FileObject,connection.FileObject);",
        "m_DirectInterface=connection.DirectInterface;",
    ):
        require_once(disconnect_internal, token, f"unregister failure must restore the target connection: {token}")
    if disconnect_internal.count("m_DisconnectInProgress=FALSE;") != 2:
        fail("disconnect success and unregister failure must both release the single-flight owner")
    for token in (
        "InterlockedCompareExchange(&m_PnpCleanupQueued,FALSE,FALSE)!=FALSE",
        "KeReadStateEvent(&m_PnpCleanupComplete)==0",
        "pnpState=InterlockedCompareExchange(&m_PnpState,0,0)",
        "m_NotificationDriverObject!=NULL",
        "m_NotificationEntry!=NULL",
        "m_InterfaceName.Buffer!=NULL",
        "fileObject=reinterpret_cast<PFILE_OBJECT>(InterlockedCompareExchangePointer((PVOIDvolatile*)&m_FileObject,NULL,NULL))",
        "fileObject!=NULL",
        "m_DirectInterface.InterfaceHeader.Context!=NULL",
    ):
        require_once(has_owner, token, f"connection ownership must include every PnP lifetime component: {token}")
    owner_state = has_owner.find("pnpState=InterlockedCompareExchange(&m_PnpState,0,0)")
    owner_file = has_owner.find(
        "fileObject=reinterpret_cast<PFILE_OBJECT>(InterlockedCompareExchangePointer((PVOIDvolatile*)&m_FileObject,NULL,NULL))"
    )
    owner_value = has_owner.find("BOOLEANowned=")
    if min(owner_state, owner_file, owner_value) < 0 or not (owner_state < owner_value and owner_file < owner_value):
        fail("connection ownership must snapshot callback-visible PnP fields atomically before evaluating ownership")

    for obsolete_state in (
        "VioGpuNamedPoolQueryRemove",
        "VioGpuNamedPoolReconnecting",
        "VioGpuNamedPoolRemoved",
    ):
        if obsolete_state in source_text or obsolete_state in header_text:
            fail(f"veto-only PnP handling must not retain an unreachable state: {obsolete_state}")

    for token in (
        "physicalAddress==NULL",
        "size==NULL",
        "KeGetCurrentIrql()>DISPATCH_LEVEL",
        "!ExAcquireRundownProtection(&m_Operations)",
        "generation=InterlockedCompareExchange64(&m_Generation,0,0)",
        "rangeBase=m_BasePA;",
        "rangeSize=m_Size;",
        "generation==InterlockedCompareExchange64(&m_Generation,0,0)",
        "*physicalAddress=rangeBase;",
        "*size=rangeSize;",
        "ExReleaseRundownProtection(&m_Operations);",
    ):
        require_once(query_range, token, f"physical-range query must retain active generation provenance: {token}")
    if "BaseVirtualAddress" in query_range or "AcquireMapping" in query_range:
        fail("physical-range query must not publish or acquire a mapping lease")

    for token in (
        "mapping==NULL",
        "mapping->m_Owner!=NULL",
        "mapping->m_OwningThread!=NULL",
        "currentIrql>DISPATCH_LEVEL",
        "!KeAreAllApcsDisabled()",
        "!ExAcquireRundownProtection(&m_Operations)",
        "m_DirectInterface.AcquireMapping(m_DirectInterface.InterfaceHeader.Context,&mappingValue)",
        "mappingValue.BasePhysicalAddress.QuadPart!=m_BasePA.QuadPart",
        "mappingValue.TotalSize!=m_Size",
        "fileObject=reinterpret_cast<PFILE_OBJECT>(InterlockedCompareExchangePointer((PVOIDvolatile*)&m_FileObject,NULL,NULL))",
        "mapping->m_Owner=this;",
        "mapping->m_OwningThread=KeGetCurrentThread();",
        "mapping->m_AcquireIrql=currentIrql;",
        "mapping->m_Generation=(ULONGLONG)generation;",
    ):
        require_once(acquire, token, f"client acquire must retain nested lease validation: {token}")
    acquire_file = acquire.find(
        "fileObject=reinterpret_cast<PFILE_OBJECT>(InterlockedCompareExchangePointer((PVOIDvolatile*)&m_FileObject,NULL,NULL))"
    )
    acquire_guard = acquire.find("if(generation>0&&IsActive()&&fileObject!=NULL")
    if min(acquire_file, acquire_guard) < 0 or acquire_file > acquire_guard:
        fail("mapping acquire must atomically snapshot the callback-visible target file object")
    for token in (
        "generation=InterlockedCompareExchange64(&m_Generation,0,0)",
        "generation!=InterlockedCompareExchange64(&m_Generation,0,0)",
    ):
        require_once(acquire, token, f"client acquire must reject a named-pool generation race: {token}")
    if source.count("InterlockedIncrement64(&m_Generation);") != 3:
        fail("connect, callback-external disconnect, and surprise removal must each advance generation")
    release_direct = release_mapping.find(
        "m_DirectInterface.ReleaseMapping(m_DirectInterface.InterfaceHeader.Context);"
    )
    clear_owner = release_mapping.find("mapping->m_Owner=NULL;")
    release_local = release_mapping.find("ExReleaseRundownProtection(&m_Operations);")
    if min(release_direct, clear_owner, release_local) < 0 or not release_direct < clear_owner < release_local:
        fail("client release must drop provider lease before local rundown")
    for token in (
        "mapping->m_OwningThread==KeGetCurrentThread()",
        "KeAreAllApcsDisabled()",
        "mapping->m_AcquireIrql!=DISPATCH_LEVEL||KeGetCurrentIrql()==DISPATCH_LEVEL",
        "mapping->m_Generation=0;",
    ):
        require_once(release_mapping, token, f"client release must enforce the no-suspend lease rule: {token}")
    if "KeEnterGuardedRegion" in acquire or "KeLeaveGuardedRegion" in release_mapping:
        fail("mapping wrapper must not own a guarded region across the public lease boundary")

    if "Allocate" in header_text or re.search(r"\bFree\s*\(", header_text):
        fail("named host-pool client header must not expose allocation operations")
    if header_text.count("QueryPhysicalRange(") != 1:
        fail("client may expose only one physical-range snapshot API")
    if "VioGpuNamedPoolMapping(const VioGpuNamedPoolMapping &);" not in header_text or (
        "VioGpuNamedPoolMapping &operator=(const VioGpuNamedPoolMapping &);" not in header_text
    ):
        fail("mapping lease must remain non-copyable")
    for token in (
        "VioGpuNamedPool(_In_reads_(expectedNameLength)constCHAR*expectedName,_In_ULONGexpectedNameLength,"
        "_In_VIOGPU_NAMED_POOL_FAILURE_CALLBACKfailureCallback,_In_opt_PVOIDfailureContext);",
        "constCHAR*m_ExpectedName;",
        "ULONGm_ExpectedNameLength;",
        "VIOGPU_NAMED_POOL_FAILURE_CALLBACKm_FailureCallback;",
        "PVOIDm_FailureContext;",
        "classVioGpuDrmHostPoolfinal:publicVioGpuNamedPool",
        "classVioGpuGuestPoolfinal:publicVioGpuNamedPool",
    ):
        require_once(header_code, token, f"generic pool client must retain exact immutable identity: {token}")
    for token in (
        "VioGpuNamedPool(VIOGPU_DRM_HOST_POOL_NAME,",
        "VioGpuNamedPool(VIOGPU_GUEST_POOL_NAME,",
    ):
        require_once(source_text, token, f"product pool wrapper must bind one exact identity: {token}")
    for token in (
        "static DRIVER_NOTIFICATION_CALLBACK_ROUTINE PnpNotificationCallback;",
        "static WORKER_THREAD_ROUTINE PnpCleanupWorker;",
        "mutable KMUTEX m_StateLock;",
        "PKTHREAD m_OwningThread;",
        "KIRQL m_AcquireIrql;",
        "ULONGLONG m_Generation;",
        "mutable DECLSPEC_ALIGN(8) volatile LONG64 m_Generation;",
        "EX_RUNDOWN_REF m_NotificationCallbacks;",
        "EX_RUNDOWN_REF m_PnpCleanupWorkerReferences;",
        "BOOLEAN m_NotificationRundownCompleted;",
        "BOOLEAN m_DisconnectInProgress;",
        "WORK_QUEUE_ITEM m_PnpCleanupWorkItem;",
        "KEVENT m_PnpCleanupComplete;",
        "KSPIN_LOCK m_PnpCleanupLock;",
        "mutable volatile LONG m_PnpCleanupQueued;",
        "volatile LONG m_PnpCleanupGeneration;",
        "volatile LONG m_PnpCleanupStatus;",
        "volatile LONG m_PnpCleanupWorkerState;",
        "volatile LONG m_ShuttingDown;",
        "PDRIVER_OBJECT m_NotificationDriverObject;",
        "PVOID m_NotificationEntry;",
        "UNICODE_STRING m_InterfaceName;",
    ):
        if header_text.count(token) != 1:
            fail(f"named-pool client must retain remote-PnP lifetime state: {token}")
    if "DROIDVMPOOL_INTERFACE_VERSION" not in interface_text or "IOCTL_DROIDVMPOOL_QUERY" not in interface_text:
        fail("client must consume the versioned provider query ABI")


def check_adapter(adapter_text: str, adapter_header_text: str) -> None:
    adapter = strip_comments_and_literals(adapter_text)
    adapter_code = compact(adapter)
    start = compact(function_body("VioGpuAdapter::StartNativeContextTransport", adapter))
    begin = compact(function_body("VioGpuAdapter::BeginNativeContextInitialization", adapter))
    complete = compact(function_body("VioGpuAdapter::CompleteNativeContextInitialization", adapter))
    query_readiness = compact(function_body("VioGpuAdapter::QueryNativeContextReadiness", adapter))
    generation_current = compact(function_body("VioGpuAdapter::IsNativeContextGenerationCurrent", adapter))
    query_segment = compact(function_body("VioGpuAdapter::QueryVidMmSegment", adapter))
    connect_host = compact(function_body("VioGpuAdapter::ConnectDrmHostPool", adapter))
    connect_guest = compact(function_body("VioGpuAdapter::ConnectGpuGuestPool", adapter))
    pool_failure = compact(function_body("VioGpuAdapter::NamedPoolFailureCallback", adapter))
    stop = compact(function_body("VioGpuAdapter::StopNativeContextTransportLocked", adapter))
    destructor = compact(function_body("VioGpuAdapter::~VioGpuAdapter", adapter))

    rdma = start.find("status=ConnectRestrictedDma();")
    host = start.find("status=ConnectDrmHostPool();")
    guest = start.find("status=ConnectGpuGuestPool();")
    virtio = start.find("status=VioGpuAdapterInit(pDispInfo);")
    if min(rdma, host, guest, virtio) < 0 or not rdma < host < guest < virtio:
        fail("adapter must connect restricted DMA, drm2kgsl_host, and gpu_guest before VirtIO initialization")
    require_once(
        connect_host,
        "returnConnectNamedPoolWithRetry(&m_DrmHostPool);",
        "drm2kgsl_host connect wrapper must target only its own pool instance",
    )
    require_once(
        connect_guest,
        "returnConnectNamedPoolWithRetry(&m_GpuGuestPool);",
        "gpu_guest connect wrapper must target only its own pool instance",
    )
    for member in ("m_DrmHostPool", "m_GpuGuestPool"):
        require_once(begin, f"{member}.HasConnectionOwner()", "initialization must reject every stale pool owner")
        require_once(complete, f"{member}.IsActive()", "Ready publication must require both named pools")
        if query_readiness.count(f"{member}.IsActive()") != 2:
            fail("readiness snapshots must check both named pools before and after copying published state")
        require_once(
            generation_current,
            f"{member}.IsActive()",
            "context generation must become invalid while either named pool is unavailable",
        )
    require_once(
        pool_failure,
        "adapter->FailNativeContextAtAnyIrql();",
        "surprise removal must advance adapter and reset generations before asynchronous teardown",
    )
    reset_gate = pool_failure.find("dod->RequestHardwareResetAtAnyIrql();")
    native_failure = pool_failure.find("adapter->FailNativeContextAtAnyIrql();")
    if min(reset_gate, native_failure) < 0 or reset_gate > native_failure:
        fail("named-pool loss must close the outer DDI gate before poisoning Native Context state")
    for token in (
        "m_DrmHostPool(NamedPoolFailureCallback,this)",
        "m_GpuGuestPool(NamedPoolFailureCallback,this)",
    ):
        require_once(adapter_code, token, "both named pools must propagate surprise removal to their owning adapter")
    guest_close = stop.find("status=m_GpuGuestPool.Disconnect();")
    guest_failure = stop.find("if(!NT_SUCCESS(status)){FailNativeContextAtAnyIrql();returnstatus;}", guest_close)
    host_close = stop.find("status=m_DrmHostPool.Disconnect();")
    host_failure = stop.find("if(!NT_SUCCESS(status)){FailNativeContextAtAnyIrql();returnstatus;}", host_close)
    rdma_close = stop.find("status=m_RdmaPool.Disconnect();")
    offline = stop.find("InterlockedExchange(&m_NativeContextState,VioGpuNativeContextOffline);")
    if min(guest_close, guest_failure, host_close, host_failure, rdma_close, offline) < 0 or not (
        guest_close < guest_failure < host_close < host_failure < rdma_close < offline
    ):
        fail("teardown must release gpu_guest then drm2kgsl_host before RDMA and Offline publication")
    for member in ("m_DrmHostPool", "m_GpuGuestPool"):
        if destructor.count(f"{member}.HasConnectionOwner()") != 2:
            fail("destructor must detect and then assert release of both named-pool owners")
        require_once(
            destructor,
            f"NT_ASSERT(!{member}.HasConnectionOwner());",
            "destructor must require both named-pool owners to be released",
        )

    segment_branches = (
        "#ifdefined(VIOGPU_WDDM_CI_ONLY)"
        "returnm_GpuGuestPool.QueryPhysicalRange(physicalAddress,size);"
        "#else"
        "PVOIDbaseAddress=NULL;"
        "returnm_RdmaPool.QueryVidMmSegment(&baseAddress,physicalAddress,size);"
        "#endif"
    )
    require_once(
        query_segment,
        segment_branches,
        "full WDDM must publish gpu_guest while stable Display-Only retains restricted-DMA provenance",
    )

    if adapter_text.count("AcquireDrmHostPoolMapping") != 3 or adapter_header_text.count(
        "AcquireDrmHostPoolMapping"
    ) != 1:
        fail("native control responses must use exactly two short mapping leases plus one adapter wrapper")

    seed = compact(
        function_body_with_parameters(
            "VioGpuSeedNativeControlResponse",
            "_In_ VioGpuAdapter *adapter, _Inout_ VIOGPU_NATIVE_CONTEXT_OWNER *owner, _In_ ULONG sequence",
            adapter,
        )
    )
    consume = compact(
        function_body_with_parameters(
            "VioGpuConsumeNativeControlResponse",
            "_In_ VioGpuAdapter *adapter, _In_ const VIOGPU_NATIVE_CONTEXT_OWNER *owner, _In_ ULONG sequence, "
            "_In_ ULONG parameter, _Out_ PULONGLONG value",
            adapter,
        )
    )
    for owner, body in (("seed", seed), ("consume", consume)):
        if body.count("adapter->AcquireDrmHostPoolMapping(&mapping)") != 1 or body.count("mapping.Release();") != 1:
            fail(f"native response {owner} must hold exactly one mapping lease")
        if "KeWaitForSingleObject" in body or "SubmitNativeControl" in body or "PAGED_CODE" in body:
            fail(f"native response {owner} must not wait, submit, or page while holding its mapping lease")
        enter = body.find("KeEnterGuardedRegion();")
        acquire_mapping = body.find("adapter->AcquireDrmHostPoolMapping(&mapping)")
        release_mapping = body.find("mapping.Release();")
        leave = body.find("KeLeaveGuardedRegion();")
        if min(enter, acquire_mapping, release_mapping, leave) < 0 or not enter < acquire_mapping < release_mapping < leave:
            fail(f"native response {owner} must keep APCs disabled across the complete mapping lease")
    if seed.count("owner->ControlPoolGeneration=mapping.GetGeneration();") != 1 or consume.count(
        "owner->ControlPoolGeneration==mapping.GetGeneration()"
    ) != 1:
        fail("native response leases must capture and revalidate one exact named-pool generation")

    guarded_header = compact(adapter_header_text)
    require_once(
        guarded_header,
        '#ifdefined(VIOGPU_WDDM_CI_ONLY)#include"viogpu_named_pool.h"#endif',
        "stable Display-Only target must not include the compile-only named-pool client",
    )
    for token in ("VioGpuDrmHostPoolm_DrmHostPool;", "VioGpuGuestPoolm_GpuGuestPool;"):
        require_once(guarded_header, token, "full WDDM target must own both compile-only named-pool clients")
    require_once(
        guarded_header,
        "staticVOIDNamedPoolFailureCallback(_In_opt_PVOIDcontext);",
        "adapter must expose one private nonblocking named-pool failure bridge",
    )
    require_once(
        guarded_header,
        "VOIDRequestHardwareResetAtAnyIrql(void){InterlockedExchange(&m_HardwareResetState,"
        "VioGpuHardwareResetRequested);}",
        "named-pool loss must have a nonblocking outer hardware-reset gate",
    )


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
