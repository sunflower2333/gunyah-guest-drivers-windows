#!/usr/bin/env python3

import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Optional


PROJECT_DIR = Path(__file__).resolve().parent
DRIVER_SOURCE_PATH = (PROJECT_DIR / "driver_entry.cpp").resolve()
DRIVER_SOURCE = DRIVER_SOURCE_PATH.read_text(encoding="utf-8")
WDDM_DDI_SOURCE_PATH = (PROJECT_DIR / "wddmddi.cpp").resolve()
WDDM_DDI_SOURCE = WDDM_DDI_SOURCE_PATH.read_text(encoding="utf-8")
WDDM_DDI_HEADER_PATH = (PROJECT_DIR / "wddmddi.h").resolve()
WDDM_DDI_HEADER_SOURCE = WDDM_DDI_HEADER_PATH.read_text(encoding="utf-8")
VIOGPU_SOURCE_PATH = (PROJECT_DIR.parent / "viogpudo" / "viogpudo.cpp").resolve()
VIOGPU_HEADER_PATH = (PROJECT_DIR.parent / "viogpudo" / "viogpudo.h").resolve()
DOD_DRIVER_SOURCE_PATH = (PROJECT_DIR.parent / "viogpudo" / "driver.cpp").resolve()
QUEUE_HEADER_PATH = (PROJECT_DIR.parent / "common" / "viogpu_queue.h").resolve()
WIRE_HEADER_PATH = (PROJECT_DIR.parent / "common" / "viogpu_3d_wire.h").resolve()
QUEUE_SOURCE_PATH = (PROJECT_DIR.parent / "common" / "viogpu_queue.cpp").resolve()
RDMA_SOURCE_PATH = (PROJECT_DIR.parent / "common" / "viogpu_rdma.cpp").resolve()
RDMA_HEADER_PATH = (PROJECT_DIR.parent / "common" / "viogpu_rdma.h").resolve()
RDMAPOOL_DIR = (PROJECT_DIR.parent.parent / "rdmapool").resolve()
RDMAPOOL_INTERFACE_PATH = RDMAPOOL_DIR / "rdmapool_interface.h"
RDMAPOOL_PROVIDER_PATH = RDMAPOOL_DIR / "rdmapool.c"
DMAPOOL_SOURCE_PATH = RDMAPOOL_DIR / "dmapool.c"
SHARED_RDMA_CLIENT_PATH = RDMAPOOL_DIR / "rdmaclient.c"
SHARED_RDMA_CLIENT_HEADER_PATH = RDMAPOOL_DIR / "rdmaclient.h"
WDF_DIR = (PROJECT_DIR.parent.parent / "VirtIO" / "WDF").resolve()
WDF_DMA_PATH = WDF_DIR / "Dma.c"
WDF_SOURCE_PATH = WDF_DIR / "VirtIOWdf.c"
WDF_HEADER_PATH = WDF_DIR / "VirtIOWdf.h"
PCI_SOURCE_PATH = (PROJECT_DIR.parent / "common" / "viogpu_pci.cpp").resolve()
VIRTIO_DIR = (PROJECT_DIR.parent.parent / "VirtIO").resolve()
VIRTIO_HEADER_PATH = VIRTIO_DIR / "virtio_pci.h"
VIRTIO_COMMON_PATH = VIRTIO_DIR / "VirtIOPCICommon.c"
VIRTIO_MODERN_PATH = VIRTIO_DIR / "VirtIOPCIModern.c"
VIRTIO_LEGACY_PATH = VIRTIO_DIR / "VirtIOPCILegacy.c"
NETKVM_DIR = (PROJECT_DIR.parent.parent / "NetKVM").resolve()
NETKVM_RDMA_SOURCE_PATH = NETKVM_DIR / "Common" / "ParaNdis_RdmaPool.cpp"
NETKVM_COMMON_SOURCE_PATH = NETKVM_DIR / "Common" / "ParaNdis_Common.cpp"
NETKVM_UTIL_SOURCE_PATH = NETKVM_DIR / "Common" / "ParaNdis_Util.cpp"
NETKVM_HEADER_SOURCE_PATH = NETKVM_DIR / "Common" / "ndis56common.h"
NETKVM_IMPL_SOURCE_PATH = NETKVM_DIR / "wlh" / "ParaNdis6_Impl.cpp"
NETKVM_DRIVER_SOURCE_PATH = NETKVM_DIR / "wlh" / "ParaNdis6_Driver.cpp"
PROJECT = PROJECT_DIR / "viogpuwddm.vcxproj"
WPP_NON_OWNER_TEMPLATE = PROJECT_DIR / "wpp-non-owner.tpl"
NAMESPACE = {"msbuild": "http://schemas.microsoft.com/developer/msbuild/2003"}
REGISTRATION_HELPER = "VioGpuWddmInitializeMiniportCompileOnly"
STORAGE_PROJECTS = (
    (
        "viostor",
        (PROJECT_DIR.parent.parent / "viostor" / "viostor.vcxproj").resolve(),
        "VirtIoFindAdapter",
        "RhelGetDiskGeometry",
        "RhelSetGuestFeatures",
    ),
    (
        "vioscsi",
        (PROJECT_DIR.parent.parent / "vioscsi" / "vioscsi.vcxproj").resolve(),
        "VioScsiFindAdapter",
        "GetScsiConfig",
        "SetGuestFeatures",
    ),
)
STORPORT_PROHIBITED_BROKER_DDIS = (
    "IoBuildDeviceIoControlRequest",
    "IoCallDriver",
    "IoGetDeviceInterfaces",
    "IoGetDeviceObjectPointer",
)


def strip_cpp_comments_and_literals(source: str) -> str:
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

        raw_string = re.match(r'(?:u8|u|U|L)?R"([^ ()\\\t\r\n]{0,16})\(', source[offset:])
        if raw_string is not None:
            delimiter = raw_string.group(1)
            marker = ")" + delimiter + '"'
            end = source.find(marker, offset + raw_string.end())
            end = len(source) if end == -1 else end + len(marker)
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
            if source[offset:end] != '"C"':
                blank(offset, end)
            offset = end
            continue

        offset += 1

    return "".join(result)


DRIVER_CODE = strip_cpp_comments_and_literals(DRIVER_SOURCE)
WDDM_DDI_CODE = strip_cpp_comments_and_literals(WDDM_DDI_SOURCE)
WDDM_DDI_HEADER_CODE = strip_cpp_comments_and_literals(WDDM_DDI_HEADER_SOURCE)
VIOGPU_SOURCE = VIOGPU_SOURCE_PATH.read_text(encoding="utf-8")
VIOGPU_HEADER_SOURCE = VIOGPU_HEADER_PATH.read_text(encoding="utf-8")
DOD_DRIVER_SOURCE = DOD_DRIVER_SOURCE_PATH.read_text(encoding="utf-8")
QUEUE_HEADER_SOURCE = QUEUE_HEADER_PATH.read_text(encoding="utf-8")
RDMA_SOURCE = RDMA_SOURCE_PATH.read_text(encoding="utf-8")
RDMA_HEADER_SOURCE = RDMA_HEADER_PATH.read_text(encoding="utf-8")
RDMAPOOL_INTERFACE_SOURCE = RDMAPOOL_INTERFACE_PATH.read_text(encoding="utf-8")
RDMAPOOL_PROVIDER_SOURCE = RDMAPOOL_PROVIDER_PATH.read_text(encoding="utf-8")
DMAPOOL_SOURCE = DMAPOOL_SOURCE_PATH.read_text(encoding="utf-8")
SHARED_RDMA_CLIENT_SOURCE = SHARED_RDMA_CLIENT_PATH.read_text(encoding="utf-8")
SHARED_RDMA_CLIENT_HEADER_SOURCE = SHARED_RDMA_CLIENT_HEADER_PATH.read_text(encoding="utf-8")
WDF_DMA_SOURCE = WDF_DMA_PATH.read_text(encoding="utf-8")
WDF_SOURCE = WDF_SOURCE_PATH.read_text(encoding="utf-8")
WDF_HEADER_SOURCE = WDF_HEADER_PATH.read_text(encoding="utf-8")
PCI_SOURCE = PCI_SOURCE_PATH.read_text(encoding="utf-8")
VIRTIO_HEADER_SOURCE = VIRTIO_HEADER_PATH.read_text(encoding="utf-8")
VIRTIO_COMMON_SOURCE = VIRTIO_COMMON_PATH.read_text(encoding="utf-8")
VIRTIO_MODERN_SOURCE = VIRTIO_MODERN_PATH.read_text(encoding="utf-8")
VIRTIO_LEGACY_SOURCE = VIRTIO_LEGACY_PATH.read_text(encoding="utf-8")
NETKVM_RDMA_SOURCE = NETKVM_RDMA_SOURCE_PATH.read_text(encoding="utf-8")
NETKVM_COMMON_SOURCE = NETKVM_COMMON_SOURCE_PATH.read_text(encoding="utf-8")
NETKVM_UTIL_SOURCE = NETKVM_UTIL_SOURCE_PATH.read_text(encoding="utf-8")
NETKVM_HEADER_SOURCE = NETKVM_HEADER_SOURCE_PATH.read_text(encoding="utf-8")
NETKVM_IMPL_SOURCE = NETKVM_IMPL_SOURCE_PATH.read_text(encoding="utf-8")
NETKVM_DRIVER_SOURCE = NETKVM_DRIVER_SOURCE_PATH.read_text(encoding="utf-8")
VIOGPU_CODE = strip_cpp_comments_and_literals(VIOGPU_SOURCE)
VIOGPU_HEADER_CODE = strip_cpp_comments_and_literals(VIOGPU_HEADER_SOURCE)
DOD_DRIVER_CODE = strip_cpp_comments_and_literals(DOD_DRIVER_SOURCE)
WIRE_HEADER_CODE = strip_cpp_comments_and_literals(WIRE_HEADER_PATH.read_text(encoding="utf-8"))
QUEUE_CODE = strip_cpp_comments_and_literals(QUEUE_SOURCE_PATH.read_text(encoding="utf-8"))
RDMA_CODE = strip_cpp_comments_and_literals(RDMA_SOURCE)
RDMA_HEADER_CODE = strip_cpp_comments_and_literals(RDMA_HEADER_SOURCE)
RDMAPOOL_INTERFACE_CODE = strip_cpp_comments_and_literals(RDMAPOOL_INTERFACE_SOURCE)
RDMAPOOL_PROVIDER_CODE = strip_cpp_comments_and_literals(RDMAPOOL_PROVIDER_SOURCE)
DMAPOOL_CODE = strip_cpp_comments_and_literals(DMAPOOL_SOURCE)
SHARED_RDMA_CLIENT_CODE = strip_cpp_comments_and_literals(SHARED_RDMA_CLIENT_SOURCE)
SHARED_RDMA_CLIENT_HEADER_CODE = strip_cpp_comments_and_literals(SHARED_RDMA_CLIENT_HEADER_SOURCE)
WDF_DMA_CODE = strip_cpp_comments_and_literals(WDF_DMA_SOURCE)
WDF_CODE = strip_cpp_comments_and_literals(WDF_SOURCE)
WDF_HEADER_CODE = strip_cpp_comments_and_literals(WDF_HEADER_SOURCE)
PCI_CODE = strip_cpp_comments_and_literals(PCI_SOURCE)
VIRTIO_HEADER_CODE = strip_cpp_comments_and_literals(VIRTIO_HEADER_SOURCE)
VIRTIO_COMMON_CODE = strip_cpp_comments_and_literals(VIRTIO_COMMON_SOURCE)
VIRTIO_MODERN_CODE = strip_cpp_comments_and_literals(VIRTIO_MODERN_SOURCE)
VIRTIO_LEGACY_CODE = strip_cpp_comments_and_literals(VIRTIO_LEGACY_SOURCE)
NETKVM_RDMA_CODE = strip_cpp_comments_and_literals(NETKVM_RDMA_SOURCE)
NETKVM_COMMON_CODE = strip_cpp_comments_and_literals(NETKVM_COMMON_SOURCE)
NETKVM_UTIL_CODE = strip_cpp_comments_and_literals(NETKVM_UTIL_SOURCE)
NETKVM_HEADER_CODE = strip_cpp_comments_and_literals(NETKVM_HEADER_SOURCE)
NETKVM_IMPL_CODE = strip_cpp_comments_and_literals(NETKVM_IMPL_SOURCE)
NETKVM_DRIVER_CODE = strip_cpp_comments_and_literals(NETKVM_DRIVER_SOURCE)


def fail(message: str) -> None:
    print(f"viogpuwddm contract failure: {message}", file=sys.stderr)
    raise SystemExit(1)


def function_body_span(name: str, source: Optional[str] = None) -> tuple[str, int, int]:
    if source is None:
        source = DRIVER_CODE

    matches = list(
        re.finditer(
            rf"\b{re.escape(name)}\s*\([^;{{}}]*?\)\s*(?:const\s*)?\{{",
            source,
            re.DOTALL,
        )
    )
    if len(matches) != 1:
        fail(f"expected one definition of {name}, found {len(matches)}")

    match = matches[0]
    start = match.end() - 1
    depth = 0
    for offset, character in enumerate(source[start:], start=start):
        if character == "{":
            depth += 1
        elif character == "}":
            depth -= 1
            if depth == 0:
                return source[start + 1 : offset], start + 1, offset

    fail(f"unterminated function {name}")
    return "", 0, 0


def function_body(name: str, source: Optional[str] = None) -> str:
    return function_body_span(name, source)[0]


def function_body_with_parameters(name: str, parameters: str, source: str) -> str:
    expected = compact_code(parameters)
    matches = [
        match
        for match in re.finditer(
            rf"\b{re.escape(name)}\s*\((?P<parameters>[^;{{}}]*?)\)\s*(?:const\s*)?\{{",
            source,
            re.DOTALL,
        )
        if compact_code(match.group("parameters")) == expected
    ]
    if len(matches) != 1:
        fail(f"expected one definition of {name}({parameters}), found {len(matches)}")

    start = matches[0].end() - 1
    depth = 0
    for offset, character in enumerate(source[start:], start=start):
        if character == "{":
            depth += 1
        elif character == "}":
            depth -= 1
            if depth == 0:
                return source[start + 1 : offset]

    fail(f"unterminated function {name}({parameters})")
    return ""


def compact_code(source: str) -> str:
    return re.sub(r"\s+", "", source)


def canonical_code(source: str) -> str:
    code = compact_code(source)
    code = re.sub(r"\bnullptr\b", "NULL", code)
    code = re.sub(r"\bfalse\b", "FALSE", code)
    code = re.sub(r"\btrue\b", "TRUE", code)
    code = re.sub(r"(?<=\d)[uU]\b", "", code)
    return code


def is_failure_condition(condition: str, status: str) -> bool:
    condition = canonical_code(condition)
    escaped = re.escape(status)
    return any(
        re.fullmatch(pattern, condition) is not None
        for pattern in (
            rf"!NT_SUCCESS\({escaped}\)",
            rf"NT_SUCCESS\({escaped}\)==FALSE",
            rf"FALSE==NT_SUCCESS\({escaped}\)",
        )
    )


def is_success_condition(condition: str, status: str) -> bool:
    condition = canonical_code(condition)
    escaped = re.escape(status)
    return any(
        re.fullmatch(pattern, condition) is not None
        for pattern in (
            rf"NT_SUCCESS\({escaped}\)",
            rf"NT_SUCCESS\({escaped}\)!=FALSE",
            rf"FALSE!=NT_SUCCESS\({escaped}\)",
        )
    )


def is_equality_condition(condition: str, left: str, right: str) -> bool:
    condition = canonical_code(condition)
    return condition in (f"{left}=={right}", f"{right}=={left}")


def if_blocks(source: str) -> list[tuple[str, str, int, int]]:
    blocks: list[tuple[str, str, int, int]] = []
    for match in re.finditer(r"\bif\s*\(", source):
        condition_start = source.find("(", match.start())
        depth = 0
        condition_end = -1
        for offset in range(condition_start, len(source)):
            if source[offset] == "(":
                depth += 1
            elif source[offset] == ")":
                depth -= 1
                if depth == 0:
                    condition_end = offset
                    break
        if condition_end < 0:
            continue

        body_start = condition_end + 1
        while body_start < len(source) and source[body_start].isspace():
            body_start += 1
        if body_start >= len(source) or source[body_start] != "{":
            continue

        depth = 0
        body_end = -1
        for offset in range(body_start, len(source)):
            if source[offset] == "{":
                depth += 1
            elif source[offset] == "}":
                depth -= 1
                if depth == 0:
                    body_end = offset
                    break
        if body_end >= 0:
            blocks.append(
                (
                    source[condition_start + 1 : condition_end],
                    source[body_start + 1 : body_end],
                    match.start(),
                    body_end + 1,
                )
            )
    return blocks


def check_virtio_reset_contract() -> None:
    """Require a bounded, status-returning reset path without constraining legacy callers."""
    public_checked = set(
        re.findall(
            r"\bNTSTATUS\s+(virtio_[A-Za-z0-9_]*reset[A-Za-z0-9_]*)\s*\(",
            VIRTIO_HEADER_CODE,
        )
    )
    if not public_checked:
        fail("VirtIO must expose a checked NTSTATUS reset API")

    initialize = canonical_code(function_body("virtio_device_initialize", VIRTIO_COMMON_CODE))
    init_calls = re.findall(
        r"\b(virtio_[A-Za-z0-9_]*reset[A-Za-z0-9_]*)\s*\(\s*vdev\s*\)",
        initialize,
    )
    init_calls = [name for name in init_calls if name in public_checked]
    if len(init_calls) != 1:
        fail("virtio_device_initialize must use exactly one checked reset API")
    checked_name = init_calls[0]
    reset_assignment = f"status={checked_name}(vdev);"
    reset_failure = "if(!NT_SUCCESS(status)){returnstatus;}"
    acknowledge = "virtio_add_status(vdev,VIRTIO_CONFIG_S_ACKNOWLEDGE);"
    if (
        initialize.count(reset_assignment) != 1
        or initialize.count(reset_failure) != 1
        or initialize.find(reset_assignment) > initialize.find(reset_failure)
        or initialize.find(reset_failure) > initialize.find(acknowledge)
    ):
        fail("virtio_device_initialize must propagate checked reset failure before acknowledging the device")

    checked_definition = canonical_code(function_body(checked_name, VIRTIO_COMMON_CODE))
    if not re.search(r"\breturn(?:\s+|vdev->)", checked_definition):
        fail("checked VirtIO reset API must return a reset status")
    if not re.search(r"returnvdev->device->reset_checked\(vdev\);", checked_definition):
        fail("checked VirtIO reset API must expose the transport reset result")

    stop = canonical_code(function_body("VioGpuAdapter::StopNativeContextTransportLocked", VIOGPU_CODE))
    stop_calls = re.findall(
        r"\b(virtio_[A-Za-z0-9_]*reset[A-Za-z0-9_]*)\s*\(\s*&m_VioDev\s*\)",
        stop,
    )
    stop_calls = [name for name in stop_calls if name in public_checked]
    if len(stop_calls) != 1 or stop_calls[0] != checked_name:
        fail("native-context teardown must consume the same checked reset API")
    if stop.count(f"status={checked_name}(&m_VioDev);") != 1:
        fail("native-context teardown must consume checked reset status before queue teardown")

    for source, transport in (
        (VIRTIO_MODERN_CODE, "modern"),
        (VIRTIO_LEGACY_CODE, "legacy"),
    ):
        candidates = re.findall(
            rf"\b(?:static\s+)?NTSTATUS\s+([A-Za-z_][A-Za-z0-9_]*{transport}[A-Za-z0-9_]*reset[A-Za-z0-9_]*)\s*"
            rf"\(\s*VirtIODevice\s*\*\s*vdev\s*\)\s*\{{",
            source,
        )
        if len(candidates) != 1:
            fail(f"{transport} VirtIO transport must expose one checked bounded reset implementation")
        name = candidates[0]
        definition = canonical_code(function_body(name, source))
        poll_limits = re.findall(
            rf"#define\s+(VIRTIO_[A-Z0-9_]*{transport.upper()}[A-Z0-9_]*RESET_POLL_LIMIT)\s+([0-9]+U?)",
            source,
        )
        if len(poll_limits) != 1 or int(poll_limits[0][1].rstrip("U")) <= 0:
            fail(f"{transport} VirtIO reset must define one finite poll limit")
        poll_limit = poll_limits[0][0]
        loop = re.search(
            rf"for\(poll=0;poll<{re.escape(poll_limit)};\+\+poll\)\{{",
            definition,
        )
        if loop is None:
            fail(f"{name} must use a bounded reset poll loop")
        if definition.count("vdev_sleep(vdev,1);") != 1:
            fail(f"{name} must sleep once per unsuccessful reset poll")
        for result in ("STATUS_SUCCESS", "STATUS_DEVICE_NOT_CONNECTED", "STATUS_IO_TIMEOUT"):
            if definition.count(f"return{result};") != 1:
                fail(f"{name} must report {result} exactly once")
        if len(re.findall(rf"\.reset_checked\s*=\s*{re.escape(name)}\s*,", source)) != 1:
            fail(f"{transport} VirtIO ops must wire its checked reset callback")


