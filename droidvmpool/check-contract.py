#!/usr/bin/env python3

import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


DRIVER_DIR = Path(__file__).resolve().parent
REPO_DIR = DRIVER_DIR.parent
SOURCE_PATH = DRIVER_DIR / "droidvmpool.c"
HEADER_PATH = DRIVER_DIR / "droidvmpool.h"
INTERFACE_PATH = DRIVER_DIR / "droidvmpool_interface.h"
INF_PATH = DRIVER_DIR / "droidvmpool.inf"
PROJECT_PATH = DRIVER_DIR / "droidvmpool.vcxproj"
ABI_TEST_DIR = DRIVER_DIR / "tests" / "interface-abi"
WORKFLOW_PATH = REPO_DIR / ".github" / "workflows" / "viogpuwddm-arm64-ci.yml"
NAMESPACE = {"msbuild": "http://schemas.microsoft.com/developer/msbuild/2003"}


def fail(message: str) -> None:
    print(f"droidvmpool contract failure: {message}", file=sys.stderr)
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
    matches = list(re.finditer(rf"\b{re.escape(name)}\s*\([^;{{}}]*?\)\s*{{", source, re.DOTALL))
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


def check_provider(source: str) -> None:
    code = compact(source)
    for forbidden in (
        "IoGetDeviceInterfaces",
        "DevicePropertyInstanceId",
        "DmaPool",
        "MmAllocate",
        "ExAllocatePool",
        "RtlZeroMemory",
    ):
        if forbidden in source:
            fail(f"provider must not use instance-id discovery, allocation, or pool clearing: {forbidden}")

    require_once(
        code,
        "WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes,DROIDVMPOOL_DEVICE_CONTEXT);",
        "provider must have exactly one per-WDFDEVICE context",
    )
    require_once(
        code,
        "WdfDeviceCreateDeviceInterface(device,&GUID_DEVINTERFACE_DROIDVMPOOL,NULL);",
        "provider must publish exactly one isolated pool interface per device",
    )
    for assertion in (
        "C_ASSERT(sizeof(DROIDVMPOOL_QUERY_OUTPUT)==96);",
        "C_ASSERT(FIELD_OFFSET(DROIDVMPOOL_QUERY_OUTPUT,PoolName)==32);",
        "C_ASSERT(sizeof(DROIDVMPOOL_MAPPING)==24);",
        "C_ASSERT(sizeof(DROIDVMPOOL_DIRECT_INTERFACE)==48);",
        "C_ASSERT(FIELD_OFFSET(DROIDVMPOOL_DIRECT_INTERFACE,AcquireMapping)==32);",
        "C_ASSERT(FIELD_OFFSET(DROIDVMPOOL_DIRECT_INTERFACE,ReleaseMapping)==40);",
    ):
        require_once(code, assertion, f"provider must retain ABI assertion: {assertion}")

    device_add = compact(function_body("DroidVmPoolEvtDeviceAdd", source))
    interface_steps = (
        "ExInitializeRundownProtection(&deviceContext->MappingReferences);",
        "directInterface.InterfaceHeader.Size=sizeof(directInterface);",
        "directInterface.InterfaceHeader.Version=DROIDVMPOOL_DIRECT_VERSION;",
        "directInterface.InterfaceHeader.Context=device;",
        "directInterface.InterfaceHeader.InterfaceReference=DroidVmPoolInterfaceReference;",
        "directInterface.InterfaceHeader.InterfaceDereference=DroidVmPoolInterfaceDereference;",
        "directInterface.AcquireMapping=DroidVmPoolAcquireMapping;",
        "directInterface.ReleaseMapping=DroidVmPoolReleaseMapping;",
        "WDF_QUERY_INTERFACE_CONFIG_INIT(&queryInterfaceConfig,(PINTERFACE)&directInterface,"
        "&GUID_DROIDVMPOOL_DIRECT_INTERFACE,NULL);",
        "status=WdfDeviceAddQueryInterface(device,&queryInterfaceConfig);",
        "status=WdfDeviceCreateDeviceInterface(device,&GUID_DEVINTERFACE_DROIDVMPOOL,NULL);",
    )
    interface_offsets = [device_add.find(step) for step in interface_steps]
    if min(interface_offsets) < 0 or interface_offsets != sorted(interface_offsets):
        fail("provider must initialize and register the complete current direct interface before discovery")

    interface_reference = compact(function_body("DroidVmPoolInterfaceReference", source))
    interface_dereference = compact(function_body("DroidVmPoolInterfaceDereference", source))
    require_once(
        interface_reference,
        "WdfObjectReference((WDFDEVICE)context);",
        "direct interface reference callback must retain the provider device",
    )
    require_once(
        interface_dereference,
        "WdfObjectDereference((WDFDEVICE)context);",
        "direct interface dereference callback must release the provider device",
    )

    read_uid = compact(function_body("DroidVmPoolReadUid", source))
    for token in (
        "input.Signature=ACPI_EVAL_INPUT_BUFFER_SIGNATURE;",
        "input.MethodNameAsUlong=DROIDVMPOOL_ACPI_UID_NAME;",
        "WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(device),NULL,IOCTL_ACPI_EVAL_METHOD,",
        "output->Signature!=ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE",
        "output->Count!=1",
        "argument->Type!=ACPI_METHOD_ARGUMENT_STRING",
        "argument->DataLength>DROIDVMPOOL_NAME_CAPACITY",
        "ACPI_METHOD_ARGUMENT_LENGTH(argument->DataLength)>output->Length-argumentOffset",
        "argument->Data[argument->DataLength-1]!=",
        "!DroidVmPoolNameCharacterIsValid(argument->Data[index])",
        "poolName[nameLength]=",
    ):
        require_once(read_uid, token, f"_UID parsing must retain fail-closed check: {token}")

    prepare = compact(function_body("DroidVmPoolEvtDevicePrepareHardware", source))
    uid_read = prepare.find("status=DroidVmPoolReadUid(")
    resource_walk = prepare.find("WdfCmResourceListGetCount(resourcesTranslated)")
    cache_policy = prepare.find("(descriptor->Flags&CM_RESOURCE_MEMORY_CACHEABLE)==0")
    map_pool = prepare.find("MmMapIoSpaceEx(")
    publish_ready = prepare.find("InterlockedExchange(&deviceContext->PoolReady,TRUE);")
    if min(uid_read, resource_walk, cache_policy, map_pool, publish_ready) < 0 or not (
        uid_read < resource_walk < cache_policy < map_pool < publish_ready
    ):
        fail("PrepareHardware must validate _UID, resources, and cache policy before mapping and publishing readiness")
    for token in (
        "if(memoryFound){returnSTATUS_DEVICE_CONFIGURATION_ERROR;}",
        "((ULONG64)poolPhysicalBase.QuadPart&(PAGE_SIZE-1))!=0",
        "(poolSize&(PAGE_SIZE-1))!=0",
        "(ULONG64)poolPhysicalBase.QuadPart>MAXULONGLONG-((ULONG64)poolSize-1)",
        "(descriptor->Flags&CM_RESOURCE_MEMORY_CACHEABLE)==0",
        "descriptor->Flags&(CM_RESOURCE_MEMORY_COMBINEDWRITE|CM_RESOURCE_MEMORY_PREFETCHABLE|"
        "CM_RESOURCE_MEMORY_READ_ONLY|CM_RESOURCE_MEMORY_WRITE_ONLY)",
        "poolVirtualBase=MmMapIoSpaceEx(poolPhysicalBase,poolSize,PAGE_READWRITE);",
        "if(deviceContext->PoolVirtualBase!=NULL){returnSTATUS_INVALID_DEVICE_STATE;}",
    ):
        require_once(prepare, token, f"PrepareHardware must retain resource bound: {token}")
    if "MmMapIoSpace(" in source:
        fail("provider must not retain the old-target MmMapIoSpace fallback")

    release = compact(function_body("DroidVmPoolEvtDeviceReleaseHardware", source))
    offline = release.find("InterlockedExchange(&deviceContext->PoolReady,FALSE);")
    rundown = release.find("ExWaitForRundownProtectionRelease(&deviceContext->MappingReferences);")
    rundown_complete = release.find("ExRundownCompleted(&deviceContext->MappingReferences);", rundown)
    unmap = release.find("MmUnmapIoSpace(deviceContext->PoolVirtualBase,deviceContext->PoolSize);")
    clear = release.find("deviceContext->PoolVirtualBase=NULL;")
    if min(offline, rundown, rundown_complete, unmap, clear) < 0 or not offline < rundown < rundown_complete < unmap < clear:
        fail("ReleaseHardware must withdraw readiness, complete mapping rundown, and drain leases before unmapping")
    if release.count("ExWaitForRundownProtectionRelease(&deviceContext->MappingReferences);") != 1 or release.count(
        "ExRundownCompleted(&deviceContext->MappingReferences);"
    ) != 1:
        fail("ReleaseHardware must complete mapping rundown exactly once")

    query = compact(function_body("DroidVmPoolEvtIoDeviceControl", source))
    acquire_query = query.find("ExAcquireRundownProtection(&deviceContext->MappingReferences)")
    ready_query = query.find("InterlockedCompareExchange(&deviceContext->PoolReady,FALSE,FALSE)==FALSE")
    release_query = query.rfind("ExReleaseRundownProtection(&deviceContext->MappingReferences);")
    complete_query = query.find("WdfRequestCompleteWithInformation(request,status,bytesReturned);")
    if min(acquire_query, ready_query, release_query, complete_query) < 0 or not (
        acquire_query < ready_query < release_query < complete_query
    ):
        fail("query endpoint must snapshot discovery metadata under provider rundown")
    for token in (
        "WdfRequestGetRequestorMode(request)!=KernelMode",
        "InterlockedCompareExchange(&deviceContext->PoolReady,FALSE,FALSE)==FALSE",
        "if(ioControlCode==IOCTL_DROIDVMPOOL_QUERY)",
        "inputBufferLength!=0||outputBufferLength!=sizeof(*output)",
        "outputValue.InterfaceVersion=DROIDVMPOOL_INTERFACE_VERSION;",
        "outputValue.StructureSize=sizeof(outputValue);",
        "outputValue.PoolNameLength=deviceContext->PoolNameLength;",
        "outputValue.BasePhysicalAddress=deviceContext->PoolPhysicalBase;",
        "outputValue.TotalSize=deviceContext->PoolSize;",
    ):
        require_once(query, token, f"query endpoint must retain kernel-only exact metadata contract: {token}")

    acquire = compact(function_body("DroidVmPoolAcquireMapping", source))
    release_mapping = compact(function_body("DroidVmPoolReleaseMapping", source))
    for token in (
        "KeGetCurrentIrql()>DISPATCH_LEVEL",
        "!KeAreAllApcsDisabled()",
        "DroidVmPoolGetDeviceContext((WDFDEVICE)context)",
        "ExAcquireRundownProtection(&deviceContext->MappingReferences)",
        "InterlockedCompareExchange(&deviceContext->PoolReady,FALSE,FALSE)==FALSE",
        "mappingValue.BaseVirtualAddress=deviceContext->PoolVirtualBase;",
        "mappingValue.BasePhysicalAddress=deviceContext->PoolPhysicalBase;",
        "mappingValue.TotalSize=deviceContext->PoolSize;",
    ):
        require_once(acquire, token, f"direct mapping acquire must retain lease step: {token}")
    require_once(
        release_mapping,
        "KeAreAllApcsDisabled()",
        "direct mapping release must require APCs to remain disabled",
    )
    require_once(
        release_mapping,
        "ExReleaseRundownProtection(&deviceContext->MappingReferences);",
        "direct mapping release must drop exactly one provider lease",
    )


