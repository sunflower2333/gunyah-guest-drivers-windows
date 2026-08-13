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
    validate = compact(function_body("ValidateNamedPoolQuery", source))
    validate_direct = compact(function_body("ValidateDirectInterface", source))
    validate_mapping = compact(function_body("ValidateNamedPoolMapping", source))
    query_direct = compact(function_body("NamedPoolQueryDirectInterface", source))
    disconnect = compact(function_body("VioGpuDrmHostPool::Disconnect", source))
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
        "IoGetDeviceInterfaces(&GUID_DEVINTERFACE_DROIDVMPOOL,NULL,0,&interfaceList)",
        "for(PWSTRinterfaceName=interfaceList;",
        "interfaceName+=(deviceName.Length/sizeof(WCHAR))+1)",
        "IoGetDeviceObjectPointer(&deviceName,FILE_ALL_ACCESS,&fileObject,&deviceObject)",
        "NamedPoolQuery(deviceObject,fileObject,&query)",
        "ValidateNamedPoolQuery(&query,ioctlResult.Information,&isDrmHost)",
        "if(!isDrmHost){ObDereferenceObject(fileObject);continue;}",
        "NamedPoolQueryDirectInterface(deviceObject,&directInterface)",
        "ValidateDirectInterface(&directInterface)",
        "directInterface.AcquireMapping(directInterface.InterfaceHeader.Context,&mapping)",
        "ValidateNamedPoolMapping(&mapping,&query)",
        "directInterface.ReleaseMapping(directInterface.InterfaceHeader.Context);",
        "++matchCount;",
        "if(!NT_SUCCESS(firstFailure)||matchCount!=1)",
        "returnmatchCount==0?STATUS_NOT_FOUND:STATUS_DEVICE_CONFIGURATION_ERROR;",
        "m_FileObject=selectedFileObject;",
        "m_DirectInterface=selectedDirectInterface;",
        "InterlockedExchange(&m_Ready,TRUE);",
    ):
        require_once(connect, token, f"enumeration must retain exact fail-closed selection step: {token}")

    selected_release = (
        "if(selectedFileObject!=NULL){"
        "DereferenceDirectInterface(&selectedDirectInterface);"
        "ObDereferenceObject(selectedFileObject);"
        "}"
        "if(!NT_SUCCESS(firstFailure)){returnfirstFailure;}"
    )
    require_once(connect, selected_release, "any malformed or unreachable provider must release a provisional match and fail")
    if connect.find("ExFreePool(interfaceList);") > connect.find("m_FileObject=selectedFileObject;"):
        fail("client must release the interface MULTI_SZ before publishing its connection")

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

    withdraw = disconnect.find("InterlockedExchange(&m_Ready,FALSE);")
    drain = disconnect.find("ExWaitForRundownProtectionRelease(&m_Operations);")
    clear_direct = disconnect.find("RtlZeroMemory(&m_DirectInterface,sizeof(m_DirectInterface));")
    dereference_direct = disconnect.find("DereferenceDirectInterface(&directInterface);")
    dereference_file = disconnect.find("ObDereferenceObject(fileObject);")
    if min(withdraw, drain, clear_direct, dereference_direct, dereference_file) < 0 or not (
        withdraw < drain < clear_direct < dereference_direct < dereference_file
    ):
        fail("disconnect must drain mapping operations before releasing the direct interface and file owner")

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
    if "DROIDVMPOOL_INTERFACE_VERSION_V1" not in interface_text or "IOCTL_DROIDVMPOOL_QUERY" not in interface_text:
        fail("client must consume the versioned provider query ABI")


def check_adapter(adapter_text: str, adapter_header_text: str) -> None:
    adapter = strip_comments_and_literals(adapter_text)
    start = compact(function_body("VioGpuAdapter::StartNativeContextTransport", adapter))
    begin = compact(function_body("VioGpuAdapter::BeginNativeContextInitialization", adapter))
    complete = compact(function_body("VioGpuAdapter::CompleteNativeContextInitialization", adapter))
    stop = compact(function_body("VioGpuAdapter::StopNativeContextTransportLocked", adapter))
    destructor = compact(function_body("VioGpuAdapter::~VioGpuAdapter", adapter))

    rdma = start.find("status=ConnectRestrictedDma();")
    named = start.find("status=ConnectDrmHostPool();")
    virtio = start.find("status=VioGpuAdapterInit(pDispInfo);")
    if min(rdma, named, virtio) < 0 or not rdma < named < virtio:
        fail("adapter must connect restricted DMA, then drm2kgsl_host, before VirtIO initialization")
    require_once(begin, "m_DrmHostPool.HasConnectionOwner()", "initialization must reject a stale named-pool owner")
    require_once(complete, "m_DrmHostPool.IsActive()", "Ready publication must require the named pool connection")
    named_close = stop.find("m_DrmHostPool.Disconnect();")
    rdma_close = stop.find("status=m_RdmaPool.Disconnect();")
    offline = stop.find("InterlockedExchange(&m_NativeContextState,VioGpuNativeContextOffline);")
    if min(named_close, rdma_close, offline) < 0 or not named_close < rdma_close < offline:
        fail("teardown must release the named pool before RDMA and before Offline publication")
    if destructor.count("m_DrmHostPool.HasConnectionOwner()") != 2:
        fail("destructor must detect and then assert release of the named-pool owner")
    require_once(destructor, "NT_ASSERT(!m_DrmHostPool.HasConnectionOwner());", "destructor must require named-pool release")

    if adapter_text.count("AcquireDrmHostPoolMapping") != 1 or adapter_header_text.count(
        "AcquireDrmHostPoolMapping"
    ) != 1:
        fail("mapping access must remain an unused scaffold until target-device PnP coordination is implemented")

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