def check_virtio_queue_allocation_cleanup() -> None:
    """Require modern queue setup to release every allocation on failure."""
    setup_body = function_body("vio_modern_setup_vq", VIRTIO_MODERN_CODE)
    setup = canonical_code(setup_body)
    allocation = "vq_addr=mem_alloc_nonpaged_block(vdev,heap_size);"
    allocation_offset = setup.find(allocation)
    allocation_failures = [
        canonical_code(body)
        for condition, body, start, _ in if_blocks(setup_body)
        if canonical_code(condition) == "vq_addr==NULL"
        and len(canonical_code(setup_body[:start])) > allocation_offset
    ]
    expected_failure = (
        "mem_free_contiguous_pages(vdev,info->queue);"
        "info->queue=NULL;"
        "returnSTATUS_INSUFFICIENT_RESOURCES;"
    )
    if allocation_offset < 0 or allocation_failures != [expected_failure]:
        fail("modern queue setup must release its contiguous ring when control-block allocation fails")

    shared_failure = (
        "mem_free_nonpaged_block(vdev,vq_addr);"
        "mem_free_contiguous_pages(vdev,info->queue);"
        "info->queue=NULL;"
        "returnstatus;"
    )
    if setup.count(shared_failure) != 1:
        fail("modern queue setup must clear the ring owner after every later setup failure")

    delete = canonical_code(function_body("vio_modern_del_vq", VIRTIO_MODERN_CODE))
    delete_release = (
        "mem_free_nonpaged_block(vdev,vq);"
        "mem_free_contiguous_pages(vdev,info->queue);"
        "info->queue=NULL;"
    )
    if delete.count(delete_release) != 1:
        fail("modern queue deletion must clear the ring owner after releasing both allocations")


def check_dod_reset_entrypoints() -> None:
    """Check reset callbacks before they dereference the replaceable adapter."""
    dpc = canonical_code(function_body("VioGpuDod::DpcRoutine", VIOGPU_CODE))
    dpc_acquire = dpc.find("if(ExAcquireRundownProtection(&m_HardwareOperations))")
    dpc_adapter = dpc.find("VioGpuAdapter*adapter=m_pHWDevice;", dpc_acquire)
    dpc_call = dpc.find("adapter->DpcRoutine(&m_DxgkInterface);", dpc_adapter)
    dpc_release = dpc.find("ExReleaseRundownProtection(&m_HardwareOperations);", dpc_call)
    dpc_notify = dpc.find("m_DxgkInterface.DxgkCbNotifyDpc((HANDLE)m_DxgkInterface.DeviceHandle);", dpc_release)
    if min(dpc_acquire, dpc_adapter, dpc_call, dpc_release, dpc_notify) < 0 or not (
        dpc_acquire < dpc_adapter < dpc_call < dpc_release < dpc_notify
    ):
        fail("DpcRoutine must hold hardware rundown across adapter access and notify DxgK after release")
    if (
        dpc.count("ExAcquireRundownProtection(&m_HardwareOperations)") != 1
        or dpc.count("ExReleaseRundownProtection(&m_HardwareOperations);") != 1
        or dpc.count("adapter->DpcRoutine(&m_DxgkInterface);") != 1
        or "m_pHWDevice->DpcRoutine" in dpc
    ):
        fail("DpcRoutine must use one balanced protected hardware snapshot")

    reset = canonical_code(function_body("VioGpuDod::ResetDevice", VIOGPU_CODE))
    reset_gate = "InterlockedExchange(&m_HardwareResetRequested,TRUE);"
    reset_acquire = "ExAcquireRundownProtection(&m_HardwareOperations)"
    reset_release = "ExReleaseRundownProtection(&m_HardwareOperations);"
    reset_adapter = "VioGpuAdapter*adapter=m_pHWDevice;"
    if (
        reset.count(reset_gate) != 1
        or reset.count(reset_acquire) != 1
        or reset.count(reset_release) != 1
    ):
        fail("ResetDevice must publish its reset gate and balance hardware rundown protection")
    reset_acquire_gate = (
        "if(KeGetCurrentIrql()<=DISPATCH_LEVEL&&"
        "ExAcquireRundownProtection(&m_HardwareOperations)){"
    )
    if reset.count(reset_acquire_gate) != 1:
        fail("ResetDevice must acquire nonpaged hardware rundown only at or below DISPATCH_LEVEL")
    reset_stages = (
        (reset.find(reset_gate), "reset gate publication"),
        (reset.find(reset_acquire_gate), "IRQL and rundown gate"),
        (reset.find(reset_acquire), "hardware rundown acquire"),
        (reset.find(reset_adapter), "adapter snapshot"),
        (reset.find("adapter->ResetDevice();"), "adapter reset"),
        (reset.find(reset_release), "hardware rundown release"),
    )
    for (offset, description), (next_offset, next_description) in zip(reset_stages, reset_stages[1:]):
        if offset < 0 or next_offset < 0 or offset > next_offset:
            fail(f"ResetDevice must perform {description} before {next_description}")

    display = canonical_code(function_body("VioGpuDod::SystemDisplayEnable", VIOGPU_CODE))
    display_gate = "InterlockedExchange(&m_HardwareResetRequested,TRUE);"
    display_acquire = "ExAcquireRundownProtection(&m_HardwareOperations)"
    display_release = "ExReleaseRundownProtection(&m_HardwareOperations);"
    display_adapter = "VioGpuAdapter*adapter=m_pHWDevice;"
    if (
        display.count(display_gate) != 1
        or display.count(display_acquire) != 1
        or display.count(display_release) != 1
    ):
        fail("SystemDisplayEnable must publish its reset gate and balance hardware rundown protection")
    if "KeGetCurrentIrql()!=PASSIVE_LEVEL" not in display:
        fail("SystemDisplayEnable must require PASSIVE_LEVEL before its hardware rundown acquire")
    first_return = display.find("return")
    if first_return >= 0 and display.find(display_gate) > first_return:
        fail("SystemDisplayEnable must publish its reset gate before every early return")
    display_stages = (
        (display.find(display_gate), "reset gate publication"),
        (display.find("if(KeGetCurrentIrql()!=PASSIVE_LEVEL)"), "IRQL gate"),
        (display.find(display_acquire), "hardware rundown acquire"),
        (display.find(display_adapter), "adapter snapshot"),
        (display.find("adapter->ResetToVgaMode()"), "VGA reset"),
        (display.find(display_release), "hardware rundown release"),
    )
    for (offset, description), (next_offset, next_description) in zip(display_stages, display_stages[1:]):
        if offset < 0 or next_offset < 0 or offset > next_offset:
            fail(f"SystemDisplayEnable must perform {description} before {next_description}")


def variable_write_offsets(source: str, expression: str) -> list[int]:
    """Find direct, compound, increment, and Rtl memory-helper writes."""
    target = re.escape(expression)
    assignment = rf"(?<![A-Za-z0-9_]){target}\s*(?:<<=|>>=|[+\-*/%&|^]=|=(?!=)|\+\+|--)"
    prefix_increment = rf"(?:\+\+|--)\s*{target}(?![A-Za-z0-9_])"
    memory_write = (
        rf"\bRtl(?:Zero|Copy|Move|Fill)Memory\s*\(\s*&\s*{target}"
        rf"(?![A-Za-z0-9_])"
    )
    return sorted(
        match.start()
        for pattern in (assignment, prefix_increment, memory_write)
        for match in re.finditer(pattern, source)
    )


def aliases_of(source: str, expression: str) -> set[str]:
    """Resolve simple local pointer/reference aliases used by lifecycle code."""
    aliases = {expression}
    while True:
        changed = False
        rhs = "|".join(re.escape(alias) for alias in sorted(aliases, key=len, reverse=True))
        pattern = rf"\b([A-Za-z_]\w*)\s*=\s*&?\s*(?:{rhs})(?![A-Za-z0-9_])\s*;"
        for match in re.finditer(pattern, source):
            alias = match.group(1)
            if alias not in aliases:
                aliases.add(alias)
                changed = True
        if not changed:
            return aliases


def method_call_offsets(source: str, receivers: set[str], method: str) -> list[int]:
    receiver = "|".join(re.escape(name) for name in sorted(receivers, key=len, reverse=True))
    return [
        match.start()
        for match in re.finditer(rf"(?<![A-Za-z0-9_])(?:{receiver})\s*(?:\.|->)\s*{re.escape(method)}\s*\(", source)
    ]


def top_level_control_transfers(source: str) -> list[tuple[str, int]]:
    """Return function-body return/goto tokens outside nested brace scopes."""
    transfers: list[tuple[str, int]] = []
    depth = 0
    for match in re.finditer(r"[{}]|\breturn\b|\bgoto\b", source):
        token = match.group(0)
        if token == "{":
            depth += 1
        elif token == "}":
            depth -= 1
        elif depth == 0:
            transfers.append((token, match.start()))
    return transfers


def require_single_final_return(body: str, expected: str, owner: str) -> None:
    transfers = top_level_control_transfers(body)
    if [token for token, _ in transfers] != ["return"] or not canonical_code(body).endswith(canonical_code(expected)):
        fail(f"{owner} must have only its required final top-level return")


def require_call_count(body: str, name: str, expected: int, owner: str) -> None:
    count = len(re.findall(rf"\b{re.escape(name)}\s*\(", body))
    if count != expected:
        fail(f"{owner} must call {name} exactly {expected} time(s), found {count}")


def require_integer_define(source: str, name: str, expected: int, owner: str) -> None:
    definitions = re.findall(
        rf"^[ \t]*#[ \t]*define[ \t]+{re.escape(name)}[ \t]+([^\r\n]+)$",
        source,
        re.MULTILINE,
    )
    if len(definitions) != 1:
        fail(f"{owner} must define {name} exactly once")

    value = definitions[0].strip()
    match = re.fullmatch(r"\(*\s*(0[xX][0-9a-fA-F]+|[0-9]+)(?:[uUlL]+)?\s*\)*", value)
    if match is None or int(match.group(1), 0) != expected:
        fail(f"{owner} must define {name} as {expected}, found {value}")


def require_alias_define(source: str, name: str, expected: str, owner: str) -> None:
    definitions = re.findall(
        rf"^[ \t]*#[ \t]*define[ \t]+{re.escape(name)}[ \t]+([^\r\n]+)$",
        source,
        re.MULTILINE,
    )
    if len(definitions) != 1 or compact_code(definitions[0]) != expected:
        fail(f"{owner} must define {name} only as {expected}")


def project_compile_sources(root: ET.Element, project: Path = PROJECT) -> dict[Path, str]:
    sources: dict[Path, str] = {}
    for element in root.findall(".//msbuild:ClCompile[@Include]", NAMESPACE):
        include = element.attrib["Include"].replace("\\", "/")
        path = (project.parent / include).resolve()
        if not path.is_file():
            fail(f"project ClCompile input does not exist: {include}")
        if path in sources:
            fail(f"project contains duplicate ClCompile input: {include}")
        sources[path] = strip_cpp_comments_and_literals(path.read_text(encoding="utf-8"))

    if not sources:
        fail("project contains no ClCompile inputs")
    return sources


def require_storport_access_platform_gate(
    owner: str,
    sources: dict[Path, str],
    find_adapter_name: str,
    read_features_name: str,
    acknowledge_features_name: str,
) -> None:
    definitions = [source for source in sources.values() if re.search(
        rf"\b{re.escape(find_adapter_name)}\s*\([^;{{}}]*\)\s*\{{", source, re.DOTALL
    )]
    if len(definitions) != 1:
        fail(f"{owner} must compile exactly one {find_adapter_name} definition")

    find_adapter = function_body(find_adapter_name, definitions[0])
    compact = canonical_code(find_adapter)
    feature_read = compact.find(f"{read_features_name}(DeviceExtension);")
    feature_acknowledge = compact.find(f"{acknowledge_features_name}(DeviceExtension);")
    queue_sizing = compact.find("virtio_query_queue_allocation(")
    dma_allocation = compact.find("StorPortGetUncachedExtension(")
    if min(feature_read, feature_acknowledge, queue_sizing, dma_allocation) < 0:
        fail(f"{owner} FindAdapter is missing a required feature, queue, or DMA initialization stage")

    gates = [
        (body, start, end)
        for condition, body, start, end in if_blocks(find_adapter)
        if canonical_code(condition) == "CHECKBIT(adaptExt->features,VIRTIO_F_ACCESS_PLATFORM)"
        and canonical_code(body).endswith("returnSP_RETURN_ERROR;")
    ]
    if len(gates) != 1:
        fail(f"{owner} must reject VIRTIO_F_ACCESS_PLATFORM exactly once in FindAdapter")
    gate_start = len(canonical_code(find_adapter[: gates[0][1]]))
    gate_end = len(canonical_code(find_adapter[: gates[0][2]]))
    if not feature_read < gate_start < gate_end <= min(feature_acknowledge, queue_sizing, dma_allocation):
        fail(
            f"{owner} must reject VIRTIO_F_ACCESS_PLATFORM after reading host features and before "
            "feature acknowledgement, queue sizing, or DMA allocation"
        )

    acknowledge_definitions = [source for source in sources.values() if re.search(
        rf"\b{re.escape(acknowledge_features_name)}\s*\([^;{{}}]*\)\s*\{{", source, re.DOTALL
    )]
    if len(acknowledge_definitions) != 1:
        fail(f"{owner} must compile exactly one {acknowledge_features_name} definition")
    acknowledge = function_body(acknowledge_features_name, acknowledge_definitions[0])
    if re.search(r"\bVIRTIO_F_ACCESS_PLATFORM\b", acknowledge):
        fail(f"{owner} must never acknowledge VIRTIO_F_ACCESS_PLATFORM")


def require_viostor_restart_access_platform_gate(sources: dict[Path, str]) -> None:
    feature_reader_definitions = [
        source
        for source in sources.values()
        if re.search(
            r"\bVOID\s+RhelGetDiskGeometry\s*\([^;{}]*\)\s*\{",
            source,
            re.DOTALL,
        )
    ]
    if len(feature_reader_definitions) != 1:
        fail("viostor must compile exactly one RhelGetDiskGeometry definition")
    feature_reader = canonical_code(
        function_body("RhelGetDiskGeometry", feature_reader_definitions[0])
    )
    offered_feature_refresh = "adaptExt->features=virtio_get_features(&adaptExt->vdev);"
    if feature_reader.count(offered_feature_refresh) != 1:
        fail("viostor feature reader must refresh offered features exactly once")

    definitions = [
        (source, match)
        for source in sources.values()
        for match in re.finditer(
            r"\bBOOLEAN\s+VirtIoHwReinitialize\s*\([^;{}]*\)\s*\{",
            source,
            re.DOTALL,
        )
    ]
    if len(definitions) != 1:
        fail("viostor must compile exactly one VirtIoHwReinitialize definition")

    source, definition = definitions[0]
    body_start = definition.end() - 1
    depth = 0
    body_end = -1
    for offset in range(body_start, len(source)):
        if source[offset] == "{":
            depth += 1
        elif source[offset] == "}":
            depth -= 1
            if depth == 0:
                body_end = offset
                break
    if body_end < 0:
        fail("viostor VirtIoHwReinitialize definition is unterminated")
    restart = source[body_start + 1 : body_end]
    compact = canonical_code(restart)
    device_initialize = compact.find("InitVirtIODevice(DeviceExtension)")
    feature_read = compact.find("RhelGetDiskGeometry(DeviceExtension);")
    feature_acknowledge = compact.find("RhelSetGuestFeatures(DeviceExtension);")
    queue_initialization = compact.find("VirtIoHwInitialize(DeviceExtension)")
    gates = [
        (body, start, end)
        for condition, body, start, end in if_blocks(restart)
        if canonical_code(condition) == "CHECKBIT(adaptExt->features,VIRTIO_F_ACCESS_PLATFORM)"
        and canonical_code(body).endswith("returnFALSE;")
    ]
    restart_stages = (
        "InitVirtIODevice(DeviceExtension)",
        "RhelGetDiskGeometry(DeviceExtension);",
        "RhelSetGuestFeatures(DeviceExtension);",
        "VirtIoHwInitialize(DeviceExtension)",
    )
    if (
        min(device_initialize, feature_read, feature_acknowledge, queue_initialization) < 0
        or any(compact.count(stage) != 1 for stage in restart_stages)
        or len(gates) != 1
    ):
        fail("viostor restart must re-read and reject VIRTIO_F_ACCESS_PLATFORM exactly once")
    gate_start = len(canonical_code(restart[: gates[0][1]]))
    gate_end = len(canonical_code(restart[: gates[0][2]]))
    if not (
        device_initialize < feature_read < gate_start < gate_end
        <= min(feature_acknowledge, queue_initialization)
    ):
        fail(
            "viostor restart must reject refreshed VIRTIO_F_ACCESS_PLATFORM before feature "
            "acknowledgement or queue initialization"
        )


def check_storport_restricted_dma_policy() -> None:
    for owner, project, find_adapter, read_features, acknowledge_features in STORAGE_PROJECTS:
        root = ET.parse(project).getroot()
        sources = project_compile_sources(root, project)
        compiled_paths = {path.as_posix().lower() for path in sources}
        forbidden_suffixes = (
            "/rdmapool/rdmaclient.c",
            f"/{owner}/{owner}_rdma.c",
        )
        for suffix in forbidden_suffixes:
            if any(path.endswith(suffix) for path in compiled_paths):
                fail(f"{owner} physical StorPort project must not compile restricted-DMA client {suffix}")

        for ddi in STORPORT_PROHIBITED_BROKER_DDIS:
            callers = [path for path, source in sources.items() if re.search(rf"\b{ddi}\s*\(", source)]
            if callers:
                locations = ", ".join(path.as_posix() for path in callers)
                fail(f"{owner} physical StorPort sources must not call prohibited {ddi}: {locations}")

        require_storport_access_platform_gate(
            owner,
            sources,
            find_adapter,
            read_features,
            acknowledge_features,
        )
        if owner == "viostor":
            require_viostor_restart_access_platform_gate(sources)


def source_occurrences(sources: dict[Path, str], pattern: str) -> list[tuple[Path, int]]:
    return [
        (path, match.start())
        for path, source in sources.items()
        for match in re.finditer(pattern, source)
    ]


def check_driver_entry_gate() -> None:
    body = function_body("DriverEntry")
    normalized = re.sub(r"\s+", " ", body).strip()
    expected = (
        "PAGED_CODE(); "
        "UNREFERENCED_PARAMETER(driverObject); "
        "UNREFERENCED_PARAMETER(registryPath); "
        "return STATUS_NOT_SUPPORTED;"
    )
    if normalized != expected:
        fail("DriverEntry must contain only the exact compile-only fail-closed statement sequence")


def check_registration_helper(sources: dict[Path, str]) -> None:
    helper_definitions = list(
        re.finditer(
            rf'\bextern\s+"C"\s+NTSTATUS\s+{REGISTRATION_HELPER}\s*\(', DRIVER_CODE
        )
    )
    if len(helper_definitions) != 1:
        fail("compile-only registration helper must have exactly one C-linkage definition")

    body, helper_start, helper_end = function_body_span(REGISTRATION_HELPER)
    normalized = re.sub(r"\s+", " ", body).strip()
    expected = (
        "PAGED_CODE(); "
        "DRIVER_INITIALIZATION_DATA initialData; "
        "VioGpuWddmBuildInitializationData(&initialData); "
        "WPP_INIT_TRACING(driverObject, registryPath); "
        "NTSTATUS status = DxgkInitialize(driverObject, registryPath, &initialData); "
        "if (!NT_SUCCESS(status)) { WPP_CLEANUP(NULL); } "
        "return status;"
    )
    if normalized != expected:
        fail("compile-only registration helper must contain only the exact initialization and cleanup sequence")

    helper_occurrences = source_occurrences(sources, rf"\b{REGISTRATION_HELPER}\b")
    if len(helper_occurrences) != 1 or helper_occurrences[0][0] != DRIVER_SOURCE_PATH:
        locations = ", ".join(path.as_posix() for path, _ in helper_occurrences)
        fail(f"registration helper must occur only at its driver_entry.cpp definition; found: {locations or 'none'}")

    initialize_calls = source_occurrences(sources, r"\bDxgkInitialize\s*\(")
    if len(initialize_calls) != 1:
        locations = ", ".join(path.as_posix() for path, _ in initialize_calls)
        fail(f"target must contain exactly one DxgkInitialize call; found: {locations or 'none'}")

    call_path, call_offset = initialize_calls[0]
    if call_path != DRIVER_SOURCE_PATH or not helper_start <= call_offset < helper_end:
        fail("the target's only DxgkInitialize call must be inside the compile-only registration helper")

    unload_definitions = [
        (path, source)
        for path, source in sources.items()
        if re.search(r"\bVioGpuDodUnload\s*\([^;{}]*\)\s*\{", source, re.DOTALL)
    ]
    if len(unload_definitions) != 1:
        fail(f"target must contain exactly one VioGpuDodUnload definition; found {len(unload_definitions)}")
    unload_body = function_body("VioGpuDodUnload", unload_definitions[0][1])
    if len(re.findall(r"\bWPP_CLEANUP\s*\(\s*NULL\s*\)\s*;", unload_body)) != 1:
        fail("registered unload callback must clean up WPP exactly once after successful initialization")