def check_interface(interface: str, header: str) -> None:
    code = strip_comments_and_literals(interface)
    if len(re.findall(r"\bIOCTL_DROIDVMPOOL_[A-Z_]+\b", code)) != 1:
        fail("public interface must define only the QUERY IOCTL")
    require_once(code, "IOCTL_DROIDVMPOOL_QUERY", "public interface must define exactly one query IOCTL")
    if re.search(r"\b(?:ALLOCATE|ALLOC|FREE|RELEASE)\b", code, re.IGNORECASE):
        fail("public interface must not expose allocation or free operations")
    for token in (
        "DROIDVMPOOL_INTERFACE_VERSION",
        "DROIDVMPOOL_NAME_CAPACITY",
        "BasePhysicalAddress",
        "TotalSize",
        "PoolName",
        "DROIDVMPOOL_DIRECT_INTERFACE",
        "AcquireMapping",
        "ReleaseMapping",
    ):
        if token not in interface:
            fail(f"public query ABI is missing {token}")
    for token in (
        "#define DROIDVMPOOL_INTERFACE_VERSION 2U",
        "#define DROIDVMPOOL_DIRECT_VERSION    2U",
    ):
        require_once(interface, token, f"public interface must retain the current version-2 contract: {token}")
    if re.search(r"DROIDVMPOOL_(?:INTERFACE|DIRECT)_VERSION_V\d+", interface):
        fail("public interface must expose only the current contract, not compatibility version aliases")
    query_struct = compact(interface)
    query_match = re.search(r"typedefstruct_DROIDVMPOOL_QUERY_OUTPUT\{(?P<body>.*?)\}DROIDVMPOOL_QUERY_OUTPUT", query_struct)
    if query_match is None or "BaseVirtualAddress" in query_match.group("body"):
        fail("discovery query must not expose the provider-owned kernel VA")
    if header.count("DROIDVMPOOL_DEVICE_CONTEXT") < 4:
        fail("internal header must declare one per-device context type")


