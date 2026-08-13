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
        "C_ASSERT(sizeof(DROIDVMPOOL_QUERY_OUTPUT)==104);",
        "C_ASSERT(FIELD_OFFSET(DROIDVMPOOL_QUERY_OUTPUT,PoolName)==40);",
    ):
        require_once(code, assertion, f"provider must retain ABI assertion: {assertion}")

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
    map_pool = prepare.find("MmMapIoSpaceEx(")
    publish_ready = prepare.find("deviceContext->PoolReady=TRUE;")
    if min(uid_read, resource_walk, map_pool, publish_ready) < 0 or not (
        uid_read < resource_walk < map_pool < publish_ready
    ):
        fail("PrepareHardware must validate _UID and resources before mapping and publishing readiness")
    for token in (
        "if(memoryFound){returnSTATUS_DEVICE_CONFIGURATION_ERROR;}",
        "((ULONG64)poolPhysicalBase.QuadPart&(PAGE_SIZE-1))!=0",
        "(poolSize&(PAGE_SIZE-1))!=0",
        "(ULONG64)poolPhysicalBase.QuadPart>MAXULONGLONG-((ULONG64)poolSize-1)",
        "if(deviceContext->PoolVirtualBase!=NULL){returnSTATUS_INVALID_DEVICE_STATE;}",
    ):
        require_once(prepare, token, f"PrepareHardware must retain resource bound: {token}")

    release = compact(function_body("DroidVmPoolEvtDeviceReleaseHardware", source))
    offline = release.find("deviceContext->PoolReady=FALSE;")
    unmap = release.find("MmUnmapIoSpace(deviceContext->PoolVirtualBase,deviceContext->PoolSize);")
    clear = release.find("deviceContext->PoolVirtualBase=NULL;")
    if min(offline, unmap, clear) < 0 or not offline < unmap < clear:
        fail("ReleaseHardware must withdraw readiness before unmapping the provider mapping")

    query = compact(function_body("DroidVmPoolEvtIoDeviceControl", source))
    for token in (
        "WdfRequestGetRequestorMode(request)!=KernelMode",
        "if(!deviceContext->PoolReady)",
        "if(ioControlCode==IOCTL_DROIDVMPOOL_QUERY)",
        "inputBufferLength!=0||outputBufferLength!=sizeof(*output)",
        "outputValue.InterfaceVersion=DROIDVMPOOL_INTERFACE_VERSION_V1;",
        "outputValue.StructureSize=sizeof(outputValue);",
        "outputValue.PoolNameLength=deviceContext->PoolNameLength;",
        "outputValue.BaseVirtualAddress=deviceContext->PoolVirtualBase;",
        "outputValue.BasePhysicalAddress=deviceContext->PoolPhysicalBase;",
        "outputValue.TotalSize=deviceContext->PoolSize;",
    ):
        require_once(query, token, f"query endpoint must retain kernel-only exact metadata contract: {token}")


def check_interface(interface: str, header: str) -> None:
    code = strip_comments_and_literals(interface)
    if len(re.findall(r"\bIOCTL_DROIDVMPOOL_[A-Z_]+\b", code)) != 1:
        fail("public interface must define only the QUERY IOCTL")
    require_once(code, "IOCTL_DROIDVMPOOL_QUERY", "public interface must define exactly one query IOCTL")
    if re.search(r"\b(?:ALLOCATE|ALLOC|FREE|RELEASE)\b", code, re.IGNORECASE):
        fail("public interface must not expose allocation or free operations")
    for token in (
        "DROIDVMPOOL_INTERFACE_VERSION_V1",
        "DROIDVMPOOL_NAME_CAPACITY",
        "BaseVirtualAddress",
        "BasePhysicalAddress",
        "TotalSize",
        "PoolName",
    ):
        if token not in interface:
            fail(f"public query ABI is missing {token}")
    if header.count("DROIDVMPOOL_DEVICE_CONTEXT") != 4:
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
    check_workflow(workflow)
    print("droidvmpool provider contract: PASS")


if __name__ == "__main__":
    main()