def check_callback_table() -> None:
    body = function_body("VioGpuWddmBuildInitializationData")
    zero_initialization = re.findall(
        r"\bRtlZeroMemory\s*\(\s*initialData\s*,\s*sizeof\s*\(\s*\*\s*initialData\s*\)\s*\)\s*;",
        body,
    )
    if len(zero_initialization) != 1:
        fail("callback table must zero DRIVER_INITIALIZATION_DATA exactly once")

    version_assignment = re.findall(
        r"\binitialData\s*->\s*Version\s*=\s*DXGKDDI_INTERFACE_VERSION\s*;", body
    )
    if len(version_assignment) != 1:
        fail("callback table must assign DXGKDDI_INTERFACE_VERSION exactly once")

    callbacks = {
        "DxgkDdiAddDevice": "VioGpuDodAddDevice",
        "DxgkDdiStartDevice": "VioGpuDodStartDevice",
        "DxgkDdiStopDevice": "VioGpuDodStopDevice",
        "DxgkDdiResetDevice": "VioGpuDodResetDevice",
        "DxgkDdiRemoveDevice": "VioGpuDodRemoveDevice",
        "DxgkDdiDispatchIoRequest": "VioGpuDodDispatchIoRequest",
        "DxgkDdiInterruptRoutine": "VioGpuDodInterruptRoutine",
        "DxgkDdiDpcRoutine": "VioGpuDodDpcRoutine",
        "DxgkDdiQueryChildRelations": "VioGpuDodQueryChildRelations",
        "DxgkDdiQueryChildStatus": "VioGpuDodQueryChildStatus",
        "DxgkDdiQueryDeviceDescriptor": "VioGpuDodQueryDeviceDescriptor",
        "DxgkDdiSetPowerState": "VioGpuDodSetPowerState",
        "DxgkDdiUnload": "VioGpuDodUnload",
        "DxgkDdiQueryInterface": "VioGpuDodQueryInterface",
        "DxgkDdiQueryAdapterInfo": "VioGpuWddmQueryAdapterInfo",
        "DxgkDdiCreateDevice": "VioGpuWddmCreateDevice",
        "DxgkDdiDestroyDevice": "VioGpuWddmDestroyDevice",
        "DxgkDdiCreateAllocation": "VioGpuWddmCreateAllocation",
        "DxgkDdiDestroyAllocation": "VioGpuWddmDestroyAllocation",
        "DxgkDdiDescribeAllocation": "VioGpuWddmDescribeAllocation",
        "DxgkDdiGetStandardAllocationDriverData": "VioGpuWddmGetStandardAllocationDriverData",
        "DxgkDdiOpenAllocation": "VioGpuWddmOpenAllocation",
        "DxgkDdiCloseAllocation": "VioGpuWddmCloseAllocation",
        "DxgkDdiCreateContext": "VioGpuWddmCreateContext",
        "DxgkDdiDestroyContext": "VioGpuWddmDestroyContext",
        "DxgkDdiBuildPagingBuffer": "VioGpuWddmBuildPagingBuffer",
        "DxgkDdiRender": "VioGpuWddmRender",
        "DxgkDdiPresent": "VioGpuWddmPresent",
        "DxgkDdiPatch": "VioGpuWddmPatch",
        "DxgkDdiSubmitCommand": "VioGpuWddmSubmitCommand",
        "DxgkDdiPreemptCommand": "VioGpuWddmPreemptCommand",
        "DxgkDdiQueryCurrentFence": "VioGpuWddmQueryCurrentFence",
        "DxgkDdiResetFromTimeout": "VioGpuWddmResetFromTimeout",
        "DxgkDdiRestartFromTimeout": "VioGpuWddmRestartFromTimeout",
        "DxgkDdiSetPointerPosition": "VioGpuDodSetPointerPosition",
        "DxgkDdiSetPointerShape": "VioGpuDodSetPointerShape",
        "DxgkDdiEscape": "VioGpuDodEscape",
        "DxgkDdiIsSupportedVidPn": "VioGpuDodIsSupportedVidPn",
        "DxgkDdiRecommendFunctionalVidPn": "VioGpuDodRecommendFunctionalVidPn",
        "DxgkDdiEnumVidPnCofuncModality": "VioGpuDodEnumVidPnCofuncModality",
        "DxgkDdiSetVidPnSourceAddress": "VioGpuWddmSetVidPnSourceAddress",
        "DxgkDdiSetVidPnSourceVisibility": "VioGpuDodSetVidPnSourceVisibility",
        "DxgkDdiCommitVidPn": "VioGpuDodCommitVidPn",
        "DxgkDdiUpdateActiveVidPnPresentPath": "VioGpuDodUpdateActiveVidPnPresentPath",
        "DxgkDdiRecommendMonitorModes": "VioGpuDodRecommendMonitorModes",
        "DxgkDdiQueryVidPnHWCapability": "VioGpuDodQueryVidPnHWCapability",
        "DxgkDdiStopDeviceAndReleasePostDisplayOwnership": "VioGpuDodStopDeviceAndReleasePostDisplayOwnership",
        "DxgkDdiSystemDisplayEnable": "VioGpuDodSystemDisplayEnable",
        "DxgkDdiSystemDisplayWrite": "VioGpuDodSystemDisplayWrite",
    }
    for member, callback in callbacks.items():
        assignments = re.findall(
            rf"\binitialData\s*->\s*{re.escape(member)}\s*=\s*{re.escape(callback)}\s*;", body
        )
        if len(assignments) != 1:
            fail(f"callback table must assign {member} to {callback} exactly once")

    assignment_members = re.findall(r"\binitialData\s*->\s*(\w+)\s*=", body)
    expected_members = ["Version", *callbacks]
    if sorted(assignment_members) != sorted(expected_members):
        fail("callback table contains an unexpected, missing, or duplicate initialData assignment")

    expected_statements = [
        "RtlZeroMemory(initialData, sizeof(*initialData));",
        "initialData->Version = DXGKDDI_INTERFACE_VERSION;",
        *(f"initialData->{member} = {callback};" for member, callback in callbacks.items()),
    ]
    if re.sub(r"\s+", " ", body).strip() != " ".join(expected_statements):
        fail("callback table must contain only the exact expected initialization statement sequence")

    if re.search(r"\bDxgkDdiPresentDisplayOnly\b", body):
        fail("full miniport must not register the KMDOD-only PresentDisplayOnly callback")


def check_native_context_readiness(
    viogpu_code: Optional[str] = None,
    viogpu_header_code: Optional[str] = None,
    wire_header_code: Optional[str] = None,
) -> None:
    if viogpu_code is None:
        viogpu_code = VIOGPU_CODE
    if viogpu_header_code is None:
        viogpu_header_code = VIOGPU_HEADER_CODE
    if wire_header_code is None:
        wire_header_code = WIRE_HEADER_CODE

    required_features = (
        "VIRTIO_GPU_F_VIRGL",
        "VIRTIO_GPU_F_RESOURCE_BLOB",
        "VIRTIO_GPU_F_CONTEXT_INIT",
        # The product Native Context data path needs the guest-handle contract
        # exposed only by crosvm when udmabuf=true.  DriverEntry remains
        # unreachable in this compile-only target, but the source contract must
        # still fail closed if that feature is absent.
        "VIRTIO_GPU_F_CREATE_GUEST_HANDLE",
    )

    require_integer_define(wire_header_code, "VIRTGPU_DRM_CAPSET_DRM", 6, "wire header")
    require_integer_define(wire_header_code, "VIRTGPU_DRM_CONTEXT_MSM", 1, "wire header")
    require_integer_define(wire_header_code, "VIRTGPU_DRM_WIRE_FORMAT_VERSION", 2, "wire header")
    require_alias_define(wire_header_code, "VIRTIO_GPU_CAPSET_DRM", "VIRTGPU_DRM_CAPSET_DRM", "wire header")
    require_alias_define(
        wire_header_code,
        "VIRTIO_GPU_DRM_CONTEXT_MSM",
        "VIRTGPU_DRM_CONTEXT_MSM",
        "wire header",
    )
    require_alias_define(
        wire_header_code,
        "VIRTIO_GPU_DRM_WIRE_FORMAT_VERSION",
        "VIRTGPU_DRM_WIRE_FORMAT_VERSION",
        "wire header",
    )
    if len(
        re.findall(
            r"\bVIOGPU_WIRE_ASSERT_SIZE\s*\(\s*virgl_renderer_capset_drm\s*,\s*112\s*\)\s*;",
            wire_header_code,
        )
    ) != 1:
        fail("wire header must assert the GPU_CAPSET_DRM prefix size as exactly 112 bytes")

    require_integer_define(viogpu_code, "VIOGPU_MAX_CAPSETS", 64, "viogpudo.cpp")
    require_integer_define(viogpu_code, "VIOGPU_MINIMUM_MSM_VERSION_MINOR", 9, "viogpudo.cpp")

    negotiation = function_body("VioGpuAdapter::NegotiateNativeContextFeatures", viogpu_code)
    probe = function_body("VioGpuAdapter::ProbeNativeContextReadiness", viogpu_code)

    negotiation_compact = compact_code(negotiation)
    expected_negotiation = "if(" + "||".join(f"!AckFeature({feature})" for feature in required_features) + ")"
    if expected_negotiation not in negotiation_compact:
        fail("native-context feature negotiation must fail closed on the exact required feature set")
    for feature in required_features:
        count = len(re.findall(rf"\b{re.escape(feature)}\b", negotiation))
        if count != 1:
            fail(f"feature negotiation must require {feature} exactly once, found {count}")
        if len(re.findall(rf"\bAckFeature\s*\(\s*{re.escape(feature)}\s*\)", negotiation)) != 1:
            fail(f"feature negotiation must acknowledge {feature} through AckFeature exactly once")

    probe_compact = compact_code(probe)
    for feature in required_features:
        expected_probe = f"!virtio_is_feature_enabled(m_u64GuestFeatures,{feature})"
        if expected_probe not in probe_compact:
            fail(f"readiness probe must revalidate negotiated feature {feature}")
    require_call_count(probe, "ClearNativeContextReadiness", 1, "readiness probe")
    if probe.find("ClearNativeContextReadiness") > probe.find("virtio_is_feature_enabled"):
        fail("readiness probe must clear stale readiness before validation")

    probe_requirements = {
        "a nonzero capset count": "m_u32NumCapsets==0",
        "the capset count limit": "m_u32NumCapsets>VIOGPU_MAX_CAPSETS",
        "bounded enumeration of every advertised capset":
            "for(UINTcapsetIndex=0;capsetIndex<m_u32NumCapsets;++capsetIndex)",
        "capset information queries by enumerated index": "QueryCapsetInfo(capsetIndex,&info)",
        "capset ID 6 selection": "info.capset_id!=VIRTIO_GPU_CAPSET_DRM",
        "duplicate capset ID 6 rejection": "if(found)",
        "a selected capset": "!found",
        "the 112-byte capset prefix lower bound": "selectedInfo.capset_max_size<sizeof(GPU_CAPSET_DRM)",
        "the capset response allocation upper bound":
            "selectedInfo.capset_max_size>PAGE_SIZE-sizeof(GPU_CTRL_HDR)",
        "a capset ID 6 query": "QueryCapset(VIRTIO_GPU_CAPSET_DRM,selectedInfo.capset_max_version,"
            "selectedInfo.capset_max_size,&capset)",
        "wire format version 2 validation":
            "capset.wire_format_version!=VIRTIO_GPU_DRM_WIRE_FORMAT_VERSION",
        "MSM context type 1 validation": "capset.context_type!=VIRTIO_GPU_DRM_CONTEXT_MSM",
        "MSM major version 1 validation": "capset.version_major!=1",
        "the MSM minor version floor": "capset.version_minor<VIOGPU_MINIMUM_MSM_VERSION_MINOR",
        "nonzero priorities validation": "capset.msm.priorities==0",
        "nonzero VA size validation": "capset.msm.va_size==0",
        "VA start page alignment validation": "(capset.msm.va_start&(PAGE_SIZE-1))!=0",
        "VA size page alignment validation": "(capset.msm.va_size&(PAGE_SIZE-1))!=0",
        "VA range overflow validation": "vaEnd<capset.msm.va_start",
    }
    for description, fragment in probe_requirements.items():
        count = probe_compact.count(fragment)
        if count != 1:
            fail(f"readiness probe must contain {description} exactly once, found {count}")

    if re.search(
        r"\bselectedInfo\s*\.\s*capset_max_size\s*(?:==|!=)\s*sizeof\s*\(\s*GPU_CAPSET_DRM\s*\)",
        probe,
    ):
        fail("readiness probe must accept host capsets larger than the 112-byte Windows prefix")

    selection_sequence = (
        "if(info.capset_id!=VIRTIO_GPU_CAPSET_DRM){continue;}"
        "if(found){returnSTATUS_NOT_SUPPORTED;}"
        "selectedInfo=info;found=TRUE;"
    )
    if selection_sequence not in probe_compact:
        fail("readiness probe must select exactly one capset ID 6")

    ready_assignments = re.findall(r"\bm_NativeContextReadiness\s*\.\s*Ready\s*=\s*TRUE\s*;", viogpu_code)
    if len(ready_assignments) != 1:
        fail("the target must publish native-context Ready = TRUE exactly once")
    ready_offset = probe_compact.find("m_NativeContextReadiness.Ready=TRUE;")
    acquire_offset = probe_compact.rfind("KeAcquireSpinLock(&m_NativeContextReadinessLock,&oldIrql);", 0, ready_offset)
    release_offset = probe_compact.find("KeReleaseSpinLock(&m_NativeContextReadinessLock,oldIrql);", ready_offset)
    success_offset = probe_compact.find("returnSTATUS_SUCCESS;", ready_offset)
    if min(ready_offset, acquire_offset, release_offset, success_offset) < 0 or not (
        acquire_offset < ready_offset < release_offset < success_offset
    ):
        fail("readiness must be published under its spin lock only after all validation")
    if any(probe_compact.find(fragment) >= acquire_offset for fragment in probe_requirements.values()):
        fail("all readiness validation must complete before readiness is published")
    if re.search(r"\bReady\s*=\s*TRUE\s*;", probe[:acquire_offset]):
        fail("readiness probe must not publish readiness before final validation")
    publish_sequence = (
        "m_NativeContextReadiness.Generation=generation;"
        "m_NativeContextReadiness.CapsetVersion=selectedInfo.capset_max_version;"
        "m_NativeContextReadiness.CapsetSize=selectedInfo.capset_max_size;"
        "m_NativeContextReadiness.Capset=capset;"
        "m_NativeContextReadiness.Ready=TRUE;"
    )
    if publish_sequence not in probe_compact:
        fail("readiness probe must atomically publish the fully validated capset")
    publish_offset = probe_compact.find(publish_sequence)
    if not (
        acquire_offset < publish_offset
        and publish_offset < release_offset
        and probe_compact.find("if(!m_CtrlQueue.IsSynchronousRequestsHealthy()", acquire_offset, publish_offset)
        >= 0
    ):
        fail("readiness publication must remain under the lock after the final generation/queue guard")

    readiness_members = (
        "BOOLEAN Ready;",
        "UINT CapsetVersion;",
        "UINT CapsetSize;",
        "GPU_CAPSET_DRM Capset;",
    )
    for member in readiness_members:
        if compact_code(member) not in compact_code(viogpu_header_code):
            fail(f"readiness state is missing {member}")

    hw_init = function_body("VioGpuAdapter::HWInit", viogpu_code)
    adapter_init = function_body("VioGpuAdapter::VioGpuAdapterInit", viogpu_code)
    d0_power = function_body("VioGpuAdapter::SetPowerState", viogpu_code)
    reset = function_body("VioGpuAdapter::ResetDevice", viogpu_code)
    failure = function_body("VioGpuAdapter::FailNativeContextInitialization", viogpu_code)
    hw_close = function_body("VioGpuAdapter::HWClose", viogpu_code)
    stop = function_body("VioGpuAdapter::StopNativeContextTransportLocked", viogpu_code)
    destructor = function_body("VioGpuAdapter::~VioGpuAdapter", viogpu_code)
    buffer_close = function_body("VioGpuBuf::Close", QUEUE_CODE)
    segment_close = function_body("VioGpuMemSegment::Close", QUEUE_CODE)
    rdma_connect = function_body("VioGpuAdapter::ConnectRestrictedDma", viogpu_code)

    require_single_final_return(stop, "return STATUS_SUCCESS;", "native-context transport teardown")
    if re.search(r"\bgoto\b", stop):
        fail("native-context transport teardown must not jump around ownership transitions")
    if any(canonical_code(condition) in ("FALSE", "0", "!TRUE") for condition, _, _, _ in if_blocks(stop)):
        fail("native-context transport teardown must not hide required work in constant-false control flow")

    probe_occurrences = len(re.findall(r"\bProbeNativeContextReadiness\s*\(", viogpu_code))
    if probe_occurrences != 2:
        fail("ProbeNativeContextReadiness must have one definition and only the transport call site")
    transport_start = function_body("VioGpuAdapter::StartNativeContextTransport", viogpu_code)
    require_call_count(transport_start, "ProbeNativeContextReadiness", 1, "transport start")
    require_call_count(transport_start, "ConnectRestrictedDma", 1, "transport start")
    transport_start_compact = compact_code(transport_start)
    connect_sequence = (
        "NTSTATUSstatus=ConnectRestrictedDma();"
        "if(!NT_SUCCESS(status)){returnstatus;}"
        "status=VioGpuAdapterInit(pDispInfo);"
    )
    if connect_sequence not in transport_start_compact:
        fail("transport start must directly connect restricted DMA and propagate failure before VirtIO initialization")
    if transport_start_compact.find("ConnectRestrictedDma()") > transport_start_compact.find(
        "VioGpuAdapterInit(pDispInfo)"
    ):
        fail("transport start must connect restricted DMA before initializing VirtIO")
    if len(re.findall(r"\bm_RdmaPool\s*\.\s*Connect\s*\(", viogpu_code)) != 1 or len(
        re.findall(r"\bm_RdmaPool\s*\.\s*Connect\s*\(", rdma_connect)
    ) != 1:
        fail("restricted-DMA connection must be owned only by the restartable transport-start helper")
    hw_init_compact = compact_code(hw_init)
    probe_offset = compact_code(transport_start).find("status=ProbeNativeContextReadiness();")
    buffer_offset = compact_code(transport_start).find("m_GpuBuf.Init(allocation,this)")
    idr_offset = compact_code(transport_start).find("m_Idr.Init(1)")
    if min(probe_offset, buffer_offset, idr_offset) < 0 or not (buffer_offset < idr_offset < probe_offset):
        fail("HWInit must probe only after control buffers and the ID allocator are initialized")
    require_call_count(hw_init, "StartWorkThread", 1, "HWInit")
    if probe_offset > transport_start_compact.find("returnSTATUS_SUCCESS;"):
        fail("transport start must complete readiness probing before returning success")
    if hw_init_compact.count("returnFailNativeContextInitialization(status);") != 1:
        fail("HWInit must use one complete native-context failure unwind")
    adapter_init_compact = compact_code(adapter_init)
    if adapter_init_compact.count("m_bVirtioInitialized=TRUE;") != 1:
        fail("adapter initialization must retain the transport ownership flag exactly once")
    if adapter_init_compact.count("m_bQueuesInitialized=TRUE;") != 1:
        fail("adapter initialization must retain the queue ownership flag exactly once")

    d0_compact = compact_code(d0_power)
    for fragment, description in (
        ("BeginNativeContextInitialization()", "D0 initialization begin"),
        ("StartNativeContextTransport(&pCurrentMode->DispInfo)", "D0 transport start"),
        ("StartWorkThread()", "D0 worker start"),
        ("CompleteNativeContextInitialization()", "D0 readiness completion"),
    ):
        if d0_compact.count(compact_code(fragment)) != 1:
            fail(f"D0 must perform {description} exactly once")
    if d0_compact.find("BeginNativeContextInitialization()") > d0_compact.find("StartNativeContextTransport(&pCurrentMode->DispInfo)"):
        fail("D0 must begin lifecycle before starting transport")
    if d0_compact.find("StartNativeContextTransport(&pCurrentMode->DispInfo)") > d0_compact.find("StartWorkThread()"):
        fail("D0 must start transport before its worker")
    if d0_compact.find("StartWorkThread()") > d0_compact.find("CompleteNativeContextInitialization()"):
        fail("D0 must publish readiness only after its worker starts")
    if d0_compact.count("returnFailNativeContextInitialization(") != 2:
        fail("D0 must use the native-context failure unwind for transport and readiness failures")
    for power_state in ("PowerDeviceD1", "PowerDeviceD2", "PowerDeviceD3"):
        if len(re.findall(rf"\bcase\s+{power_state}\s*:", d0_power)) != 1:
            fail(f"power teardown must handle {power_state} exactly once")
    teardown_start = d0_compact.find("casePowerDeviceD1:casePowerDeviceD2:casePowerDeviceD3:")
    teardown_end = d0_compact.find("break;", teardown_start)
    teardown = d0_compact[teardown_start:teardown_end]
    if teardown_start < 0 or teardown.count("StopNativeContextTransport()") != 1:
        fail("D1/D2/D3 teardown must stop the native-context transport exactly once")
    d123_failure = (
        "NTSTATUSstatus=StopNativeContextTransport();"
        "if(!NT_SUCCESS(status)){returnstatus;}"
        "pCurrentMode->Flags.FrameBufferIsActive=FALSE;"
    )
    if d123_failure not in teardown:
        fail("D1/D2/D3 teardown must propagate stop failure before clearing display ownership")

    if "StopNativeContextTransportLocked()" not in compact_code(failure):
        fail("initialization failure must use the locked native-context teardown")
    if "KeReleaseMutex(&m_NativeContextLifecycleMutex,FALSE);" not in compact_code(failure):
        fail("initialization failure must release the lifecycle mutex")
    if "StopNativeContextTransport()" not in compact_code(hw_close):
        fail("HWClose must use the complete native-context transport unwind")
    stop_compact = compact_code(stop)
    for fragment, description in (
        ("InvalidateNativeContextRegistrationsLocked()", "runtime context invalidation"),
        ("m_CtrlQueue.QuiesceSynchronousRequests()", "synchronous queue quiesce"),
        ("StopWorkThread()", "worker stop"),
        ("InterlockedExchange(&m_InterruptDispatchEnabled,FALSE)", "ISR publication gate"),
        ("m_CtrlQueue.DisableInterrupt()", "control-queue interrupt disable"),
        ("m_CursorQueue.DisableInterrupt()", "cursor-queue interrupt disable"),
        ("virtio_device_reset_checked(&m_VioDev)", "VirtIO reset"),
        ("virtio_get_status(&m_VioDev)", "VirtIO reset-status proof"),
        ("RetireAllNativeContextOwnersLocked()", "Host context retirement"),
        ("KeFlushQueuedDpcs()", "DPC drain"),
        ("virtio_delete_queues(&m_VioDev)", "VirtIO queue deletion"),
        ("m_CtrlQueue.CompleteSynchronousRequestTeardown()", "synchronous queue teardown"),
        ("m_FrameSegment.Close()", "frame segment teardown"),
        ("m_CursorSegment.Close()", "cursor segment teardown"),
        ("m_GpuBuf.Close()", "control-buffer allocator teardown"),
        ("status=m_RdmaPool.Disconnect()", "restricted-DMA arena disconnect"),
        ("InterlockedExchange(&m_NativeContextState,VioGpuNativeContextOffline)", "offline publication"),
    ):
        if fragment not in stop_compact:
            fail(f"transport teardown must include {description}")

    quiescing_publications = (
        stop_compact.find(
            "InterlockedCompareExchange(&m_NativeContextState,VioGpuNativeContextQuiescing,VioGpuNativeContextFailed)"
        ),
        stop_compact.find(
            "InterlockedCompareExchange(&m_NativeContextState,VioGpuNativeContextQuiescing,state)"
        ),
    )
    generation_advances = [
        match.start()
        for match in re.finditer(
            re.escape("InterlockedIncrement(&m_NativeContextGeneration)"),
            stop_compact,
        )
    ]
    invalidate_offset = stop_compact.find("InvalidateNativeContextRegistrationsLocked()")
    if (
        min(quiescing_publications) < 0
        or len(generation_advances) != 2
        or max(*quiescing_publications, *generation_advances) > invalidate_offset
    ):
        fail("every teardown path must publish Quiescing and advance generation before invalidating registrations")
    quiesce_offset = stop_compact.find("m_CtrlQueue.QuiesceSynchronousRequests()")
    worker_offset = stop_compact.find("StopWorkThread()")
    gate_offset = stop_compact.find("InterlockedExchange(&m_InterruptDispatchEnabled,FALSE)", worker_offset)
    reset_offset = stop_compact.find("virtio_device_reset_checked(&m_VioDev)")
    reset_status_offset = stop_compact.find("virtio_get_status(&m_VioDev)", reset_offset)
    retire_offset = stop_compact.find("RetireAllNativeContextOwnersLocked()", reset_status_offset)
    delete_queue_offset = stop_compact.find("virtio_delete_queues(&m_VioDev)", retire_offset)
    ordered_offsets = (
        (max(quiescing_publications), "quiescing publication"),
        (invalidate_offset, "runtime context invalidation"),
        (quiesce_offset, "synchronous queue quiesce"),
        (worker_offset, "worker stop"),
        (gate_offset, "ISR publication gate"),
        (reset_offset, "VirtIO reset"),
        (reset_status_offset, "VirtIO reset-status proof"),
        (retire_offset, "Host context retirement"),
        (delete_queue_offset, "VirtIO queue deletion"),
    )
    for (offset, description), (next_offset, next_description) in zip(ordered_offsets, ordered_offsets[1:]):
        if offset < 0 or next_offset < 0 or offset > next_offset:
            fail(f"transport teardown must perform {description} before {next_description}")

    no_reset_blocks = [
        canonical_code(body)
        for condition, body, _, _ in if_blocks(stop)
        if set(canonical_code(condition).split("&&"))
        == {"!IsListEmpty(&m_NativeContextRegistry)", "!m_bVirtioInitialized"}
    ]
    no_reset_failure = "FailNativeContextAtAnyIrql();returnSTATUS_DEVICE_NOT_READY;"
    if len(no_reset_blocks) != 1 or no_reset_failure not in no_reset_blocks[0]:
        fail("transport teardown must retain Host ownership when no VirtIO reset can prove retirement")

    reset_guard_blocks = [
        (canonical_code(condition), canonical_code(body))
        for condition, body, _, _ in if_blocks(stop)
        if "virtio_get_status(&m_VioDev)" in canonical_code(condition)
    ]
    expected_reset_guard = "!NT_SUCCESS(status)||virtio_get_status(&m_VioDev)!=0"
    reset_failure_body = "FailNativeContextAtAnyIrql();returnNT_SUCCESS(status)?STATUS_DEVICE_NOT_READY:status;"
    if (
        len(reset_guard_blocks) != 1
        or reset_guard_blocks[0][0] != expected_reset_guard
        or reset_guard_blocks[0][1] != reset_failure_body
    ):
        fail("transport teardown must retire Host ownership only after successful reset and exact zero device status")

    gate_offset = stop_compact.find("InterlockedExchange(&m_InterruptDispatchEnabled,FALSE)")
    first_barrier_offset = stop_compact.find("status=SynchronizeInterruptMessages();", gate_offset)
    flush_offset = stop_compact.find("KeFlushQueuedDpcs()", first_barrier_offset)
    disable_control_offset = stop_compact.find("m_CtrlQueue.DisableInterrupt()", flush_offset)
    disable_cursor_offset = stop_compact.find("m_CursorQueue.DisableInterrupt()", disable_control_offset)
    if not (
        0 <= gate_offset < first_barrier_offset < flush_offset < disable_control_offset < disable_cursor_offset
    ):
        fail("queue teardown must gate, barrier, drain DPCs, and disable both queue interrupts in order")

    teardown_order = (
        ("RetireAllNativeContextOwnersLocked()", "Host context retirement"),
        ("virtio_delete_queues(&m_VioDev)", "VirtIO queue deletion"),
        ("virtio_device_shutdown(&m_VioDev)", "VirtIO shutdown"),
        ("m_CtrlQueue.CompleteSynchronousRequestTeardown()", "synchronous queue teardown"),
        ("m_FrameSegment.Close()", "frame segment teardown"),
        ("m_CursorSegment.Close()", "cursor segment teardown"),
        ("m_GpuBuf.Close()", "control-buffer allocator teardown"),
        ("status=m_RdmaPool.Disconnect()", "restricted-DMA arena disconnect"),
        ("InterlockedExchange(&m_NativeContextState,VioGpuNativeContextOffline)", "offline publication"),
    )
    teardown_offsets = [(stop_compact.find(fragment), description) for fragment, description in teardown_order]
    for (offset, description), (next_offset, next_description) in zip(teardown_offsets, teardown_offsets[1:]):
        if offset > next_offset:
            fail(f"transport teardown must perform {description} before {next_description}")
    rdma_disconnect = stop_compact.find("status=m_RdmaPool.Disconnect()")
    rdma_failure = stop_compact.find("if(!NT_SUCCESS(status)){FailNativeContextAtAnyIrql();returnstatus;}", rdma_disconnect)
    offline_publish = stop_compact.find("InterlockedExchange(&m_NativeContextState,VioGpuNativeContextOffline)")
    if min(rdma_disconnect, rdma_failure, offline_publish) < 0 or not (
        rdma_disconnect < rdma_failure < offline_publish
    ):
        fail("transport teardown must retain the adapter when restricted-DMA disconnect fails")
    failed_path = stop_compact.find("if(state==VioGpuNativeContextFailed)")
    failed_transition = stop_compact.find(
        "InterlockedCompareExchange(&m_NativeContextState,VioGpuNativeContextQuiescing,VioGpuNativeContextFailed)"
    )
    if failed_path < 0 or failed_transition < failed_path:
        fail("failed transport state must transition to quiescing instead of returning early")
    if stop_compact[:failed_path].count(no_reset_failure) != 1:
        fail("only the no-reset Host-ownership guard may fail before processing retained Failed state")
    failed_block_end = stop_compact.find("if(state==VioGpuNativeContextOffline)", failed_path)
    if failed_block_end < 0 or "return" in stop_compact[failed_path:failed_transition]:
        fail("failed transport state must not return before transitioning to quiescing")

    helper = canonical_code(function_body("VioGpuAdapter::SynchronizeInterruptMessages", VIOGPU_CODE))
    message_count = "ULONGmessageCount=m_PciResources.IsMSIEnabled()&&m_bQueuesInitialized?3:1;"
    barrier_loop = "for(ULONGmessageNumber=0;messageNumber<messageCount;++messageNumber)"
    barrier_failure = "if(!NT_SUCCESS(barrierStatus)||!barrierResult)"
    barrier_return = "returnNT_SUCCESS(barrierStatus)?STATUS_DEVICE_NOT_READY:barrierStatus;"
    message_offset = helper.find(message_count)
    loop_offset = helper.find(barrier_loop, message_offset)
    failure_offset = helper.find(barrier_failure, loop_offset)
    return_offset = helper.find(barrier_return, failure_offset)
    if min(message_offset, loop_offset, failure_offset, return_offset) < 0 or not (
        message_offset < loop_offset < failure_offset < return_offset
    ):
        fail("ISR barrier helper must synchronize every configured message and fail closed")
    if (
        helper.count("barrierStatus=") != 1
        or helper.count("barrierResult=") != 1
        or "&barrierResult);if(!NT_SUCCESS(barrierStatus)||!barrierResult)" not in helper
    ):
        fail("ISR barrier status and result must be validated immediately after synchronization")
    stop_canonical = canonical_code(stop)
    helper_calls = [match.start() for match in re.finditer(r"\bSynchronizeInterruptMessages\s*\(\s*\)", stop_canonical)]
    final_barrier = helper_calls[1] if len(helper_calls) == 2 else -1
    reset_status_canonical_offset = stop_canonical.find("virtio_get_status(&m_VioDev)", reset_offset)
    reset_guard_end = stop_canonical.find("RetireAllNativeContextOwnersLocked()", reset_status_canonical_offset)
    if (
        len(helper_calls) != 2
        or final_barrier < reset_status_canonical_offset
        or final_barrier > reset_guard_end
    ):
        fail("transport teardown must run a final all-message ISR barrier after reset proof and before owner retirement")
    if compact_code(reset).count("FailNativeContextAtAnyIrql()") != 1:
        fail("ResetDevice must fail closed through the nonpaged native-context failure path exactly once")
    destructor_compact = compact_code(destructor)
    if "StopNativeContextTransport()" not in destructor_compact:
        fail("adapter destruction must use the native-context transport teardown")

    buffer_close_compact = compact_code(buffer_close)
    lock_offsets = [match.start() for match in re.finditer(r"\bKeAcquireSpinLock\s*\(", buffer_close)]
    unlock_offsets = [match.start() for match in re.finditer(r"\bKeReleaseSpinLock\s*\(", buffer_close)]
    free_offsets = [match.start() for match in re.finditer(r"\bFreeMemory\s*\(", buffer_close)]
    owner_clear_offset = buffer_close_compact.find("m_pPci=NULL;")
    if len(lock_offsets) != 2 or len(unlock_offsets) != 2 or len(free_offsets) != 3:
        fail("control-buffer teardown must use two lock scopes and free response, data, and descriptor storage")
    if not (
        lock_offsets[0] < unlock_offsets[0] < free_offsets[0] <= free_offsets[-1] < lock_offsets[1] < unlock_offsets[1]
    ):
        fail("control-buffer teardown must return all allocations outside the spin lock before its final owner update")
    last_free_compact = buffer_close_compact.rfind("FreeMemory(")
    if owner_clear_offset < last_free_compact or owner_clear_offset > buffer_close_compact.rfind("KeReleaseSpinLock("):
        fail("control-buffer teardown must clear its allocation owner only after every allocation is returned")
    for argument in ("buffer->resp_buf", "buffer->data_buf", "buffer"):
        if len(re.findall(rf"\bFreeMemory\s*\(\s*{re.escape(argument)}\s*\)\s*;", buffer_close)) != 1:
            fail(f"control-buffer teardown must free exactly {argument}")
    first_unlock = buffer_close.find("KeReleaseSpinLock")
    first_free = min(free_offsets)
    owner_writes = [match.start() for match in re.finditer(r"\bm_pPci\b\s*=", buffer_close)]
    if any(first_unlock < offset < first_free for offset in owner_writes):
        fail("control-buffer teardown must not clear its allocator owner through an alias before freeing allocations")
    if "&m_pPci" in buffer_close_compact[compact_code(buffer_close).find("KeReleaseSpinLock("):first_free]:
        fail("control-buffer teardown must not expose its allocator-owner slot before freeing allocations")

    segment_close_compact = compact_code(segment_close)
    for fragment, description in (
        ("if(m_pVAddr!=NULL&&m_bSystemMemory)", "owned-memory guard"),
        ("if(m_pPci!=NULL){m_pPci->FreeDmaMemory(m_pVAddr);}", "restricted-DMA owner guard"),
        ("elseif(m_pVAddr!=NULL&&m_bMapped)", "mapped-framebuffer guard"),
        ("m_pVAddr=NULL;", "address clear"),
        ("m_bSystemMemory=FALSE;", "system-memory clear"),
        ("m_bMapped=FALSE;", "mapping clear"),
        ("m_bRestrictedDma=FALSE;", "restricted-DMA clear"),
        ("m_Size=0;", "size clear"),
        ("m_pPci=NULL;", "allocator-owner clear"),
    ):
        if segment_close_compact.count(fragment) != 1:
            fail(f"memory-segment teardown must contain exactly one {description}")
    if segment_close_compact.find("m_pPci->FreeDmaMemory(m_pVAddr)") > segment_close_compact.find("m_pPci=NULL;"):
        fail("memory-segment teardown must return restricted DMA before clearing its allocator owner")
    for member in (
        "m_pVAddr",
        "m_pSGList",
        "m_bSystemMemory",
        "m_bMapped",
        "m_bRestrictedDma",
        "m_Size",
        "m_pPci",
    ):
        if len(variable_write_offsets(segment_close, member)) != 1:
            fail(f"memory-segment teardown must publish exactly one final write to {member}")