def check_inf(inf: str) -> None:
    if inf.count(r"ACPI\DRVM0001") != 1:
        fail("INF must bind exactly the shared ACPI\\DRVM0001 hardware ID")
    for forbidden in ("RDMA0000", "gpu_guest", "drm2kgsl_host"):
        if forbidden in inf:
            fail(f"INF must not attempt per-instance pool selection: {forbidden}")
    if "StartType      = 0" not in inf or "KmdfLibraryVersion = $KMDFVERSION$" not in inf:
        fail("provider INF must remain a boot-start KMDF driver")


def check_project() -> None:
    root = ET.parse(PROJECT_PATH).getroot()
    compile_inputs = [
        element.attrib["Include"].replace("\\", "/")
        for element in root.findall(".//msbuild:ClCompile[@Include]", NAMESPACE)
    ]
    if compile_inputs != ["droidvmpool.c"]:
        fail(f"provider project must compile only its provider source, found {compile_inputs}")
    inf_inputs = [
        element.attrib["Include"].replace("\\", "/")
        for element in root.findall(".//msbuild:Inf[@Include]", NAMESPACE)
    ]
    if inf_inputs != ["droidvmpool.inf"]:
        fail("provider project must track exactly its own INF")
    driver_types = [(element.text or "").strip() for element in root.findall(".//msbuild:DriverType", NAMESPACE)]
    platforms = [(element.text or "").strip() for element in root.findall(".//msbuild:Platform", NAMESPACE)]
    if not driver_types or any(driver_type != "KMDF" for driver_type in driver_types):
        fail("every provider configuration must remain KMDF")
    if platforms.count("ARM64") != 2:
        fail("provider project must retain Debug and Release ARM64 configurations")