def check_no_retired_variant_contract(sources: dict[Path, str]) -> None:
    """Keep compiled sources independent of the retired split variant."""
    retired_tokens = ("VIOGPU_FULL_WDDM",)
    compiled_source = "\n".join(sources.values())
    for token in retired_tokens:
        if token in compiled_source:
            fail(f"compiled source must not retain retired variant machinery: {token}")


def check_queue_failure_semantics() -> None:
    queue_header = strip_cpp_comments_and_literals(QUEUE_HEADER_SOURCE)
    add_buf = function_body("AddBuf", queue_header)
    if len(re.findall(r"\bvirtqueue_add_buf\s*\(", add_buf)) != 1:
        fail("AddBuf must submit exactly once when the virtqueue exists")
    add_buf_compact = compact_code(add_buf)
    conditional_return = re.search(
        r"\breturn\s+m_pVirtQueue\s*\?\s*virtqueue_add_buf\s*\([^;]+\)\s*:\s*-1\s*;",
        add_buf,
        re.DOTALL,
    )
    guarded_return = re.search(
        r"\bif\s*\(\s*m_pVirtQueue\s*==\s*(?:NULL|nullptr)\s*\)\s*\{\s*return\s+-1\s*;\s*\}"
        r"\s*return\s+virtqueue_add_buf\s*\(",
        add_buf,
        re.DOTALL,
    )
    if conditional_return is None and guarded_return is None:
        fail("AddBuf must return a negative error when no virtqueue is initialized")

    for name in ("CtrlQueue::QueueBuffer", "CrsrQueue::QueueCursor"):
        body = compact_code(function_body(name, QUEUE_CODE))
        enqueue = "ret=AddBuf("
        guarded_kick = "if(ret>=0){Kick();}"
        if body.count(enqueue) != 1 or body.count(guarded_kick) != 1:
            fail(f"{name} must kick exactly once and only after AddBuf succeeds")

    close_body = function_body("VioGpuQueue::Close", QUEUE_CODE)
    close = canonical_code(close_body)
    close_match = re.search(
        r"Lock\(&(?P<irql>\w+)\);m_pVirtQueue=NULL;Unlock\((?P=irql)\);",
        close,
    )
    if (
        close.count("m_pVirtQueue=NULL;") != 1
        or len(variable_write_offsets(close_body, "m_pVirtQueue")) != 1
        or close_match is None
    ):
        fail("queue close must clear the virtqueue while holding the queue lock")

    submit_body = function_body_with_parameters(
        "CtrlQueue::SubmitSynchronousLocked",
        "PGPU_VBUFFER buf, _Out_ PBOOLEAN release_buffer, _Out_ PBOOLEAN submitted",
        QUEUE_CODE,
    )
    submit = compact_code(submit_body)
    require_single_final_return(submit_body, "return TRUE;", "synchronous submit")
    initial_epoch = "requestEpochState=VioGpuReadSynchronousEpochState(&m_SynchronousEpochState)"
    finite_wait = "KeWaitForSingleObject(&buf->completion_event,Executive,KernelMode,FALSE,&timeout)"
    if submit.count(initial_epoch) != 1 or submit.count(finite_wait) != 1:
        fail("synchronous submit must capture one epoch and use one finite descriptor wait")
    timeout_blocks = [
        body
        for condition, body, _, _ in if_blocks(submit_body)
        if canonical_code(condition) in ("status!=STATUS_SUCCESS", "STATUS_SUCCESS!=status")
    ]
    if len(timeout_blocks) != 1 or not canonical_code(timeout_blocks[0]).startswith(
        "PoisonSynchronousRequests();*release_buffer=FALSE;"
    ):
        fail("synchronous submit timeout must poison the epoch and retain the device-owned descriptor")
    epoch_terms = {
        "completedEpochState!=requestEpochState",
        "buf->synchronous_epoch_state!=requestEpochState",
        "VioGpuSynchronousState(completedEpochState)!=VioGpuSynchronousEnabled",
    }
    epoch_blocks = [
        (condition, body, start)
        for condition, body, start, _ in if_blocks(submit_body)
        if set(canonical_code(condition).split("||")) == epoch_terms
    ]
    if len(epoch_blocks) != 1:
        fail("synchronous submit must reject completion from a raced epoch")
    if "*release_buffer=FALSE;" not in canonical_code(epoch_blocks[0][1]):
        fail("synchronous submit must retain a descriptor whose completion epoch raced teardown")
    release_writes = re.findall(
        r"\*\s*release_buffer\s*=(?!=)\s*(TRUE|FALSE|true|false)\s*;",
        submit_body,
    )
    release_writes = [canonical_code(value) for value in release_writes]
    if (
        release_writes.count("TRUE") != 1
        or release_writes.count("FALSE") != 2
        or len(release_writes) != 3
        or len(variable_write_offsets(submit_body, "*release_buffer")) != 3
    ):
        fail("synchronous submit must publish one initial and two failure-path descriptor ownership decisions")

    submitted_writes = re.findall(
        r"\*\s*submitted\s*=(?!=)\s*(TRUE|FALSE|true|false)\s*;",
        submit_body,
    )
    submitted_writes = [canonical_code(value) for value in submitted_writes]
    queue_call = submit.find("if(QueueBuffer(buf)<0)")
    submitted_true = submit.find("*submitted=TRUE;")
    if (
        submitted_writes != ["FALSE", "TRUE"]
        or len(variable_write_offsets(submit_body, "*submitted")) != 2
        or queue_call < 0
        or submitted_true < queue_call
    ):
        fail("synchronous submit must publish Submitted only after the descriptor enters the queue")