def check_abi_fixture(workflow: str) -> None:
    for name in ("abi_manifest.cpp", "expected-v2.txt", "run-local.sh", "run-msvc.cmd"):
        if not (ABI_TEST_DIR / name).is_file():
            fail(f"provider ABI fixture is missing {name}")
    require_once(
        workflow,
        r"droidvmpool\tests\interface-abi\run-msvc.cmd",
        "ARM64 workflow must run the provider ABI fixture exactly once",
    )


def check_workflow(workflow: str) -> None:
    if workflow.count('"droidvmpool/**"') != 2:
        fail("ARM64 contract workflow must trigger on every provider change")
    require_once(
        workflow,
        "python droidvmpool/check-contract.py",
        "ARM64 workflow must run the provider contract checker exactly once",
    )
    require_once(
        workflow,
        "@{ Path = 'droidvmpool/droidvmpool.vcxproj'; Configuration = 'Release'; Platform = 'ARM64' },",
        "ARM64 workflow must compile the provider exactly once",
    )
    if "droidvmpool.sys" in workflow:
        fail("compile-only workflow must not upload the untested provider binary")


def main() -> None:
    source_text = SOURCE_PATH.read_text(encoding="utf-8")
    source = strip_comments_and_literals(source_text)
    header = HEADER_PATH.read_text(encoding="utf-8")
    interface = INTERFACE_PATH.read_text(encoding="utf-8")
    inf = INF_PATH.read_text(encoding="utf-8-sig")
    workflow = WORKFLOW_PATH.read_text(encoding="utf-8")

    check_provider(source)
    check_interface(interface, header)
    check_inf(inf)
    check_project()
    check_abi_fixture(workflow)
    check_workflow(workflow)
    print("droidvmpool provider contract: PASS")


if __name__ == "__main__":
    main()