def check_native_context_ownership() -> None:
    queue_header = canonical_code(strip_cpp_comments_and_literals(QUEUE_HEADER_SOURCE))
    expected_results = (
        "VioGpuHostContextNotSubmitted=0,"
        "VioGpuHostContextConfirmed,"
        "VioGpuHostContextRejected,"
        "VioGpuHostContextUnknown,"
    )
    if queue_header.count(expected_results) != 1:
        fail("Host context queue results must keep the four distinct ownership outcomes")

    create_queue = canonical_code(function_body("CtrlQueue::CreateNativeContext", QUEUE_CODE))
    destroy_queue = canonical_code(function_body("CtrlQueue::DestroyNativeContext", QUEUE_CODE))
    for owner, body in (("create", create_queue), ("destroy", destroy_queue)):
        required = (
            "BOOLEANsubmitted=FALSE;",
            "SubmitSynchronousLocked(vbuf,&releaseBuffer,&submitted)",
            "VIOGPU_HOST_CONTEXT_RESULTresult=VioGpuHostContextUnknown;",
            "if(!submitted){result=VioGpuHostContextNotSubmitted;}",
            "result=VioGpuHostContextConfirmed;",
            "returnresult;",
        )
        if any(body.count(fragment) != 1 for fragment in required):
            fail(f"Host context {owner} must classify NotSubmitted, Confirmed, and Unknown separately")
        if body.find("VioGpuHostContextUnknown") > body.find("if(!submitted)"):
            fail(f"Host context {owner} must default to Unknown before interpreting completion")
        if body.find("if(!submitted)") > body.find("elseif(completed&&vbuf->response_size==sizeof(GPU_CTRL_HDR))"):
            fail(f"Host context {owner} must classify non-submission before any response")

    create_rejected = (
        "elseif(IsPlainControlErrorResponse(response))"
        "{result=VioGpuHostContextRejected;}"
    )
    if create_queue.count(create_rejected) != 1:
        fail("Host create may classify Rejected only from a complete plain VirtIO error response")
    destroy_rejected = (
        "elseif(IsPlainControlErrorResponse(response)&&"
        "response->type==VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID)"
        "{result=VioGpuHostContextRejected;}"
    )
    if destroy_queue.count(destroy_rejected) != 1:
        fail("Host destroy may classify Rejected only from exact INVALID_CONTEXT_ID acknowledgement")
    if destroy_queue.count("VioGpuHostContextRejected") != 1:
        fail("Host destroy must leave every other submitted non-success response Unknown")

    create = canonical_code(function_body("VioGpuAdapter::CreateNativeContext", VIOGPU_CODE))
    registry_insert = create.find("InsertTailList(&m_NativeContextRegistry,&owner->AdapterLink);")
    host_create = create.find("m_CtrlQueue.CreateNativeContext(contextId)")
    if registry_insert < 0 or host_create < 0 or registry_insert > host_create:
        fail("adapter must record Creating Host ownership before submitting CTX_CREATE")
    if create.count("owner->State=VioGpuNativeContextOwnerCreating;") != 1 or create.count(
        "owner->State=VioGpuNativeContextOwnerLive;"
    ) != 1:
        fail("adapter create must publish one Creating-to-Live Host-owner transition")
    create_retire = (
        "if(createResult==VioGpuHostContextNotSubmitted||createResult==VioGpuHostContextRejected)"
        "{RetireNativeContextOwnerLocked(owner);}"
        "else{owner->Registration=NULL;}"
    )
    if create.count(create_retire) != 1:
        fail("adapter create must retain only submitted ownership whose outcome is Unknown")

    destroy = canonical_code(function_body("VioGpuAdapter::DestroyNativeContext", VIOGPU_CODE))
    mark_destroying = destroy.find("owner->State=VioGpuNativeContextOwnerDestroying;")
    host_destroy = destroy.find("m_CtrlQueue.DestroyNativeContext(contextId)")
    if mark_destroying < 0 or host_destroy < 0 or mark_destroying > host_destroy:
        fail("adapter must retain and mark Host ownership Destroying before CTX_DESTROY")
    retire_guard = (
        "if(destroyResult==VioGpuHostContextConfirmed||destroyResult==VioGpuHostContextRejected)"
        "{RetireNativeContextOwnerLocked(owner);}"
        "else{owner->Registration=NULL;}"
    )
    if destroy.count(retire_guard) != 1:
        fail("adapter destroy may retire only Confirmed or exact INVALID_CONTEXT_ID ownership")
    failure_guard = (
        "if(destroyResult!=VioGpuHostContextConfirmed&&destroyResult!=VioGpuHostContextRejected)"
        "{FailNativeContextAtAnyIrql();"
    )
    if destroy.count(failure_guard) != 1:
        fail("adapter destroy must fail the transport while retaining NotSubmitted or Unknown ownership")

    submit = canonical_code(function_body("VioGpuWddmSubmitCommand", WDDM_DDI_CODE))
    expected_submit = (
        "UNREFERENCED_PARAMETER(hAdapter);"
        "UNREFERENCED_PARAMETER(submitCommand);"
        "returnSTATUS_NOT_SUPPORTED;"
    )
    if submit != expected_submit:
        fail("SubmitCommand must remain an exact fail-closed stub until GPU retirement exists")


def check_wddm_context_lifetime() -> None:
    context_header = canonical_code(WDDM_DDI_HEADER_CODE)
    for required in (
        "EX_RUNDOWN_REFOperations;",
        "BOOLEANOperationsRundownCompleted;",
    ):
        if context_header.count(required) != 1:
            fail(f"WDDM context must expose one retry-safe operations rundown field: {required}")

    closing_definitions = re.findall(
        r"\bconst\s+LONG\s+VIOGPU_WDDM_DEVICE_CLOSING\s*=\s*"
        r"static_cast\s*<\s*LONG\s*>\s*\(\s*0x80000000UL\s*\)\s*;",
        WDDM_DDI_CODE,
    )
    mask_definitions = re.findall(
        r"\bconst\s+LONG\s+VIOGPU_WDDM_DEVICE_REFERENCE_MASK\s*=\s*0x7FFFFFFF\s*;",
        WDDM_DDI_CODE,
    )
    if len(closing_definitions) != 1 or len(mask_definitions) != 1:
        fail("device lifetime must reserve the high bit for closing and low 31 bits for references")

    reference = canonical_code(
        function_body_with_parameters(
            "ReferenceDevice",
            "VIOGPU_WDDM_DEVICE *device",
            WDDM_DDI_CODE,
        )
    )
    reference_stages = (
        (reference.find("LONGstate=InterlockedCompareExchange(&device->ReferenceState,0,0);"), "state snapshot"),
        (
            reference.find(
                "while((state&VIOGPU_WDDM_DEVICE_CLOSING)==0&&state<VIOGPU_WDDM_DEVICE_REFERENCE_MASK)"
            ),
            "closing gate",
        ),
        (
            reference.find(
                "LONGobserved=InterlockedCompareExchange(&device->ReferenceState,state+1,state);"
            ),
            "reference acquisition",
        ),
        (reference.find("if(device->Signature==VIOGPU_WDDM_DEVICE_SIGNATURE)"), "signature validation"),
        (reference.find("DereferenceDevice(device);", 0), "failed-signature release"),
    )
    for (offset, description), (next_offset, next_description) in zip(reference_stages, reference_stages[1:]):
        if offset < 0 or next_offset < 0 or offset > next_offset:
            fail(f"device reference must perform {description} before {next_description}")
    if reference.count("InterlockedCompareExchange(&device->ReferenceState,state+1,state)") != 1:
        fail("device reference must use one closing-aware atomic acquisition")

    dereference = canonical_code(
        function_body_with_parameters(
            "DereferenceDevice",
            "VIOGPU_WDDM_DEVICE *device",
            WDDM_DDI_CODE,
        )
    )
    expected_dereference = (
        "LONGstate=InterlockedDecrement(&device->ReferenceState);"
        "NT_ASSERT((state&VIOGPU_WDDM_DEVICE_REFERENCE_MASK)!=VIOGPU_WDDM_DEVICE_REFERENCE_MASK);"
    )
    if dereference != expected_dereference:
        fail("device dereference must preserve the closing bit while decrementing one low-bit reference")

    create_device = canonical_code(function_body("VioGpuWddmCreateDevice", WDDM_DDI_CODE))
    if create_device.count("device->ReferenceState=0;") != 1:
        fail("CreateDevice must initialize one open zero-reference state")

    destroy_device = canonical_code(function_body("VioGpuWddmDestroyDevice", WDDM_DDI_CODE))
    destroy_stages = (
        (destroy_device.find("LONGstate=InterlockedCompareExchange(&device->ReferenceState,0,0);"), "state snapshot"),
        (destroy_device.find("LONGclosingState=state|VIOGPU_WDDM_DEVICE_CLOSING;"), "closing state construction"),
        (
            destroy_device.find(
                "InterlockedCompareExchange(&device->ReferenceState,closingState,state)"
            ),
            "closing publication",
        ),
        (
            destroy_device.find("if((state&VIOGPU_WDDM_DEVICE_REFERENCE_MASK)!=0)"),
            "reference drain check",
        ),
        (destroy_device.find("returnSTATUS_DEVICE_BUSY;"), "busy retention"),
        (destroy_device.find("device->Signature=0;"), "signature invalidation"),
        (destroy_device.find("deletedevice;"), "device deletion"),
    )
    for (offset, description), (next_offset, next_description) in zip(destroy_stages, destroy_stages[1:]):
        if offset < 0 or next_offset < 0 or offset > next_offset:
            fail(f"DestroyDevice must perform {description} before {next_description}")
    if destroy_device.count(
        "InterlockedCompareExchange(&device->ReferenceState,closingState,state)"
    ) != 1:
        fail("DestroyDevice must publish closing exactly once and retain it across busy retry")

    create = canonical_code(function_body("VioGpuWddmCreateContext", WDDM_DDI_CODE))
    reserve = create.find("if(!ReferenceDevice(device))")
    allocate = create.find("context=new(NonPagedPoolNx)VIOGPU_WDDM_CONTEXT;")
    initialize_rundown = create.find("ExInitializeRundownProtection(&context->Operations);")
    publish_open = create.find("context->OperationsRundownCompleted=FALSE;")
    binding_lock = create.find("KeInitializeSpinLock(&context->NativeContext.BindingLock);")
    host_create = create.find("device->Adapter->CreateNativeContext(&context->NativeContext)")
    publish = create.find("createContext->hContext=context;")
    if min(reserve, allocate, initialize_rundown, publish_open, binding_lock, host_create, publish) < 0 or not (
        reserve < allocate < initialize_rundown < publish_open < binding_lock < host_create < publish
    ):
        fail("CreateContext must initialize open operations rundown before Host creation and handle publication")
    if create.count("ExInitializeRundownProtection(&context->Operations)") != 1 or create.count(
        "context->OperationsRundownCompleted=FALSE;"
    ) != 1:
        fail("CreateContext must publish exactly one initialized open operations rundown")
    if create.count("ReferenceDevice(device)") != 1 or create.count("DereferenceDevice(device)") != 2:
        fail("CreateContext must release its device reservation on both post-reservation failure paths")
    for condition, required_prefix in (
        (
            "context==NULL",
            "DereferenceDevice(device);returnSTATUS_NO_MEMORY;",
        ),
        (
            "!NT_SUCCESS(status)",
            "context->Signature=0;deletecontext;DereferenceDevice(device);returnstatus;",
        ),
    ):
        blocks = [
            canonical_code(body)
            for candidate, body, _, _ in if_blocks(function_body("VioGpuWddmCreateContext", WDDM_DDI_CODE))
            if canonical_code(candidate) == condition
        ]
        if len(blocks) != 1 or blocks[0] != required_prefix:
            fail("CreateContext must release the device reservation on every failed construction")

    destroy_body = function_body("VioGpuWddmDestroyContext", WDDM_DDI_CODE)
    destroy = canonical_code(destroy_body)
    rundown_blocks = [
        canonical_code(body)
        for condition, body, _, _ in if_blocks(destroy_body)
        if canonical_code(condition) == "!context->OperationsRundownCompleted"
    ]
    expected_rundown_close = (
        "ExWaitForRundownProtectionRelease(&context->Operations);"
        "ExRundownCompleted(&context->Operations);"
        "context->OperationsRundownCompleted=TRUE;"
    )
    if len(rundown_blocks) != 1 or rundown_blocks[0] != expected_rundown_close:
        fail("DestroyContext must wait and complete operations rundown exactly once across retries")
    native_destroy = destroy.find("context->Device->Adapter->DestroyNativeContext(")
    rundown_close = destroy.find(expected_rundown_close)
    release_guard = destroy.find("if(!released)", native_destroy)
    if min(rundown_close, native_destroy, release_guard) < 0 or not rundown_close < native_destroy < release_guard:
        fail("DestroyContext must close operations before native destroy and prove release before deletion")
    if (
        destroy.count("ExWaitForRundownProtectionRelease(&context->Operations)") != 1
        or destroy.count("ExRundownCompleted(&context->Operations)") != 1
        or destroy.count("context->OperationsRundownCompleted=TRUE;") != 1
        or "ExReInitializeRundownProtection(&context->Operations)" in WDDM_DDI_CODE
        or len(variable_write_offsets(destroy_body, "context->OperationsRundownCompleted")) != 1
    ):
        fail("context operations rundown must close once, stay closed on failure, and never reopen")
    destroy_sequence = (
        "VIOGPU_WDDM_DEVICE*device=context->Device;"
        "context->Signature=0;deletecontext;DereferenceDevice(device);returnSTATUS_SUCCESS;"
    )
    if destroy.count(destroy_sequence) != 1:
        fail("DestroyContext must release its device reservation only after context destruction is proven safe")

    snapshot = function_body("VioGpuAdapter::AcquireNativeContextSnapshot", VIOGPU_CODE)
    snapshot_compact = canonical_code(snapshot)
    irql_guard = snapshot_compact.find("KeGetCurrentIrql()!=PASSIVE_LEVEL")
    wait = snapshot_compact.find("KeWaitForSingleObject(&adapter->m_NativeContextLifecycleMutex")
    if irql_guard < 0 or wait < 0 or irql_guard > wait:
        fail("native-context snapshot acquisition must reject non-PASSIVE callers before waiting")

    generation_current = canonical_code(
        function_body("VioGpuAdapter::IsNativeContextGenerationCurrent", VIOGPU_CODE)
    )
    reset_gate = "!m_pVioGpuDod->IsHardwareResetRequested()"
    if generation_current.count(reset_gate) != 1:
        fail("native-context generation validation must fail closed while hardware reset is requested")
    state_check = "InterlockedCompareExchange(&m_NativeContextState,VioGpuNativeContextOffline,VioGpuNativeContextOffline)"
    if generation_current.find(reset_gate) > generation_current.find(state_check):
        fail("native-context generation validation must check the hardware reset gate before publishing Ready state")

    render_body = function_body("VioGpuWddmRender", WDDM_DDI_CODE)
    render = canonical_code(render_body)
    acquire_rundown = render.find("if(!ExAcquireRundownProtection(&context->Operations))")
    signature_check = render.find("if(context->Signature!=VIOGPU_WDDM_CONTEXT_SIGNATURE)")
    acquire = render.find("VioGpuAdapter::AcquireNativeContextSnapshot(&context->NativeContext,&snapshot)")
    if min(acquire_rundown, signature_check, acquire) < 0 or not acquire_rundown < signature_check < acquire:
        fail("Render must acquire context operations before signature validation and native snapshot use")
    if (
        render.count("ExAcquireRundownProtection(&context->Operations)") != 1
        or render.count("VioGpuAdapter::AcquireNativeContextSnapshot(") != 1
    ):
        fail("Render must acquire exactly one context rundown and one native-context snapshot")
    acquire_failure = [
        canonical_code(body)
        for condition, body, _, _ in if_blocks(render_body)
        if canonical_code(condition) == "!ExAcquireRundownProtection(&context->Operations)"
    ]
    signature_failure = [
        canonical_code(body)
        for condition, body, _, _ in if_blocks(render_body)
        if canonical_code(condition) == "context->Signature!=VIOGPU_WDDM_CONTEXT_SIGNATURE"
    ]
    if acquire_failure != ["returnSTATUS_DEVICE_NOT_READY;"] or signature_failure != [
        "ExReleaseRundownProtection(&context->Operations);returnSTATUS_INVALID_HANDLE;"
    ]:
        fail("Render must fail closed on rundown acquisition and release it after a bad signature")
    if render.count("VioGpuAdapter::ReleaseNativeContextSnapshot(&snapshot);") != 4:
        fail("Render must release its native-context snapshot on every post-acquisition exit")
    if render.count("ExReleaseRundownProtection(&context->Operations);") != 6:
        fail("Render must release context operations on every post-acquisition exit")
    post_acquire_guards = (
        "render->DmaSize<render->CommandLength||render->PatchLocationListOutSize<render->PatchLocationListInSize",
        "!NT_SUCCESS(status)",
        "!snapshot.Adapter->IsNativeContextGenerationCurrent(snapshot.Generation)",
    )
    for condition in post_acquire_guards:
        blocks = [
            canonical_code(body)
            for candidate, body, start, _ in if_blocks(render_body)
            if canonical_code(candidate) == condition
            and len(canonical_code(render_body[:start])) > acquire
            and "return" in canonical_code(body)
        ]
        if len(blocks) != 1 or not blocks[0].startswith(
            "VioGpuAdapter::ReleaseNativeContextSnapshot(&snapshot);"
            "ExReleaseRundownProtection(&context->Operations);return"
        ):
            fail("Render must release snapshot and context rundown before every failed post-acquisition return")
    render_success = (
        "VioGpuAdapter::ReleaseNativeContextSnapshot(&snapshot);"
        "ExReleaseRundownProtection(&context->Operations);returnSTATUS_SUCCESS;"
    )
    if not render.endswith(render_success):
        fail("Render must release snapshot and context rundown on its success path")


def check_dpc_completion_semantics() -> None:
    dpc_body = function_body("VioGpuAdapter::DpcRoutine", VIOGPU_CODE)
    dpc = compact_code(dpc_body)
    if top_level_control_transfers(dpc_body):
        fail("DPC must not return or jump before draining queued completions")
    callback_prefix = canonical_code(
        """
        void (*completeCallback)(void *) = pvbuf->complete_cb;
        void *completeContext = pvbuf->complete_ctx;
        """
    )
    callback_blocks = [
        (body, start, end)
        for condition, body, start, end in if_blocks(dpc_body)
        if canonical_code(condition) in ("completeCallback!=NULL", "completeCallback")
    ]
    callback_body = canonical_code(callback_blocks[0][0]) if len(callback_blocks) == 1 else ""
    callback_actions = (
        "pvbuf->complete_cb=NULL;"
        "pvbuf->complete_ctx=NULL;"
        "completeCallback(completeContext);"
        "continue;"
    )
    callback_prefix_offset = dpc.find(callback_prefix)
    if len(callback_blocks) != 1 or callback_prefix_offset < 0 or callback_actions not in callback_body:
        fail("DPC must clear and invoke a synchronous callback before considering automatic release")
    control_release_calls = method_call_offsets(dpc_body, aliases_of(dpc_body, "m_CtrlQueue"), "ReleaseBuffer")
    release_match = re.search(r"\bm_CtrlQueue\s*\.\s*ReleaseBuffer\s*\(\s*pvbuf\s*\)", dpc_body)
    release_offset = release_match.start() if release_match is not None else -1
    if release_offset < callback_blocks[0][2]:
        fail("DPC must not release a control buffer before the callback path continues")
    if len(control_release_calls) != 1:
        fail("DPC must release a control buffer only through the post-callback automatic-release path")


def check_segment_failure_semantics() -> None:
    init_body = function_body("VioGpuMemSegment::Init", QUEUE_CODE)
    init = compact_code(init_body)
    require_single_final_return(init_body, "return TRUE;", "memory-segment initialization")
    size_candidates = ("m_Size=size;", "m_Size=pages*PAGE_SIZE;")
    size_offsets = [init.find(candidate) for candidate in size_candidates if candidate in init]
    size_offset = size_offsets[0] if len(size_offsets) == 1 else -1
    mdl_offset = init.find("m_pMdl=IoAllocateMdl(")
    sg_offset = init.find("m_pSGList=reinterpret_cast<PSCATTER_GATHER_LIST>(new(")
    if min(size_offset, mdl_offset, sg_offset) < 0 or not (size_offset < mdl_offset < sg_offset):
        fail("memory-segment initialization must retain the rounded extent before fallible MDL and SG setup")

    rollback_paths = (
        "if(!m_pMdl)",
        "__except(EXCEPTION_EXECUTE_HANDLER)",
        "if(m_pSGList==NULL)",
        "if(pa.QuadPart==0LL)",
        "if(m_pSGList->NumberOfElements!=pages)",
    )
    for guard in rollback_paths:
        guard_offset = init.find(guard)
        if guard_offset < 0:
            fail(f"memory-segment initialization is missing rollback guard {guard}")
        return_offset = init.find("returnFALSE;", guard_offset)
        close_offset = init.find("Close();", guard_offset, return_offset)
        if (
            return_offset < 0
            or close_offset < 0
            or init[close_offset : return_offset + len("returnFALSE;")] != "Close();returnFALSE;"
        ):
            fail(f"memory-segment initialization must call Close before failing {guard}")
    if len(re.findall(r"\bm_pPci\s*=(?!=)\s*pPci\s*;", init_body)) != 1:
        fail("memory-segment initialization must acquire its allocator owner exactly once")

    sg_construction = (
        "for(UINTi=0;i<pages;++i){"
        "PHYSICAL_ADDRESSpa={0};"
        "ASSERT(MmIsAddressValid(buf));"
        "pa=m_pPci->GetDmaPhysicalAddress(buf);"
    )
    sg_publish = (
        "m_pSGList->Elements[i].Address=pa;"
        "m_pSGList->Elements[i].Length=PAGE_SIZE;"
        "buf=(PVOID)((LONG_PTR)(buf)+PAGE_SIZE);"
        "m_pSGList->NumberOfElements++;"
    )
    exact_count = "if(m_pSGList->NumberOfElements!=pages){Close();returnFALSE;}"
    if sg_construction not in init or sg_publish not in init or exact_count not in init:
        fail("memory-segment initialization must construct and validate one exact SG element per page")


def check_pci_resource_lifetime() -> None:
    unmap_body = function_body("CPciBar::Unmap", PCI_CODE)
    unmap = canonical_code(unmap_body)
    require_single_final_return(unmap_body, "return STATUS_SUCCESS;", "PCI BAR unmap")
    mapping_guard = "if(!m_bPortSpace||m_bIoMapped)"
    unmap_call = "status=pDxgkInterface->DxgkCbUnmapMemory(pDxgkInterface->DeviceHandle,m_BaseVA);"
    clear_mapping = "m_BaseVA=NULL;"
    failure_blocks = [
        (body, start, end)
        for condition, body, start, end in if_blocks(unmap_body)
        if is_failure_condition(condition, "status") and canonical_code(body) == "returnstatus;"
    ]
    guard_offset = unmap.find(mapping_guard)
    call_offset = unmap.find(unmap_call)
    failure_offset = len(canonical_code(unmap_body[: failure_blocks[0][1]])) if len(failure_blocks) == 1 else -1
    clear_offset = unmap.find(clear_mapping, call_offset)
    if min(guard_offset, call_offset, failure_offset, clear_offset) < 0 or not (
        guard_offset < call_offset < failure_offset < clear_offset
    ):
        fail("PCI BAR unmap must retain its mapping until Dxgk confirms unmap success")
    if len(re.findall(r"\bstatus\s*=(?!=)", unmap_body)) != 1:
        fail("PCI BAR unmap status must flow directly from Dxgk into its failure guard")
    raw_call_start = unmap_body.find("NTSTATUS status = pDxgkInterface->DxgkCbUnmapMemory")
    raw_call_end = unmap_body.find(";", raw_call_start) + 1
    if len(failure_blocks) != 1 or canonical_code(unmap_body[raw_call_end : failure_blocks[0][1]]) != "":
        fail("PCI BAR unmap status must be validated immediately after the Dxgk callback")

    close_body = function_body("CPciResources::Close", PCI_CODE)
    close = canonical_code(close_body)
    require_single_final_return(close_body, "return STATUS_SUCCESS;", "PCI resource close")
    unmap_bar = "status=m_Bars[bar].Unmap(m_pDxgkInterface);"
    record_failure = "if(!NT_SUCCESS(status)&&NT_SUCCESS(firstFailure)){firstFailure=status;}"
    failure_return = "if(!NT_SUCCESS(firstFailure)){returnfirstFailure;}"
    reset_bar = "m_Bars[bar]=CPciBar();"
    clear_owner = "m_pDxgkInterface=NULL;"
    loops = list(re.finditer(r"for\(UINTbar=0;bar<PCI_TYPE0_ADDRESSES;(?:\+\+bar|bar\+\+)\)", close))
    loop_offset = loops[0].start() if len(loops) == 2 else -1
    unmap_offset = close.find(unmap_bar, loop_offset)
    record_offset = close.find(record_failure, unmap_offset)
    failure_offset = close.find(failure_return, record_offset)
    reset_offset = close.find(reset_bar, failure_offset)
    owner_offset = close.find(clear_owner, reset_offset)
    if min(loop_offset, unmap_offset, record_offset, failure_offset, reset_offset, owner_offset) < 0 or not (
        loop_offset < unmap_offset < record_offset < failure_offset < reset_offset < owner_offset
    ):
        fail("PCI close must try every BAR and publish clean metadata only after all unmaps succeed")
    if close.count(unmap_bar) != 1 or len(loops) != 2 or close.count(clear_owner) != 1:
        fail("PCI close must use one full unmap pass followed by one metadata reset pass")
    if len(variable_write_offsets(close_body, "firstFailure")) != 2:
        fail("PCI close must preserve the first BAR unmap failure until it is returned")

    init = function_body("CPciResources::Init", PCI_CODE)
    init_compact = compact_code(init)
    owner_assign = init_compact.find("m_pDxgkInterface=pDxgkInterface;")
    success_return = init_compact.rfind("returntrue;")
    if owner_assign < 0 or success_return < owner_assign:
        fail("PCI initialization must acquire its DXGK owner before enumeration")
    failure_tail = init_compact[owner_assign:success_return]
    rollback_sequence = "Status=Close();NT_ASSERT(NT_SUCCESS(Status));returnfalse;"
    if failure_tail.count(rollback_sequence) != 3:
        fail("PCI initialization must transactionally roll back every post-ownership failure")
    if len(variable_write_offsets(init, "m_pDxgkInterface")) != 1:
        fail("PCI initialization must not overwrite its owner around rollback")


def check_rdma_ioctl_lifetime() -> None:
    interface = canonical_code(RDMAPOOL_INTERFACE_CODE)
    for name, value in (
        ("RDMAPOOL_INTERFACE_VERSION_V2", 2),
        ("IOCTL_RDMAPOOL_ALLOCATE", 0x805),
        ("IOCTL_RDMAPOOL_FREE", 0x806),
        ("IOCTL_RDMAPOOL_QUERY_POOL", 0x807),
    ):
        if name.startswith("IOCTL_"):
            matches = re.findall(
                rf"#define{re.escape(name)}CTL_CODE\(FILE_DEVICE_RDMAPOOL,(0x[0-9A-Fa-f]+),METHOD_BUFFERED,FILE_ANY_ACCESS\)",
                interface,
            )
            if len(matches) != 1 or int(matches[0], 0) != value:
                fail(f"restricted-DMA V2 must define {name} with function {value:#x} exactly once")
        else:
            require_integer_define(RDMAPOOL_INTERFACE_SOURCE, name, value, "restricted-DMA V2 interface")

    allocate_output = re.search(
        r"typedefstruct_RDMAPOOL_ALLOCATE_OUTPUT\{([^{}]+)\}RDMAPOOL_ALLOCATE_OUTPUT",
        interface,
    )
    free_input = re.search(r"typedefstruct_RDMAPOOL_FREE_INPUT\{([^{}]+)\}RDMAPOOL_FREE_INPUT", interface)
    query_output = re.search(
        r"typedefstruct_RDMAPOOL_QUERY_POOL_OUTPUT\{([^{}]+)\}RDMAPOOL_QUERY_POOL_OUTPUT",
        interface,
    )
    if allocate_output is None or free_input is None or query_output is None:
        fail("restricted-DMA V2 interface structures are missing")
    for body, fields, owner in (
        (
            allocate_output.group(1),
            ("ULONGInterfaceVersion;", "ULONGNumPages;", "PVOIDVirtualAddress;", "ULONG64AllocationToken;"),
            "ALLOCATE output",
        ),
        (
            free_input.group(1),
            ("ULONGInterfaceVersion;", "ULONGNumPages;", "PVOIDVirtualAddress;", "ULONG64AllocationToken;"),
            "FREE input",
        ),
        (
            query_output.group(1),
            ("ULONG64TotalSize;", "ULONGInterfaceVersion;", "ULONGPageSize;"),
            "QUERY_POOL output",
        ),
    ):
        if any(body.count(field) != 1 for field in fields):
            fail(f"restricted-DMA V2 {owner} must carry every required versioned ownership field")

    ioctl_body = function_body("RdmaPoolIoctl", RDMA_CODE)
    ioctl = canonical_code(ioctl_body)
    if "VIOGPU_RDMA_IOCTL_RESULTresult={STATUS_INSUFFICIENT_RESOURCES,0,FALSE};" not in ioctl:
        fail("RDMA IOCTL must distinguish requests that were never submitted")
    submit_offset = ioctl.find("result.Submitted=TRUE;")
    dispatch_offset = ioctl.find("NTSTATUSstatus=IoCallDriver(deviceObject,irp);")
    if min(submit_offset, dispatch_offset) < 0 or submit_offset > dispatch_offset:
        fail("RDMA IOCTL must publish Submitted before dispatch")
    pending_blocks = [
        (body, start, end)
        for condition, body, start, end in if_blocks(ioctl_body)
        if is_equality_condition(condition, "status", "STATUS_PENDING")
    ]
    pending = canonical_code(pending_blocks[0][0]) if len(pending_blocks) == 1 else ""
    wait = "NTSTATUSwaitStatus=KeWaitForSingleObject(&event,Executive,KernelMode,FALSE,NULL);"
    if len(pending_blocks) != 1 or wait not in pending or "status=NT_SUCCESS(waitStatus)?ioStatus.Status:waitStatus;" not in pending:
        fail("RDMA IOCTL must synchronously drain a pending IRP before stack buffers leave scope")
    if ioctl.count("KeWaitForSingleObject(") != 1 or "IoCancelIrp(" in ioctl or "IoSetCompletionRoutine(" in ioctl:
        fail("RDMA IOCTL must use one unbounded synchronous drain without a partial cancellation handoff")
    if "result.Status=status;result.Information=ioStatus.Information;returnresult;" not in ioctl:
        fail("RDMA IOCTL must return final status, Information, and submission state together")

    connect = canonical_code(function_body("VioGpuRdmaPool::Connect", RDMA_CODE))
    for required in (
        "ioctlResult.Information!=sizeof(query)",
        "query.InterfaceVersion!=RDMAPOOL_INTERFACE_VERSION_V2",
        "query.PageSize!=PAGE_SIZE",
        "ioctlResult.Information!=sizeof(allocation)",
        "ioctlResult.Information!=sizeof(output)",
        "output.InterfaceVersion!=RDMAPOOL_INTERFACE_VERSION_V2",
        "output.NumPages!=arenaPages",
        "output.AllocationToken==0",
        "vaOffset!=paOffset",
        "vaOffset>query.TotalSize-arenaSize",
        "m_AllocationToken=output.AllocationToken;",
        "m_ArenaOwned=TRUE;",
        "m_Ready=TRUE;",
    ):
        if connect.count(required) != 1:
            fail(f"RDMA Connect must validate and publish V2 ownership exactly once: {required}")
    if not connect.find("RtlZeroMemory(m_BaseVA,m_Size);") < connect.find("m_Ready=TRUE;"):
        fail("RDMA Connect must publish Ready only after arena initialization completes")
    if "m_ArenaOwned||m_FileObject!=NULL||m_DeviceObject!=NULL||m_Bitmap!=NULL" not in connect:
        fail("RDMA Connect must refuse to replace retained arena ownership")

    disconnect_body = function_body("VioGpuRdmaPool::Disconnect", RDMA_CODE)
    disconnect = canonical_code(disconnect_body)
    for required in (
        "m_Ready=FALSE;",
        "ExWaitForRundownProtectionRelease(&m_Operations);",
        "input.InterfaceVersion=RDMAPOOL_INTERFACE_VERSION_V2;",
        "input.NumPages=m_PageCount;",
        "input.AllocationToken=m_AllocationToken;",
        "if(!ioctlResult.Submitted){m_DisconnectStatus=ioctlResult.Status;returnm_DisconnectStatus;}",
        "if(m_DisconnectAttempted){returnm_DisconnectStatus;}",
        "m_DisconnectAttempted=TRUE;",
    ):
        if disconnect.count(required) != 1:
            fail(f"RDMA Disconnect must retain exact V2 ownership until confirmed FREE: {required}")
    if disconnect.count("ClearConnection();returnSTATUS_SUCCESS;") != 2:
        fail("RDMA Disconnect may clear only no-owner state or provider-confirmed FREE state")
    failure_blocks = [
        (body, start, end)
        for condition, body, start, end in if_blocks(disconnect)
        if is_failure_condition(condition, "m_DisconnectStatus")
    ]
    if len(failure_blocks) != 1 or canonical_code(failure_blocks[0][0]) != "returnm_DisconnectStatus;":
        fail("RDMA Disconnect must retain exact V2 ownership on one provider FREE failure path")
    failure_return = failure_blocks[0][1]
    final_clear = disconnect.rfind("ClearConnection();returnSTATUS_SUCCESS;")
    if final_clear < failure_return:
        fail("RDMA Disconnect must retain ownership on provider FREE failure")
    submitted_failure = disconnect.find("m_DisconnectAttempted=TRUE;")
    not_submitted = disconnect.find("if(!ioctlResult.Submitted)")
    if disconnect.count("RdmaPoolIoctl(") != 1 or not (not_submitted < submitted_failure < failure_return):
        fail("RDMA FREE must cache only a submitted attempt and prohibit duplicate submission")
    if not disconnect.find("m_Ready=FALSE;") < disconnect.find("ExWaitForRundownProtectionRelease") < disconnect.find("RdmaPoolIoctl("):
        fail("RDMA Disconnect must close operations and drain rundown before provider FREE")

    header = canonical_code(RDMA_HEADER_CODE)
    for required in (
        "BOOLEANm_ArenaOwned;",
        "BOOLEANm_Ready;",
        "BOOLEANm_RundownCompleted;",
        "BOOLEANm_DisconnectAttempted;",
        "ULONG64m_AllocationToken;",
        "mutableEX_RUNDOWN_REFm_Operations;",
        "returnm_Ready;",
        "returnm_ArenaOwned;",
    ):
        if header.count(required) != 1:
            fail(f"RDMA client must separate arena ownership, readiness, and rundown: {required}")
    clear_connection = canonical_code(function_body("VioGpuRdmaPool::ClearConnection", RDMA_CODE))
    if clear_connection.count("m_DisconnectAttempted=FALSE;") != 1:
        fail("RDMA client may clear submitted-FREE state only with the complete connection")

    allocate = canonical_code(function_body("VioGpuRdmaPool::Allocate", RDMA_CODE))
    free = canonical_code(function_body("VioGpuRdmaPool::Free", RDMA_CODE))
    if allocate.count("ExAcquireRundownProtection(&m_Operations)") != 1 or allocate.count("ExReleaseRundownProtection(&m_Operations)") < 5:
        fail("RDMA sub-allocation must hold rundown across bitmap publication and memory initialization")
    if free.count("ExAcquireRundownProtection(&m_Operations)") != 1 or free.count("ExReleaseRundownProtection(&m_Operations)") < 4:
        fail("RDMA sub-free must hold rundown across header validation and bitmap consumption")

    provider = canonical_code(RDMAPOOL_PROVIDER_CODE)
    if provider.count("WdfRequestGetRequestorMode(Request)!=KernelMode") != 1:
        fail("rdmapool provider must reject user-mode kernel-VA requests")
    for required in (
        "WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&fileAttributes,RDMAPOOL_FILE_CONTEXT);",
        "WDF_FILEOBJECT_CONFIG_INIT(&fileConfig,RdmaPoolEvtDeviceFileCreate,RdmaPoolEvtFileClose,WDF_NO_EVENT_CALLBACK);",
        "WdfDeviceInitSetFileObjectConfig(DeviceInit,&fileConfig,&fileAttributes);",
        "fileContext->InitializingCount=0;",
        "KeInitializeEvent(&fileContext->NoInitializersEvent,NotificationEvent,TRUE);",
        "WdfRequestComplete(Request,STATUS_SUCCESS);",
        "ULONGreleased=DmaPoolCloseOwner(fileContext);",
        "outputValue.InterfaceVersion=RDMAPOOL_INTERFACE_VERSION_V2;",
        "outputValue.NumPages=inputValue.NumPages;",
        "*output=outputValue;",
        "status=DmaPoolFreePages(fileContext,inputValue.VirtualAddress,inputValue.NumPages,inputValue.AllocationToken);",
        "InputBufferLength!=0||OutputBufferLength!=sizeof(RDMAPOOL_QUERY_ALLOCATION_OUTPUT)",
    ):
        if provider.count(required) != 1:
            fail(f"rdmapool dispatch must preserve V2 file ownership and buffered input: {required}")
    if provider.count("inputValue=*input;") != 2:
        fail("rdmapool mutating IOCTLs must snapshot each METHOD_BUFFERED input")
    if provider.count("fileContext==NULL||inputValue.InterfaceVersion!=RDMAPOOL_INTERFACE_VERSION_V2") != 2:
        fail("rdmapool V2 ALLOCATE and FREE must validate file ownership and ABI version")

    allocator = canonical_code(DMAPOOL_CODE)
    retired_reserve = ("IOCTL_RDMAPOOL_RESERVE", "RDMAPOOL_RESERVE_INPUT", "DmaPoolReservePages")
    if any(
        token in RDMAPOOL_INTERFACE_SOURCE or token in RDMAPOOL_PROVIDER_SOURCE or token in DMAPOOL_SOURCE
        for token in retired_reserve
    ):
        fail("restricted-DMA V2 must not retain the ownerless legacy RESERVE path")
    allocate_pages = canonical_code(function_body("DmaPoolAllocatePages", DMAPOOL_CODE))
    free_pages = canonical_code(function_body("DmaPoolFreePages", DMAPOOL_CODE))
    close_owner = canonical_code(function_body("DmaPoolCloseOwner", DMAPOOL_CODE))
    destroy = canonical_code(function_body("DmaPoolDestroy", DMAPOOL_CODE))
    if allocator.count("staticULONG64gNextAllocationToken;") != 1 or "gNextAllocationToken=0;" in canonical_code(
        function_body("DmaPoolInit", DMAPOOL_CODE)
    ):
        fail("rdmapool allocation tokens must not reset across PnP PrepareHardware cycles")
    for required in (
        "gNextAllocationToken==MAXULONGLONG",
        "Allocation->Owner=Owner;",
        "Allocation->Token=gNextAllocationToken;",
        "Allocation->Initializing=TRUE;",
        "InsertTailList(&gAllocationList,&Allocation->ListEntry);",
        "Owner->InitializingCount++;",
        "KeClearEvent(&Owner->NoInitializersEvent);",
        "RtlZeroMemory(allocationVa,(SIZE_T)NumPages*PAGE_SIZE);",
        "if(!gPoolReady||Owner->Closing||Allocation->Cancelled)",
        "Allocation->Initializing=FALSE;",
        "Owner->InitializingCount--;",
        "KeSetEvent(&Owner->NoInitializersEvent,IO_NO_INCREMENT,FALSE);",
    ):
        if allocate_pages.count(required) != 1:
            fail(f"rdmapool ALLOCATE must use two-phase token ownership: {required}")
    for required in (
        "Candidate->Token==AllocationToken",
        "Allocation->Owner!=Owner",
        "Allocation->VirtualAddress!=VirtualAddress",
        "Allocation->NumPages!=NumPages",
        "RemoveEntryList(&Allocation->ListEntry);",
    ):
        if free_pages.count(required) != 1:
            fail(f"rdmapool FREE must atomically consume exact owner/token/extent: {required}")
    if "Owner->Closing=TRUE;" not in close_owner or "Allocation->Cancelled=TRUE;" not in close_owner:
        fail("rdmapool file close must close the owner and cancel unpublished allocations")
    cleanup_wait = close_owner.find("KeWaitForSingleObject(&Owner->NoInitializersEvent")
    if cleanup_wait < close_owner.find("Owner->Closing=TRUE;") or cleanup_wait > close_owner.find("returnReleased;"):
        fail("rdmapool file close must retain owner context until initializers drain")
    destroy_gate = destroy.find("gPoolReady=FALSE;")
    destroy_wait = destroy.find("KeWaitForSingleObject(&gNoInitializersEvent")
    if min(destroy_gate, destroy_wait) < 0 or destroy_gate > destroy_wait:
        fail("rdmapool ReleaseHardware must stop new allocations before draining initializers")


def check_shared_rdma_clients() -> None:
    shared_header = canonical_code(SHARED_RDMA_CLIENT_HEADER_CODE)
    for required in (
        "BOOLEANActive;",
        "ULONGAllocationPages;",
        "ULONG64AllocationToken;",
        "BOOLEANDisconnectAttempted;",
        "NTSTATUSDisconnectStatus;",
    ):
        if shared_header.count(required) != 1:
            fail(f"shared RDMA client must retain readiness and exact V2 ownership: {required}")

    shared_ioctl_body = function_body("RdmaClientIoctl", SHARED_RDMA_CLIENT_CODE)
    shared_ioctl = canonical_code(shared_ioctl_body)
    if "RDMA_CLIENT_IOCTL_RESULTresult={STATUS_INSUFFICIENT_RESOURCES,0,FALSE};" not in shared_ioctl:
        fail("shared RDMA IOCTL must distinguish requests that were never submitted")
    submitted = shared_ioctl.find("result.Submitted=TRUE;")
    dispatched = shared_ioctl.find("status=IoCallDriver(c->PoolDeviceObject,irp);")
    if min(submitted, dispatched) < 0 or submitted > dispatched:
        fail("shared RDMA IOCTL must publish Submitted immediately before dispatch")
    if shared_ioctl.count("KeWaitForSingleObject(&event,Executive,KernelMode,FALSE,NULL)") != 1:
        fail("shared RDMA IOCTL must synchronously drain a pending stack-buffer IRP")
    if "result.Status=status;result.Information=iosb.Information;returnresult;" not in shared_ioctl:
        fail("shared RDMA IOCTL must return status, Information, and submission state together")

    shared_connect = canonical_code(function_body("RdmaClientConnect", SHARED_RDMA_CLIENT_CODE))
    retained_guard = (
        "c->Active||c->PoolFileObject!=NULL||c->PoolDeviceObject!=NULL||"
        "c->BaseVA!=NULL||c->Size!=0||c->AllocationPages!=0||"
        "c->AllocationToken!=0||c->DisconnectAttempted"
    )
    if retained_guard not in shared_connect:
        fail("shared RDMA Connect must refuse to overwrite any retained owner tuple")
    for required in (
        "ioctlResult.Information!=sizeof(queryOutput)",
        "queryOutput.InterfaceVersion!=RDMAPOOL_INTERFACE_VERSION_V2",
        "queryOutput.PageSize!=PAGE_SIZE",
        "allocInput.InterfaceVersion=RDMAPOOL_INTERFACE_VERSION_V2;",
        "ioctlResult.Information!=sizeof(allocOutput)",
        "!RdmaClientValidAllocation(&queryOutput,&allocOutput,totalPages)",
        "freeInput.InterfaceVersion=RDMAPOOL_INTERFACE_VERSION_V2;",
        "freeInput.NumPages=allocOutput.NumPages;",
        "freeInput.VirtualAddress=allocOutput.VirtualAddress;",
        "freeInput.AllocationToken=allocOutput.AllocationToken;",
        "c->AllocationPages=allocOutput.NumPages;",
        "c->DisconnectAttempted=rollbackResult.Submitted;",
    ):
        if shared_connect.count(required) != 1:
            fail(f"shared RDMA Connect must validate or retain malformed V2 ownership: {required}")
    if shared_connect.count("c->AllocationToken=allocOutput.AllocationToken;") != 2:
        fail("shared RDMA Connect must retain malformed and publish valid allocation tokens")
    active_publish = shared_connect.rfind("c->Active=TRUE;")
    token_publish = shared_connect.rfind("c->AllocationToken=allocOutput.AllocationToken;")
    if active_publish < token_publish or active_publish < 0:
        fail("shared RDMA Connect must publish Active only after the complete owner tuple")

    shared_disconnect_body = function_body("RdmaClientDisconnect", SHARED_RDMA_CLIENT_CODE)
    shared_disconnect = canonical_code(shared_disconnect_body)
    for required in (
        "if(c->PollThread!=NULL){returnSTATUS_DEVICE_BUSY;}",
        "if(c->DisconnectAttempted){returnc->DisconnectStatus;}",
        "freeInput.InterfaceVersion=RDMAPOOL_INTERFACE_VERSION_V2;",
        "freeInput.VirtualAddress=c->BaseVA;",
        "freeInput.NumPages=c->AllocationPages;",
        "freeInput.AllocationToken=c->AllocationToken;",
        "if(!ioctlResult.Submitted){c->DisconnectStatus=status;returnc->DisconnectStatus;}",
        "c->DisconnectAttempted=TRUE;",
        "RdmaClientClosePoolFile(c);",
        "c->AllocationPages=0;",
        "c->AllocationToken=0;",
        "c->Active=FALSE;",
    ):
        if shared_disconnect.count(required) != 1:
            fail(f"shared RDMA Disconnect must retain exact ownership until confirmed FREE: {required}")
    not_submitted = shared_disconnect.find("if(!ioctlResult.Submitted)")
    attempted = shared_disconnect.find("c->DisconnectAttempted=TRUE;")
    failed = shared_disconnect.find("if(!NT_SUCCESS(status)||ioctlResult.Information!=0)")
    close_file = shared_disconnect.find("RdmaClientClosePoolFile(c);")
    if shared_disconnect.count("RdmaClientIoctl(") != 1 or not (
        0 <= not_submitted < attempted < failed < close_file
    ):
        fail("shared RDMA FREE must cache only submitted attempts and clear only confirmed ownership")

    wdf_header = canonical_code(WDF_HEADER_CODE)
    for required in (
        "WDFWAITLOCKRdmaPoolIoctlLock;",
        "BOOLEANRdmaPoolActive;",
        "BOOLEANRdmaPoolClosing;",
        "BOOLEANRdmaPoolOwnerUnknown;",
        "LIST_ENTRYRdmaPoolAllocList;",
    ):
        if wdf_header.count(required) != 1:
            fail(f"WDF RDMA client must separate readiness, closing, and owner state: {required}")

    wdf_ioctl = canonical_code(function_body("RdmaPoolIoctl", WDF_DMA_CODE))
    if "RDMAPOOL_IOCTL_RESULTresult={STATUS_INSUFFICIENT_RESOURCES,0,FALSE};" not in wdf_ioctl:
        fail("WDF RDMA IOCTL must distinguish requests that were never submitted")
    submitted = wdf_ioctl.find("result.Submitted=TRUE;")
    dispatched = wdf_ioctl.find("status=IoCallDriver(pWdfDriver->RdmaPoolDeviceObject,irp);")
    if min(submitted, dispatched) < 0 or submitted > dispatched:
        fail("WDF RDMA IOCTL must publish Submitted immediately before dispatch")

    free_entry = canonical_code(function_body("FreeRdmaPoolEntryLocked", WDF_DMA_CODE))
    for required in (
        "if(entry->FreeAttempted){returnentry->FreeStatus;}",
        "freeInput.InterfaceVersion=RDMAPOOL_INTERFACE_VERSION_V2;",
        "freeInput.NumPages=entry->NumPages;",
        "freeInput.VirtualAddress=entry->VirtualAddress;",
        "freeInput.AllocationToken=entry->AllocationToken;",
        "if(!ioctlResult.Submitted){entry->FreeStatus=ioctlResult.Status;returnentry->FreeStatus;}",
        "entry->FreeAttempted=TRUE;",
        "pWdfDriver->RdmaPoolClosing=TRUE;",
    ):
        if free_entry.count(required) != 1:
            fail(f"WDF RDMA FREE must retain exact ownership and submitted state: {required}")
    if not free_entry.find("if(!ioctlResult.Submitted)") < free_entry.find("entry->FreeAttempted=TRUE;"):
        fail("WDF RDMA FREE must allow retry only when no IRP was submitted")

    wdf_allocate = canonical_code(function_body("AllocateFromRdmaPool", WDF_DMA_CODE))
    for required in (
        "!pWdfDriver->RdmaPoolActive||pWdfDriver->RdmaPoolClosing||pWdfDriver->RdmaPoolFileObject==NULL",
        "entry->VirtualAddress=allocOutput.VirtualAddress;",
        "entry->NumPages=allocOutput.NumPages;",
        "entry->AllocationToken=allocOutput.AllocationToken;",
        "ioctlResult.Information!=sizeof(allocOutput)",
        "!ValidateRdmaPoolAllocation(pWdfDriver,&allocOutput,allocInput.NumPages)",
        "rollbackStatus=FreeRdmaPoolEntryLocked(pWdfDriver,entry);",
        "pWdfDriver->RdmaPoolOwnerUnknown=TRUE;",
    ):
        if wdf_allocate.count(required) != 1:
            fail(f"WDF RDMA ALLOCATE must validate or retain malformed ownership: {required}")
    if wdf_allocate.count("InsertTailList(&pWdfDriver->RdmaPoolAllocList,&entry->ListEntry);") != 2:
        fail("WDF RDMA ALLOCATE must track both valid and failed-rollback owner tuples")

    release = canonical_code(function_body("VirtIOWdfReleaseRdmaPoolAllocations", WDF_DMA_CODE))
    unknown = release.find("if(pWdfDriver->RdmaPoolOwnerUnknown)")
    close_unknown = release.find("ObDereferenceObject(pWdfDriver->RdmaPoolFileObject);", unknown)
    free_known = release.find("status=FreeRdmaPoolEntryLocked(pWdfDriver,allocation);")
    remove_known = release.find("RemoveEntryList(&allocation->ListEntry);", free_known)
    if min(unknown, close_unknown, free_known, remove_known) < 0 or not (
        unknown < close_unknown < free_known < remove_known
    ):
        fail("WDF shutdown must use file close for unknown ownership and remove known entries only after FREE")

    wdf_disconnect = canonical_code(function_body("VirtIOWdfDisconnectRdmaPool", WDF_CODE))
    release_call = wdf_disconnect.find("status=VirtIOWdfReleaseRdmaPoolAllocations(pWdfDriver);")
    failure_return = wdf_disconnect.find("if(!NT_SUCCESS(status)){returnstatus;}")
    close_file = wdf_disconnect.find("ObDereferenceObject(pWdfDriver->RdmaPoolFileObject);")
    if min(release_call, failure_return, close_file) < 0 or not release_call < failure_return < close_file:
        fail("WDF disconnect must retain its file owner whenever allocation release fails")

    shutdown = canonical_code(function_body("VirtIOWdfShutdown", WDF_CODE))
    reset = shutdown.find("virtio_device_reset(&pWdfDriver->VIODevice);")
    delete = shutdown.find("virtio_delete_queues(&pWdfDriver->VIODevice);")
    device_shutdown = shutdown.find("virtio_device_shutdown(&pWdfDriver->VIODevice);")
    disconnect = shutdown.find("status=VirtIOWdfDisconnectRdmaPool(pWdfDriver);")
    pci_free = shutdown.find("PCIFreeBars(pWdfDriver);")
    if min(reset, delete, device_shutdown, disconnect, pci_free) < 0 or not (
        reset < delete < device_shutdown < disconnect < pci_free
    ):
        fail("WDF shutdown must quiesce device and queues before RDMA owner cleanup")

    initialize = canonical_code(function_body("VirtIOWdfInitialize", WDF_CODE))
    fallback = (
        "elseif(rdmaStatus==STATUS_NOT_FOUND&&!virtio_is_feature_enabled("
        "VirtIOWdfGetDeviceFeatures(pWdfDriver),VIRTIO_F_ACCESS_PLATFORM))"
    )
    if fallback not in initialize or "status=rdmaStatus;" not in initialize:
        fail("WDF initialization may fall back to normal DMA only when rdmapool is absent")

    set_features = canonical_code(function_body("VirtIOWdfSetDriverFeatures", WDF_CODE))
    access_platform = set_features.find(
        "if(virtio_is_feature_enabled(uDeviceFeatures,VIRTIO_F_ACCESS_PLATFORM))"
    )
    owner_gate = set_features.find(
        "if(!pWdfDriver->RdmaPoolActive||pWdfDriver->RdmaPoolClosing||"
        "pWdfDriver->RdmaPoolFileObject==NULL)",
        access_platform,
    )
    acknowledge = set_features.find(
        "virtio_feature_enable(uFeatures,VIRTIO_F_ACCESS_PLATFORM);", owner_gate
    )
    if min(access_platform, owner_gate, acknowledge) < 0 or not access_platform < owner_gate < acknowledge:
        fail("WDF must require a live rdmapool owner before acknowledging ACCESS_PLATFORM")


def check_netkvm_terminal_cleanup() -> None:
    header = canonical_code(NETKVM_HEADER_CODE)
    for required in (
        "BOOLEANCleanupComplete=FALSE;",
        "NTSTATUSCleanupStatus=STATUS_SUCCESS;",
        "BOOLEANm_OwnerOpen=FALSE;",
        "BOOLEANm_DisconnectStarted=FALSE;",
        "NTSTATUSm_Status=STATUS_SUCCESS;",
    ):
        if header.count(required) != 1:
            fail(f"NetKVM must retain explicit cleanup and rdmapool owner state: {required}")

    free_allocation = canonical_code(function_body("RdmaPoolFreeAllocationLocked", NETKVM_RDMA_CODE))
    cached_free = free_allocation.find(
        "if(Allocation->FreeSubmitted){returnAllocation->FreeStatus;}"
    )
    free_ioctl = free_allocation.find("status=RdmaPoolIoctl(")
    if (
        cached_free < 0
        or free_ioctl < 0
        or cached_free > free_ioctl
        or free_allocation.count("RdmaPoolIoctl(") != 1
    ):
        fail("NetKVM cached FREE completion must return before the only provider IOCTL")
    for required in (
        "if(submitted){Allocation->FreeSubmitted=TRUE;Allocation->FreeStatus=status;}",
        "freeInput.InterfaceVersion=RDMAPOOL_INTERFACE_VERSION_V2;",
        "freeInput.NumPages=Allocation->NumPages;",
        "freeInput.VirtualAddress=Allocation->VirtualAddress;",
        "freeInput.AllocationToken=Allocation->AllocationToken;",
    ):
        if free_allocation.count(required) != 1:
            fail(f"NetKVM FREE must cache one exact provider submission: {required}")

    publish = canonical_code(function_body("RdmaPoolPublishOwnerCleanupLocked", NETKVM_RDMA_CODE))
    for required in (
        "allocation->FreeSubmitted=TRUE;",
        "allocation->FreeStatus=STATUS_SUCCESS;",
        "pContext->RdmaPoolAutoDisconnect.m_EmergencyActive=FALSE;",
        "pContext->RdmaPoolAutoDisconnect.m_Status=STATUS_SUCCESS;",
    ):
        if publish.count(required) != 1:
            fail(f"NetKVM file-owner cleanup must publish terminal tombstones: {required}")
    if "RdmaPoolIoctl(" in publish or "RdmaPoolFreeAllocationLocked(" in publish:
        fail("NetKVM tombstone publication must not submit provider I/O")

    release_body = function_body("ParaNdis_RdmaPoolReleaseAllocations", NETKVM_RDMA_CODE)
    release = canonical_code(release_body)
    cached_status = (
        "if(!NT_SUCCESS(pContext->RdmaPoolAutoDisconnect.m_Status)){"
        "firstFailure=pContext->RdmaPoolAutoDisconnect.m_Status;}"
    )
    release_loop = (
        "for(entry=pContext->RdmaPoolAutoDisconnect.m_Allocations.Flink;"
        "NT_SUCCESS(firstFailure)&&entry!=&pContext->RdmaPoolAutoDisconnect.m_Allocations;"
        "entry=entry->Flink)"
    )
    emergency_gate = "if(NT_SUCCESS(firstFailure)){status=RdmaPoolFreeEmergencyAllocationLocked(pContext);"
    if release.count(cached_status) != 1 or release.count(release_loop) != 1:
        fail("NetKVM release must skip all further FREE submissions after a cached or first failure")
    if release.count(emergency_gate) != 1:
        fail("NetKVM emergency FREE must run only while no earlier FREE has failed")
    if release.count("status=RdmaPoolFreeAllocationLocked(pContext,allocation);") != 1:
        fail("NetKVM release must have one FREE site guarded by the failure-stopping loop")

    terminal_blocks = [
        body
        for condition, body, _, _ in if_blocks(release_body)
        if is_failure_condition(condition, "firstFailure")
    ]
    if len(terminal_blocks) != 1:
        fail("NetKVM release must have one terminal first-failure recovery block")
    terminal = canonical_code(terminal_blocks[0])
    close_file = terminal.find("RdmaPoolCloseFileLocked(pContext);")
    publish_tombstones = terminal.find("RdmaPoolPublishOwnerCleanupLocked(pContext);")
    unlock = terminal.find("RdmaPoolUnlock(pContext);")
    failure_return = terminal.find("returnfirstFailure;")
    if min(close_file, publish_tombstones, unlock, failure_return) < 0 or not (
        close_file < publish_tombstones < unlock < failure_return
    ):
        fail("NetKVM must close the file owner before publishing terminal tombstones")
    if release.count("RdmaPoolCloseFileLocked(pContext);") != 1 or release.count(
        "RdmaPoolPublishOwnerCleanupLocked(pContext);"
    ) != 1:
        fail("NetKVM terminal recovery must close and publish exactly once")

    late_free = canonical_code(function_body("ParaNdis_RdmaPoolFree", NETKVM_RDMA_CODE))
    tombstone_gate = (
        "if(pContext->RdmaPoolAutoDisconnect.m_DisconnectStarted&&!allocation->FreeSubmitted)"
        "{RdmaPoolUnlock(pContext);returnSTATUS_INVALID_DEVICE_STATE;}"
    )
    consume = late_free.find("status=RdmaPoolFreeAllocationLocked(pContext,allocation);")
    failure = late_free.find("if(!NT_SUCCESS(status))", consume)
    remove = late_free.find("RemoveEntryList(&allocation->ListEntry);", failure)
    unlock = late_free.find("RdmaPoolUnlock(pContext);", remove)
    free_record = late_free.find("NdisFreeMemoryWithTagPriority(", unlock)
    if late_free.count(tombstone_gate) != 1 or min(consume, failure, remove, unlock, free_record) < 0 or not (
        consume < failure < remove < unlock < free_record
    ):
        fail("NetKVM late free must accept a submitted tombstone and consume its tracking record")
    if late_free.count("RdmaPoolFreeAllocationLocked(pContext,allocation);") != 1:
        fail("NetKVM late free must use exactly one cached-or-submit FREE helper")

    cleanup = canonical_code(function_body("ParaNdis_CleanupContext", NETKVM_COMMON_CODE))
    cached_cleanup = "if(pContext->CleanupComplete){returnpContext->CleanupStatus;}"
    release_call = cleanup.find("status=ParaNdis_RdmaPoolReleaseAllocations(pContext);")
    disconnect_call = cleanup.find("status=ParaNdis_RdmaPoolDisconnect(pContext);", release_call)
    publish_status = cleanup.find("pContext->CleanupStatus=status;", disconnect_call)
    publish_complete = cleanup.find("pContext->CleanupComplete=TRUE;", publish_status)
    if cleanup.count(cached_cleanup) != 1 or not (
        0 <= cleanup.find(cached_cleanup) < release_call < disconnect_call < publish_status < publish_complete
    ):
        fail("NetKVM cleanup must cache its terminal result before any repeated provider operation")
    if cleanup.count("ParaNdis_RdmaPoolReleaseAllocations(pContext)") != 1 or cleanup.count(
        "ParaNdis_RdmaPoolDisconnect(pContext)"
    ) != 1:
        fail("NetKVM cleanup must have one normal/terminal rdmapool cleanup sequence")

    for owner, source in (
        ("ParaNdis6_Initialize", NETKVM_DRIVER_CODE),
        ("ParaNdis6_Halt", NETKVM_DRIVER_CODE),
    ):
        body = canonical_code(function_body(owner, source))
        cleanup_call = body.find("ParaNdis_CleanupContext(pContext);")
        destroy_call = body.find("pContext->Destroy(pContext,pContext->MiniportHandle);")
        if (
            cleanup_call < 0
            or destroy_call < 0
            or cleanup_call > destroy_call
            or body.count("ParaNdis_CleanupContext(pContext)") != 1
            or body.count("pContext->Destroy(pContext,pContext->MiniportHandle)") != 1
        ):
            fail(f"{owner} must complete NetKVM terminal cleanup before adapter destruction")


def check_adapter_lifecycle() -> None:
    adapter_header = canonical_code(VIOGPU_HEADER_CODE)
    for required in (
        "mutableEX_RUNDOWN_REFm_HardwareOperations;",
        "BOOLEANm_HardwareRundownCompleted;",
    ):
        if adapter_header.count(required) != 1:
            fail(f"DOD adapter must expose one retry-safe hardware rundown field: {required}")

    owner_checks = re.findall(r"\bm_RdmaPool\s*\.\s*HasArenaOwner\s*\(\s*\)", VIOGPU_CODE)
    if len(owner_checks) != 4:
        fail("adapter teardown and restart gates must check retained RDMA arena ownership exactly four times")
    if len(re.findall(r"\bm_RdmaPool\s*\.\s*IsActive\s*\(\s*\)", VIOGPU_CODE)) != 5:
        fail("adapter data paths must use RDMA readiness only in the five active-allocation decisions")

    start = function_body("VioGpuDod::StartDevice", VIOGPU_CODE)
    start_compact = canonical_code(start)
    active_guard = "if(IsDriverActive()){returnSTATUS_ALREADY_INITIALIZED;}"
    allocation = "m_pHWDevice=new(NonPagedPoolNx)VioGpuAdapter(this);"
    retained_blocks = [
        start_offset
        for condition, body, start_offset, _ in if_blocks(start)
        if canonical_code(condition) in ("m_pHWDevice!=NULL", "m_pHWDevice")
        and canonical_code(body) == "returnSTATUS_DEVICE_NOT_READY;"
    ]
    if start_compact.count(active_guard) != 1:
        fail("StartDevice must reject reentry while the retained adapter is still active")
    if len(retained_blocks) != 1 or start_compact.count(allocation) != 1:
        fail("StartDevice must reject a retained adapter before allocating its replacement")
    mode_reset = start_compact.find("RtlZeroMemory(&m_CurrentMode,sizeof(m_CurrentMode))")
    interface_copy = start_compact.find("RtlCopyMemory(&m_DxgkInterface")
    allocation_offset = start_compact.find(allocation)
    retained_offset = len(canonical_code(start[: retained_blocks[0]]))
    if min(mode_reset, interface_copy, allocation_offset) < 0 or not (
        start_compact.find(active_guard) < retained_offset < interface_copy < mode_reset < allocation_offset
    ):
        fail("StartDevice must reject retained ownership before replacing DXGK or mode state")


    failed_start_cleanup = re.findall(
        r"\bInterlockedExchange\s*\(\s*&m_HardwareResetRequested\s*,\s*TRUE\s*\)\s*;\s*"
        r"\bNTSTATUS\s+closeStatus\s*=\s*m_pHWDevice\s*->\s*HWClose\s*\(\s*\)\s*;"
        r"\s*if\s*\(\s*NT_SUCCESS\s*\(\s*closeStatus\s*\)\s*\)\s*\{"
        r"\s*delete\s+m_pHWDevice\s*;\s*m_pHWDevice\s*=\s*NULL\s*;\s*\}"
        r"\s*return\s+NT_SUCCESS\s*\(\s*closeStatus\s*\)\s*\?\s*"
        r"(?:Status|STATUS_UNSUCCESSFUL)\s*:\s*closeStatus\s*;",
        start,
        re.DOTALL,
    )
    if len(failed_start_cleanup) != 3 or len(re.findall(r"\bm_pHWDevice\s*->\s*HWClose\s*\(", start)) != 3:
        fail("every failed StartDevice unwind must gate ISR/DPC access and retain the adapter unless HWClose succeeds")
    if len(variable_write_offsets(start, "m_pHWDevice")) != 4 or len(
        re.findall(r"\bdelete\s+m_pHWDevice\s*;", start)
    ) != 3:
        fail("StartDevice must replace or delete its hardware adapter only in the checked ownership paths")

    constructor = canonical_code(function_body("VioGpuDod::VioGpuDod", VIOGPU_CODE))
    constructor_definition = re.findall(
        r"\bVioGpuDod\s*::\s*VioGpuDod\s*\([^{};]*\)\s*"
        r":\s*[^{};]*\bm_HardwareRundownCompleted\s*\(\s*FALSE\s*\)[^{};]*\{",
        VIOGPU_CODE,
        re.DOTALL,
    )
    initialize_rundown = constructor.find("ExInitializeRundownProtection(&m_HardwareOperations);")
    if len(constructor_definition) != 1 or constructor.count(
        "ExInitializeRundownProtection(&m_HardwareOperations);"
    ) != 1 or initialize_rundown < 0:
        fail("DOD constructor must initialize one open hardware rundown epoch")

    hardware_close = (
        "ExWaitForRundownProtectionRelease(&m_HardwareOperations);"
        "ExRundownCompleted(&m_HardwareOperations);"
        "m_HardwareRundownCompleted=TRUE;"
    )
    destructor_body = function_body("VioGpuDod::~VioGpuDod", VIOGPU_CODE)
    destructor = canonical_code(destructor_body)
    destructor_close_blocks = [
        canonical_code(body)
        for condition, body, _, _ in if_blocks(destructor_body)
        if canonical_code(condition) == "!m_HardwareRundownCompleted"
    ]
    delete_hardware = destructor.find("deletem_pHWDevice;")
    if (
        destructor_close_blocks != [hardware_close]
        or destructor.count("ExWaitForRundownProtectionRelease(&m_HardwareOperations)") != 1
        or destructor.count("ExRundownCompleted(&m_HardwareOperations)") != 1
        or delete_hardware < destructor.find(hardware_close)
    ):
        fail("DOD destructor must close an open hardware rundown exactly once before deletion")

    stop_body = function_body("VioGpuDod::StopDevice", VIOGPU_CODE)
    stop = canonical_code(stop_body)
    stop_close_blocks = [
        canonical_code(body)
        for condition, body, _, _ in if_blocks(stop_body)
        if canonical_code(condition) == "!m_HardwareRundownCompleted"
    ]
    hw_close = stop.find("status=m_pHWDevice->HWClose();")
    delete_hardware = stop.find("deletem_pHWDevice;", hw_close)
    clear_hardware = stop.find("m_pHWDevice=NULL;", delete_hardware)
    clear_started = stop.find("m_Flags.DriverStarted=FALSE;", clear_hardware)
    reinitialize = stop.find("ExReInitializeRundownProtection(&m_HardwareOperations);", clear_started)
    publish_open = stop.find("m_HardwareRundownCompleted=FALSE;", reinitialize)
    stop_stages = (stop.find(hardware_close), hw_close, delete_hardware, clear_hardware, clear_started, reinitialize, publish_open)
    if stop_close_blocks != [hardware_close] or min(stop_stages) < 0 or tuple(sorted(stop_stages)) != stop_stages:
        fail("StopDevice must close hardware once and reopen only after complete adapter teardown")
    reopen_blocks = [
        canonical_code(body)
        for condition, body, _, _ in if_blocks(stop_body)
        if is_success_condition(condition, "status")
        and "ExReInitializeRundownProtection(&m_HardwareOperations)" in canonical_code(body)
    ]
    expected_reopen = (
        "ExReInitializeRundownProtection(&m_HardwareOperations);"
        "m_HardwareRundownCompleted=FALSE;"
    )
    if reopen_blocks != [expected_reopen]:
        fail("StopDevice may publish an open hardware rundown only on complete teardown success")
    if (
        stop.count("ExWaitForRundownProtectionRelease(&m_HardwareOperations)") != 1
        or stop.count("ExRundownCompleted(&m_HardwareOperations)") != 1
        or stop.count("ExReInitializeRundownProtection(&m_HardwareOperations)") != 1
        or len(variable_write_offsets(stop_body, "m_HardwareRundownCompleted")) != 2
    ):
        fail("StopDevice must retain a completed rundown across failure and skip repeated waits")
    if len(variable_write_offsets(stop_body, "m_pHWDevice")) != 1 or stop.count("deletem_pHWDevice;") != 1:
        fail("StopDevice must delete and clear the hardware adapter only on the checked success path")

    post_display_body = function_body("VioGpuDod::StopDeviceAndReleasePostDisplayOwnership", VIOGPU_CODE)
    post_display = canonical_code(post_display_body)
    acquire_hardware = post_display.find("if(!ExAcquireRundownProtection(&m_HardwareOperations))")
    snapshot_hardware = post_display.find("VioGpuAdapter*adapter=m_pHWDevice;", acquire_hardware)
    reject_unavailable = post_display.find("if(IsHardwareResetRequested()||adapter==NULL)", snapshot_hardware)
    blackout = post_display.find("adapter->BlackOutScreen(&m_CurrentMode);", reject_unavailable)
    stop_device = post_display.find("returnStopDevice();", blackout)
    if min(acquire_hardware, snapshot_hardware, reject_unavailable, blackout, stop_device) < 0 or not (
        acquire_hardware < snapshot_hardware < reject_unavailable < blackout < stop_device
    ):
        fail("post-display ownership release must hold hardware rundown across screen blackout")
    reject_blocks = [
        canonical_code(body)
        for condition, body, _, _ in if_blocks(post_display_body)
        if canonical_code(condition) == "IsHardwareResetRequested()||adapter==NULL"
    ]
    if reject_blocks != [
        "ExReleaseRundownProtection(&m_HardwareOperations);returnSTATUS_DEVICE_NOT_READY;"
    ]:
        fail("post-display ownership release must balance hardware rundown when the adapter is unavailable")
    if (
        post_display.count("ExAcquireRundownProtection(&m_HardwareOperations)") != 1
        or post_display.count("ExReleaseRundownProtection(&m_HardwareOperations);") != 2
        or post_display.count("adapter->BlackOutScreen(&m_CurrentMode);") != 1
        or "m_pHWDevice->BlackOutScreen" in post_display
    ):
        fail("post-display ownership release must use one balanced protected hardware snapshot")

    wrapper_contracts = (
        (
            "VioGpuDod::QueryVidMmSegment",
            "returnFALSE;",
            "VioGpuAdapter*adapter=m_pHWDevice;"
            "BOOLEANavailable=!IsHardwareResetRequested()&&adapter!=NULL&&"
            "adapter->QueryVidMmSegment(baseAddress,physicalAddress,size);"
            "ExReleaseRundownProtection(&m_HardwareOperations);returnavailable;",
        ),
        (
            "VioGpuDod::QueryNativeContextReadiness",
            "returnFALSE;",
            "VioGpuAdapter*adapter=m_pHWDevice;"
            "BOOLEANready=!IsHardwareResetRequested()&&adapter!=NULL&&"
            "adapter->QueryNativeContextReadiness(capset,capsetVersion,capsetSize);"
            "ExReleaseRundownProtection(&m_HardwareOperations);returnready;",
        ),
        (
            "VioGpuDod::CreateNativeContext",
            "returnSTATUS_DEVICE_NOT_READY;",
            "VioGpuAdapter*adapter=m_pHWDevice;"
            "NTSTATUSstatus=!IsHardwareResetRequested()&&adapter!=NULL?"
            "adapter->CreateNativeContext(context):STATUS_DEVICE_NOT_READY;"
            "ExReleaseRundownProtection(&m_HardwareOperations);returnstatus;",
        ),
    )
    acquire_prefix = "if(!ExAcquireRundownProtection(&m_HardwareOperations)){"
    for wrapper_name, failure, protected_tail in wrapper_contracts:
        wrapper = canonical_code(function_body(wrapper_name, VIOGPU_CODE))
        expected = acquire_prefix + failure + "}" + protected_tail
        if wrapper != expected:
            fail(f"{wrapper_name} must hold hardware rundown across every m_pHWDevice use")

    destroy_wrapper = canonical_code(function_body("VioGpuDod::DestroyNativeContext", VIOGPU_CODE))
    expected_destroy_wrapper = (
        "if(released==NULL){returnSTATUS_INVALID_PARAMETER;}"
        "*released=FALSE;"
        + acquire_prefix
        + "returnSTATUS_DEVICE_NOT_READY;}"
        "VioGpuAdapter*adapter=m_pHWDevice;"
        "NTSTATUSstatus=!IsHardwareResetRequested()&&adapter!=NULL?"
        "adapter->DestroyNativeContext(context,released):STATUS_DEVICE_NOT_READY;"
        "ExReleaseRundownProtection(&m_HardwareOperations);returnstatus;"
    )
    if destroy_wrapper != expected_destroy_wrapper:
        fail("DestroyNativeContext must initialize release state and hold hardware rundown across adapter use")

    hw_close_body = function_body("VioGpuAdapter::HWClose", VIOGPU_CODE)
    hw_close = compact_code(hw_close_body)
    transport_stop = hw_close.find("NTSTATUSstatus=StopNativeContextTransport();")
    pci_close = hw_close.find("if(NT_SUCCESS(status)){status=m_PciResources.Close();}", transport_stop)
    close_return = hw_close.find("returnstatus;", pci_close)
    if min(transport_stop, pci_close, close_return) < 0 or not transport_stop < pci_close < close_return:
        fail("HWClose must close PCI resources only after transport teardown succeeds")
    if len(variable_write_offsets(hw_close_body, "status")) != 2:
        fail("HWClose must preserve transport failure until its success-gated PCI close")

    destructor = canonical_code(function_body("VioGpuAdapter::~VioGpuAdapter", VIOGPU_CODE))
    destructor_stop = destructor.find("NTSTATUSstatus=StopNativeContextTransport();")
    destructor_failure = destructor.find("if(!NT_SUCCESS(status)){DbgPrintEx(", destructor_stop)
    destructor_assert = destructor.find("NT_ASSERT(NT_SUCCESS(status));", destructor_failure)
    if min(destructor_stop, destructor_failure, destructor_assert) < 0 or not (
        destructor_stop < destructor_failure < destructor_assert
    ):
        fail("adapter destructor must execute transport teardown and consume failures in release builds")

    transport_close = function_body("VioGpuAdapter::StopNativeContextTransportLocked", VIOGPU_CODE)
    if method_call_offsets(transport_close, aliases_of(transport_close, "m_PciResources"), "Close"):
        fail("restartable D-state transport teardown must preserve PCI resources")

    power = function_body("VioGpuDod::SetPowerState", VIOGPU_CODE)
    power_compact = canonical_code(power)
    transition = "Status=m_pHWDevice->SetPowerState(&m_DeviceInfo,DevicePowerState,&m_CurrentMode);"
    publish_blocks = [
        (body, start_offset, end_offset)
        for condition, body, start_offset, end_offset in if_blocks(power)
        if is_success_condition(condition, "Status")
        and canonical_code(body) == "m_AdapterPowerState=DevicePowerState;"
    ]
    if power_compact.count(transition) != 1 or len(publish_blocks) != 1:
        fail("adapter power state must be published only after the hardware transition succeeds")
    publish_offset = len(canonical_code(power[: publish_blocks[0][1]]))
    if power_compact.find(transition) > publish_offset:
        fail("adapter power state publication must follow the hardware transition")
    publish_assignments = list(re.finditer(r"\bm_AdapterPowerState\s*=\s*DevicePowerState\s*;", power))
    if len(publish_assignments) != 1 or len(variable_write_offsets(power, "m_AdapterPowerState")) != 1:
        fail("adapter power state must have one success-gated publication")
    if re.search(r"&\s*m_AdapterPowerState\b", power):
        fail("adapter power state storage must not be exposed for publication through an alias")

    remove = function_body("VioGpuDodRemoveDevice", DOD_DRIVER_CODE)
    if len(re.findall(r"\bpVioGpuDod\s*->\s*StopDevice\s*\(\s*\)", remove)) != 1:
        fail("RemoveDevice must stop the adapter exactly once before deletion")
    if len(re.findall(r"\bdelete\s+pVioGpuDod\s*;", remove)) != 1:
        fail("RemoveDevice must delete the adapter exactly once")
    stop_call = re.search(r"\bNTSTATUS\s+status\s*=\s*pVioGpuDod\s*->\s*StopDevice\s*\(\s*\)\s*;", remove)
    remove_failure = [
        (body, start_offset, end_offset)
        for condition, body, start_offset, end_offset in if_blocks(remove)
        if is_failure_condition(condition, "status") and "returnstatus;" in canonical_code(body)
    ]
    delete_offset = re.search(r"\bdelete\s+pVioGpuDod\s*;", remove)
    if (
        stop_call is None
        or len(remove_failure) != 1
        or delete_offset is None
        or not stop_call.end() <= remove_failure[0][1] < remove_failure[0][2] < delete_offset.start()
    ):
        fail("RemoveDevice must retain the complete adapter when transport teardown fails")
    if re.search(r"\bdelete\b|->\s*~\s*VioGpuDod\s*\(", remove_failure[0][0]):
        fail("RemoveDevice failure path must not delete the retained adapter through any alias")
    if re.search(r"\bExFreePool(?:WithTag)?\s*\(", remove):
        fail("RemoveDevice must not bypass adapter destructors with pool deallocation")


def check_worker_thread_lifetime() -> None:
    start = canonical_code(function_body("VioGpuAdapter::StartWorkThread", VIOGPU_CODE))
    stop = canonical_code(function_body("VioGpuAdapter::StopWorkThread", VIOGPU_CODE))

    retained_handle = "m_WorkThreadHandle=threadHandle;"
    typed_start_reference = (
        "status=ObReferenceObjectByHandle(threadHandle,SYNCHRONIZE,*PsThreadType,KernelMode,"
        "reinterpret_cast<PVOID*>(&workThread),NULL);"
    )
    reference_failure = (
        "if(!NT_SUCCESS(status)){m_bStopWorkThread=TRUE;"
        "KeSetEvent(&m_ConfigUpdateEvent,IO_NO_INCREMENT,FALSE);"
    )
    retained_offset = start.find(retained_handle)
    reference_offset = start.find(typed_start_reference, retained_offset)
    failure_offset = start.find(reference_failure, reference_offset)
    if start.count(retained_handle) != 1 or min(retained_offset, reference_offset, failure_offset) < 0:
        fail("worker start must retain its handle and acquire a typed thread-object reference")

    typed_fallback_reference = (
        "if(m_pWorkThread==NULL){PETHREADworkThread=NULL;"
        "status=ObReferenceObjectByHandle(m_WorkThreadHandle,SYNCHRONIZE,*PsThreadType,KernelMode,"
        "reinterpret_cast<PVOID*>(&workThread),NULL);"
    )
    publish_reference = "m_pWorkThread=workThread;"
    object_wait = (
        "status=KeWaitForSingleObject(m_pWorkThread,Executive,KernelMode,FALSE,&timeout);"
    )
    failure_return = "if(status!=STATUS_SUCCESS){"
    dereference = "ObDereferenceObject(m_pWorkThread);"
    handle_close = "ZwClose(m_WorkThreadHandle);"
    fallback_offset = stop.find(typed_fallback_reference)
    publish_offset = stop.find(publish_reference, fallback_offset)
    object_wait_offset = stop.find(object_wait, publish_offset)
    failure_offset = stop.find(failure_return, object_wait_offset)
    dereference_offset = stop.find(dereference, failure_offset)
    close_offset = stop.find(handle_close, dereference_offset)
    if min(fallback_offset, publish_offset, object_wait_offset, failure_offset, dereference_offset, close_offset) < 0 or not (
        fallback_offset < publish_offset < object_wait_offset < failure_offset < dereference_offset < close_offset
    ):
        fail("worker stop must retain a typed thread reference through termination before releasing owners")
    if "ZwWaitForSingleObject" in VIOGPU_CODE or "m_WorkThreadExited" in VIOGPU_SOURCE + VIOGPU_HEADER_SOURCE:
        fail("worker teardown must wait on the thread dispatcher object, not a handle or pre-termination event")


def check_project_safety(root: ET.Element) -> None:
    definitions = [
        token.strip()
        for element in root.findall(".//msbuild:PreprocessorDefinitions", NAMESPACE)
        for token in (element.text or "").split(";")
        if token.strip()
    ]
    for required in (
        "VIOGPU_WDDM_CI_ONLY=1",
        "VIOGPU_EXTERNAL_DRIVER_ENTRY=1",
    ):
        if definitions.count(required) != 1:
            fail(f"project must define {required} exactly once")

    expected_interface = "DXGKDDI_INTERFACE_VERSION=DXGKDDI_INTERFACE_VERSION_WIN8"
    interface_definitions = [
        definition for definition in definitions if definition.startswith("DXGKDDI_INTERFACE_VERSION=")
    ]
    if interface_definitions != [expected_interface]:
        fail(f"project must fix the interface version only as {expected_interface}")

    static_asserts = re.findall(
        r"\bstatic_assert\s*\(\s*DXGKDDI_INTERFACE_VERSION\s*==\s*"
        r"DXGKDDI_INTERFACE_VERSION_WIN8\s*,",
        DRIVER_CODE,
    )
    if len(static_asserts) != 1:
        fail("driver_entry.cpp must assert the Win8/WDDM 1.2 interface exactly once")

    sign_modes = [
        (element.text or "").strip() for element in root.findall(".//msbuild:SignMode", NAMESPACE)
    ]
    if not sign_modes or any(sign_mode != "Off" for sign_mode in sign_modes):
        fail(f"compile-only project must set every SignMode to Off; found: {sign_modes or ['none']}")

    optimize_references = [
        (element.text or "").strip()
        for element in root.findall(".//msbuild:Link/msbuild:OptimizeReferences", NAMESPACE)
    ]
    if optimize_references != ["false"]:
        fail("compile-only project must disable reference optimization so the unreachable helper is linked")

    forced_symbols = [
        (element.text or "").strip()
        for element in root.findall(".//msbuild:Link/msbuild:ForceSymbolReferences", NAMESPACE)
    ]
    if forced_symbols != [REGISTRATION_HELPER]:
        fail("compile-only project must force-link only the unreachable registration helper")

    generate_map_files = [
        (element.text or "").strip()
        for element in root.findall(".//msbuild:Link/msbuild:GenerateMapFile", NAMESPACE)
    ]
    if generate_map_files != ["true"]:
        fail("compile-only project must generate one linker map for retention evidence")

    map_file_names = [
        (element.text or "").strip()
        for element in root.findall(".//msbuild:Link/msbuild:MapFileName", NAMESPACE)
    ]
    if map_file_names != [r"$(OutDir)$(TargetName).map"]:
        fail("compile-only project must emit its linker map beside the compile-only driver")

    driver_items = [
        element
        for element in root.findall(".//msbuild:ClCompile[@Include]", NAMESPACE)
        if element.attrib["Include"].replace("\\", "/").endswith("/viogpudo/driver.cpp")
    ]
    if len(driver_items) != 1:
        fail("compile-only project must contain exactly one inherited viogpudo driver.cpp input")

    non_owner_templates = driver_items[0].findall(
        "msbuild:WppGenerateUsingTemplateFile", NAMESPACE
    )
    expected_template = r"{$(MSBuildProjectDirectory)\wpp-non-owner.tpl}*.tmh"
    if [element.text for element in non_owner_templates] != [expected_template]:
        fail("inherited driver.cpp must use the project-local WPP non-owner template")

    if not WPP_NON_OWNER_TEMPLATE.is_file():
        fail("project-local WPP non-owner template is missing")
    expected_template_source = (
        "`INCLUDE km-header.tpl`\n"
        "`INCLUDE control.tpl`\n"
        "`INCLUDE tracemacro.tpl`\n"
    )
    if WPP_NON_OWNER_TEMPLATE.read_text(encoding="utf-8") != expected_template_source:
        fail("WPP non-owner template must contain only declarations, control data, and trace macros")

    template_inputs = [
        element.attrib.get("Include", "").replace("\\", "/")
        for element in root.findall(".//msbuild:None[@Include]", NAMESPACE)
    ]
    if template_inputs.count("wpp-non-owner.tpl") != 1:
        fail("compile-only project must track the WPP non-owner template exactly once")

    inputs = [element.attrib.get("Include", "").lower() for element in root.iter()]
    if any(path.endswith((".inf", ".inx")) for path in inputs):
        fail("compile-only project must not contain INF or INX inputs")


def main() -> None:
    root = ET.parse(PROJECT).getroot()
    sources = project_compile_sources(root)
    check_driver_entry_gate()
    check_registration_helper(sources)
    check_callback_table()
    check_virtio_reset_contract()
    check_virtio_queue_allocation_cleanup()
    check_dod_reset_entrypoints()
    check_native_context_readiness()
    check_no_retired_variant_contract(sources)
    check_queue_failure_semantics()
    check_native_context_ownership()
    check_wddm_context_lifetime()
    check_dpc_completion_semantics()
    check_segment_failure_semantics()
    check_pci_resource_lifetime()
    check_rdma_ioctl_lifetime()
    check_shared_rdma_clients()
    check_netkvm_terminal_cleanup()
    check_storport_restricted_dma_policy()
    check_adapter_lifecycle()
    check_worker_thread_lifetime()
    check_project_safety(root)
    print("viogpuwddm compile-only safety contract: PASS")


if __name__ == "__main__":
    main()
