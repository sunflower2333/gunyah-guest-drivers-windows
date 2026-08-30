#!/usr/bin/env python3

import os
import re
import subprocess
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
RESOURCE_HEADER_PATH = (PROJECT_DIR.parent / "common" / "viogpu.h").resolve()
IDR_SOURCE_PATH = (PROJECT_DIR.parent / "common" / "viogpu_idr.cpp").resolve()
IDR_HEADER_PATH = (PROJECT_DIR.parent / "common" / "viogpu_idr.h").resolve()
WDDM_ABI_HEADER_PATH = (PROJECT_DIR.parent / "shared" / "viogpu_wddm_abi.h").resolve()
WDDM_ABI_FIXTURE_DIR = (PROJECT_DIR.parent / "tests" / "wddm-private-abi").resolve()
QUEUE_SOURCE_PATH = (PROJECT_DIR.parent / "common" / "viogpu_queue.cpp").resolve()
PCI_SOURCE_PATH = (PROJECT_DIR.parent / "common" / "viogpu_pci.cpp").resolve()
PCI_HEADER_PATH = (PROJECT_DIR.parent / "common" / "viogpu_pci.h").resolve()
VIRTIO_DIR = (PROJECT_DIR.parent.parent / "VirtIO").resolve()
VIRTIO_HEADER_PATH = VIRTIO_DIR / "virtio_pci.h"
VIRTIO_COMMON_PATH = VIRTIO_DIR / "VirtIOPCICommon.c"
VIRTIO_MODERN_PATH = VIRTIO_DIR / "VirtIOPCIModern.c"
VIRTIO_LEGACY_PATH = VIRTIO_DIR / "VirtIOPCILegacy.c"
PROJECT = PROJECT_DIR / "viogpuwddm.vcxproj"
INF_TEMPLATE = PROJECT_DIR / "viogpuwddm.inx"
WPP_NON_OWNER_TEMPLATE = PROJECT_DIR / "wpp-non-owner.tpl"
UMD_PROJECT_DIR = (PROJECT_DIR.parent / "viogpud3d").resolve()
UMD_PROJECT = UMD_PROJECT_DIR / "viogpud3d.vcxproj"
UMD_SOURCE_PATH = UMD_PROJECT_DIR / "viogpud3d.cpp"
UMD_DEF_PATH = UMD_PROJECT_DIR / "viogpud3d.def"
UMD_HEADER_PATH = UMD_PROJECT_DIR / "d3dumddi_compat.h"
NAMESPACE = {"msbuild": "http://schemas.microsoft.com/developer/msbuild/2003"}
REGISTRATION_HELPER = "VioGpuWddmInitializeMiniport"
WORKFLOW_PATH = (PROJECT_DIR.parent.parent / ".github" / "workflows" / "viogpuwddm-arm64-ci.yml").resolve()
PRODUCT_WORKFLOW_PATH = (PROJECT_DIR.parent.parent / ".github" / "workflows" / "build-arm64-drivers.yml").resolve()
WINDOWS_KIT_SCRIPT_PATH = (PROJECT_DIR.parent.parent / ".github" / "scripts" / "locate-windows-kit.ps1").resolve()
START_DIAGNOSTIC_SCRIPT_PATH = (
    PROJECT_DIR.parent.parent / ".install_scripts" / "viogpu-native-start-diagnostics.ps1"
).resolve()
PRESENT_DIAGNOSTIC_SCRIPT_PATH = (
    PROJECT_DIR.parent.parent / ".install_scripts" / "viogpu-native-present-diagnostics.ps1"
).resolve()
PRESENT_DIAGNOSTIC_TEST_PATH = (
    PROJECT_DIR.parent.parent / ".install_scripts" / "test-viogpu-native-present-diagnostics.ps1"
).resolve()


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
PCI_SOURCE = PCI_SOURCE_PATH.read_text(encoding="utf-8")
PCI_HEADER_SOURCE = PCI_HEADER_PATH.read_text(encoding="utf-8")
VIRTIO_HEADER_SOURCE = VIRTIO_HEADER_PATH.read_text(encoding="utf-8")
VIRTIO_COMMON_SOURCE = VIRTIO_COMMON_PATH.read_text(encoding="utf-8")
VIRTIO_MODERN_SOURCE = VIRTIO_MODERN_PATH.read_text(encoding="utf-8")
VIRTIO_LEGACY_SOURCE = VIRTIO_LEGACY_PATH.read_text(encoding="utf-8")
VIOGPU_CODE = strip_cpp_comments_and_literals(VIOGPU_SOURCE)
VIOGPU_HEADER_CODE = strip_cpp_comments_and_literals(VIOGPU_HEADER_SOURCE)
DOD_DRIVER_CODE = strip_cpp_comments_and_literals(DOD_DRIVER_SOURCE)
WIRE_HEADER_CODE = strip_cpp_comments_and_literals(WIRE_HEADER_PATH.read_text(encoding="utf-8"))
RESOURCE_HEADER_CODE = strip_cpp_comments_and_literals(RESOURCE_HEADER_PATH.read_text(encoding="utf-8"))
IDR_CODE = strip_cpp_comments_and_literals(IDR_SOURCE_PATH.read_text(encoding="utf-8"))
IDR_HEADER_CODE = strip_cpp_comments_and_literals(IDR_HEADER_PATH.read_text(encoding="utf-8"))
WDDM_ABI_HEADER_SOURCE = WDDM_ABI_HEADER_PATH.read_text(encoding="utf-8")
WDDM_ABI_HEADER_CODE = strip_cpp_comments_and_literals(WDDM_ABI_HEADER_SOURCE)
QUEUE_HEADER_CODE = strip_cpp_comments_and_literals(QUEUE_HEADER_SOURCE)
QUEUE_CODE = strip_cpp_comments_and_literals(QUEUE_SOURCE_PATH.read_text(encoding="utf-8"))
PCI_CODE = strip_cpp_comments_and_literals(PCI_SOURCE)
PCI_HEADER_CODE = strip_cpp_comments_and_literals(PCI_HEADER_SOURCE)
VIRTIO_HEADER_CODE = strip_cpp_comments_and_literals(VIRTIO_HEADER_SOURCE)
VIRTIO_COMMON_CODE = strip_cpp_comments_and_literals(VIRTIO_COMMON_SOURCE)
VIRTIO_MODERN_CODE = strip_cpp_comments_and_literals(VIRTIO_MODERN_SOURCE)
VIRTIO_LEGACY_CODE = strip_cpp_comments_and_literals(VIRTIO_LEGACY_SOURCE)


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
    code = re.sub(r"(?<![A-Za-z0-9_])(0[xX][0-9A-Fa-f]+|[0-9]+)[uU]\b", r"\1", code)
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


def else_block_after(source: str, if_end: int) -> Optional[str]:
    match = re.match(r"\s*else\s*", source[if_end:])
    if match is None:
        return None
    body_start = if_end + match.end()
    if body_start >= len(source) or source[body_start] != "{":
        return None

    depth = 0
    for offset in range(body_start, len(source)):
        if source[offset] == "{":
            depth += 1
        elif source[offset] == "}":
            depth -= 1
            if depth == 0:
                return source[body_start + 1 : offset]
    return None


def require_order(code: str, fragments: tuple[str, ...], message: str) -> None:
    offsets = [code.find(fragment) for fragment in fragments]
    if min(offsets) < 0 or offsets != sorted(offsets):
        fail(message)


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
    reset_state = canonical_code(VIOGPU_HEADER_CODE)
    reset_state_contract = (
        "enumVIOGPU_HARDWARE_RESET_STATE:LONG{"
        "VioGpuHardwareActive=0,VioGpuHardwareResetRequested,VioGpuHardwareRecovering,};"
    )
    if reset_state.count(reset_state_contract) != 1:
        fail("DOD reset state must expose exactly Active, ResetRequested, and Recovering")

    dpc = canonical_code(function_body("VioGpuDod::DpcRoutine", VIOGPU_CODE))
    dpc_acquire = dpc.find("if(ExAcquireRundownProtection(&m_HardwareOperations))")
    dpc_adapter = dpc.find("VioGpuAdapter*adapter=m_pHWDevice;", dpc_acquire)
    dpc_call = dpc.find("adapter->DpcRoutine(&m_DxgkInterface);", dpc_adapter)
    dpc_release = dpc.find("ExReleaseRundownProtection(&m_HardwareOperations);", dpc_call)
    dpc_software = dpc.find("if(!DrainNativeSoftwareSubmissionCompletionsFromDpc())", dpc_release)
    dpc_reset = dpc.find("RequestHardwareResetAtAnyIrql();", dpc_software)
    dpc_notify = dpc.find("m_DxgkInterface.DxgkCbNotifyDpc((HANDLE)m_DxgkInterface.DeviceHandle);", dpc_reset)
    if min(dpc_acquire, dpc_adapter, dpc_call, dpc_release, dpc_notify) < 0 or not (
        dpc_acquire < dpc_adapter < dpc_call < dpc_release < dpc_software < dpc_reset < dpc_notify
    ):
        fail("DpcRoutine must defer software fence retirement until after hardware dispatch and before notifying DxgK")
    if (
        dpc.count("ExAcquireRundownProtection(&m_HardwareOperations)") != 1
        or dpc.count("ExReleaseRundownProtection(&m_HardwareOperations);") != 1
        or dpc.count("adapter->DpcRoutine(&m_DxgkInterface);") != 1
        or "m_pHWDevice->DpcRoutine" in dpc
        or dpc.count("IsHardwareInterruptDispatchAllowed()") != 1
    ):
        fail("DpcRoutine must use one balanced protected hardware snapshot during active or recovery dispatch")

    interrupt = canonical_code(function_body("VioGpuDod::InterruptRoutine", VIOGPU_CODE))
    expected_interrupt_tail = (
        "VioGpuAdapter*adapter=m_pHWDevice;"
        "returnIsHardwareInterruptDispatchAllowed()&&adapter!=NULL?"
        "adapter->InterruptRoutine(&m_DxgkInterface,MessageNumber):FALSE;"
    )
    if not interrupt.endswith(expected_interrupt_tail) or interrupt.count("m_pHWDevice") != 1:
        fail("ISR wrapper must use one raw adapter snapshot only while active or recovering")

    reset = canonical_code(function_body("VioGpuDod::ResetDevice", VIOGPU_CODE))
    reset_gate = "InterlockedExchange(&m_HardwareResetState,VioGpuHardwareResetRequested);"
    reset_acquire = "ExAcquireRundownProtection(&m_HardwareOperations)"
    reset_release = "ExReleaseRundownProtection(&m_HardwareOperations);"
    reset_adapter = "VioGpuAdapter*adapter=m_pHWDevice;"
    reset_gate_offsets = [match.start() for match in re.finditer(re.escape(reset_gate), reset)]
    if (
        len(reset_gate_offsets) != 2
        or reset.count(reset_acquire) != 1
        or reset.count(reset_release) != 1
    ):
        fail("ResetDevice must publish before and after reset work and balance hardware rundown protection")
    reset_acquire_gate = (
        "if(KeGetCurrentIrql()<=DISPATCH_LEVEL&&"
        "ExAcquireRundownProtection(&m_HardwareOperations)){"
    )
    if reset.count(reset_acquire_gate) != 1:
        fail("ResetDevice must acquire nonpaged hardware rundown only at or below DISPATCH_LEVEL")
    reset_stages = (
        (reset_gate_offsets[0], "initial reset gate publication"),
        (reset.find(reset_acquire_gate), "IRQL and rundown gate"),
        (reset.find(reset_acquire), "hardware rundown acquire"),
        (reset.find(reset_adapter), "adapter snapshot"),
        (reset.find("adapter->ResetDevice();"), "adapter reset"),
        (reset_gate_offsets[1], "post-reset gate publication"),
        (reset.find(reset_release), "hardware rundown release"),
    )
    for (offset, description), (next_offset, next_description) in zip(reset_stages, reset_stages[1:]):
        if offset < 0 or next_offset < 0 or offset > next_offset:
            fail(f"ResetDevice must perform {description} before {next_description}")

    display = canonical_code(function_body("VioGpuDod::SystemDisplayEnable", VIOGPU_CODE))
    display_gate = "InterlockedExchange(&m_HardwareResetState,VioGpuHardwareResetRequested);"
    display_acquire = "ExAcquireRundownProtection(&m_HardwareOperations)"
    display_release = "ExReleaseRundownProtection(&m_HardwareOperations);"
    display_adapter = "VioGpuAdapter*adapter=m_pHWDevice;"
    display_gate_offsets = [match.start() for match in re.finditer(re.escape(display_gate), display)]
    if (
        len(display_gate_offsets) != 2
        or display.count(display_acquire) != 1
        or display.count(display_release) != 1
    ):
        fail("SystemDisplayEnable must publish before and after reset work and balance hardware rundown protection")
    if "KeGetCurrentIrql()!=PASSIVE_LEVEL" not in display:
        fail("SystemDisplayEnable must require PASSIVE_LEVEL before its hardware rundown acquire")
    first_return = display.find("return")
    if first_return >= 0 and display.find(display_gate) > first_return:
        fail("SystemDisplayEnable must publish its reset gate before every early return")
    display_stages = (
        (display_gate_offsets[0], "initial reset gate publication"),
        (display.find("if(KeGetCurrentIrql()!=PASSIVE_LEVEL)"), "IRQL gate"),
        (display.find(display_acquire), "hardware rundown acquire"),
        (display.find(display_adapter), "adapter snapshot"),
        (display.find("adapter->ResetToVgaMode()"), "VGA reset"),
        (display_gate_offsets[1], "post-reset gate publication"),
        (display.find(display_release), "hardware rundown release"),
    )
    for (offset, description), (next_offset, next_description) in zip(display_stages, display_stages[1:]):
        if offset < 0 or next_offset < 0 or offset > next_offset:
            fail(f"SystemDisplayEnable must perform {description} before {next_description}")


def check_adapter_line_interrupt_bitmap() -> None:
    """Require one read-and-ack and independent decoding of both line ISR bits."""
    require_integer_define(
        VIRTIO_HEADER_SOURCE,
        "VIRTIO_PCI_ISR_CONFIG",
        2,
        "VirtIO PCI ISR bitmap",
    )
    interrupt = canonical_code(function_body("VioGpuAdapter::InterruptRoutine", VIOGPU_CODE))
    reason_init = "ULONGintReason=0;"
    line_decode = (
        "UNREFERENCED_PARAMETER(MessageNumber);"
        "UCHARisrstat=virtio_read_isr_status(&m_VioDev);"
        "if((isrstat&1)!=0){intReason|=ISR_REASON_DISPLAY|ISR_REASON_CURSOR;}"
        "if((isrstat&VIRTIO_PCI_ISR_CONFIG)!=0){intReason|=ISR_REASON_CHANGE;}}"
    )
    serviced = "BOOLEANserviced=intReason!=0;"
    publish = "if(serviced){"
    returned = "returnserviced;"

    reason_offset = interrupt.find(reason_init)
    line_offset = interrupt.find(line_decode, reason_offset)
    serviced_offset = interrupt.find(serviced, line_offset)
    publish_offset = interrupt.find(publish, serviced_offset)
    return_offset = interrupt.rfind(returned)
    if (
        min(reason_offset, line_offset, serviced_offset, publish_offset, return_offset) < 0
        or not (reason_offset < line_offset < serviced_offset < publish_offset < return_offset)
        or serviced_offset != line_offset + len(line_decode)
    ):
        fail("line ISR must independently accumulate queue and configuration bits before deciding service")
    if (
        interrupt.count("virtio_read_isr_status(&m_VioDev)") != 1
        or interrupt.count(line_decode) != 1
        or interrupt.count(serviced) != 1
        or interrupt.count(publish) != 1
        or interrupt.count(returned) != 1
        or "switch(isrstat)" in interrupt
    ):
        fail("line ISR must read-and-ack once, decode a bitmap, and reject zero or unknown-only status")


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
        "return VioGpuWddmInitializeMiniport(driverObject, registryPath);"
    )
    if normalized != expected:
        fail("DriverEntry must contain only the exact full-miniport registration call")


def check_viogpudo_code_segment_contract() -> None:
    # MSVC treats code_seg() with no argument as a switch back to the default
    # .text section.  code_seg(push) alone only saves the surrounding PAGE
    # section, so both directives are required for these DISPATCH/DPC paths
    # and for readiness code that executes while holding a spin lock.
    push_default_pattern = re.compile(
        r"(?m)^[ \t]*#pragma[ \t]+code_seg[ \t]*\([ \t]*push[ \t]*\)[ \t]*$\r?\n"
        r"[ \t]*#pragma[ \t]+code_seg[ \t]*\([ \t]*\)[ \t]*$"
    )
    empty_default_pattern = re.compile(
        r"(?m)^[ \t]*#pragma[ \t]+code_seg[ \t]*\([ \t]*\)[ \t]*$"
    )
    pop_pattern = re.compile(
        r"(?m)^[ \t]*#pragma[ \t]+code_seg[ \t]*\([ \t]*pop[ \t]*\)[^\r\n]*$"
    )
    ranges = list(push_default_pattern.finditer(VIOGPU_SOURCE))
    if len(ranges) != 4 or len(list(empty_default_pattern.finditer(VIOGPU_SOURCE))) != 4:
        fail("viogpudo must declare exactly four explicit default-.text nonpaged code ranges")

    first_open, dod_readiness_open, dpc_open, adapter_readiness_open = ranges
    first_close = pop_pattern.search(VIOGPU_SOURCE, first_open.end())
    dod_readiness_close = pop_pattern.search(VIOGPU_SOURCE, dod_readiness_open.end())
    dpc_close = pop_pattern.search(VIOGPU_SOURCE, dpc_open.end())
    adapter_readiness_close = pop_pattern.search(VIOGPU_SOURCE, adapter_readiness_open.end())
    if (
        first_close is None
        or dod_readiness_close is None
        or dpc_close is None
        or adapter_readiness_close is None
        or not (
            first_close.start()
            < dod_readiness_open.start()
            < dod_readiness_close.start()
            < dpc_open.start()
            < dpc_close.start()
            < adapter_readiness_open.start()
            < adapter_readiness_close.start()
        )
    ):
        fail("viogpudo default-.text ranges must each close with the next code_seg(pop)")

    first_declaration = VIOGPU_CODE.find("PGPU_VBUFFER VioGpuDod::PrepareNativeSubmit", first_open.end())
    _, _, first_last_end = function_body_span("VioGpuDod::AllocateNativeResourceId", VIOGPU_CODE)
    if (
        first_declaration < 0
        or not first_open.end() <= first_declaration < first_last_end < first_close.start()
        or VIOGPU_CODE[first_open.end() : first_declaration].strip()
        or VIOGPU_CODE[first_last_end + 1 : first_close.start()].strip()
    ):
        fail("the WDDM Native Context bridge must occupy the complete first default-.text range")
    for function_name in (
        "VioGpuDod::PrepareNativeSubmit",
        "VioGpuDod::RefreshNativeSubmit",
        "VioGpuDod::QueueNativeSubmit",
        "VioGpuDod::ReleaseNativeSubmitBuffer",
        "VioGpuDod::NotifyNativeSchedulerInterrupt",
        "VioGpuDod::QueueNativePassiveWork",
        "VioGpuDod::RequestWddmSubmissionDrainAtAnyIrql",
        "VioGpuDod::NativePassiveWorker",
        "VioGpuDod::AllocateNativeResourceId",
    ):
        _, function_start, function_end = function_body_span(function_name, VIOGPU_CODE)
        if not first_open.end() < function_start < function_end < first_close.start():
            fail(f"WDDM Native Context bridge routine must remain in default .text: {function_name}")

    readiness_annotation = "_IRQL_requires_max_(DISPATCH_LEVEL)"
    annotated_declaration = readiness_annotation + "BOOLEANQueryNativeContextReadiness("
    if canonical_code(VIOGPU_HEADER_CODE).count(annotated_declaration) != 2:
        fail("both Native Context readiness declarations must permit at most DISPATCH_LEVEL")
    for function_name, range_open, range_close in (
        ("VioGpuDod::QueryNativeContextReadiness", dod_readiness_open, dod_readiness_close),
        ("VioGpuAdapter::QueryNativeContextReadiness", adapter_readiness_open, adapter_readiness_close),
    ):
        _, function_start, function_end = function_body_span(function_name, VIOGPU_CODE)
        declaration_start = VIOGPU_CODE.find(f"BOOLEAN {function_name}", range_open.end(), function_start)
        if (
            declaration_start < 0
            or not range_open.end() < declaration_start < function_start < function_end < range_close.start()
            or canonical_code(VIOGPU_CODE[range_open.end() : declaration_start]) != readiness_annotation
            or VIOGPU_CODE[function_end + 1 : range_close.start()].strip()
        ):
            fail(f"{function_name} must exclusively occupy an annotated default-.text range")

    dpc_declaration = VIOGPU_CODE.find("static ULONG VioGpuReadSharedU32", dpc_open.end())
    _, _, dpc_last_end = function_body_span("VioGpuDod::SystemDisplayWrite", VIOGPU_CODE)
    dpc_prefix = VIOGPU_CODE[dpc_open.end() : dpc_declaration]
    if (
        dpc_declaration < 0
        or compact_code(dpc_prefix) != "#ifdefined(VIOGPU_NATIVE_CONTEXT)"
        or not dpc_open.end() < dpc_declaration < dpc_last_end < dpc_close.start()
        or VIOGPU_CODE[dpc_last_end + 1 : dpc_close.start()].strip()
    ):
        fail("the DPC/ISR bridge must occupy the complete second default-.text range")
    for function_name in (
        "VioGpuAdapter::FailNativeContextAtAnyIrql",
        "VioGpuDod::DpcRoutine",
        "VioGpuDod::InterruptRoutine",
        "VioGpuDod::ResetDevice",
        "VioGpuDod::SystemDisplayEnable",
        "VioGpuDod::SystemDisplayWrite",
    ):
        _, function_start, function_end = function_body_span(function_name, VIOGPU_CODE)
        if not dpc_open.end() < function_start < function_end < dpc_close.start():
            fail(f"DPC/ISR bridge routine must remain in default .text: {function_name}")


def check_arm64_workflow_contract() -> None:
    workflows = {
        "Native Context full-miniport": WORKFLOW_PATH,
        "product drivers": PRODUCT_WORKFLOW_PATH,
    }
    sources: dict[str, str] = {}
    if not WINDOWS_KIT_SCRIPT_PATH.is_file():
        fail(f"missing shared Windows SDK/WDK locator: {WINDOWS_KIT_SCRIPT_PATH}")
    kit_script = WINDOWS_KIT_SCRIPT_PATH.read_text(encoding="utf-8")
    required_toolchain_fragments = (
        "runs-on: windows-11-arm",
        "Locate preinstalled Windows SDK and WDK",
        ".github/scripts/locate-windows-kit.ps1",
    )
    for label, path in workflows.items():
        if not path.is_file():
            fail(f"missing {label} ARM64 workflow: {path}")
        source = path.read_text(encoding="utf-8")
        sources[label] = source
        for fragment in required_toolchain_fragments:
            if source.count(fragment) != 1:
                fail(
                    f"{label} workflow must discover and verify one preinstalled ARM64 "
                    f"Windows SDK/WDK: {fragment}"
                )

        if re.search(r"winget\s+install\s+--id\s+Microsoft\.Windows(?:SDK|WDK)", source, re.I):
            fail(f"{label} workflow must not install the runner's preinstalled Windows SDK/WDK")

        if r"Tools\x64\infverif.exe" in source:
            fail(f"{label} workflow must discover the versioned InfVerif.exe path")

        if source.count("& $infverif /w /v") != 1:
            fail(f"{label} workflow must run InfVerif /w /v exactly once on the Native Context INF")

    required_locator_fragments = (
        "Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\\10'",
        "Join-Path ${env:ProgramFiles} 'Windows Kits\\10'",
        "$env:KitsRoot10",
        "Get-ChildItem -LiteralPath $includeRoot -Directory",
        "[version]$include.Name",
        "'km\\ntddk.h'",
        "'ntoskrnl.lib'",
        r"'\\km\\arm64\\'",
        "Sort-Object Version -Descending",
        "DROIDVM_KIT_VERSION",
        "INFVERIF_PATH",
    )
    for fragment in required_locator_fragments:
        if kit_script.count(fragment) < 1:
            fail(f"shared Windows SDK/WDK locator must contain {fragment!r}")
    if kit_script.count("'tracewpp.exe'") < 1:
        fail("shared Windows SDK/WDK locator must probe tracewpp in the selected version root")
    if kit_script.count("'InfVerif.exe'") < 2:
        fail("shared Windows SDK/WDK locator must probe InfVerif in both versioned and Tools roots")
    if "winget install" in kit_script.lower() or "Invoke-WebRequest" in kit_script:
        fail("shared Windows SDK/WDK locator must not install or download a kit")
    if kit_script.count("RequirePackagingTools") < 1:
        fail("shared Windows SDK/WDK locator must expose one packaging-tools switch")

    contract_platforms = re.findall(r"\bPlatform\s*=\s*'([^']+)'", sources["Native Context full-miniport"])
    product_platforms = re.findall(r"\bplat\s*=\s*'([^']+)'", sources["product drivers"])
    if not contract_platforms or set(contract_platforms) != {"ARM64"}:
        fail(f"full-miniport workflow driver projects must all target ARM64: {contract_platforms or ['none']}")
    if not product_platforms or set(product_platforms) != {"ARM64"}:
        fail(f"product workflow driver projects must all target ARM64: {product_platforms or ['none']}")
    if sources["Native Context full-miniport"].count("/p:Platform=ARM64") != 3:
        fail("the full WDDM contract, UMD, and opt-in test targets must be built explicitly for ARM64")
    experimental_workflow = sources["Native Context full-miniport"]
    if experimental_workflow.count("Compile opt-in WDDM test implementations") != 1:
        fail("the full WDDM workflow must compile the opt-in test implementations exactly once")
    if experimental_workflow.count("/p:VIOGPU_WDDM_TEST_IMPLEMENTATIONS=1") != 2:
        fail("the opt-in WDDM workflow must compile both guarded UMD and KMD implementations")
    for fragment in (
        "/p:OutDir=\"$testOutDir\"",
        "/p:IntDir=\"$testOutDir\"",
        "$testUmdOutDir = [IO.Path]::GetFullPath('viogpu/viogpuwddm/objtest_umd_win11_arm64/arm64/')",
        "objtest_umd_win11_arm64/arm64/",
        "$testUmd = Join-Path $testUmdOutDir 'viogpud3d.dll'",
        "experimental UMD build did not produce",
        "Copy-Item -LiteralPath $testUmd -Destination (Join-Path $testOutDir 'viogpud3d.dll') -Force",
        "$testOutDir = [IO.Path]::GetFullPath('viogpu/viogpuwddm/objtest_win11_arm64/arm64/')",
        "objtest_win11_arm64/arm64/",
        "experimental WDDM build did not produce",
        "experimental WDDM output is missing the matching UMD",
    ):
        if experimental_workflow.count(fragment) != 1:
            fail(f"the opt-in WDDM workflow must isolate and verify its test output: {fragment}")
    for label, source in sources.items():
        if source.count("$reader.ReadUInt16() -ne 0xaa64") != 1:
            fail(f"{label} workflow must verify both Native Context PEs as ARM64")
        if source.count("$expectedExports = @('OpenAdapter', 'OpenAdapter10', 'OpenAdapter10_2')") != 1:
            fail(f"{label} workflow must verify the exact legacy D3D UMD exports")

        for fragment in (
            "$exportNames = @(",
            "Compare-Object -ReferenceObject $expectedExports -DifferenceObject $exportNames",
            "$exportNames.Count -ne $expectedExports.Count",
        ):
            if source.count(fragment) != 1:
                fail(f"{label} workflow must parse and compare the exact UMD export table: {fragment}")
        if source.count("'legacy UMD shim must not export OpenAdapter12'") != 1:
            fail(f"{label} workflow must reject a D3D12 export from the legacy UMD shim")
        if source.count("Assert-Arm64Pe $driver") != 1 or source.count("Assert-Arm64Pe $umd") != 1:
            fail(f"{label} workflow must apply the AA64 gate to the SYS and D3D UMD DLL")
        for fragment in (
            "$sectionRows = @(",
            "$_.Name.StartsWith('.text', [System.StringComparison]::Ordinal)",
            "$requiredTextSymbols = @(",
            "'?QueryNativeContextReadiness@VioGpuDod@@'",
            "'?QueryNativeContextReadiness@VioGpuAdapter@@'",
            "$textSectionIds -notcontains $sectionId",
            "$tokens[-1] -notlike '*viogpudo.obj'",
        ):
            if source.count(fragment) != 1:
                fail(f"{label} workflow must prove readiness routines are linked into default .text: {fragment}")
    if not PRESENT_DIAGNOSTIC_TEST_PATH.is_file():
        fail("Native Present diagnostic decoder fixture is missing")
    if sources["Native Context full-miniport"].count(
        ".install_scripts/test-viogpu-native-present-diagnostics.ps1"
    ) != 1:
        fail("full-miniport workflow must execute the Native Present diagnostic decoder fixture")
    if sources["Native Context full-miniport"].count("viogpu/viogpud3d/viogpud3d.vcxproj") != 2:
        fail("the full WDDM contract workflow must build the normal and opt-in ARM64 D3D UMD targets")
    if sources["product drivers"].count("viogpu/viogpud3d/viogpud3d.vcxproj") != 1:
        fail("the signed ARM64 product workflow must build the D3D UMD shim exactly once")
    if sources["product drivers"].count("viogpu/viogpuwddm/viogpuwddm.vcxproj") != 1:
        fail("the signed ARM64 product workflow must build the Native Context full miniport exactly once")
    if "viogpu/viogpudo/viogpudo.vcxproj" in sources["product drivers"]:
        fail("the signed ARM64 product workflow must not build a second display-only viogpu SYS")
    product_package = (
        "@{ n='viogpu'; root='viogpu/viogpuwddm'; "
        "bins=@('viogpuwddm.sys','viogpud3d.dll'); inf='viogpuwddm.inf' }"
    )
    if sources["product drivers"].count(product_package) != 1:
        fail("the signed ARM64 product workflow must stage one integrated viogpu SYS/D3D-UMD/INF package")
    if "n='viogpuwddm'" in sources["product drivers"]:
        fail("the signed ARM64 product workflow must not stage a second viogpuwddm package")
    for label, source in sources.items():
        for display_only_name in ("viogpudo.sys", "viogpudo.inf", "viogpudo.cat"):
            if display_only_name in source:
                fail(f"{label} workflow must not emit the retired display-only package {display_only_name}")
    product_debug_fragments = (
        "$nativeDebugRoot = 'viogpu/viogpuwddm/objfre_win11_arm64/arm64'",
        "$nativeDebugFiles = @('viogpuwddm.pdb', 'viogpuwddm.map', 'viogpud3d.pdb')",
        "$debugSource = Join-Path $nativeDebugRoot $debugFile",
        'throw "Native Context product debug file is missing or empty: $debugSource"',
        "Copy-Item -LiteralPath $debugSource -Destination $dest -Force",
    )
    for fragment in product_debug_fragments:
        if sources["product drivers"].count(fragment) != 1:
            fail(f"the signed ARM64 product workflow must stage exact-build debug evidence: {fragment}")
    product_version_fragments = (
        "$epoch = 'cd6097248fe17b459b8587021799bd071f0f029f'",
        'git rev-list --count "$epoch..HEAD"',
        '"DROIDVM_DRIVER_MINOR=$minor" | Out-File -FilePath $env:GITHUB_ENV',
        "[int]$env:DROIDVM_DRIVER_MINOR -le 58000",
        'Native Context INF does not contain expected DriverVer $infVersion',
    )
    for fragment in product_version_fragments:
        if sources["product drivers"].count(fragment) != 1:
            fail(f"the signed ARM64 product workflow must enforce monotonic package versioning: {fragment}")


def check_d3d_umd_shim_contract() -> None:
    for path in (UMD_PROJECT, UMD_SOURCE_PATH, UMD_DEF_PATH, UMD_HEADER_PATH):
        if not path.is_file():
            fail(f"missing ARM64 D3D UMD shim input: {path}")

    source = UMD_SOURCE_PATH.read_text(encoding="utf-8")
    code = strip_cpp_comments_and_literals(source)
    if source.count("#include <windows.h>") != 1:
        fail("D3D UMD shim must use the Windows ABI declarations")
    header = UMD_HEADER_PATH.read_text(encoding="utf-8")
    if source.count('#include "d3dumddi_compat.h"') != 1:
        fail("D3D UMD shim must isolate the user-mode DDI declarations in its compatibility header")
    if header.count("#include <d3d10umddi.h>") != 1 or header.count("#include <d3d11.h>") != 1:
        fail("D3D UMD compatibility header must include the Windows D3D10/11 declarations exactly once")
    if "d3dkmddi.h" in header or "viogpuwddm" in header:
        fail("D3D UMD compatibility header must not depend on the kernel miniport implementation")

    signatures = {
        "OpenAdapter": "D3DDDIARG_OPENADAPTER",
        "OpenAdapter10": "D3D10DDIARG_OPENADAPTER",
        "OpenAdapter10_2": "D3D10DDIARG_OPENADAPTER",
    }
    for function_name, argument_type in signatures.items():
        signature = (
            rf'\bextern\s+"C"\s+HRESULT\s+APIENTRY\s+{function_name}\s*\('
            rf'\s*_Inout_\s+{argument_type}\s*\*\s*openData\s*\)'
        )
        if len(re.findall(signature, source)) != 1:
            fail(f"D3D UMD shim must expose one ABI-shaped {function_name} definition")

    if canonical_code(function_body("OpenAdapter", code)) != "UNREFERENCED_PARAMETER(openData);returnE_NOTIMPL;":
        fail("legacy D3D9 OpenAdapter must remain fail closed")
    if "returnOpenAdapter10Common(openData);" not in canonical_code(function_body("OpenAdapter10", code)):
        fail("OpenAdapter10 must route through the activation adapter contract")
    open10_2 = canonical_code(function_body("OpenAdapter10_2", code))
    for fragment in (
        "openData->pAdapterFuncs_2",
        "OpenAdapter10Common(openData)",
        "functions.pfnGetSupportedVersions=ActivationGetSupportedVersions",
        "functions.pfnGetCaps=ActivationGetCaps",
        "returnS_OK",
    ):
        if fragment not in open10_2:
            fail(f"OpenAdapter10_2 must publish the activation adapter table: {fragment}")
    if "OpenAdapter12" in source:
        fail("legacy WDDMv1 D3D UMD shim must not imply D3D12 support")
    create_device = canonical_code(function_body("ActivationCreateDevice", code))
    if "returnE_NOTIMPL" not in create_device:
        fail("activation-only D3D UMD must keep the product CreateDevice path fail closed")
    if source.count("#if defined(VIOGPU_WDDM_TEST_IMPLEMENTATIONS)") != 4:
        fail("D3D UMD test lifecycle/capability paths must use one explicit opt-in macro for each guarded block")
    calc_private_device_size = canonical_code(function_body("ActivationCalcPrivateDeviceSize", code))
    for fragment in (
        "returnsizeof(ACTIVATION_DEVICE);",
        "return0;",
    ):
        if fragment not in calc_private_device_size:
            fail(f"D3D UMD test lifecycle size gate is missing: {fragment}")
    for fragment in (
        "arguments==NULL||arguments->pDeviceFuncs==NULL||arguments->hDrvDevice.pDrvPrivate==NULL",
        "state->Signature=ACTIVATION_DEVICE_SIGNATURE;",
        "state->CallCount=0;",
        "state->LastCall=0;",
        "functions.pfnDraw=ActivationDraw;",
        "functions.pfnDrawIndexed=ActivationDrawIndexed;",
        "functions.pfnDrawInstanced=ActivationDrawInstanced;",
        "functions.pfnDrawIndexedInstanced=ActivationDrawIndexedInstanced;",
        "functions.pfnDrawAuto=ActivationDrawAuto;",
        "functions.pfnDynamicIABufferMapNoOverwrite=ActivationResourceMap;",
        "functions.pfnDynamicIABufferUnmap=ActivationResourceUnmap;",
        "functions.pfnDynamicConstantBufferMapDiscard=ActivationResourceMap;",
        "functions.pfnDynamicIABufferMapDiscard=ActivationResourceMap;",
        "functions.pfnDynamicConstantBufferUnmap=ActivationResourceUnmap;",
        "functions.pfnDynamicResourceMapDiscard=ActivationResourceMap;",
        "functions.pfnDynamicResourceUnmap=ActivationResourceUnmap;",
        "functions.pfnIaSetInputLayout=ActivationIaSetInputLayout;",
        "functions.pfnIaSetVertexBuffers=ActivationIaSetVertexBuffers;",
        "functions.pfnIaSetIndexBuffer=ActivationIaSetIndexBuffer;",
        "functions.pfnIaSetTopology=ActivationIaSetTopology;",
        "functions.pfnVsSetShader=ActivationVsSetShader;",
        "functions.pfnPsSetShader=ActivationPsSetShader;",
        "functions.pfnGsSetShader=ActivationGsSetShader;",
        "functions.pfnVsSetConstantBuffers=ActivationVsSetConstantBuffers;",
        "functions.pfnPsSetConstantBuffers=ActivationPsSetConstantBuffers;",
        "functions.pfnGsSetConstantBuffers=ActivationGsSetConstantBuffers;",
        "functions.pfnVsSetShaderResources=ActivationVsSetShaderResources;",
        "functions.pfnPsSetShaderResources=ActivationPsSetShaderResources;",
        "functions.pfnGsSetShaderResources=ActivationGsSetShaderResources;",
        "functions.pfnVsSetSamplers=ActivationVsSetSamplers;",
        "functions.pfnPsSetSamplers=ActivationPsSetSamplers;",
        "functions.pfnGsSetSamplers=ActivationGsSetSamplers;",
        "functions.pfnSetRenderTargets=ActivationSetRenderTargets;",
        "functions.pfnSetBlendState=ActivationSetBlendState;",
        "functions.pfnSetDepthStencilState=ActivationSetDepthStencilState;",
        "functions.pfnSetRasterizerState=ActivationSetRasterizerState;",
        "functions.pfnSetViewports=ActivationSetViewports;",
        "functions.pfnSetScissorRects=ActivationSetScissorRects;",
        "functions.pfnSetPredication=ActivationSetPredication;",
        "functions.pfnClearRenderTargetView=ActivationClearRenderTargetView;",
        "functions.pfnClearDepthStencilView=ActivationClearDepthStencilView;",
        "functions.pfnResourceCopyRegion=ActivationResourceCopyRegion;",
        "functions.pfnResourceCopy=ActivationResourceCopy;",
        "functions.pfnResourceUpdateSubresourceUP=ActivationResourceUpdateSubresourceUP;",
        "functions.pfnDefaultConstantBufferUpdateSubresourceUP=ActivationDefaultConstantBufferUpdateSubresourceUP;",
        "functions.pfnGenMips=ActivationGenerateMips;",
        "functions.pfnSoSetTargets=ActivationSetStreamOutputTargets;",
        "functions.pfnResourceResolveSubresource=ActivationResolveSubresource;",
        "functions.pfnResourceMap=ActivationResourceMap;",
        "functions.pfnResourceUnmap=ActivationResourceUnmap;",
        "functions.pfnStagingResourceMap=ActivationResourceMap;",
        "functions.pfnStagingResourceUnmap=ActivationResourceUnmap;",
        "functions.pfnSetTextFilterSize=ActivationSetTextFilterSize;",
        "functions.pfnDestroyDevice=ActivationDestroyDevice;",
        "functions.pfnCalcPrivateResourceSize=ActivationCalcPrivateResourceSize;",
        "functions.pfnCreateResource=ActivationCreateResource;",
        "functions.pfnDestroyResource=ActivationDestroyResource;",
        "functions.pfnFlush=ActivationFlush;",
        "functions.pfnResourceReadAfterWriteHazard=ActivationResourceReadAfterWriteHazard;",
        "functions.pfnResourceIsStagingBusy=ActivationResourceIsStagingBusy;",
        "functions.pfnShaderResourceViewReadAfterWriteHazard=ActivationShaderResourceViewReadAfterWriteHazard;",
        "functions.pfnRelocateDeviceFuncs=ActivationRelocateDeviceFuncs;",
        "returnS_OK;",
    ):
        if fragment not in create_device:
            fail(f"D3D UMD test lifecycle is missing guarded implementation fragment: {fragment}")
    destroy_device = canonical_code(function_body("ActivationDestroyDevice", code))
    if "state==NULL||state->Signature!=ACTIVATION_DEVICE_SIGNATURE" not in destroy_device:
        fail("D3D UMD test lifecycle destroy must validate its private record")
    for fragment in ("state->Signature=0;", "state->CallCount=0;", "state->LastCall=0;"):
        if fragment not in destroy_device:
            fail(f"D3D UMD test lifecycle destroy must clear its private record: {fragment}")
    deferred_record = canonical_code(function_body("ActivationRecordDeferredContextCall", code))
    for fragment in (
        "state==NULL||state->Signature!=ACTIVATION_DEFERRED_CONTEXT_SIGNATURE",
        "InterlockedExchange(&state->LastCall,static_cast<LONG>(call));",
        "InterlockedIncrement(&state->CallCount);",
    ):
        if fragment not in deferred_record:
            fail(f"D3D UMD test deferred-context call recorder is missing: {fragment}")
    deferred_size = canonical_code(function_body("ActivationCalcPrivateDeferredContextSize", code))
    for fragment in (
        "arguments==NULL||arguments->Flags!=0",
        "returnsizeof(ACTIVATION_DEFERRED_CONTEXT);",
        "return0;",
    ):
        if fragment not in deferred_size:
            fail(f"D3D UMD test deferred-context size gate is missing: {fragment}")
    deferred_create = canonical_code(function_body("ActivationCreateDeferredContext", code))
    for fragment in (
        "arguments==NULL||arguments->Flags!=0||arguments->hDrvContext.pDrvPrivate==NULL",
        "state->Signature=ACTIVATION_DEFERRED_CONTEXT_SIGNATURE;",
        "state->RuntimeCoreLayer=arguments->hRTCoreLayer;",
        "state->Flags=arguments->Flags;",
        "state->CallCount=0;",
        "state->LastCall=0;",
        "if(arguments->p11ContextFuncs!=NULL)",
        "functions->pfnCheckDeferredContextHandleSizes=ActivationCheckDeferredContextHandleSizes;",
        "functions->pfnCalcDeferredContextHandleSize=ActivationCalcDeferredContextHandleSize;",
        "functions->pfnCalcPrivateDeferredContextSize=ActivationCalcPrivateDeferredContextSize;",
        "functions->pfnCreateDeferredContext=ActivationCreateDeferredContext;",
        "functions->pfnAbandonCommandList=ActivationAbandonCommandList;",
        "functions->pfnCalcPrivateCommandListSize=ActivationCalcPrivateCommandListSize;",
        "functions->pfnCreateCommandList=ActivationCreateCommandList;",
        "functions->pfnCommandListExecute=ActivationCommandListExecute;",
        "functions->pfnDestroyCommandList=ActivationDestroyCommandList;",
        "functions->pfnRecycleCommandList=ActivationRecycleCommandList;",
        "functions->pfnRecycleCreateCommandList=ActivationRecycleCreateCommandList;",
        "functions->pfnRecycleCreateDeferredContext=ActivationRecycleCreateDeferredContext;",
        "functions->pfnRecycleDestroyCommandList=ActivationRecycleDestroyCommandList;",
        "ActivationRecordDeviceCall(device,ActivationCallCreateDeferredContext);",
    ):
        if fragment not in deferred_create:
            fail(f"D3D UMD test deferred-context creation is missing: {fragment}")
    abandon = canonical_code(function_body("ActivationAbandonCommandList", code))
    for fragment in (
        "state==NULL||state->Signature!=ACTIVATION_DEFERRED_CONTEXT_SIGNATURE",
        "state->Flags=0;",
        "ActivationRecordDeferredContextCall(device,ActivationCallAbandonCommandList);",
    ):
        if fragment not in abandon:
            fail(f"D3D UMD test deferred-context abandon is missing: {fragment}")
    recycle_deferred = canonical_code(function_body("ActivationRecycleCreateDeferredContext", code))
    for fragment in (
        "arguments==NULL||arguments->Flags!=0||arguments->hDrvContext.pDrvPrivate==NULL",
        "state->Signature=ACTIVATION_DEFERRED_CONTEXT_SIGNATURE;",
        "state->RuntimeCoreLayer=arguments->hRTCoreLayer;",
        "state->Flags=arguments->Flags;",
        "state->CallCount=0;",
        "state->LastCall=0;",
        "ActivationRecordDeviceCall(device,ActivationCallRecycleCreateDeferredContext);",
        "returnS_OK;",
    ):
        if fragment not in recycle_deferred:
            fail(f"D3D UMD test deferred-context recycle-create is missing: {fragment}")
    recycle_object = canonical_code(function_body("ActivationRecycleObject", code))
    for fragment in (
        "state==NULL||state->Signature!=signature",
        "state->RuntimeHandle=NULL;",
    ):
        if fragment not in recycle_object:
            fail(f"D3D UMD test recycle owner is missing: {fragment}")
    recycle_command_list = canonical_code(function_body("ActivationRecycleCommandList", code))
    if "ActivationRecycleObject(commandList.pDrvPrivate,ACTIVATION_COMMAND_LIST_SIGNATURE);" not in recycle_command_list:
        fail("D3D UMD test command-list recycle must retain its private allocation")
    recycle_create_command_list = canonical_code(function_body("ActivationRecycleCreateCommandList", code))
    for fragment in (
        "arguments==NULL||commandList.pDrvPrivate==NULL||runtimeCommandList.handle==NULL",
        "ActivationInitializeObject(commandList.pDrvPrivate,runtimeCommandList.handle,ACTIVATION_COMMAND_LIST_SIGNATURE);",
        "ActivationRecordDeviceCall(device,ActivationCallRecycleCreateCommandList);",
        "returnS_OK;",
    ):
        if fragment not in recycle_create_command_list:
            fail(f"D3D UMD test command-list recycle-create is missing: {fragment}")
    recycle_destroy_command_list = canonical_code(function_body("ActivationRecycleDestroyCommandList", code))
    if "ActivationRecycleObject(commandList.pDrvPrivate,ACTIVATION_COMMAND_LIST_SIGNATURE);" not in recycle_destroy_command_list:
        fail("D3D UMD test command-list recycle-destroy must retain its private allocation")
    handle_sizes = canonical_code(function_body("ActivationCheckDeferredContextHandleSizes", code))
    for fragment in (
        "if(handleSizeArray!=NULL){*handleSizeArray=0;}",
        "ActivationRecordDeviceCall(device,ActivationCallCheckDeferredContextHandleSizes);",
    ):
        if fragment not in handle_sizes:
            fail(f"D3D UMD test deferred handle-size query is missing: {fragment}")
    handle_size = canonical_code(function_body("ActivationCalcDeferredContextHandleSize", code))
    for fragment in (
        "ActivationRecordDeviceCall(device,ActivationCallCalcDeferredContextHandleSize);",
        "return0;",
    ):
        if fragment not in handle_size:
            fail(f"D3D UMD test deferred handle-size calculation is missing: {fragment}")
    resource_size = canonical_code(function_body("ActivationCalcPrivateResourceSize", code))
    if "returnsizeof(ACTIVATION_RESOURCE);" not in resource_size:
        fail("D3D UMD test resource lifecycle size gate is missing")
    resource_create = canonical_code(function_body("ActivationCreateResource", code))
    for fragment in (
        "arguments==NULL||arguments->pMipInfoList==NULL||arguments->MipLevels==0||arguments->ArraySize==0||resource.pDrvPrivate==NULL||runtimeResource.handle==NULL",
        "state->Signature=ACTIVATION_RESOURCE_SIGNATURE;",
        "state->RuntimeResource=runtimeResource;",
    ):
        if fragment not in resource_create:
            fail(f"D3D UMD test resource creation is missing guarded implementation fragment: {fragment}")
    resource_destroy = canonical_code(function_body("ActivationDestroyResource", code))
    if "state==NULL||state->Signature!=ACTIVATION_RESOURCE_SIGNATURE" not in resource_destroy:
        fail("D3D UMD test resource destroy must validate its private record")
    for fragment in ("state->Signature=0;", "state->RuntimeResource.handle=NULL;"):
        if fragment not in resource_destroy:
            fail(f"D3D UMD test resource destroy must clear its private record: {fragment}")
    d3d11_resource_size = canonical_code(function_body("ActivationCalcPrivateResourceSize11", code))
    if "returnsizeof(ACTIVATION_RESOURCE);" not in d3d11_resource_size:
        fail("D3D11 UMD test resource lifecycle size gate is missing")
    d3d11_resource_create = canonical_code(function_body("ActivationCreateResource11", code))
    for fragment in (
        "arguments==NULL||arguments->pMipInfoList==NULL||arguments->MipLevels==0||arguments->ArraySize==0||resource.pDrvPrivate==NULL||runtimeResource.handle==NULL",
        "state->Signature=ACTIVATION_RESOURCE_SIGNATURE;",
        "state->RuntimeResource=runtimeResource;",
    ):
        if fragment not in d3d11_resource_create:
            fail(f"D3D11 UMD test resource creation is missing guarded implementation fragment: {fragment}")
    d3d11_object_wrappers = (
        ("ActivationCalcPrivateShaderResourceViewSize11", "returnsizeof(ACTIVATION_OBJECT);"),
        ("ActivationCalcPrivateDepthStencilViewSize11", "returnsizeof(ACTIVATION_OBJECT);"),
        ("ActivationCalcPrivateBlendStateSize11", "returnsizeof(ACTIVATION_OBJECT);"),
        ("ActivationCalcPrivateGeometryShaderWithStreamOutput11", "returnsizeof(ACTIVATION_OBJECT);"),
    )
    for callback, fragment in d3d11_object_wrappers:
        if fragment not in canonical_code(function_body(callback, code)):
            fail(f"D3D11 UMD test object size callback is missing: {callback}")
    d3d11_create_wrappers = (
        ("ActivationCreateShaderResourceView11", "ActivationInitializeObject(view.pDrvPrivate,runtimeView.handle,ACTIVATION_SHADER_VIEW_SIGNATURE);"),
        ("ActivationCreateDepthStencilView11", "ActivationInitializeObject(view.pDrvPrivate,runtimeView.handle,ACTIVATION_DEPTH_STENCIL_VIEW_SIGNATURE);"),
        ("ActivationCreateBlendState11", "ActivationInitializeObject(state.pDrvPrivate,runtimeState.handle,ACTIVATION_BLEND_STATE_SIGNATURE);"),
        ("ActivationCreateGeometryShaderWithStreamOutput11", "ActivationCreateShader(shader,runtimeShader);"),
    )
    for callback, fragment in d3d11_create_wrappers:
        if fragment not in canonical_code(function_body(callback, code)):
            fail(f"D3D11 UMD test object creation callback is missing: {callback}")
    create_device_fragments = (
        "functions.pfnCalcPrivateOpenedResourceSize=ActivationCalcPrivateOpenedResourceSize;",
        "functions.pfnOpenResource=ActivationOpenResource;",
        "functions.pfnCalcPrivateQuerySize=ActivationCalcPrivateQuerySize;",
        "functions.pfnCreateQuery=ActivationCreateQuery;",
        "functions.pfnDestroyQuery=ActivationDestroyQuery;",
        "functions.pfnQueryBegin=ActivationQueryBegin;",
        "functions.pfnQueryEnd=ActivationQueryEnd;",
        "functions.pfnQueryGetData=ActivationQueryGetData;",
        "functions.pfnCheckCounterInfo=ActivationCheckCounterInfo;",
        "functions.pfnCheckCounter=ActivationCheckCounter;",
        "functions.pfnCheckFormatSupport=ActivationCheckFormatSupport;",
        "functions.pfnCheckMultisampleQualityLevels=ActivationCheckMultisampleQualityLevels;",
        "functions.pfnCalcPrivateElementLayoutSize=ActivationCalcPrivateElementLayoutSize;",
        "functions.pfnCreateElementLayout=ActivationCreateElementLayout;",
        "functions.pfnDestroyElementLayout=ActivationDestroyElementLayout;",
        "functions.pfnCalcPrivateSamplerSize=ActivationCalcPrivateSamplerSize;",
        "functions.pfnCreateSampler=ActivationCreateSampler;",
        "functions.pfnDestroySampler=ActivationDestroySampler;",
        "functions.pfnCalcPrivateShaderSize=ActivationCalcPrivateShaderSize;",
        "functions.pfnCreateVertexShader=ActivationCreateVertexShader;",
        "functions.pfnCreateGeometryShader=ActivationCreateGeometryShader;",
        "functions.pfnCreatePixelShader=ActivationCreatePixelShader;",
        "functions.pfnCalcPrivateGeometryShaderWithStreamOutput=ActivationCalcPrivateGeometryShaderWithStreamOutput;",
        "functions.pfnCreateGeometryShaderWithStreamOutput=ActivationCreateGeometryShaderWithStreamOutput;",
        "functions.pfnDestroyShader=ActivationDestroyShader;",
        "functions.pfnCalcPrivateShaderResourceViewSize=ActivationCalcPrivateShaderResourceViewSize;",
        "functions.pfnCreateShaderResourceView=ActivationCreateShaderResourceView;",
        "functions.pfnDestroyShaderResourceView=ActivationDestroyShaderResourceView;",
        "functions.pfnCalcPrivateRenderTargetViewSize=ActivationCalcPrivateRenderTargetViewSize;",
        "functions.pfnCreateRenderTargetView=ActivationCreateRenderTargetView;",
        "functions.pfnDestroyRenderTargetView=ActivationDestroyRenderTargetView;",
        "functions.pfnCalcPrivateDepthStencilViewSize=ActivationCalcPrivateDepthStencilViewSize;",
        "functions.pfnCreateDepthStencilView=ActivationCreateDepthStencilView;",
        "functions.pfnDestroyDepthStencilView=ActivationDestroyDepthStencilView;",
        "functions.pfnCalcPrivateBlendStateSize=ActivationCalcPrivateBlendStateSize;",
        "functions.pfnCreateBlendState=ActivationCreateBlendState;",
        "functions.pfnDestroyBlendState=ActivationDestroyBlendState;",
        "functions.pfnCalcPrivateDepthStencilStateSize=ActivationCalcPrivateDepthStencilStateSize;",
        "functions.pfnCreateDepthStencilState=ActivationCreateDepthStencilState;",
        "functions.pfnDestroyDepthStencilState=ActivationDestroyDepthStencilState;",
        "functions.pfnCalcPrivateRasterizerStateSize=ActivationCalcPrivateRasterizerStateSize;",
        "functions.pfnCreateRasterizerState=ActivationCreateRasterizerState;",
        "functions.pfnDestroyRasterizerState=ActivationDestroyRasterizerState;",
    )
    for fragment in create_device_fragments:
        if fragment not in create_device:
            fail(f"D3D UMD test device table is missing guarded callback: {fragment}")
    d3d11_fragments = (
        "arguments->Interface==D3D11_0_DDI_INTERFACE_VERSION&&arguments->p11DeviceFuncs!=NULL",
        "ZeroMemory(functions11,sizeof(*functions11));",
        "functions11->pfnDefaultConstantBufferUpdateSubresourceUP=ActivationDefaultConstantBufferUpdateSubresourceUP;",
        "functions11->pfnVsSetConstantBuffers=ActivationVsSetConstantBuffers;",
        "functions11->pfnPsSetShaderResources=ActivationPsSetShaderResources;",
        "functions11->pfnPsSetShader=ActivationPsSetShader;",
        "functions11->pfnPsSetSamplers=ActivationPsSetSamplers;",
        "functions11->pfnVsSetShader=ActivationVsSetShader;",
        "functions11->pfnDrawIndexed=ActivationDrawIndexed;",
        "functions11->pfnDraw=ActivationDraw;",
        "functions11->pfnDynamicIABufferMapNoOverwrite=ActivationResourceMap;",
        "functions11->pfnDynamicIABufferUnmap=ActivationResourceUnmap;",
        "functions11->pfnDynamicConstantBufferMapDiscard=ActivationResourceMap;",
        "functions11->pfnDynamicIABufferMapDiscard=ActivationResourceMap;",
        "functions11->pfnDynamicConstantBufferUnmap=ActivationResourceUnmap;",
        "functions11->pfnPsSetConstantBuffers=ActivationPsSetConstantBuffers;",
        "functions11->pfnIaSetInputLayout=ActivationIaSetInputLayout;",
        "functions11->pfnIaSetVertexBuffers=ActivationIaSetVertexBuffers;",
        "functions11->pfnIaSetIndexBuffer=ActivationIaSetIndexBuffer;",
        "functions11->pfnDrawIndexedInstanced=ActivationDrawIndexedInstanced;",
        "functions11->pfnDrawInstanced=ActivationDrawInstanced;",
        "functions11->pfnDynamicResourceMapDiscard=ActivationResourceMap;",
        "functions11->pfnDynamicResourceUnmap=ActivationResourceUnmap;",
        "functions11->pfnGsSetConstantBuffers=ActivationGsSetConstantBuffers;",
        "functions11->pfnGsSetShader=ActivationGsSetShader;",
        "functions11->pfnIaSetTopology=ActivationIaSetTopology;",
        "functions11->pfnStagingResourceMap=ActivationResourceMap;",
        "functions11->pfnStagingResourceUnmap=ActivationResourceUnmap;",
        "functions11->pfnVsSetShaderResources=ActivationVsSetShaderResources;",
        "functions11->pfnVsSetSamplers=ActivationVsSetSamplers;",
        "functions11->pfnGsSetShaderResources=ActivationGsSetShaderResources;",
        "functions11->pfnGsSetSamplers=ActivationGsSetSamplers;",
        "functions11->pfnSetRenderTargets=ActivationSetRenderTargets11;",
        "functions11->pfnShaderResourceViewReadAfterWriteHazard=ActivationShaderResourceViewReadAfterWriteHazard;",
        "functions11->pfnResourceReadAfterWriteHazard=ActivationResourceReadAfterWriteHazard;",
        "functions11->pfnSetBlendState=ActivationSetBlendState;",
        "functions11->pfnSetDepthStencilState=ActivationSetDepthStencilState;",
        "functions11->pfnSetRasterizerState=ActivationSetRasterizerState;",
        "functions11->pfnQueryEnd=ActivationQueryEnd;",
        "functions11->pfnQueryBegin=ActivationQueryBegin;",
        "functions11->pfnResourceCopyRegion=ActivationResourceCopyRegion;",
        "functions11->pfnResourceUpdateSubresourceUP=ActivationResourceUpdateSubresourceUP;",
        "functions11->pfnSoSetTargets=ActivationSetStreamOutputTargets;",
        "functions11->pfnDrawAuto=ActivationDrawAuto;",
        "functions11->pfnSetViewports=ActivationSetViewports;",
        "functions11->pfnSetScissorRects=ActivationSetScissorRects;",
        "functions11->pfnClearRenderTargetView=ActivationClearRenderTargetView;",
        "functions11->pfnClearDepthStencilView=ActivationClearDepthStencilView;",
        "functions11->pfnSetPredication=ActivationSetPredication;",
        "functions11->pfnQueryGetData=ActivationQueryGetData;",
        "functions11->pfnFlush=ActivationFlush;",
        "functions11->pfnGenMips=ActivationGenerateMips;",
        "functions11->pfnResourceCopy=ActivationResourceCopy;",
        "functions11->pfnResourceResolveSubresource=ActivationResolveSubresource;",
        "functions11->pfnResourceMap=ActivationResourceMap;",
        "functions11->pfnResourceUnmap=ActivationResourceUnmap;",
        "functions11->pfnResourceIsStagingBusy=ActivationResourceIsStagingBusy;",
        "functions11->pfnRelocateDeviceFuncs=ActivationRelocateDeviceFuncs11;",
        "functions11->pfnCalcPrivateResourceSize=ActivationCalcPrivateResourceSize11;",
        "functions11->pfnCalcPrivateOpenedResourceSize=ActivationCalcPrivateOpenedResourceSize;",
        "functions11->pfnCreateResource=ActivationCreateResource11;",
        "functions11->pfnOpenResource=ActivationOpenResource;",
        "functions11->pfnDestroyResource=ActivationDestroyResource;",
        "functions11->pfnCalcPrivateShaderResourceViewSize=ActivationCalcPrivateShaderResourceViewSize11;",
        "functions11->pfnCreateShaderResourceView=ActivationCreateShaderResourceView11;",
        "functions11->pfnDestroyShaderResourceView=ActivationDestroyShaderResourceView;",
        "functions11->pfnCalcPrivateRenderTargetViewSize=ActivationCalcPrivateRenderTargetViewSize;",
        "functions11->pfnCreateRenderTargetView=ActivationCreateRenderTargetView;",
        "functions11->pfnDestroyRenderTargetView=ActivationDestroyRenderTargetView;",
        "functions11->pfnCalcPrivateDepthStencilViewSize=ActivationCalcPrivateDepthStencilViewSize11;",
        "functions11->pfnCreateDepthStencilView=ActivationCreateDepthStencilView11;",
        "functions11->pfnDestroyDepthStencilView=ActivationDestroyDepthStencilView;",
        "functions11->pfnCalcPrivateElementLayoutSize=ActivationCalcPrivateElementLayoutSize;",
        "functions11->pfnCreateElementLayout=ActivationCreateElementLayout;",
        "functions11->pfnDestroyElementLayout=ActivationDestroyElementLayout;",
        "functions11->pfnCalcPrivateBlendStateSize=ActivationCalcPrivateBlendStateSize11;",
        "functions11->pfnCreateBlendState=ActivationCreateBlendState11;",
        "functions11->pfnDestroyBlendState=ActivationDestroyBlendState;",
        "functions11->pfnCalcPrivateDepthStencilStateSize=ActivationCalcPrivateDepthStencilStateSize;",
        "functions11->pfnCreateDepthStencilState=ActivationCreateDepthStencilState;",
        "functions11->pfnDestroyDepthStencilState=ActivationDestroyDepthStencilState;",
        "functions11->pfnCalcPrivateRasterizerStateSize=ActivationCalcPrivateRasterizerStateSize;",
        "functions11->pfnCreateRasterizerState=ActivationCreateRasterizerState;",
        "functions11->pfnDestroyRasterizerState=ActivationDestroyRasterizerState;",
        "functions11->pfnCalcPrivateShaderSize=ActivationCalcPrivateShaderSize;",
        "functions11->pfnCreateVertexShader=ActivationCreateVertexShader;",
        "functions11->pfnCreateGeometryShader=ActivationCreateGeometryShader;",
        "functions11->pfnCreatePixelShader=ActivationCreatePixelShader;",
        "functions11->pfnCalcPrivateGeometryShaderWithStreamOutput=ActivationCalcPrivateGeometryShaderWithStreamOutput11;",
        "functions11->pfnCreateGeometryShaderWithStreamOutput=ActivationCreateGeometryShaderWithStreamOutput11;",
        "functions11->pfnDestroyShader=ActivationDestroyShader;",
        "functions11->pfnCalcPrivateSamplerSize=ActivationCalcPrivateSamplerSize;",
        "functions11->pfnCreateSampler=ActivationCreateSampler;",
        "functions11->pfnDestroySampler=ActivationDestroySampler;",
        "functions11->pfnCalcPrivateQuerySize=ActivationCalcPrivateQuerySize;",
        "functions11->pfnCreateQuery=ActivationCreateQuery;",
        "functions11->pfnDestroyQuery=ActivationDestroyQuery;",
        "functions11->pfnCheckFormatSupport=ActivationCheckFormatSupport;",
        "functions11->pfnCheckMultisampleQualityLevels=ActivationCheckMultisampleQualityLevels;",
        "functions11->pfnCheckCounterInfo=ActivationCheckCounterInfo;",
        "functions11->pfnCheckCounter=ActivationCheckCounter;",
        "functions11->pfnDestroyDevice=ActivationDestroyDevice;",
        "functions11->pfnSetTextFilterSize=ActivationSetTextFilterSize;",
        "functions11->pfnResourceConvert=ActivationResourceCopy;",
        "functions11->pfnResourceConvertRegion=ActivationResourceCopyRegion;",
        "functions11->pfnVsSetShaderWithIfaces=ActivationVsSetShaderWithIfaces;",
        "functions11->pfnPsSetShaderWithIfaces=ActivationPsSetShaderWithIfaces;",
        "functions11->pfnGsSetShaderWithIfaces=ActivationGsSetShaderWithIfaces;",
        "functions11->pfnCreateComputeShader=ActivationCreateComputeShader;",
        "functions11->pfnCsSetShader=ActivationCsSetShader;",
        "functions11->pfnCsSetShaderWithIfaces=ActivationCsSetShaderWithIfaces;",
        "functions11->pfnCsSetShaderResources=ActivationCsSetShaderResources;",
        "functions11->pfnCsSetSamplers=ActivationCsSetSamplers;",
        "functions11->pfnCsSetConstantBuffers=ActivationCsSetConstantBuffers;",
        "functions11->pfnHsSetShaderResources=ActivationHsSetShaderResources;",
        "functions11->pfnHsSetShader=ActivationHsSetShader;",
        "functions11->pfnHsSetShaderWithIfaces=ActivationHsSetShaderWithIfaces;",
        "functions11->pfnHsSetSamplers=ActivationHsSetSamplers;",
        "functions11->pfnHsSetConstantBuffers=ActivationHsSetConstantBuffers;",
        "functions11->pfnDsSetShaderResources=ActivationDsSetShaderResources;",
        "functions11->pfnDsSetShader=ActivationDsSetShader;",
        "functions11->pfnDsSetShaderWithIfaces=ActivationDsSetShaderWithIfaces;",
        "functions11->pfnDsSetSamplers=ActivationDsSetSamplers;",
        "functions11->pfnDsSetConstantBuffers=ActivationDsSetConstantBuffers;",
        "functions11->pfnCreateHullShader=ActivationCreateHullShader;",
        "functions11->pfnCreateDomainShader=ActivationCreateDomainShader;",
        "functions11->pfnCalcPrivateTessellationShaderSize=ActivationCalcPrivateTessellationShaderSize;",
        "functions11->pfnCalcPrivateUnorderedAccessViewSize=ActivationCalcPrivateUnorderedAccessViewSize;",
        "functions11->pfnCreateUnorderedAccessView=ActivationCreateUnorderedAccessView;",
        "functions11->pfnDestroyUnorderedAccessView=ActivationDestroyUnorderedAccessView;",
        "functions11->pfnClearUnorderedAccessViewUint=ActivationClearUnorderedAccessViewUint;",
        "functions11->pfnClearUnorderedAccessViewFloat=ActivationClearUnorderedAccessViewFloat;",
        "functions11->pfnCsSetUnorderedAccessViews=ActivationCsSetUnorderedAccessViews;",
        "functions11->pfnResourceConvert=ActivationResourceCopy;",
        "functions11->pfnResourceConvertRegion=ActivationResourceCopyRegion;",
        "functions11->pfnDispatch=ActivationDispatch;",
        "functions11->pfnDispatchIndirect=ActivationDispatchIndirect;",
        "functions11->pfnDrawIndexedInstancedIndirect=ActivationDrawIndexedInstancedIndirect;",
        "functions11->pfnDrawInstancedIndirect=ActivationDrawInstancedIndirect;",
        "functions11->pfnSetResourceMinLOD=ActivationSetResourceMinLOD;",
        "functions11->pfnCopyStructureCount=ActivationCopyStructureCount;",
        "functions11->pfnCommandListExecute=ActivationCommandListExecute;",
        "functions11->pfnCalcPrivateCommandListSize=ActivationCalcPrivateCommandListSize;",
        "functions11->pfnCreateCommandList=ActivationCreateCommandList;",
        "functions11->pfnDestroyCommandList=ActivationDestroyCommandList;",
        "functions11->pfnCheckDeferredContextHandleSizes=ActivationCheckDeferredContextHandleSizes;",
        "functions11->pfnCalcDeferredContextHandleSize=ActivationCalcDeferredContextHandleSize;",
        "functions11->pfnCalcPrivateDeferredContextSize=ActivationCalcPrivateDeferredContextSize;",
        "functions11->pfnCreateDeferredContext=ActivationCreateDeferredContext;",
        "functions11->pfnAbandonCommandList=ActivationAbandonCommandList;",
        "functions11->pfnRecycleCommandList=ActivationRecycleCommandList;",
        "functions11->pfnRecycleCreateCommandList=ActivationRecycleCreateCommandList;",
        "functions11->pfnRecycleCreateDeferredContext=ActivationRecycleCreateDeferredContext;",
        "functions11->pfnRecycleDestroyCommandList=ActivationRecycleDestroyCommandList;",
    )
    for fragment in d3d11_fragments:
        if fragment not in create_device:
            fail(f"D3D UMD test D3D11 table is missing guarded callback: {fragment}")
    object_helper = canonical_code(function_body("ActivationInitializeObject", code))
    for fragment in (
        "if(privateData==NULL||runtimeHandle==NULL){return;}",
        "state->Signature=signature;",
        "state->RuntimeHandle=runtimeHandle;",
    ):
        if fragment not in object_helper:
            fail(f"D3D UMD test object initialization is missing: {fragment}")
    object_destroy = canonical_code(function_body("ActivationDestroyObject", code))
    for fragment in (
        "state==NULL||state->Signature!=signature",
        "state->RuntimeHandle=NULL;",
        "state->Signature=0;",
    ):
        if fragment not in object_destroy:
            fail(f"D3D UMD test object destruction is missing: {fragment}")
    object_callbacks = (
        "ActivationCreateElementLayout",
        "ActivationDestroyElementLayout",
        "ActivationCreateSampler",
        "ActivationDestroySampler",
        "ActivationCreateVertexShader",
        "ActivationCreateGeometryShader",
        "ActivationCreatePixelShader",
        "ActivationCreateGeometryShaderWithStreamOutput",
        "ActivationDestroyShader",
        "ActivationCreateShaderResourceView",
        "ActivationDestroyShaderResourceView",
        "ActivationCreateRenderTargetView",
        "ActivationDestroyRenderTargetView",
        "ActivationCreateDepthStencilView",
        "ActivationDestroyDepthStencilView",
        "ActivationCreateBlendState",
        "ActivationDestroyBlendState",
        "ActivationCreateDepthStencilState",
        "ActivationDestroyDepthStencilState",
        "ActivationCreateRasterizerState",
        "ActivationDestroyRasterizerState",
        "ActivationCreateHullShader",
        "ActivationCreateDomainShader",
        "ActivationCreateUnorderedAccessView",
        "ActivationDestroyUnorderedAccessView",
        "ActivationCreateCommandList",
        "ActivationDestroyCommandList",
    )
    for callback in object_callbacks:
        if len(re.findall(rf"\b{callback}\s*\(", source)) != 1:
            fail(f"D3D UMD test object callback must have one implementation: {callback}")
    for callback in (
        "ActivationResourceMap",
        "ActivationResourceUnmap",
        "ActivationSetStreamOutputTargets",
        "ActivationResolveSubresource",
        "ActivationSetTextFilterSize",
        "ActivationDispatch",
        "ActivationDispatchIndirect",
        "ActivationDrawIndexedInstancedIndirect",
        "ActivationDrawInstancedIndirect",
        "ActivationSetResourceMinLOD",
        "ActivationCreateComputeShader",
        "ActivationCsSetShader",
        "ActivationCsSetShaderResources",
        "ActivationCsSetSamplers",
        "ActivationCsSetConstantBuffers",
        "ActivationSetRenderTargets11",
        "ActivationRelocateDeviceFuncs11",
        "ActivationHsSetShaderResources",
        "ActivationHsSetShader",
        "ActivationHsSetSamplers",
        "ActivationHsSetConstantBuffers",
        "ActivationDsSetShaderResources",
        "ActivationDsSetShader",
        "ActivationDsSetSamplers",
        "ActivationDsSetConstantBuffers",
        "ActivationVsSetShaderWithIfaces",
        "ActivationPsSetShaderWithIfaces",
        "ActivationGsSetShaderWithIfaces",
        "ActivationHsSetShaderWithIfaces",
        "ActivationDsSetShaderWithIfaces",
        "ActivationCsSetShaderWithIfaces",
        "ActivationClearUnorderedAccessViewUint",
        "ActivationClearUnorderedAccessViewFloat",
        "ActivationCsSetUnorderedAccessViews",
        "ActivationCopyStructureCount",
        "ActivationCommandListExecute",
        "ActivationCalcPrivateCommandListSize",
        "ActivationCheckDeferredContextHandleSizes",
        "ActivationCalcDeferredContextHandleSize",
        "ActivationCalcPrivateTessellationShaderSize",
        "ActivationCalcPrivateUnorderedAccessViewSize",
    ):
        # Match the function body rather than every token occurrence: a few
        # deferred-context callbacks are intentionally forward-declared before
        # CreateDeferredContext wires their table.
        function_body(callback, code)
    shader_with_ifaces = canonical_code(function_body("ActivationSetShaderWithIfaces", code))
    for fragment in (
        "!ActivationIsObject(shader.pDrvPrivate,ACTIVATION_SHADER_SIGNATURE)",
        "classInstanceCount!=0&&(classInstances==NULL||interfacePointerData==NULL)",
        "ActivationRecordDeviceCall(device,call);",
    ):
        if fragment not in shader_with_ifaces:
            fail(f"D3D UMD test shader-with-interfaces validation is missing: {fragment}")
    for callback, call in (
        ("ActivationVsSetShaderWithIfaces", "ActivationCallVsSetShaderWithIfaces"),
        ("ActivationPsSetShaderWithIfaces", "ActivationCallPsSetShaderWithIfaces"),
        ("ActivationGsSetShaderWithIfaces", "ActivationCallGsSetShaderWithIfaces"),
        ("ActivationHsSetShaderWithIfaces", "ActivationCallHsSetShaderWithIfaces"),
        ("ActivationDsSetShaderWithIfaces", "ActivationCallDsSetShaderWithIfaces"),
        ("ActivationCsSetShaderWithIfaces", "ActivationCallCsSetShaderWithIfaces"),
    ):
        callback_body = canonical_code(function_body(callback, code))
        if f"ActivationSetShaderWithIfaces(device,shader,classInstanceCount,classInstances,interfacePointerData,{call});" not in callback_body:
            fail(f"D3D UMD test shader-with-interfaces callback is not wired: {callback}")
    resource_map = canonical_code(function_body("ActivationResourceMap", code))
    for fragment in (
        "state==NULL||state->Signature!=ACTIVATION_RESOURCE_SIGNATURE",
        "if(mappedResource!=NULL){ZeroMemory(mappedResource,sizeof(*mappedResource));}",
        "ActivationRecordDeviceCall(device,ActivationCallResourceMap);",
    ):
        if fragment not in resource_map:
            fail(f"D3D UMD test resource map callback is missing: {fragment}")
    resource_unmap = canonical_code(function_body("ActivationResourceUnmap", code))
    for fragment in (
        "state==NULL||state->Signature!=ACTIVATION_RESOURCE_SIGNATURE",
        "ActivationRecordDeviceCall(device,ActivationCallResourceUnmap);",
    ):
        if fragment not in resource_unmap:
            fail(f"D3D UMD test resource unmap callback is missing: {fragment}")
    opened_size = canonical_code(function_body("ActivationCalcPrivateOpenedResourceSize", code))
    if "returnsizeof(ACTIVATION_RESOURCE);" not in opened_size:
        fail("D3D UMD test opened-resource size gate is missing")
    opened = canonical_code(function_body("ActivationOpenResource", code))
    for fragment in (
        "arguments==NULL||resource.pDrvPrivate==NULL||runtimeResource.handle==NULL",
        "state->Signature=ACTIVATION_RESOURCE_SIGNATURE;",
        "state->RuntimeResource=runtimeResource;",
    ):
        if fragment not in opened:
            fail(f"D3D UMD test opened-resource lifecycle is missing: {fragment}")
    query_size = canonical_code(function_body("ActivationCalcPrivateQuerySize", code))
    if "returnsizeof(ACTIVATION_QUERY);" not in query_size:
        fail("D3D UMD test query lifecycle size gate is missing")
    query_create = canonical_code(function_body("ActivationCreateQuery", code))
    for fragment in (
        "arguments==NULL||query.pDrvPrivate==NULL||runtimeQuery.handle==NULL",
        "state->Signature=ACTIVATION_QUERY_SIGNATURE;",
        "state->RuntimeQuery=runtimeQuery;",
    ):
        if fragment not in query_create:
            fail(f"D3D UMD test query creation is missing: {fragment}")
    query_destroy = canonical_code(function_body("ActivationDestroyQuery", code))
    if "state==NULL||state->Signature!=ACTIVATION_QUERY_SIGNATURE" not in query_destroy:
        fail("D3D UMD test query destroy must validate its private record")
    for fragment in ("state->Signature=0;", "state->RuntimeQuery.handle=NULL;"):
        if fragment not in query_destroy:
            fail(f"D3D UMD test query destroy must clear its private record: {fragment}")
    query_data = canonical_code(function_body("ActivationQueryGetData", code))
    for callback in ("ActivationQueryBegin", "ActivationQueryEnd", "ActivationQueryGetData"):
        callback_body = canonical_code(function_body(callback, code))
        if "state==NULL||state->Signature!=ACTIVATION_QUERY_SIGNATURE" not in callback_body:
            fail(f"D3D UMD test {callback} must validate its private query record")
    if "if(data!=NULL&&dataSize!=0){ZeroMemory(data,dataSize);}" not in query_data:
        fail("D3D UMD test query data callback must initialize optional output")
    counter_info = canonical_code(function_body("ActivationCheckCounterInfo", code))
    if "if(counterInfo!=NULL){ZeroMemory(counterInfo,sizeof(*counterInfo));}" not in counter_info:
        fail("D3D UMD test counter info callback must initialize its output")
    counter = canonical_code(function_body("ActivationCheckCounter", code))
    for fragment in (
        "if(counterType!=NULL){*counterType=static_cast<D3D10DDI_COUNTER_TYPE>(0);}",
        "if(activeCounters!=NULL){*activeCounters=0;}",
        "if(nameLength!=NULL){*nameLength=0;}",
        "if(unitsLength!=NULL){*unitsLength=0;}",
        "if(descriptionLength!=NULL){*descriptionLength=0;}",
    ):
        if fragment not in counter:
            fail(f"D3D UMD test counter query must clear its output: {fragment}")
    format_support = canonical_code(function_body("ActivationCheckFormatSupport", code))
    if "if(formatSupport!=NULL){*formatSupport=0;}" not in format_support:
        fail("D3D UMD test format-support query must publish an empty capability mask")
    multisample = canonical_code(function_body("ActivationCheckMultisampleQualityLevels", code))
    if "if(qualityLevels!=NULL){*qualityLevels=0;}" not in multisample:
        fail("D3D UMD test multisample query must publish zero quality levels")
    caps = canonical_code(function_body("ActivationGetCaps", code))
    for fragment in (
        "if(arguments->DataSize==0){returnarguments->pData==NULL?S_OK:E_INVALIDARG;}",
        "if(arguments->pData==NULL){returnE_INVALIDARG;}",
        "switch(arguments->Type)",
        "caseD3D11DDICAPS_THREADING:",
        "caseD3D11DDICAPS_3DPIPELINESUPPORT:",
        "caseD3D11DDICAPS_SHADER:",
        "caseD3D11_1DDICAPS_D3D11_OPTIONS:",
        "caseD3D11_1DDICAPS_ARCHITECTURE_INFO:",
        "caseD3D11_1DDICAPS_SHADER_MIN_PRECISION_SUPPORT:",
        "ZeroMemory(arguments->pData,arguments->DataSize);",
        "default:returnE_NOTIMPL;",
    ):
        if fragment not in caps:
            fail(f"opt-in D3D capability negotiation is missing: {fragment}")
    compact_umd = re.sub(r"\s+", "", code)
    if "HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(ACTIVATION_ADAPTER))" not in compact_umd:
        fail("activation adapter must use a bounded process-heap owner")
    if "HeapFree(GetProcessHeap(),0,state)" not in compact_umd:
        fail("activation adapter must release its owner through CloseAdapter")
    if re.search(r"\bOpenAdapter12\b", source):
        fail("legacy WDDMv1 D3D UMD shim must not imply D3D12 support")

    dll_main = canonical_code(function_body("DllMain", code))
    expected_dll_main = (
        "UNREFERENCED_PARAMETER(instance);UNREFERENCED_PARAMETER(reason);"
        "UNREFERENCED_PARAMETER(reserved);returnTRUE;"
    )
    if dll_main != expected_dll_main:
        fail("D3D UMD DllMain must remain a side-effect-free loader entry point")

    definition = UMD_DEF_PATH.read_text(encoding="utf-8")
    definition_lines = [line.strip() for line in definition.splitlines() if line.strip()]
    if definition_lines != [
        'LIBRARY "viogpud3d"',
        "EXPORTS",
        "OpenAdapter",
        "OpenAdapter10",
        "OpenAdapter10_2",
    ]:
        fail("D3D UMD module definition must export exactly the three legacy entry points")

    root = ET.parse(UMD_PROJECT).getroot()
    configurations = [
        element.attrib.get("Include", "")
        for element in root.findall(".//msbuild:ProjectConfiguration", NAMESPACE)
    ]
    if configurations != ["Win11 Release|ARM64"]:
        fail(f"D3D UMD project must remain ARM64-only: {configurations or ['none']}")
    test_definition_groups = [
        element
        for element in root.findall(".//msbuild:ItemDefinitionGroup", NAMESPACE)
        if element.attrib.get("Condition") == "'$(VIOGPU_WDDM_TEST_IMPLEMENTATIONS)'=='1'"
    ]
    if len(test_definition_groups) != 1:
        fail("D3D UMD project must contain exactly one opt-in lifecycle test property group")
    test_definitions = [
        (element.text or "")
        for element in test_definition_groups[0].findall(
            "msbuild:ClCompile/msbuild:PreprocessorDefinitions", NAMESPACE
        )
    ]
    if len(test_definitions) != 1 or "VIOGPU_WDDM_TEST_IMPLEMENTATIONS=1" not in test_definitions[0].split(";"):
        fail("D3D UMD lifecycle test property group must define its macro exactly once")
    scalar_contract = {
        "ConfigurationType": "DynamicLibrary",
        "PlatformToolset": "WindowsApplicationForDrivers10.0",
        "TargetName": "viogpud3d",
        "ModuleDefinitionFile": "viogpud3d.def",
    }
    for tag, expected in scalar_contract.items():
        values = [(element.text or "").strip() for element in root.findall(f".//msbuild:{tag}", NAMESPACE)]
        if values != [expected]:
            fail(f"D3D UMD project {tag} must be {expected}: {values or ['none']}")
    path_contract = {
        "OutDir": "../viogpuwddm/objfre_win11_arm64/arm64/",
        "IntDir": "objfre_win11_arm64/arm64/",
    }
    for tag, expected in path_contract.items():
        values = [
            (element.text or "").strip().replace(chr(92), "/")
            for element in root.findall(f".//msbuild:{tag}", NAMESPACE)
        ]
        if values != [expected]:
            fail(f"D3D UMD project {tag} must be {expected}: {values or ['none']}")
    compile_inputs = [
        element.attrib.get("Include", "").replace("\\", "/")
        for element in root.findall(".//msbuild:ClCompile[@Include]", NAMESPACE)
    ]
    if compile_inputs != ["viogpud3d.cpp"]:
        fail(f"D3D UMD project must compile only its activation source: {compile_inputs}")


def check_retired_pool_absence() -> None:
    repository_root = PROJECT_DIR.parent.parent
    tracked_result = subprocess.run(
        ["git", "ls-files", "-z"],
        cwd=repository_root,
        check=False,
        capture_output=True,
    )
    tracked_paths = None
    if tracked_result.returncode == 0:
        tracked_paths = {
            Path(entry.decode("utf-8")).as_posix()
            for entry in tracked_result.stdout.split(b"\0")
            if entry
        }
    retired_paths = (
        repository_root / "rdmapool",
        repository_root / "droidvmpool",
        repository_root / "viogpu" / "common" / "viogpu_rdma.cpp",
        repository_root / "viogpu" / "common" / "viogpu_rdma.h",
        repository_root / "viogpu" / "common" / "viogpu_named_pool.cpp",
        repository_root / "viogpu" / "common" / "viogpu_named_pool.h",
        repository_root / "viogpu" / "viogpuwddm" / "check-named-pool.py",
        repository_root / "viogpu" / "viogpuwddm" / "test-rdma-contract.py",
        repository_root / "NetKVM" / "Common" / "ParaNdis_RdmaPool.cpp",
        repository_root / "NetKVM" / "Common" / "ParaNdis_RdmaPool.h",
    )
    present = [path.relative_to(repository_root).as_posix() for path in retired_paths if path.exists()]
    if present:
        fail(f"protected/restricted DMA pool sources must remain removed: {present}")

    source_roots = (
        repository_root / ".github" / "workflows",
        repository_root / ".install_scripts",
        repository_root / "VirtIO",
        repository_root / "NetKVM",
        repository_root / "viogpu",
        repository_root / "vioscsi",
        repository_root / "viostor",
        repository_root / "pvmpower",
    )
    source_suffixes = {
        ".c",
        ".cc",
        ".cmd",
        ".cpp",
        ".h",
        ".hpp",
        ".inf",
        ".inx",
        ".props",
        ".ps1",
        ".sln",
        ".targets",
        ".vcxproj",
        ".yaml",
        ".yml",
    }
    retired_token = re.compile(
        r"(?i)rdma[_-]?pool|rdmapool|viogpu_rdma|droidvmpool|drvm0001|"
        r"viogpu_named_pool|named[ _-]?pool|drm2kgsl_host|gpu_guest"
    )
    references = []
    for source_root in source_roots:
        if not source_root.exists():
            continue
        for path in source_root.rglob("*"):
            if not path.is_file() or path.suffix.lower() not in source_suffixes or path == Path(__file__).resolve():
                continue
            relative_path = path.relative_to(repository_root).as_posix()
            if tracked_paths is not None and relative_path not in tracked_paths:
                continue
            if retired_token.search(path.read_text(encoding="utf-8", errors="ignore")):
                references.append(relative_path)
    if references:
        fail(f"production source/build wiring must not reference retired Windows pools: {references}")


def check_native_start_diagnostics() -> None:
    expected_stages = {
        "VioGpuNativeStartEntered": 0x0100,
        "VioGpuNativeStartPreconditions": 0x0110,
        "VioGpuNativeStartDeviceInformation": 0x0120,
        "VioGpuNativeStartHardwareIdentity": 0x0130,
        "VioGpuNativeStartAdapterAllocation": 0x0140,
        "VioGpuNativeStartRegistryConfiguration": 0x0150,
        "VioGpuNativeStartBeginInitialization": 0x0200,
        "VioGpuNativeStartPciResources": 0x0210,
        "VioGpuNativeStartVirtioPreconditions": 0x0300,
        "VioGpuNativeStartVirtioDevice": 0x0310,
        "VioGpuNativeStartVirtioVersion": 0x0320,
        "VioGpuNativeStartVirtioNativeFeatures": 0x0330,
        "VioGpuNativeStartVirtioSetFeatures": 0x0340,
        "VioGpuNativeStartVirtioFindQueues": 0x0350,
        "VioGpuNativeStartVirtioQueueObjects": 0x0360,
        "VioGpuNativeStartVirtioQueueBacklog": 0x0370,
        "VioGpuNativeStartVirtioConfig": 0x0380,
        "VioGpuNativeStartHostVisibleRegion": 0x0400,
        "VioGpuNativeStartQueueBuffer": 0x0410,
        "VioGpuNativeStartResourceIds": 0x0420,
        "VioGpuNativeStartQueueInterrupts": 0x0430,
        "VioGpuNativeStartDriverReady": 0x0440,
        "VioGpuNativeStartSynchronousRequests": 0x0450,
        "VioGpuNativeStartCapsetFeatureState": 0x0500,
        "VioGpuNativeStartCapsetCount": 0x0510,
        "VioGpuNativeStartCapsetInfoQuery": 0x0520,
        "VioGpuNativeStartCapsetInfoUnique": 0x0530,
        "VioGpuNativeStartCapsetInfoLayout": 0x0540,
        "VioGpuNativeStartCapsetPayloadQuery": 0x0550,
        "VioGpuNativeStartCapsetPayloadValidation": 0x0560,
        "VioGpuNativeStartCapsetPublish": 0x0570,
        "VioGpuNativeStartModeList": 0x0600,
        "VioGpuNativeStartFrameSegment": 0x0610,
        "VioGpuNativeStartCursorSegment": 0x0620,
        "VioGpuNativeStartWorkThread": 0x0700,
        "VioGpuNativeStartCompleteInitialization": 0x0710,
        "VioGpuNativeStartHardwareInformation": 0x0800,
        "VioGpuNativeStartPostDisplayOwnership": 0x0810,
        "VioGpuNativeStartFinalState": 0x0820,
        "VioGpuNativeStartComplete": 0x0FFF,
    }
    observed_stages = {
        name: int(value, 16)
        for name, value in re.findall(
            r"\b(VioGpuNativeStart[A-Za-z0-9]+)\s*=\s*(0x[0-9A-Fa-f]+)\s*,",
            VIOGPU_HEADER_SOURCE,
        )
        if not name.startswith("VioGpuNativeStartDetail")
    }
    if observed_stages != expected_stages:
        fail(f"native StartDevice diagnostic stage ABI drifted: {observed_stages}")
    if len(set(observed_stages.values())) != len(observed_stages):
        fail("native StartDevice diagnostic stage values must remain unique")
    for stage in expected_stages:
        if len(re.findall(rf"\b{stage}\b", VIOGPU_SOURCE + VIOGPU_HEADER_SOURCE)) < 2:
            fail(f"native StartDevice diagnostic stage is defined but not recorded: {stage}")

    required_details = (
        "VioGpuNativeStartDetailMissingVirgl",
        "VioGpuNativeStartDetailMissingResourceBlob",
        "VioGpuNativeStartDetailMissingContextInit",
        "VioGpuNativeStartDetailMissingGuestHandle",
        "VioGpuNativeStartDetailInvalidWireVersion",
        "VioGpuNativeStartDetailInvalidContextType",
        "VioGpuNativeStartDetailInvalidPadding",
        "VioGpuNativeStartDetailInvalidMsmVersion",
        "VioGpuNativeStartDetailInvalidPriorities",
        "VioGpuNativeStartDetailInvalidVaStart",
        "VioGpuNativeStartDetailInvalidVaSize",
        "VioGpuNativeStartDetailInvalidVaRange",
    )
    for detail in required_details:
        if len(re.findall(rf"\b{detail}\b", VIOGPU_SOURCE + VIOGPU_HEADER_SOURCE)) < 2:
            fail(f"native StartDevice diagnostic detail is not populated: {detail}")

    recorder = function_body("VioGpuDod::RecordNativeStartDiagnostic", VIOGPU_SOURCE)
    if "IoOpenDeviceRegistryKey" not in recorder or "PLUGPLAY_REGKEY_DRIVER" not in recorder:
        fail("native StartDevice diagnostics must persist on the device driver registry key")
    writes = (
        'WriteRegistryDWORD(deviceKey, L"NativeStartStatus", &statusValue)',
        'WriteRegistryDWORD(deviceKey, L"NativeStartDetail", &detail)',
        'WriteRegistryDWORD(deviceKey, L"NativeStartStage", &stageValue)',
    )
    write_offsets = [compact_code(recorder).find(compact_code(write)) for write in writes]
    if any(offset < 0 for offset in write_offsets) or write_offsets != sorted(write_offsets):
        fail("native StartDevice diagnostic must write status/detail before the stage commit marker")
    if recorder.count("ZwClose(deviceKey)") != 1:
        fail("native StartDevice diagnostic must close exactly one device registry handle")
    if not re.search(
        r"\bVOID\s+VioGpuDod::RecordNativeStartDiagnostic\s*\(", VIOGPU_SOURCE
    ):
        fail("native StartDevice diagnostic must remain a best-effort void operation")

    start = function_body("VioGpuDod::StartDevice", VIOGPU_CODE)
    if not re.search(
        r"VIOGPU_RECORD_NATIVE_START\s*\(\s*this\s*,\s*VioGpuNativeStartComplete\s*,"
        r"\s*STATUS_SUCCESS\s*,\s*VioGpuNativeStartDetailNone\s*\)",
        start,
        re.DOTALL,
    ):
        fail("successful StartDevice must publish the terminal native-start breadcrumb")
    unwind = function_body("VioGpuDod::UnwindFailedStart", VIOGPU_CODE)
    if "VIOGPU_RECORD_NATIVE_START" in unwind:
        fail("failed-start unwind must not overwrite the root-cause native-start breadcrumb")

    if not START_DIAGNOSTIC_SCRIPT_PATH.is_file():
        fail("native StartDevice diagnostic reader is missing")
    reader = START_DIAGNOSTIC_SCRIPT_PATH.read_text(encoding="utf-8")
    reader_stage_block = re.search(
        r"^\$stageNames\s*=\s*@\{(?P<body>.*?)^\}", reader, re.MULTILINE | re.DOTALL
    )
    if reader_stage_block is None:
        fail("native StartDevice diagnostic reader stage map is missing")
    reader_stages = {
        int(value, 16): name
        for value, name in re.findall(
            r"^\s*(0x[0-9A-Fa-f]+)\s*=\s*'([A-Za-z0-9]+)'\s*$",
            reader_stage_block.group("body"),
            re.MULTILINE,
        )
    }
    expected_reader_stages = {
        value: name.removeprefix("VioGpuNativeStart") for name, value in expected_stages.items()
    }
    if reader_stages != expected_reader_stages:
        fail(f"native StartDevice diagnostic reader stage map drifted: {reader_stages}")
    for value_name in ("NativeStartStage", "NativeStartStatus", "NativeStartDetail"):
        if reader.count(value_name) < 2:
            fail(f"native StartDevice diagnostic reader must require and report {value_name}")
    if re.search(r"\b(?:Set-ItemProperty|New-ItemProperty|Remove-ItemProperty|pnputil|devcon)\b", reader, re.IGNORECASE):
        fail("native StartDevice diagnostic reader must remain read-only")


def check_native_query_adapter_info_diagnostics() -> None:
    declaration = canonical_code(VIOGPU_HEADER_SOURCE)
    expected_declaration = (
        "VOIDRecordNativeQueryAdapterInfoDiagnostic(_In_UINTtype,_In_NTSTATUSstatus,"
        "_In_UINTinputDataSize,_In_UINToutputDataSize);"
    )
    if declaration.count(expected_declaration) != 1:
        fail("adapter must declare exactly one persistent QueryAdapterInfo diagnostic recorder")

    recorder = function_body("VioGpuDod::RecordNativeQueryAdapterInfoDiagnostic", VIOGPU_SOURCE)
    if "IoOpenDeviceRegistryKey" not in recorder or "PLUGPLAY_REGKEY_DRIVER" not in recorder:
        fail("QueryAdapterInfo diagnostics must persist on the device driver registry key")
    writes = (
        'WriteRegistryDWORD(deviceKey, L"NativeQueryAdapterInfoStatus", &statusValue)',
        'WriteRegistryDWORD(deviceKey, L"NativeQueryAdapterInfoInputSize", &inputSizeValue)',
        'WriteRegistryDWORD(deviceKey, L"NativeQueryAdapterInfoOutputSize", &outputSizeValue)',
        'WriteRegistryDWORD(deviceKey, L"NativeQueryAdapterInfoType", &typeValue)',
    )
    write_offsets = [compact_code(recorder).find(compact_code(write)) for write in writes]
    if any(offset < 0 for offset in write_offsets) or write_offsets != sorted(write_offsets):
        fail("QueryAdapterInfo diagnostics must write status/sizes before the type commit marker")
    if recorder.count("ZwClose(deviceKey)") != 1:
        fail("QueryAdapterInfo diagnostics must close exactly one device registry handle")
    if not re.search(
        r"\bVOID\s+VioGpuDod::RecordNativeQueryAdapterInfoDiagnostic\s*\(", VIOGPU_SOURCE
    ):
        fail("QueryAdapterInfo diagnostics must remain a best-effort void operation")

    query_body = function_body("VioGpuWddmQueryAdapterInfo", WDDM_DDI_CODE)
    query = canonical_code(query_body)
    for fragment in (
        "NTSTATUSstatus=STATUS_NOT_SUPPORTED;",
        "status=QueryUmdPrivateInfo(adapter,pQueryAdapterInfo);",
        "status=QuerySegment(adapter,pQueryAdapterInfo);",
        "static_cast<UINT>(pQueryAdapterInfo->Type)==24||static_cast<UINT>(pQueryAdapterInfo->Type)==25",
        "if(pQueryAdapterInfo->OutputDataSize!=0&&pQueryAdapterInfo->pOutputData==NULL){status=STATUS_INVALID_PARAMETER;}",
        "if(pQueryAdapterInfo->OutputDataSize!=0){RtlZeroMemory(pQueryAdapterInfo->pOutputData,pQueryAdapterInfo->OutputDataSize);}",
        "status=STATUS_SUCCESS;",
        "status=VioGpuDodQueryAdapterInfo(hAdapter,pQueryAdapterInfo);",
        "if(!NT_SUCCESS(status)){adapter->RecordNativeQueryAdapterInfoDiagnostic("
        "pQueryAdapterInfo->Type,status,pQueryAdapterInfo->InputDataSize,"
        "pQueryAdapterInfo->OutputDataSize);}",
        "returnstatus;",
    ):
        if query.count(fragment) != 1:
            fail(f"QueryAdapterInfo must retain one routed and diagnosed result path: {fragment}")
    if query.count("return") != 2:
        fail("QueryAdapterInfo may return early only for an invalid adapter/argument pair")

    reader = START_DIAGNOSTIC_SCRIPT_PATH.read_text(encoding="utf-8")
    for value_name in (
        "NativeQueryAdapterInfoType",
        "NativeQueryAdapterInfoStatus",
        "NativeQueryAdapterInfoInputSize",
        "NativeQueryAdapterInfoOutputSize",
    ):
        if reader.count(value_name) < 2:
            fail(f"native diagnostic reader must optionally report {value_name}")


def check_native_present_diagnostics() -> None:
    expected_reasons = {
        "VioGpuWddmPresentDiagnosticNone": 0,
        "VioGpuWddmPresentDiagnosticNativeSourceIdentity": 1,
        "VioGpuWddmPresentDiagnosticGdiSourcePlacement": 2,
        "VioGpuWddmPresentDiagnosticGdiSourceIdentity": 3,
        "VioGpuWddmPresentDiagnosticSourceObject": 4,
        "VioGpuWddmPresentDiagnosticDestinationObject": 5,
        "VioGpuWddmPresentDiagnosticSourcePlacement": 6,
        "VioGpuWddmPresentDiagnosticDestinationBacking": 7,
        "VioGpuWddmPresentDiagnosticDestinationPlacement": 8,
        "VioGpuWddmPresentDiagnosticGeometry": 9,
        "VioGpuWddmPresentDiagnosticSourcePrepatch": 10,
        "VioGpuWddmPresentDiagnosticDestinationPrepatch": 11,
        "VioGpuWddmPresentDiagnosticContextReference": 12,
        "VioGpuWddmPresentDiagnosticSourceReference": 13,
        "VioGpuWddmPresentDiagnosticDestinationReference": 14,
        "VioGpuWddmPresentDiagnosticSourceLifecycle": 15,
        "VioGpuWddmPresentDiagnosticDestinationLifecycle": 16,
        "VioGpuWddmPresentDiagnosticTransactionReference": 17,
        "VioGpuWddmPresentDiagnosticTransactionRegistration": 18,
        "VioGpuWddmPresentDiagnosticContextPublication": 19,
    }
    observed_reasons = {
        name: int(value)
        for name, value in re.findall(
            r"\b(VioGpuWddmPresentDiagnostic[A-Za-z0-9]+)\s*=\s*([0-9]+)\s*,",
            WDDM_DDI_HEADER_SOURCE,
        )
    }
    if observed_reasons != expected_reasons:
        fail(f"Present diagnostic reason ABI drifted: {observed_reasons}")

    expected_fields = [
        "ContextType",
        "PresentFlags",
        "SubRectCount",
        "MultipassOffset",
        "SourceFlags",
        "DestinationFlags",
        "SourceHostState",
        "DestinationHostState",
        "SourceResource2DState",
        "DestinationResource2DState",
        "SourcePlacementState",
        "DestinationPlacementState",
        "SourceFormat",
        "DestinationFormat",
        "SourceWidth",
        "SourceHeight",
        "SourcePitch",
        "DestinationWidth",
        "DestinationHeight",
        "DestinationPitch",
        "SourceAllocationListValue",
        "DestinationAllocationListValue",
        "SourceResourceId",
        "DestinationResourceId",
        "SourceRectLeft",
        "SourceRectTop",
        "SourceRectRight",
        "SourceRectBottom",
        "DestinationRectLeft",
        "DestinationRectTop",
        "DestinationRectRight",
        "DestinationRectBottom",
    ]
    diagnostic_matches = re.findall(
        r"\bstruct\s+VIOGPU_NATIVE_PRESENT_DIAGNOSTIC\s*\{(?P<body>.*?)\}\s*;",
        VIOGPU_HEADER_SOURCE,
        re.DOTALL,
    )
    if len(diagnostic_matches) != 1:
        fail("adapter must expose exactly one persistent Present diagnostic record")
    observed_fields = re.findall(r"\bDWORD\s+([A-Za-z0-9_]+)\s*;", diagnostic_matches[0])
    if observed_fields != expected_fields:
        fail(f"Present diagnostic record ABI drifted: {observed_fields}")

    adapter_header = canonical_code(VIOGPU_HEADER_SOURCE)
    declaration = (
        "VOIDRecordNativePresentDiagnostic(_In_DWORDreason,_In_NTSTATUSstatus,"
        "_In_constVIOGPU_NATIVE_PRESENT_DIAGNOSTIC*diagnostic);"
    )
    if adapter_header.count(declaration) != 1 or \
       adapter_header.count("volatileLONGm_NativePresentDiagnosticRecorded;") != 1 or \
       adapter_header.count("volatileLONGm_HardwareResetCallerRva;") != 1 or \
       adapter_header.count("volatileLONGm_NativeSubmissionFaultDiagnosticRecorded;") != 1 or \
       adapter_header.count("volatileLONGm_NativeSubmissionFaultCallerRva;") != 1 or \
       adapter_header.count("volatileLONGm_NativeSubmissionFaultExecutionDiagnosticState;") != 1 or \
       adapter_header.count("volatileLONGm_NativeSubmissionFaultPresentSubmitStage;") != 1 or \
       adapter_header.count("volatileLONGm_NativeSubmissionFaultPresentSubmitStatus;") != 1 or \
       adapter_header.count("volatileLONGm_NativeSubmissionFaultPresentSubmitDetail;") != 1:
        fail("adapter must retain one first-failure Present diagnostic recorder")
    constructor = canonical_code(function_body("VioGpuDod::VioGpuDod", VIOGPU_CODE))
    if constructor.count("m_NativePresentDiagnosticRecorded=0;") != 1 or \
       constructor.count("m_HardwareResetCallerRva=0;") != 1 or \
       constructor.count("m_NativeSubmissionFaultDiagnosticRecorded=0;") != 1 or \
       constructor.count("m_NativeSubmissionFaultCallerRva=0;") != 1 or \
       constructor.count("m_NativeSubmissionFaultExecutionDiagnosticState=0;") != 1 or \
       constructor.count("m_NativeSubmissionFaultPresentSubmitStage=0;") != 1 or \
       constructor.count("m_NativeSubmissionFaultPresentSubmitStatus=0;") != 1 or \
       constructor.count("m_NativeSubmissionFaultPresentSubmitDetail=0;") != 1:
        fail("adapter construction must reset the first-failure Present diagnostic claim")
    start_recorder = canonical_code(function_body("VioGpuDod::RecordNativeStartDiagnostic", VIOGPU_SOURCE))
    require_order(
        start_recorder,
        (
            "stage==VioGpuNativeStartEntered",
            "InterlockedExchange(&m_NativePresentDiagnosticRecorded,2);",
            "InterlockedExchange(&m_NativePresentExecutionDiagnosticRecorded,2);",
            "InterlockedExchange(&m_NativeSubmissionFaultDiagnosticRecorded,2);",
            'ReadRegistryDWORD(deviceKey,L"NativePresentDiagnosticEpoch",&previousPresentEpoch)',
            'WriteRegistryDWORD(deviceKey,L"NativePresentDiagnosticEpoch",&presentEpochInvalid)',
            'WriteRegistryDWORD(deviceKey,L"NativePresentReason",&presentReason)',
            'WriteRegistryDWORD(deviceKey,L"NativePresentExecuteStage",&presentExecuteStage)',
            'WriteRegistryDWORD(deviceKey,L"NativePresentDiagnosticEpoch",&presentEpochCommitted)',
            "InterlockedExchange(&m_NativeSubmissionFaultCallerRva,0);",
            "InterlockedExchange(&m_NativeSubmissionFaultExecutionDiagnosticState,0);",
            "InterlockedExchange(&m_NativeSubmissionFaultPresentSubmitStage,0);",
            "InterlockedExchange(&m_NativeSubmissionFaultPresentSubmitStatus,0);",
            "InterlockedExchange(&m_NativeSubmissionFaultPresentSubmitDetail,0);",
            "InterlockedExchange(&m_NativePresentDiagnosticRecorded,0);",
            "InterlockedExchange(&m_NativePresentExecutionDiagnosticRecorded,0);",
            "InterlockedExchange(&m_NativeSubmissionFaultDiagnosticRecorded,0);",
            'WriteRegistryDWORD(deviceKey,L"NativeStartStatus",&statusValue)',
            'WriteRegistryDWORD(deviceKey,L"NativeStartStage",&stageValue)',
        ),
        "StartDevice entry must disable diagnostics until both stale markers have been invalidated",
    )
    if start_recorder.count("if(NT_SUCCESS(presentEpochCommitWrite))") != 2 or \
       start_recorder.count('L"NativePresentDiagnosticEpoch"') != 3 or \
       "DWORDpreviousCommittedPresentEpoch=previousPresentEpoch&~1UL;" not in start_recorder or \
       "NT_SUCCESS(presentEpochReadStatus)||presentEpochReadStatus==STATUS_OBJECT_NAME_NOT_FOUND" not in start_recorder or \
       "previousCommittedPresentEpoch<=MAXULONG-2" not in start_recorder or \
       "previousCommittedPresentEpoch+1" not in start_recorder or \
       "previousCommittedPresentEpoch+2" not in start_recorder:
        fail("StartDevice must publish a monotonic odd-to-even Present diagnostic epoch before enabling writers")

    recorder_body = function_body("VioGpuDod::RecordNativePresentDiagnostic", VIOGPU_SOURCE)
    recorder = canonical_code(recorder_body)
    for fragment in (
        "InterlockedCompareExchange(&m_NativePresentDiagnosticRecorded,1,0)!=0",
        "IoOpenDeviceRegistryKey(m_pPhysicalDevice,PLUGPLAY_REGKEY_DRIVER,KEY_SET_VALUE,&deviceKey)",
        'WriteRegistryDWORD(deviceKey,L"NativePresentReason",&emptyReason)',
        "for(UINTindex=0;NT_SUCCESS(writeStatus)&&index<ARRAYSIZE(values);++index)",
        "WriteRegistryDWORD(deviceKey,values[index].Name,&value)",
        "NTSTATUSreasonWrite=NT_SUCCESS(writeStatus)?WriteRegistryDWORD(deviceKey,",
    ):
        if recorder.count(fragment) != 1:
            fail(f"Present diagnostics must retain one first-failure registry transaction: {fragment}")
    if recorder.count("InterlockedExchange(&m_NativePresentDiagnosticRecorded,2);") != 3 or \
       "InterlockedExchange(&m_NativePresentDiagnosticRecorded,0);" in recorder:
        fail("Present diagnostics must permanently consume the first-failure claim even when persistence fails")
    if recorder_body.count("ZwClose(deviceKey)") != 1:
        fail("Present diagnostics must close exactly one device registry handle")
    if not re.search(r"\bVOID\s+VioGpuDod::RecordNativePresentDiagnostic\s*\(", VIOGPU_SOURCE):
        fail("Present diagnostics must remain a best-effort void operation")

    for fragment in (
        "DWORDhardwareResetState=static_cast<DWORD>(QueryHardwareResetState());",
        "DWORDhardwareResetCallerRva="
        "static_cast<DWORD>(InterlockedCompareExchange(&m_HardwareResetCallerRva,0,0));",
        "InterlockedCompareExchange(&m_NativeSubmissionFaultDiagnosticRecorded,2,2)==2?1:0;",
        "InterlockedCompareExchange(&m_NativeSubmissionFaultCallerRva,0,0)",
        "InterlockedCompareExchange(&m_NativeSubmissionFaultExecutionDiagnosticState,0,0)",
        "InterlockedCompareExchange(&m_NativeSubmissionFaultPresentSubmitStage,0,0)",
        "InterlockedCompareExchange(&m_NativeSubmissionFaultPresentSubmitStatus,0,0)",
        "InterlockedCompareExchange(&m_NativeSubmissionFaultPresentSubmitDetail,0,0)",
    ):
        if recorder.count(fragment) != 1:
            fail(f"Present diagnostics must snapshot reset and first-fault provenance: {fragment}")

    value_names = [
        "NativePresentStatus",
        "NativePresentHardwareResetState",
        "NativePresentHardwareResetCallerRva",
        "NativePresentSubmissionFaultProvenanceValid",
        "NativePresentSubmissionFaultCallerRva",
        "NativePresentSubmissionFaultExecutionDiagnosticState",
        "NativePresentSubmissionFaultPresentSubmitStage",
        "NativePresentSubmissionFaultPresentSubmitStatus",
        "NativePresentSubmissionFaultPresentSubmitDetail",
    ] + [
        f"NativePresent{field.removeprefix('Present')}" for field in expected_fields
    ]
    for value_name in value_names:
        if recorder_body.count(f'L"{value_name}"') != 1:
            fail(f"Present diagnostic recorder must write exactly one {value_name} value")
    reason_write = compact_code('WriteRegistryDWORD(deviceKey, L"NativePresentReason", &reasonValue)')
    if recorder.count(reason_write) != 1:
        fail("Present diagnostic reason must be the single commit marker")
    require_order(
        recorder,
        (
            'WriteRegistryDWORD(deviceKey,L"NativePresentReason",&emptyReason)',
            "for(UINTindex=0;NT_SUCCESS(writeStatus)&&index<ARRAYSIZE(values);++index)",
            reason_write,
            "ZwClose(deviceKey);",
        ),
        "Present diagnostics must publish all values before their reason commit marker",
    )

    execution_fields = [
        "Stage",
        "Status",
        "Detail",
        "FenceId",
        "TransactionState",
        "ContextType",
        "SourceResourceId",
        "DestinationResourceId",
        "SourcePlacementState",
        "DestinationPlacementState",
        "SourceResource2DState",
        "DestinationResource2DState",
        "SourcePlacementOffsetLow",
        "SourcePlacementOffsetHigh",
        "DestinationPlacementOffsetLow",
        "DestinationPlacementOffsetHigh",
        "TransactionSourcePlacementOffsetLow",
        "TransactionSourcePlacementOffsetHigh",
        "TransactionDestinationPlacementOffsetLow",
        "TransactionDestinationPlacementOffsetHigh",
        "SourceResetGenerationLow",
        "SourceResetGenerationHigh",
        "DestinationResetGenerationLow",
        "DestinationResetGenerationHigh",
        "TransactionDestinationResetGenerationLow",
        "TransactionDestinationResetGenerationHigh",
    ]
    execution_matches = re.findall(
        r"\bstruct\s+VIOGPU_NATIVE_PRESENT_EXECUTION_DIAGNOSTIC\s*\{(?P<body>.*?)\}\s*;",
        VIOGPU_HEADER_SOURCE,
        re.DOTALL,
    )
    if len(execution_matches) != 1:
        fail("adapter must expose exactly one persistent Present execution diagnostic record")
    observed_execution_fields = re.findall(r"\bDWORD\s+([A-Za-z0-9_]+)\s*;", execution_matches[0])
    if observed_execution_fields != execution_fields:
        fail(f"Present execution diagnostic record ABI drifted: {observed_execution_fields}")

    execution_declaration = (
        "VOIDRecordNativePresentExecutionDiagnostic("
        "_In_constVIOGPU_NATIVE_PRESENT_EXECUTION_DIAGNOSTIC*diagnostic);"
    )
    reset_provenance_declaration = "VOIDRecordNativePresentExecutionResetProvenance(void);"
    if adapter_header.count("BOOLEANClaimNativePresentExecutionDiagnostic(void);") != 1 or \
       adapter_header.count(execution_declaration) != 1 or \
       adapter_header.count(reset_provenance_declaration) != 1 or \
       adapter_header.count("volatileLONGm_NativePresentExecutionDiagnosticRecorded;") != 1:
        fail("adapter must retain one first-failure Present execution diagnostic claim and recorder")
    if constructor.count("m_NativePresentExecutionDiagnosticRecorded=0;") != 1:
        fail("adapter construction must reset the Present execution diagnostic claim")
    execution_claim = canonical_code(
        function_body("VioGpuDod::ClaimNativePresentExecutionDiagnostic", VIOGPU_SOURCE)
    )
    if execution_claim.count(
        "returnInterlockedCompareExchange(&m_NativePresentExecutionDiagnosticRecorded,1,0)==0;"
    ) != 1:
        fail("Present execution diagnostics must claim the first failure before requesting reset")

    execution_recorder_body = function_body("VioGpuDod::RecordNativePresentExecutionDiagnostic", VIOGPU_SOURCE)
    execution_recorder = canonical_code(execution_recorder_body)
    for fragment in (
        "InterlockedCompareExchange(&m_NativePresentExecutionDiagnosticRecorded,1,1)!=1",
        "IoOpenDeviceRegistryKey(m_pPhysicalDevice,PLUGPLAY_REGKEY_DRIVER,KEY_SET_VALUE,&deviceKey)",
        'WriteRegistryDWORD(deviceKey,L"NativePresentExecuteStage",&emptyStage)',
        "for(UINTindex=0;NT_SUCCESS(writeStatus)&&index<ARRAYSIZE(values);++index)",
        "WriteRegistryDWORD(deviceKey,values[index].Name,&value)",
    ):
        if execution_recorder.count(fragment) != 1:
            fail(f"Present execution diagnostics must retain one first-failure registry transaction: {fragment}")
    if execution_recorder.count("InterlockedExchange(&m_NativePresentExecutionDiagnosticRecorded,2);") != 3 or \
       "InterlockedExchange(&m_NativePresentExecutionDiagnosticRecorded,0);" in execution_recorder:
        fail("Present execution diagnostics must permanently consume the first-failure claim after persistence failure")
    if execution_recorder_body.count("ZwClose(deviceKey)") != 1:
        fail("Present execution diagnostics must close exactly one device registry handle")
    execution_value_names = [f"NativePresentExecute{field}" for field in execution_fields]
    for value_name in execution_value_names[1:]:
        if execution_recorder_body.count(f'L"{value_name}"') != 1:
            fail(f"Present execution diagnostic recorder must write exactly one {value_name} value")
    if execution_recorder_body.count('L"NativePresentExecuteResetProvenanceValid"') != 1 or \
       "QueryHardwareResetState()" in execution_recorder_body or \
       "m_HardwareResetCallerRva" in execution_recorder_body:
        fail("Present execution core transaction must invalidate, but not wait for, reset provenance")
    execution_stage_write = compact_code(
        'WriteRegistryDWORD(deviceKey, L"NativePresentExecuteStage", &stageValue)'
    )
    if execution_recorder.count(execution_stage_write) != 1:
        fail("Present execution stage must be the single commit marker")
    require_order(
        execution_recorder,
        (
            'WriteRegistryDWORD(deviceKey,L"NativePresentExecuteStage",&emptyStage)',
            "for(UINTindex=0;NT_SUCCESS(writeStatus)&&index<ARRAYSIZE(values);++index)",
            execution_stage_write,
            "ZwClose(deviceKey);",
        ),
        "Present execution diagnostics must publish all values before their stage commit marker",
    )

    reset_provenance_body = function_body("VioGpuDod::RecordNativePresentExecutionResetProvenance", VIOGPU_SOURCE)
    reset_provenance = canonical_code(reset_provenance_body)
    for fragment in (
        "InterlockedCompareExchange(&m_NativePresentExecutionDiagnosticRecorded,2,2)!=2",
        "IoOpenDeviceRegistryKey(m_pPhysicalDevice,PLUGPLAY_REGKEY_DRIVER,KEY_SET_VALUE,&deviceKey)",
        "QueryHardwareResetState()",
        "InterlockedCompareExchange(&m_HardwareResetCallerRva,0,0)",
        'WriteRegistryDWORD(deviceKey,L"NativePresentExecuteHardwareResetState",&hardwareResetState)',
        'WriteRegistryDWORD(deviceKey,L"NativePresentExecuteHardwareResetCallerRva",&hardwareResetCallerRva)',
    ):
        if reset_provenance.count(fragment) != 1:
            fail(f"Present execution reset provenance must retain its optional transaction: {fragment}")
    if reset_provenance_body.count('L"NativePresentExecuteResetProvenanceValid"') != 2 or \
       reset_provenance_body.count("ZwClose(deviceKey)") != 1:
        fail("Present execution reset provenance must have one independent commit marker and one registry handle")
    require_order(
        reset_provenance,
        (
            'WriteRegistryDWORD(deviceKey,L"NativePresentExecuteResetProvenanceValid",&unavailable)',
            'WriteRegistryDWORD(deviceKey,L"NativePresentExecuteHardwareResetState",&hardwareResetState)',
            'WriteRegistryDWORD(deviceKey,L"NativePresentExecuteHardwareResetCallerRva",&hardwareResetCallerRva)',
            'WriteRegistryDWORD(deviceKey,L"NativePresentExecuteResetProvenanceValid",&available)',
            "ZwClose(deviceKey);",
        ),
        "Present execution reset provenance must publish its payload before its optional commit marker",
    )

    copy_probe_fields = [
        "FenceId",
        "SampleCount",
        "SourceRgbNonzero",
        "DestinationRgbNonzero",
        "SourceHash",
        "DestinationHash",
        "SourceFirstPixel",
        "DestinationFirstPixel",
        "SourceResourceId",
        "DestinationResourceId",
        "RectCount",
        "HostPresentCount",
        "HostPresentResult",
    ]
    copy_probe_matches = re.findall(
        r"\bstruct\s+VIOGPU_NATIVE_PRESENT_COPY_PROBE\s*\{(?P<body>.*?)\}\s*;",
        VIOGPU_HEADER_SOURCE,
        re.DOTALL,
    )
    if len(copy_probe_matches) != 1 or \
       re.findall(r"\bDWORD\s+([A-Za-z0-9_]+)\s*;", copy_probe_matches[0]) != copy_probe_fields:
        fail("adapter must expose one stable Present copy-probe record")
    copy_probe_declaration = (
        "VOIDRecordNativePresentCopyProbe(_In_constVIOGPU_NATIVE_PRESENT_COPY_PROBE*probe);"
    )
    for fragment in (
        copy_probe_declaration,
        "volatileLONGm_NativePresentCopyProbeState;",
        "volatileLONGm_NativePresentCopyProbeSequence;",
    ):
        if adapter_header.count(fragment) != 1:
            fail(f"adapter must retain one low-frequency Present copy probe: {fragment}")
    for fragment in (
        "m_NativePresentCopyProbeState=0;",
        "m_NativePresentCopyProbeSequence=0;",
    ):
        if constructor.count(fragment) != 1:
            fail(f"adapter construction must reset Present copy-probe state: {fragment}")
    for fragment in (
        "InterlockedExchange(&m_NativePresentCopyProbeState,3);",
        'WriteRegistryDWORD(deviceKey,L"NativePresentCopyProbeSequence",&presentCopyProbeSequence)',
        "InterlockedExchange(&m_NativePresentCopyProbeSequence,0);",
        "InterlockedExchange(&m_NativePresentCopyProbeState,0);",
    ):
        if start_recorder.count(fragment) != 1:
            fail(f"StartDevice must invalidate and reopen the Present copy probe: {fragment}")

    copy_probe_body = function_body("VioGpuDod::RecordNativePresentCopyProbe", VIOGPU_SOURCE)
    copy_probe = canonical_code(copy_probe_body)
    for fragment in (
        "probe==NULL||probe->SampleCount==0||probe->HostPresentCount==0",
        "LONGdesiredState=probe->SourceRgbNonzero==0?1:2;",
        "InterlockedCompareExchange(&m_NativePresentCopyProbeState,desiredState,0)",
        "InterlockedCompareExchange(&m_NativePresentCopyProbeState,desiredState,1)==1",
        "InterlockedIncrement(&m_NativePresentCopyProbeSequence)",
        'WriteRegistryDWORD(deviceKey,L"NativePresentCopyProbeSequence",&invalidSequence)',
        'WriteRegistryDWORD(deviceKey,L"NativePresentCopyProbeSequence",&sequence)',
    ):
        if copy_probe.count(fragment) != 1:
            fail(f"Present copy probe must retain one black-to-color registry transaction: {fragment}")
    for field in copy_probe_fields:
        if copy_probe_body.count(f'L"NativePresentCopyProbe{field}"') != 1:
            fail(f"Present copy probe must write exactly one NativePresentCopyProbe{field} value")
    if copy_probe_body.count('L"NativePresentCopyProbeState"') != 1 or \
       copy_probe_body.count("ZwClose(deviceKey)") != 1:
        fail("Present copy probe must publish one state payload and close one registry handle")
    require_order(
        copy_probe,
        (
            'WriteRegistryDWORD(deviceKey,L"NativePresentCopyProbeSequence",&invalidSequence)',
            "for(UINTindex=0;NT_SUCCESS(writeStatus)&&index<ARRAYSIZE(values);++index)",
            'WriteRegistryDWORD(deviceKey,L"NativePresentCopyProbeSequence",&sequence)',
            "ZwClose(deviceKey);",
        ),
        "Present copy probe must commit its sequence after every sampled value",
    )

    fault_notify_body = function_body("VioGpuDod::NotifyNativeSubmissionFault", VIOGPU_SOURCE)
    fault_notify = canonical_code(fault_notify_body)
    for fragment in (
        "InterlockedCompareExchange(&m_NativeSubmissionFaultDiagnosticRecorded,1,0)==0",
        "reinterpret_cast<ULONG_PTR>(_ReturnAddress())",
        "InterlockedCompareExchange(&m_NativePresentExecutionDiagnosticRecorded,0,0)",
        "InterlockedExchange(&m_NativeSubmissionFaultCallerRva,",
        "InterlockedExchange(&m_NativeSubmissionFaultExecutionDiagnosticState,executionDiagnosticState);",
        "InterlockedExchange(&m_NativeSubmissionFaultPresentSubmitStage,static_cast<LONG>(presentSubmitStage));",
        "InterlockedExchange(&m_NativeSubmissionFaultPresentSubmitStatus,static_cast<LONG>(presentSubmitStatus));",
        "InterlockedExchange(&m_NativeSubmissionFaultPresentSubmitDetail,static_cast<LONG>(presentSubmitDetail));",
        "KeMemoryBarrier();",
        "InterlockedExchange(&m_NativeSubmissionFaultDiagnosticRecorded,2);",
    ):
        if fault_notify.count(fragment) != 1:
            fail(f"submission-fault provenance must retain one nonpaged first-caller transaction: {fragment}")
    require_order(
        fault_notify,
        (
            "InterlockedCompareExchange(&m_NativeSubmissionFaultDiagnosticRecorded,1,0)==0",
            "InterlockedExchange(&m_NativeSubmissionFaultExecutionDiagnosticState,executionDiagnosticState);",
            "InterlockedExchange(&m_NativeSubmissionFaultPresentSubmitStage,static_cast<LONG>(presentSubmitStage));",
            "InterlockedExchange(&m_NativeSubmissionFaultPresentSubmitStatus,static_cast<LONG>(presentSubmitStatus));",
            "InterlockedExchange(&m_NativeSubmissionFaultPresentSubmitDetail,static_cast<LONG>(presentSubmitDetail));",
            "KeMemoryBarrier();",
            "InterlockedExchange(&m_NativeSubmissionFaultDiagnosticRecorded,2);",
            "RequestHardwareResetAtAnyIrql();",
        ),
        "submission-fault caller and claim provenance must commit before reset publication",
    )
    if "PAGED_CODE()" in fault_notify_body or "WriteRegistryDWORD(" in fault_notify_body or \
       VIOGPU_HEADER_SOURCE.count("__declspec(noinline) void NotifyNativeSubmissionFault(") != 1:
        fail("submission-fault provenance capture must remain nonpaged and preserve its caller frame")

    present = canonical_code(function_body("VioGpuWddmPresent", WDDM_DDI_CODE))
    for reason_name in tuple(expected_reasons)[1:]:
        if present.count(reason_name) != 1:
            fail(f"Present must map one rejection path to {reason_name}")
    if present.count("RecordPresentDiagnostic(context,present,source,destination,reason,status);") != 1 or \
       present.count("RecordPresentDiagnostic(context,present,source,destination,lateReason,status);") != 1:
        fail("Present must persist the first classified rejection before returning it")

    if not PRESENT_DIAGNOSTIC_SCRIPT_PATH.is_file():
        fail("native Present diagnostic reader is missing")
    reader = PRESENT_DIAGNOSTIC_SCRIPT_PATH.read_text(encoding="utf-8")
    reset_provenance_value_names = [
        "NativePresentExecuteResetProvenanceValid",
        "NativePresentExecuteHardwareResetState",
        "NativePresentExecuteHardwareResetCallerRva",
    ]
    for value_name in ["NativePresentDiagnosticEpoch", "NativePresentReason"] + value_names + \
            execution_value_names + reset_provenance_value_names:
        if reader.count(value_name) < 2:
            fail(f"native Present diagnostic reader must require and report {value_name}")
    reader_reason_block = re.search(
        r"^\$reasonNames\s*=\s*@\{(?P<body>.*?)^\}", reader, re.MULTILINE | re.DOTALL
    )
    if reader_reason_block is None:
        fail("native Present diagnostic reader reason map is missing")
    reader_reasons = {
        int(value): name
        for value, name in re.findall(
            r"^\s*([0-9]+)\s*=\s*'([A-Za-z0-9]+)'\s*$",
            reader_reason_block.group("body"),
            re.MULTILINE,
        )
    }
    expected_reader_reasons = {
        value: name.removeprefix("VioGpuWddmPresentDiagnostic")
        for name, value in expected_reasons.items()
        if value != 0
    }
    if reader_reasons != expected_reader_reasons:
        fail(f"native Present diagnostic reader reason map drifted: {reader_reasons}")
    for fragment in (
        "Get-ItemPropertyValue -LiteralPath $registryPath -Name 'NativePresentDiagnosticEpoch'",
        "if ($epoch -eq 0 -or ($epoch -band 1) -ne 0 -or",
        "$epochBefore -ne $epoch -or $epochAfter -ne $epoch",
        "$reasonBefore -ne $reason -or $reasonAfter -ne $reason",
        "$executeStageBefore -ne $executeStage -or $executeStageAfter -ne $executeStage",
        "if ($reason -eq 0 -and $executeStage -eq 0)",
        "if ($reason -ne 0)",
        "if ($submissionFaultProvenanceAvailable)",
        "if ($executeStage -ne 0)",
        "if ($resetProvenanceAvailable)",
    ):
        if reader.count(fragment) < 1:
            fail(f"native Present diagnostic reader must honor independent commit markers: {fragment}")
    fixture = PRESENT_DIAGNOSTIC_TEST_PATH.read_text(encoding="utf-8")
    for fragment in (
        "$values['NativePresentDiagnosticEpoch'] = 2",
        "$values['NativePresentSubmissionFaultCallerRva'] = 0x4321",
        "$result.SubmissionFaultExecutionDiagnosticState -ne 'Consumed (2)'",
        "$values['NativePresentSubmissionFaultPresentSubmitStage'] = 7",
        "$result.SubmissionFaultPresentSubmitStage -ne 'PassiveQueue (7)'",
        "$result.SubmissionFaultPresentSubmitStatus -ne '0xC00000A3'",
        "$result.SubmissionFaultPresentSubmitDetail -ne '0x00030201'",
        "$presentOnlyValues['NativePresentExecuteStage'] = 0",
        "NativePresentDiagnosticEpoch = 2",
        "NativePresentReason = 0",
        "$null -ne $presentOnly.PSObject.Properties['ExecuteStage']",
        "$null -ne $executionOnly.PSObject.Properties['Reason']",
        "$executionOnly.ExecuteResetProvenanceAvailable",
        "$null -ne $executionOnly.PSObject.Properties['ExecuteHardwareResetState']",
        "$stateTransitionValues['NativePresentExecuteStage'] = 22",
        "$stateTransition.ExecuteStage -ne 'StateTransition (22)'",
        "$staleEpochValues['NativePresentDiagnosticEpoch'] = 3",
        "throw 'Stale Present diagnostic epoch was not rejected.'",
        "EpochBefore = 2",
        "EpochAfter = 4",
        "throw 'Racing Present diagnostic marker snapshot was not rejected.'",
    ):
        if fixture.count(fragment) != 1:
            fail(f"Native Present decoder fixture must cover independent marker generations: {fragment}")
    if re.search(r"\b(?:Set-ItemProperty|New-ItemProperty|Remove-ItemProperty|pnputil|devcon)\b", reader, re.IGNORECASE):
        fail("native Present diagnostic reader must remain read-only")


def check_native_win7_driver_caps_contract() -> None:
    viogpu = canonical_code(VIOGPU_CODE)
    for fragment in (
        "staticconstULONGVIOGPU_WIN7_DRIVERCAPS_SIZE=FIELD_OFFSET(DXGK_DRIVERCAPS,PreemptionCaps);",
        "static_assert(VIOGPU_WIN7_DRIVERCAPS_SIZE==528,",
    ):
        if viogpu.count(fragment) != 1:
            fail(f"Native Context must pin the 528-byte Win7 DXGK_DRIVERCAPS prefix: {fragment}")

    helper = canonical_code(function_body("VioGpuQueryWin7DriverCaps", VIOGPU_CODE))
    for fragment in (
        "queryAdapterInfo->OutputDataSize<VIOGPU_WIN7_DRIVERCAPS_SIZE",
        "returnSTATUS_BUFFER_TOO_SMALL;",
        "RtlZeroMemory(driverCaps,VIOGPU_WIN7_DRIVERCAPS_SIZE);",
        "driverCaps->WDDMVersion=DXGKDDI_WDDMv1;",
        "driverCaps->HighestAcceptableAddress.QuadPart=(ULONG64)-1;",
        "driverCaps->MaxPointerWidth=POINTER_SIZE;",
        "driverCaps->MaxPointerHeight=POINTER_SIZE;",
        "driverCaps->PointerCaps.Color=1;",
        "driverCaps->PointerCaps.MaskedColor=1;",
    ):
        if helper.count(fragment) != 1:
            fail(f"Native Context Win7 DriverCaps output must retain its bounded legacy field contract: {fragment}")
    if helper.count("RtlZeroMemory(") != 1 or "sizeof(DXGK_DRIVERCAPS)" in helper:
        fail("Native Context must zero exactly the Win7 DriverCaps prefix, never the modern WDK structure")

    helper_fields = set(re.findall(r"\bdriverCaps->([A-Za-z_][A-Za-z0-9_]*)", helper))
    expected_helper_fields = {
        "WDDMVersion",
        "HighestAcceptableAddress",
        "MaxPointerWidth",
        "MaxPointerHeight",
        "PointerCaps",
    }
    if helper_fields != expected_helper_fields:
        fail(f"Native Context Win7 DriverCaps helper writes or reads fields outside its legacy prefix: {helper_fields}")

    query_body = function_body("VioGpuDod::QueryAdapterInfo", VIOGPU_CODE)
    driver_caps_case = re.search(
        r"case\s+DXGKQAITYPE_DRIVERCAPS\s*:\s*\{\s*"
        r"#if\s+defined\(VIOGPU_NATIVE_CONTEXT\)\s*"
        r"status\s*=\s*VioGpuQueryWin7DriverCaps\(pQueryAdapterInfo,\s*IsPointerEnabled\(\)\);\s*"
        r"#else(?P<display>.*?)#endif\s*break\s*;\s*\}",
        query_body,
        re.DOTALL,
    )
    if driver_caps_case is None:
        fail("DriverCaps dispatch must isolate the Win7 Native Context output from the display-only structure")
    display_caps = canonical_code(driver_caps_case.group("display"))
    for fragment in (
        "pQueryAdapterInfo->OutputDataSize<sizeof(DXGK_DRIVERCAPS)",
        "RtlZeroMemory(pDriverCaps,pQueryAdapterInfo->OutputDataSize);",
        "pDriverCaps->WDDMVersion=DXGKDDI_WDDMv1_2;",
        "pDriverCaps->SupportNonVGA=TRUE;",
        "pDriverCaps->SupportSmoothRotation=TRUE;",
    ):
        if display_caps.count(fragment) != 1:
            fail(f"display-only DriverCaps behavior must remain unchanged: {fragment}")

    wddm_query = canonical_code(function_body("VioGpuWddmQueryAdapterInfo", WDDM_DDI_CODE))
    for fragment in (
        "driverCaps->WDDMVersion=DXGKDDI_WDDMv1;",
        "driverCaps->GpuEngineTopology.NbAsymetricProcessingNodes=1;",
        "driverCaps->SchedulingCaps.MultiEngineAware=1;",
        "driverCaps->SchedulingCaps.PreemptionAware=0;",
        "driverCaps->SchedulingCaps.CancelCommandAware=0;",
    ):
        if wddm_query.count(fragment) != 1:
            fail(f"Native Context DriverCaps must retain its Win7 scheduler and topology output: {fragment}")
    wrapper_fields = set(re.findall(r"\bdriverCaps->([A-Za-z_][A-Za-z0-9_]*)", wddm_query))
    expected_wrapper_fields = {"WDDMVersion", "GpuEngineTopology", "SchedulingCaps"}
    if wrapper_fields != expected_wrapper_fields:
        fail(f"Native Context DriverCaps wrapper accesses a Win8-only field: {wrapper_fields}")


def check_registration_helper(sources: dict[Path, str]) -> None:
    helper_definitions = list(
        re.finditer(
            rf'\bextern\s+"C"\s+NTSTATUS\s+{REGISTRATION_HELPER}\s*\(', DRIVER_CODE
        )
    )
    if len(helper_definitions) != 1:
        fail("registration helper must have exactly one C-linkage definition")

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
        fail("registration helper must contain only the exact initialization and cleanup sequence")

    helper_occurrences = source_occurrences(sources, rf"\b{REGISTRATION_HELPER}\b")
    if len(helper_occurrences) != 2 or any(path != DRIVER_SOURCE_PATH for path, _ in helper_occurrences):
        locations = ", ".join(path.as_posix() for path, _ in helper_occurrences)
        fail(f"registration helper must occur only at its definition and DriverEntry call; found: {locations or 'none'}")

    driver_entry = canonical_code(function_body("DriverEntry"))
    if driver_entry != "PAGED_CODE();returnVioGpuWddmInitializeMiniport(driverObject,registryPath);":
        fail("DriverEntry must call the registration helper exactly once")

    initialize_calls = source_occurrences(sources, r"\bDxgkInitialize\s*\(")
    if len(initialize_calls) != 1:
        locations = ", ".join(path.as_posix() for path, _ in initialize_calls)
        fail(f"target must contain exactly one DxgkInitialize call; found: {locations or 'none'}")

    call_path, call_offset = initialize_calls[0]
    if call_path != DRIVER_SOURCE_PATH or not helper_start <= call_offset < helper_end:
        fail("the target's only DxgkInitialize call must be inside the registration helper")

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
        r"\binitialData\s*->\s*Version\s*=\s*DXGKDDI_INTERFACE_VERSION_WIN7\s*;", body
    )
    if len(version_assignment) != 1:
        fail("the legacy WDDMv1 callback table must assign DXGKDDI_INTERFACE_VERSION_WIN7 exactly once")

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
        "DxgkDdiNotifyAcpiEvent": "VioGpuWddmNotifyAcpiEvent",
        "DxgkDdiUnload": "VioGpuDodUnload",
        "DxgkDdiQueryInterface": "VioGpuDodQueryInterface",
        "DxgkDdiControlEtwLogging": "VioGpuWddmControlEtwLogging",
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
        "DxgkDdiSetPalette": "VioGpuWddmSetPalette",
        "DxgkDdiRender": "VioGpuWddmRender",
        "DxgkDdiRenderKm": "VioGpuWddmRenderKm",
        "DxgkDdiPresent": "VioGpuWddmPresent",
        "DxgkDdiPatch": "VioGpuWddmPatch",
        "DxgkDdiSubmitCommand": "VioGpuWddmSubmitCommand",
        "DxgkDdiPreemptCommand": "VioGpuWddmPreemptCommand",
        "DxgkDdiQueryCurrentFence": "VioGpuWddmQueryCurrentFence",
        "DxgkDdiResetFromTimeout": "VioGpuWddmResetFromTimeout",
        "DxgkDdiRestartFromTimeout": "VioGpuWddmRestartFromTimeout",
        "DxgkDdiCollectDbgInfo": "VioGpuWddmCollectDbgInfo",
        "DxgkDdiSetPointerPosition": "VioGpuDodSetPointerPosition",
        "DxgkDdiSetPointerShape": "VioGpuDodSetPointerShape",
        "DxgkDdiEscape": "VioGpuWddmEscape",
        "DxgkDdiIsSupportedVidPn": "VioGpuDodIsSupportedVidPn",
        "DxgkDdiRecommendFunctionalVidPn": "VioGpuDodRecommendFunctionalVidPn",
        "DxgkDdiEnumVidPnCofuncModality": "VioGpuDodEnumVidPnCofuncModality",
        "DxgkDdiSetVidPnSourceAddress": "VioGpuWddmSetVidPnSourceAddress",
        "DxgkDdiSetVidPnSourceVisibility": "VioGpuDodSetVidPnSourceVisibility",
        "DxgkDdiCommitVidPn": "VioGpuDodCommitVidPn",
        "DxgkDdiUpdateActiveVidPnPresentPath": "VioGpuDodUpdateActiveVidPnPresentPath",
        "DxgkDdiRecommendMonitorModes": "VioGpuDodRecommendMonitorModes",
        "DxgkDdiGetScanLine": "VioGpuWddmGetScanLine",
        "DxgkDdiControlInterrupt": "VioGpuWddmControlInterrupt",
        "DxgkDdiQueryVidPnHWCapability": "VioGpuDodQueryVidPnHWCapability",
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
        "initialData->Version = DXGKDDI_INTERFACE_VERSION_WIN7;",
        *(f"initialData->{member} = {callback};" for member, callback in callbacks.items()),
    ]
    if re.sub(r"\s+", " ", body).strip() != " ".join(expected_statements):
        fail("callback table must contain only the exact expected initialization statement sequence")

    if re.search(r"\bDxgkDdiPresentDisplayOnly\b", body):
        fail("full miniport must not register the KMDOD-only PresentDisplayOnly callback")

    for forbidden in (
        "DxgkDdiCancelCommand",
        "DxgkDdiQueryDependentEngineGroup",
        "DxgkDdiQueryEngineStatus",
        "DxgkDdiResetEngine",
        "DxgkDdiStopDeviceAndReleasePostDisplayOwnership",
        "DxgkDdiSystemDisplayEnable",
        "DxgkDdiSystemDisplayWrite",
    ):
        if forbidden in body:
            fail(f"legacy Win7 runtime registration must not expose Win8-only callback {forbidden}")


def check_vidpn_mode_contract() -> None:
    signal_info = canonical_code(function_body("BuildVideoSignalInfo", VIOGPU_CODE))
    require_order(
        signal_info,
        (
            "pVideoSignalInfo->TotalSize.cx=pModeInfo->VisScreenWidth;",
            "pVideoSignalInfo->TotalSize.cy=pModeInfo->VisScreenHeight;",
            "pVideoSignalInfo->ActiveSize=pVideoSignalInfo->TotalSize;",
        ),
        "video signal active size must be derived after constructing the total size",
    )
    if signal_info.count("pVideoSignalInfo->ActiveSize=pVideoSignalInfo->TotalSize;") != 1:
        fail("video signal construction must assign its active size exactly once")
    require_order(
        signal_info,
        (
            "pVideoSignalInfo->VSyncFreq.Numerator=VIOGPU_DEFAULT_REFRESH_HZ;",
            "pVideoSignalInfo->VSyncFreq.Denominator=1;",
            "pVideoSignalInfo->HSyncFreq.Numerator=pModeInfo->VisScreenHeight*VIOGPU_DEFAULT_REFRESH_HZ;",
            "pVideoSignalInfo->HSyncFreq.Denominator=1;",
            "pVideoSignalInfo->PixelRate=static_cast<UINT64>(pModeInfo->VisScreenWidth)*pModeInfo->VisScreenHeight*VIOGPU_DEFAULT_REFRESH_HZ;",
        ),
        "target timing frequencies must form one exact progressive 60 Hz signal",
    )
    if signal_info.count("staticconstUINTVIOGPU_DEFAULT_REFRESH_HZ=60;") != 1:
        fail("video signal construction must define its 60 Hz timing basis exactly once")
    if "D3DKMDT_FREQUENCY_NOTSPECIFIED" in signal_info:
        fail("full-WDDM target and monitor modes must not publish unspecified timing frequencies")

    target_modes = canonical_code(function_body("AddSingleTargetMode", VIOGPU_CODE))
    if target_modes.count("m_pHWDevice->GetModeInfo(ModeIndex)") != 1:
        fail("target mode selection must search only for a pinned source")
    if target_modes.count("m_pHWDevice->GetModeInfo(m_pHWDevice->GetCurrentModeIndex())") != 1:
        fail("unpinned target mode selection must use the selected current host mode")
    if "m_pHWDevice->GetModeInfo(SourceId)" in target_modes:
        fail("target mode selection must not use the source id as a mode index")
    if "m_CurrentMode.DispInfo" in target_modes:
        fail("unpinned target mode selection must not use stale post-display dimensions")
    if "VideoSignalInfo.ActiveSize=" in target_modes:
        fail("target mode construction must leave complete signal construction to BuildVideoSignalInfo")
    if target_modes.count("pVidPnTargetModeSetInterface->pfnCreateNewModeInfo(") != 1:
        fail("the cofunctional target set must create exactly one target mode")
    if target_modes.count("pVidPnTargetModeSetInterface->pfnAddMode(") != 1:
        fail("the cofunctional target set must add exactly one target mode")
    if target_modes.count("pVidPnTargetModeInfo->Preference=D3DKMDT_MP_PREFERRED;") != 1:
        fail("the single target mode must be preferred")
    if "D3DKMDT_MP_NOTPREFERRED" in target_modes:
        fail("the single target mode must not be marked non-preferred")
    for required in (
        "pVidPnPinnedSourceModeInfo->Type!=D3DKMDT_RMT_GRAPHICS",
        "candidate->VisScreenWidth==pVidPnPinnedSourceModeInfo->Format.Graphics.VisibleRegionSize.cx",
        "candidate->VisScreenHeight==pVidPnPinnedSourceModeInfo->Format.Graphics.VisibleRegionSize.cy",
        "returnAddStatus==STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET?STATUS_SUCCESS:AddStatus;",
    ):
        if target_modes.count(required) != 1:
            fail(f"target mode selection is missing its exact cofunctional contract: {required}")
    if target_modes.count("returnSTATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;") != 2:
        fail("target mode selection must reject non-graphics and unmatched pinned source modes")

    monitor_modes = canonical_code(function_body("AddSingleMonitorMode", VIOGPU_CODE))
    for required in (
        "pVbeModeInfo=m_pHWDevice->GetModeInfo(m_pHWDevice->GetCurrentModeIndex());",
        "if(Idx==m_pHWDevice->GetCurrentModeIndex())",
        "pMonitorSourceMode->Preference=D3DKMDT_MP_PREFERRED;",
        "pMonitorSourceMode->Preference=D3DKMDT_MP_NOTPREFERRED;",
    ):
        if required not in monitor_modes:
            fail(f"monitor mode selection must prefer the selected current host mode: {required}")
    if "VisScreenWidth==NOM_WIDTH_SIZE" in monitor_modes or "VisScreenHeight==NOM_HEIGHT_SIZE" in monitor_modes:
        fail("monitor mode preference must not be hard-coded to the nominal resolution")

    display_info = canonical_code(function_body("VioGpuAdapter::GetDisplayInfo", VIOGPU_CODE))
    require_order(
        display_info,
        (
            "SetCustomDisplay((USHORT)xres,(USHORT)yres);",
            "SetCurrentModeIndex(m_CustomModeIndex);",
            "returnTRUE;",
        ),
        "a valid host scanout must become the selected current custom mode",
    )

    build_modes = canonical_code(function_body("VioGpuAdapter::BuildModeList", VIOGPU_CODE))
    persistent = "SetCustomDisplay(m_pVioGpuDod->GetPersistentDispMode0Width(),m_pVioGpuDod->GetPersistentDispMode0Height());SetCurrentModeIndex(m_CustomModeIndex);"
    if build_modes.count(persistent) != 1:
        fail("a persistent display override must select the custom mode slot")

    escape = canonical_code(function_body("VioGpuAdapter::Escape", VIOGPU_CODE))
    custom_escape = "SetCustomDisplay(pVioGpuEscape->Resolution.XResolution,pVioGpuEscape->Resolution.YResolution);SetCurrentModeIndex(m_CustomModeIndex);"
    if escape.count(custom_escape) != 1:
        fail("a runtime custom display override must select the custom mode slot")

    is_supported = function_body("VioGpuDodIsSupportedVidPn", DOD_DRIVER_CODE)
    inactive_blocks = [
        body
        for condition, body, _, _ in if_blocks(is_supported)
        if canonical_code(condition) == "!pVioGpuDod->IsDriverActive()"
    ]
    if len(inactive_blocks) != 1:
        fail("IsSupportedVidPn must contain exactly one inactive-adapter branch")
    inactive = canonical_code(inactive_blocks[0])
    require_order(
        inactive,
        (
            "pIsSupportedVidPn->IsVidPnSupported=FALSE;",
            "returnSTATUS_SUCCESS;",
        ),
        "inactive IsSupportedVidPn must report unsupported without failing the DDI",
    )
    if "returnSTATUS_UNSUCCESSFUL;" in inactive:
        fail("inactive IsSupportedVidPn must not return generic failure")

    for ddi_name in (
        "VioGpuDodSetPointerPosition",
        "VioGpuDodSetPointerShape",
        "VioGpuDodEnumVidPnCofuncModality",
    ):
        ddi = function_body(ddi_name, DOD_DRIVER_CODE)
        inactive_blocks = [
            body
            for condition, body, _, _ in if_blocks(ddi)
            if canonical_code(condition) == "!pVioGpuDod->IsDriverActive()"
        ]
        if len(inactive_blocks) != 1:
            fail(f"{ddi_name} must contain exactly one inactive-adapter branch")
        inactive = canonical_code(inactive_blocks[0])
        if "returnSTATUS_SUCCESS;" not in inactive or "returnSTATUS_UNSUCCESSFUL;" in inactive:
            fail(f"{ddi_name} must drop transient inactive-adapter work without failing the DDI")

    for method_name in ("VioGpuDod::SetPointerPosition", "VioGpuDod::SetPointerShape"):
        pointer = canonical_code(function_body(method_name, VIOGPU_CODE))
        if "returnSTATUS_NOT_IMPLEMENTED;" in pointer or not pointer.endswith("returnSTATUS_SUCCESS;"):
            fail(f"{method_name} must accept updates when hardware pointer support is not advertised")

    update_cursor = canonical_code(function_body("VioGpuAdapter::UpdateCursor", VIOGPU_CODE))
    for fragment in (
        "pSetPointerShape==NULL||pCurrentMode==NULL||pSetPointerShape->pPixels==NULL",
        "pSetPointerShape->Flags.Monochrome",
        "RtlZeroMemory(m_pCursorBuf->GetVirtualAddress(),POINTER_SIZE*POINTER_SIZE*sizeof(ULONG));",
        "constBYTE*xorMask=andMask+static_cast<SIZE_T>(pSetPointerShape->Pitch)*pSetPointerShape->Height;",
        "destination[y*POINTER_SIZE+x]=andBit==0?(xorBit!=0?0xFFFFFFFF:0xFF000000):0;",
    ):
        if fragment not in update_cursor:
            fail(f"UpdateCursor must convert bounded monochrome AND/XOR masks: {fragment}")


def check_legacy_runtime_callback_contract() -> None:
    header = canonical_code(WDDM_DDI_HEADER_CODE)
    for declaration in (
        "DXGKDDI_NOTIFY_ACPI_EVENTVioGpuWddmNotifyAcpiEvent;",
        "DXGKDDI_CONTROL_ETW_LOGGINGVioGpuWddmControlEtwLogging;",
        "DXGKDDI_SETPALETTEVioGpuWddmSetPalette;",
        "DXGKDDI_GETSCANLINEVioGpuWddmGetScanLine;",
        "DXGKDDI_CONTROLINTERRUPTVioGpuWddmControlInterrupt;",
        "DXGKDDI_RENDERKMVioGpuWddmRenderKm;",
    ):
        if header.count(declaration) != 1:
            fail(f"legacy runtime callback must have one WDK-typed declaration: {declaration}")

    notify_acpi = canonical_code(function_body("VioGpuWddmNotifyAcpiEvent", WDDM_DDI_CODE))
    for fragment in (
        "PAGED_CODE();",
        "miniportDeviceContext==NULL||acpiFlags==NULL",
        "*acpiFlags=0;",
        "returnSTATUS_SUCCESS;",
    ):
        if notify_acpi.count(fragment) != 1:
            fail(f"legacy ACPI callback must acknowledge after clearing flags: {fragment}")

    etw = canonical_code(function_body("VioGpuWddmControlEtwLogging", WDDM_DDI_CODE))
    for fragment in (
        "PAGED_CODE();",
        "UNREFERENCED_PARAMETER(enable);",
        "UNREFERENCED_PARAMETER(flags);",
        "UNREFERENCED_PARAMETER(level);",
    ):
        if etw.count(fragment) != 1:
            fail(f"legacy ETW callback must remain a side-effect-free no-op: {fragment}")

    set_palette = canonical_code(function_body("VioGpuWddmSetPalette", WDDM_DDI_CODE))
    for fragment in (
        "hAdapter==NULL||setPalette==NULL",
        "setPalette->VidPnSourceId!=0",
        "returnSTATUS_SUCCESS;",
    ):
        if set_palette.count(fragment) != 1:
            fail(f"true-color-only palette callback must validate the source and acknowledge the no-op: {fragment}")

    get_scan_line = canonical_code(function_body("VioGpuWddmGetScanLine", WDDM_DDI_CODE))
    for fragment in (
        "hAdapter==NULL||getScanLine==NULL",
        "getScanLine->InVerticalBlank=FALSE;",
        "getScanLine->ScanLine=0;",
        "VioGpuDod*adapter=reinterpret_cast<VioGpuDod*>(hAdapter);",
        "returnadapter->GetScanLine(getScanLine);",
    ):
        if get_scan_line.count(fragment) != 1:
            fail(f"scanline callback must initialize output and use the active mode timing path: {fragment}")

    control_interrupt = canonical_code(function_body("VioGpuWddmControlInterrupt", WDDM_DDI_CODE))
    for fragment in (
        "hAdapter==NULL",
        "VioGpuDod*adapter=reinterpret_cast<VioGpuDod*>(hAdapter);",
        "returnadapter->ControlInterrupt(interruptType,enableInterrupt);",
    ):
        if control_interrupt.count(fragment) != 1:
            fail(f"interrupt-control callback must route supported DMA interrupt control: {fragment}")

    scanline = canonical_code(function_body("VioGpuDod::GetScanLine", VIOGPU_CODE))
    for fragment in (
        "pGetScanLine==NULL||pGetScanLine->VidPnTargetId!=0",
        "KeQueryPerformanceCounter(&frequency)",
        "pGetScanLine->InVerticalBlank=scanLine>=height;",
        "pGetScanLine->ScanLine=static_cast<ULONG>(min(scanLine,totalLines-1));",
        "returnSTATUS_SUCCESS;",
    ):
        if fragment not in scanline:
            fail(f"VioGpuDod::GetScanLine must publish bounded software timing: {fragment}")

    adapter_interrupt = canonical_code(function_body("VioGpuAdapter::ControlInterrupt", VIOGPU_CODE))
    for fragment in (
        "if(!m_bVirtioInitialized||!m_bQueuesInitialized||m_pVioGpuDod==NULL||!m_pVioGpuDod->IsHardwareInit())",
        "if(enableInterrupt)",
        "m_CtrlQueue.EnableInterrupt()",
        "m_CursorQueue.EnableInterrupt()",
        "InterlockedExchange(&m_InterruptDispatchEnabled,FALSE);",
        "status=SynchronizeInterruptMessages();",
        "KeFlushQueuedDpcs();",
        "m_CtrlQueue.DisableInterrupt();",
        "m_CursorQueue.DisableInterrupt();",
    ):
        if fragment not in adapter_interrupt:
            fail(f"VioGpuAdapter::ControlInterrupt must gate and synchronize queue interrupts: {fragment}")

    render_km = canonical_code(function_body("VioGpuWddmRenderKm", WDDM_DDI_CODE))
    for fragment in (
        "hContext==NULL||render==NULL",
        "returnSTATUS_ILLEGAL_INSTRUCTION;",
    ):
        if render_km.count(fragment) != 1:
            fail(f"kernel GDI command buffer callback must not enter the MSM parser: {fragment}")

    if WDDM_DDI_SOURCE.count("#if defined(VIOGPU_WDDM_TEST_IMPLEMENTATIONS)") != 2:
        fail("experimental RenderKm/reset implementations must use one explicit opt-in macro")
    if "return VioGpuWddmRender(hContext, render);" not in WDDM_DDI_SOURCE:
        fail("RenderKm experiment must exercise the existing Native Render validator")
    reset_experiment = canonical_code(function_body("VioGpuWddmResetEngine", WDDM_DDI_CODE))
    if reset_experiment.count("returnadapter->ResetFromTimeout();") != 1:
        fail("ResetEngine experiment must exercise adapter-wide recovery exactly once")


def check_wddm_handle_ownership() -> None:
    create_context = canonical_code(function_body("VioGpuWddmCreateContext", WDDM_DDI_CODE))
    if "device==NULL||device->Signature!=VIOGPU_WDDM_DEVICE_SIGNATURE||createContext==NULL" not in create_context:
        fail("CreateContext must validate the WDDM device signature before dereferencing the handle")
    if create_context.count("createContext==NULL||device->Adapter==NULL") != 1:
        fail("CreateContext must reject a device handle with no adapter before entering the lifecycle")

    open_allocation = canonical_code(function_body("VioGpuWddmOpenAllocation", WDDM_DDI_CODE))
    if open_allocation.count("openAllocation==NULL||device->Adapter==NULL") != 1:
        fail("OpenAllocation must reject a device handle with no adapter before querying dxgkrnl")

    describe = canonical_code(function_body("VioGpuWddmDescribeAllocation", WDDM_DDI_CODE))
    for fragment in (
        "VioGpuDod*adapter=reinterpret_cast<VioGpuDod*>(hAdapter);",
        "adapter==NULL||describeAllocation==NULL",
        "allocation->Signature!=VIOGPU_WDDM_ALLOCATION_SIGNATURE||allocation->Adapter!=adapter",
    ):
        if fragment not in describe:
            fail(f"DescribeAllocation must enforce adapter ownership before reading allocation metadata: {fragment}")


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
        # exposed only by crosvm when udmabuf=true. StartDevice must fail closed
        # if that feature is absent.
        "VIRTIO_GPU_F_CREATE_GUEST_HANDLE",
    )

    require_integer_define(wire_header_code, "VIRTGPU_DRM_CAPSET_DRM", 6, "wire header")
    require_integer_define(wire_header_code, "VIRTGPU_DRM_CONTEXT_MSM", 1, "wire header")
    require_integer_define(wire_header_code, "VIRTGPU_DRM_WIRE_FORMAT_VERSION", 2, "wire header")
    require_integer_define(wire_header_code, "VIRTGPU_CAP_BOOL_UNSUPPORTED_BY_HOST", 0, "wire header")
    require_integer_define(wire_header_code, "VIRTGPU_CAP_BOOL_FALSE", 0xFFFFFFFF, "wire header")
    require_integer_define(wire_header_code, "VIRTGPU_CAP_BOOL_TRUE", 1, "wire header")
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

    selection = re.search(
        r"if\(info\.capset_id!=VIRTIO_GPU_CAPSET_DRM\)\{continue;\}"
        r"if\(found\)\{(?P<duplicate>.*?)returnSTATUS_NOT_SUPPORTED;\}"
        r"selectedInfo=info;found=TRUE;",
        probe_compact,
    )
    if selection is None:
        fail("readiness probe must select exactly one capset ID 6")
    if "VioGpuNativeStartCapsetInfoUnique" not in selection.group("duplicate"):
        fail("duplicate capset ID 6 rejection must retain its diagnostic stage")

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
        "m_NativeContextReadiness.ResetGeneration=resetGeneration;"
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
        "LONG Generation;",
        "ULONGLONG ResetGeneration;",
        "UINT CapsetVersion;",
        "UINT CapsetSize;",
        "GPU_CAPSET_DRM Capset;",
    )
    for member in readiness_members:
        if compact_code(member) not in compact_code(viogpu_header_code):
            fail(f"readiness state is missing {member}")

    paired_generation_advance = (
        "InterlockedIncrement(&m_NativeContextGeneration);"
        "InterlockedIncrement64(&m_NativeContextResetGeneration);"
    )
    if (
        canonical_code(viogpu_code).count(paired_generation_advance) != 4
        or len(re.findall(r"\bInterlockedIncrement\s*\(\s*&m_NativeContextGeneration\s*\)", viogpu_code)) != 4
        or len(re.findall(r"\bInterlockedIncrement64\s*\(\s*&m_NativeContextResetGeneration\s*\)", viogpu_code)) != 4
    ):
        fail("every internal generation advance must immediately advance the 64-bit reset generation")
    if canonical_code(viogpu_code).count("m_NativeContextResetGeneration=0;") != 1:
        fail("the adapter must initialize its 64-bit reset generation exactly once")
    if compact_code("DECLSPEC_ALIGN(8) volatile LONG64 m_NativeContextResetGeneration;") not in compact_code(
        viogpu_header_code
    ):
        fail("the interlocked 64-bit reset generation must have explicit 8-byte alignment")

    hw_init = function_body("VioGpuAdapter::HWInit", viogpu_code)
    adapter_init = function_body("VioGpuAdapter::VioGpuAdapterInit", viogpu_code)
    d0_power = function_body("VioGpuAdapter::SetPowerState", viogpu_code)
    reset = function_body("VioGpuAdapter::ResetDevice", viogpu_code)
    fail_at_any_irql = canonical_code(function_body("VioGpuAdapter::FailNativeContextAtAnyIrql", viogpu_code))
    failure = function_body("VioGpuAdapter::FailNativeContextInitialization", viogpu_code)
    hw_close = function_body("VioGpuAdapter::HWClose", viogpu_code)
    stop = function_body("VioGpuAdapter::StopNativeContextTransportLocked", viogpu_code)
    destructor = function_body("VioGpuAdapter::~VioGpuAdapter", viogpu_code)
    buffer_close = function_body("VioGpuBuf::Close", QUEUE_CODE)
    segment_close = function_body("VioGpuMemSegment::Close", QUEUE_CODE)

    require_order(
        fail_at_any_irql,
        (
            "InterlockedIncrement(&m_NativeContextGeneration);",
            "InterlockedIncrement64(&m_NativeContextResetGeneration);",
            "InterlockedExchange(&m_InterruptDispatchEnabled,FALSE);",
            "m_CtrlQueue.PoisonSynchronousRequests();",
            "m_pVioGpuDod->RequestHardwareResetAtAnyIrql();",
        ),
        "an inner Native Context failure must close publication before requesting the outer reset/drain",
    )
    if fail_at_any_irql.count("m_pVioGpuDod->RequestHardwareResetAtAnyIrql();") != 1:
        fail("every Native Context failure must request exactly one outer hardware reset/drain")

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
    transport_start_compact = compact_code(transport_start)
    virtio_init_offset = transport_start_compact.find("status=VioGpuAdapterInit(pDispInfo);")
    shared_memory_offset = transport_start_compact.find(
        "m_PciResources.QueryHostVisibleRegion(&hostVisibleBar,&hostVisibleOffset,&hostVisibleSize)"
    )
    buffer_offset = transport_start_compact.find("m_GpuBuf.Init(allocation)")
    if min(virtio_init_offset, shared_memory_offset, buffer_offset) < 0 or not (
        virtio_init_offset < shared_memory_offset < buffer_offset
    ):
        fail("Native Context transport must validate the standard host-visible BAR after VirtIO PCI initialization")
    hw_init_compact = compact_code(hw_init)
    probe_offset = compact_code(transport_start).find("status=ProbeNativeContextReadiness();")
    idr_offset = compact_code(transport_start).find("m_Idr.Init(1,VIOGPU_NATIVE_RESOURCE_ID_START)")
    if min(probe_offset, buffer_offset, idr_offset) < 0 or not (buffer_offset < idr_offset < probe_offset):
        fail("HWInit must probe only after control buffers and the ID allocator are initialized")
    if compact_code(transport_start).count("BOOLEANinitializeResourceIds=TRUE;") != 1:
        fail("transport start must keep a single allocator initialization gate for DOD and Native Context")
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
    reset_generation_advances = [
        match.start()
        for match in re.finditer(
            re.escape("InterlockedIncrement64(&m_NativeContextResetGeneration)"),
            stop_compact,
        )
    ]
    invalidate_offset = stop_compact.find("InvalidateNativeContextRegistrationsLocked()")
    if (
        min(quiescing_publications) < 0
        or len(generation_advances) != 2
        or len(reset_generation_advances) != 2
        or max(*quiescing_publications, *generation_advances, *reset_generation_advances) > invalidate_offset
    ):
        fail("every teardown path must publish Quiescing and advance both generations before invalidating registrations")
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
        ("InterlockedExchange(&m_NativeContextState,VioGpuNativeContextOffline)", "offline publication"),
    )
    teardown_offsets = [(stop_compact.find(fragment), description) for fragment, description in teardown_order]
    for (offset, description), (next_offset, next_description) in zip(teardown_offsets, teardown_offsets[1:]):
        if offset > next_offset:
            fail(f"transport teardown must perform {description} before {next_description}")
    offline_publish = stop_compact.find("InterlockedExchange(&m_NativeContextState,VioGpuNativeContextOffline)")
    buffer_drain = stop_compact.find("if(!m_GpuBuf.Close())")
    buffer_drain_failure = stop_compact.find(
        "FailNativeContextAtAnyIrql();returnSTATUS_DEVICE_NOT_READY;", buffer_drain
    )
    if min(buffer_drain, buffer_drain_failure, offline_publish) < 0 or not (
        buffer_drain < buffer_drain_failure < offline_publish
    ):
        fail("transport teardown must retain the adapter when terminal control-buffer ownership cannot drain")
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
    message_count = "ULONGmessageCount=m_PciResources.GetInterruptMessageCount();"
    empty_success = "if(messageCount==0){returnSTATUS_SUCCESS;}"
    trusted_count = (
        "if(!m_PciResources.HasKnownInterruptMessageCount())"
        "{returnSTATUS_DEVICE_NOT_READY;}"
    )
    interface_guard = (
        "if(dxgkInterface==NULL||dxgkInterface->DxgkCbSynchronizeExecution==NULL)"
        "{returnSTATUS_DEVICE_NOT_READY;}"
    )
    barrier_loop = "for(ULONGmessageNumber=0;messageNumber<messageCount;++messageNumber)"
    barrier_failure = "if(!NT_SUCCESS(barrierStatus)||!barrierResult)"
    barrier_return = "returnNT_SUCCESS(barrierStatus)?STATUS_DEVICE_NOT_READY:barrierStatus;"
    message_offset = helper.find(message_count)
    empty_offset = helper.find(empty_success, message_offset)
    trusted_offset = helper.find(trusted_count, empty_offset)
    interface_offset = helper.find(interface_guard, trusted_offset)
    loop_offset = helper.find(barrier_loop, interface_offset)
    failure_offset = helper.find(barrier_failure, loop_offset)
    return_offset = helper.find(barrier_return, failure_offset)
    if min(message_offset, empty_offset, trusted_offset, interface_offset, loop_offset, failure_offset, return_offset) < 0 or not (
        message_offset < empty_offset < trusted_offset < interface_offset < loop_offset < failure_offset < return_offset
    ):
        fail("ISR barrier helper must require a complete count, synchronize every retained message, and fail closed")
    if (
        helper.count("GetInterruptMessageCount()") != 1
        or helper.count("HasKnownInterruptMessageCount()") != 1
        or "IsMSIEnabled()" in helper
        or "m_bQueuesInitialized" in helper
        or helper.count("barrierStatus=") != 1
        or helper.count("barrierResult=") != 1
        or "&barrierResult);if(!NT_SUCCESS(barrierStatus)||!barrierResult)" not in helper
    ):
        fail("ISR barrier status and result must be validated immediately after synchronization")
    stop_canonical = canonical_code(stop)
    helper_calls = [match.start() for match in re.finditer(r"\bSynchronizeInterruptMessages\s*\(\s*\)", stop_canonical)]
    final_barrier = helper_calls[1] if len(helper_calls) == 2 else -1
    reset_status_canonical_offset = stop_canonical.find("virtio_get_status(&m_VioDev)", reset_offset)
    publish_2d_retirement = stop_canonical.find("Publish2DResetRetirementLocked()", reset_status_canonical_offset)
    retire_native_owners = stop_canonical.find("RetireAllNativeContextOwnersLocked()", publish_2d_retirement)
    delete_queues = stop_canonical.find("virtio_delete_queues(&m_VioDev)", retire_native_owners)
    if (
        len(helper_calls) != 2
        or min(reset_status_canonical_offset, publish_2d_retirement, retire_native_owners, final_barrier, delete_queues) < 0
        or not reset_status_canonical_offset < publish_2d_retirement < retire_native_owners < final_barrier < delete_queues
    ):
        fail("transport teardown must publish reset-proven retirement before the final barrier and queue deletion")

    retire_all = canonical_code(function_body("VioGpuAdapter::RetireAllNativeContextOwnersLocked", VIOGPU_CODE))
    require_order(
        retire_all,
        (
            "PLIST_ENTRYentry=m_NativeContextRegistry.Flink;",
            "if(owner->ControlAddress!=NULL)",
            "m_PciResources.UnmapHostVisibleAddress(owner->ControlAddress)",
            "if(!NT_SUCCESS(status)){returnstatus;}",
            "owner->ControlAddress=NULL;",
            "owner->ControlBarOffset=0;",
            "RemoveEntryList(entry);",
            "deleteowner;",
            "returnSTATUS_SUCCESS;",
        ),
        "reset retirement must unmap each retained control BAR slot before deleting its owner",
    )
    retire_call = stop_canonical.find("status=RetireAllNativeContextOwnersLocked();", publish_2d_retirement)
    retire_failure = stop_canonical.find(
        "if(!NT_SUCCESS(status)){FailNativeContextAtAnyIrql();returnstatus;}", retire_call
    )
    if min(retire_call, retire_failure, final_barrier) < 0 or not retire_call < retire_failure < final_barrier:
        fail("transport teardown must preserve owners and stop before queue deletion when BAR-slot unmap fails")
    if compact_code(reset).count("FailNativeContextAtAnyIrql()") != 1:
        fail("ResetDevice must fail closed through the nonpaged native-context failure path exactly once")
    destructor_compact = compact_code(destructor)
    if "StopNativeContextTransport()" not in destructor_compact:
        fail("adapter destruction must use the native-context transport teardown")

    terminal_parameters = "_Inout_ PGPU_VBUFFER buffer"
    terminal_arm = canonical_code(
        function_body_with_parameters("VioGpuArmVbufferTerminalCallbacks", terminal_parameters, QUEUE_CODE)
    )
    require_order(
        terminal_arm,
        (
            "KeClearEvent(&buffer->terminal_callback_event);",
            "InterlockedCompareExchange(&buffer->terminal_callback_state,VioGpuVbufferTerminalArmed,"
            "VioGpuVbufferTerminalUnarmed)==VioGpuVbufferTerminalUnarmed",
            "if(!armed)",
            "KeSetEvent(&buffer->terminal_callback_event,IO_NO_INCREMENT,FALSE);",
            "returnarmed;",
        ),
        "terminal callback arming must clear the event only for the Unarmed-to-Armed owner",
    )
    terminal_claim = canonical_code(
        function_body_with_parameters("VioGpuClaimVbufferTerminalCallbacks", terminal_parameters, QUEUE_CODE)
    )
    for fragment in (
        "InterlockedCompareExchange(&buffer->terminal_callback_state,VioGpuVbufferTerminalClaimed,"
        "VioGpuVbufferTerminalArmed)",
        "returnVioGpuVbufferTerminalClaimWon;",
        "previous==VioGpuVbufferTerminalUnarmed?VioGpuVbufferTerminalClaimUnarmed:"
        "VioGpuVbufferTerminalClaimLost",
    ):
        if fragment not in terminal_claim:
            fail(f"terminal callbacks must have one Armed-to-Claimed owner: {fragment}")
    terminal_complete = canonical_code(
        function_body_with_parameters("VioGpuCompleteVbufferTerminalCallbacks", terminal_parameters, QUEUE_CODE)
    )
    require_order(
        terminal_complete,
        (
            "InterlockedCompareExchange(&buffer->terminal_callback_state,VioGpuVbufferTerminalCompleted,"
            "VioGpuVbufferTerminalClaimed);",
            "state==VioGpuVbufferTerminalClaimed||state==VioGpuVbufferTerminalCompleted",
            "KeSetEvent(&buffer->terminal_callback_event,IO_NO_INCREMENT,FALSE);",
        ),
        "terminal completion must publish Completed before waking teardown waiters",
    )
    terminal_wait = canonical_code(
        function_body_with_parameters("VioGpuWaitForVbufferTerminalCallbacks", terminal_parameters, QUEUE_CODE)
    )
    require_order(
        terminal_wait,
        (
            "state!=VioGpuVbufferTerminalClaimed",
            "state==VioGpuVbufferTerminalCompleted||state==VioGpuVbufferTerminalUnarmed",
            "KeGetCurrentIrql()!=PASSIVE_LEVEL",
            "KeWaitForSingleObject(&buffer->terminal_callback_event,Executive,KernelMode,FALSE,&timeout)==STATUS_SUCCESS",
            "InterlockedCompareExchange(&buffer->terminal_callback_state,0,0)==VioGpuVbufferTerminalCompleted",
        ),
        "terminal waiting must block only at PASSIVE_LEVEL and require a final Completed observation",
    )
    get_buffer = canonical_code(function_body("VioGpuBuf::GetBuf", QUEUE_CODE))
    require_order(
        get_buffer,
        (
            "memset(pbuf,0,VBUFFER_SIZE);",
            "KeInitializeEvent(&pbuf->completion_event,NotificationEvent,FALSE);",
            "KeInitializeEvent(&pbuf->terminal_callback_event,NotificationEvent,TRUE);",
            "InsertTailList(&m_InUseBufs,&pbuf->list_entry);",
        ),
        "every recycled GPU_VBUFFER must start terminal-unarmed with a signaled terminal event",
    )

    lock_offsets = [match.start() for match in re.finditer(r"\bKeAcquireSpinLock\s*\(", buffer_close)]
    unlock_offsets = [match.start() for match in re.finditer(r"\bKeReleaseSpinLock\s*\(", buffer_close)]
    free_offsets = [match.start() for match in re.finditer(r"\bFreeMemory\s*\(", buffer_close)]
    if len(lock_offsets) != 2 or len(unlock_offsets) != 2 or len(free_offsets) != 3:
        fail("control-buffer teardown must detach once, support one failed-wait reinsertion, and free three buffer stores")
    if not lock_offsets[0] < unlock_offsets[0] < lock_offsets[1] < unlock_offsets[1] < free_offsets[-1]:
        fail("control-buffer teardown must keep detach and failed-wait reinsertion under separate spin-lock regions")
    if any(offset < unlock_offsets[0] for offset in free_offsets):
        fail("control-buffer teardown must detach every buffer before freeing allocations outside the spin lock")
    for list_name in ("m_InUseBufs", "m_FreeBufs"):
        drain = rf"\bwhile\s*\(\s*!\s*IsListEmpty\s*\(\s*&{list_name}\s*\)\s*\)"
        if len(re.findall(drain, buffer_close)) != 1:
            fail(f"control-buffer teardown must detach {list_name} exactly once")
    for member in ("m_uCount", "m_uCountMin"):
        writes = variable_write_offsets(buffer_close, member)
        expected_writes = 2 if member == "m_uCount" else 1
        if len(writes) != expected_writes or not lock_offsets[0] < writes[0] < unlock_offsets[0]:
            fail(f"control-buffer teardown must clear {member} while holding the detach lock")
    if not lock_offsets[1] < variable_write_offsets(buffer_close, "m_uCount")[1] < unlock_offsets[1]:
        fail("a terminal-callback wait failure must restore the in-use buffer count under the spin lock")
    require_order(
        canonical_code(buffer_close),
        (
            "VIOGPU_VBUFFER_TERMINAL_CLAIMclaim=VioGpuClaimVbufferTerminalCallbacks(buffer);",
            "claim!=VioGpuVbufferTerminalClaimLost",
            "VioGpuDetachVbufferTerminalCallbacks(buffer);",
            "cancelCallback(cancelContext);",
            "VioGpuCompleteVbufferTerminalCallbacks(buffer);",
            "elseif(!VioGpuWaitForVbufferTerminalCallbacks(buffer))",
            "InsertTailList(&m_InUseBufs,&buffer->list_entry);",
            "++m_uCount;",
            "drained=FALSE;",
            "continue;",
            "returndrained;",
        ),
        "control-buffer teardown must claim terminal callbacks or retain a buffer whose terminal owner did not drain",
    )
    for argument in ("buffer->resp_buf", "buffer->data_buf", "buffer"):
        if len(re.findall(rf"\bFreeMemory\s*\(\s*{re.escape(argument)}\s*\)\s*;", buffer_close)) != 1:
            fail(f"control-buffer teardown must free exactly {argument}")

    segment_close_compact = compact_code(segment_close)
    for fragment, description in (
        ("if(m_pVAddr!=NULL&&m_bSystemMemory)", "owned-memory guard"),
        ("delete[]reinterpret_cast<PBYTE>(m_pVAddr);", "owned-memory release"),
        ("elseif(m_pVAddr!=NULL&&m_bMapped)", "mapped-framebuffer guard"),
        ("(void)UnmapFrameBuffer(m_pVAddr,(ULONG)m_Size);", "mapped-framebuffer release"),
        ("m_pVAddr=NULL;", "address clear"),
        ("delete[]reinterpret_cast<PBYTE>(m_pSGList);", "scatter-gather release"),
        ("m_pSGList=NULL;", "scatter-gather clear"),
        ("m_bSystemMemory=FALSE;", "system-memory clear"),
        ("m_bMapped=FALSE;", "mapping clear"),
        ("m_Size=0;", "size clear"),
    ):
        if segment_close_compact.count(fragment) != 1:
            fail(f"memory-segment teardown must contain exactly one {description}")
    for member in (
        "m_pVAddr",
        "m_pSGList",
        "m_bSystemMemory",
        "m_bMapped",
        "m_Size",
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

    for name in (
        "CtrlQueue::CreateResource",
        "CtrlQueue::ResFlush",
        "CtrlQueue::TransferToHost2D",
        "CtrlQueue::DestroyResource",
        "CtrlQueue::DetachBacking",
        "CtrlQueue::SetScanout",
    ):
        asynchronous = canonical_code(function_body(name, QUEUE_CODE))
        if "if(cmd==NULL||vbuf==NULL)" not in asynchronous:
            fail(f"{name} must fail safely when its command buffer cannot be allocated")
        queue_failure = asynchronous.find("if(QueueBuffer(vbuf)<0)")
        release = asynchronous.find("ReleaseBuffer(vbuf)", queue_failure)
        if queue_failure < 0 or release < queue_failure:
            fail(f"{name} must release an asynchronous command buffer after queue failure")

    attach = canonical_code(function_body("CtrlQueue::AttachBacking", QUEUE_CODE))
    if "returnFALSE;" not in attach or not attach.endswith("returnTRUE;"):
        fail("CtrlQueue::AttachBacking must report asynchronous enqueue failure to its caller")


def check_control_queue_dma_and_response_contract() -> None:
    """Require page-complete SG lists and an exact CTX_CREATE response classifier."""
    sg_body = canonical_code(function_body("BuildSGElements", QUEUE_CODE))
    for fragment in (
        "while(size!=0)",
        "count>=capacity",
        "!MmIsAddressValid(current)",
        "fragmentSize=min(size,static_cast<ULONG>(PAGE_SIZE-BYTE_OFFSET(current)))",
        "physicalAddress=MmGetPhysicalAddress(current)",
        "sg[count].length=fragmentSize",
        "sg[count].physAddr=physicalAddress",
        "current+=fragmentSize",
        "size-=fragmentSize",
    ):
        if sg_body.count(fragment) != 1:
            fail(f"control DMA SG builder must contain exactly one page-fragment contract: {fragment}")

    queue = canonical_code(function_body("CtrlQueue::QueueBuffer", QUEUE_CODE))
    if queue.count("BuildSGElements(") != 3:
        fail("control queue must fragment command, payload, and response DMA ranges independently")
    if queue.count("outcnt+=elementCount;sgleft-=elementCount;") != 2:
        fail("control queue must account for every command and payload output descriptor")
    if queue.count("incnt+=elementCount;sgleft-=elementCount;") != 1:
        fail("control queue must account for every response input descriptor")
    if "if(buf->resp_size){if(sgleft==0)" not in queue:
        fail("control queue must fail instead of submitting a response-bearing command without an input descriptor")

    cursor = canonical_code(function_body("CrsrQueue::QueueCursor", QUEUE_CODE))
    if "VirtIOBufferDescriptorsg[2];" not in cursor or cursor.count("BuildSGElements(") != 1:
        fail("cursor queue must permit both page fragments of its at-most-one-page command")
    if re.search(r"\bBuildSGElement\s*\(", QUEUE_CODE):
        fail("no queue may retain the truncating single-fragment DMA helper")

    validator = canonical_code(function_body("VioGpuValidatePlainControlResponse", WIRE_HEADER_CODE))
    validation_returns = (
        "returnVioGpuHostResponseNotSubmitted;",
        "returnVioGpuHostResponseNotCompleted;",
        "returnVioGpuHostResponseTooShort;",
        "returnVioGpuHostResponseWrongSize;",
        "returnVioGpuHostResponseConfirmed;",
        "returnVioGpuHostResponseRejected;",
        "returnVioGpuHostResponseMalformed;",
    )
    require_order(
        validator,
        validation_returns,
        "CTX_CREATE response validation must distinguish transport, size, success, rejection, and malformed replies",
    )
    for fragment in validation_returns:
        if validator.count(fragment) != 1:
            fail(f"CTX_CREATE response validator must publish exactly one classification: {fragment}")
    if validator.count("response_size!=VIRTIO_GPU_CTRL_HDR_WIRE_SIZE") != 1:
        fail("CTX_CREATE response validator must require the exact 24-byte control response")
    if validator.count("type>=VIRTIO_GPU_RESP_ERR_UNSPEC") != 1 or validator.count(
        "type<=VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER"
    ) != 1:
        fail("CTX_CREATE response validator must accept only the standard VirtIO-GPU error range as rejection")

    create = canonical_code(function_body("CtrlQueue::CreateNativeContext", QUEUE_CODE))
    if create.count("VioGpuValidatePlainControlResponse(") != 1:
        fail("CTX_CREATE must use the focused shared response validator exactly once")
    if create.count("PoisonSynchronousRequests();") != 1:
        fail("ambiguous CTX_CREATE completion must poison the synchronous transport generation")
    if create.count("RtlCopyMemory(&m_LastNativeContextResponseDiagnostic,output,sizeof(m_LastNativeContextResponseDiagnostic));") != 2:
        fail("CTX_CREATE must publish both pre-submit and final response diagnostics")
    if VIOGPU_SOURCE.count('L"NativeContextCreateResponseValidation"') != 1:
        fail("runtime diagnostics must persist exactly one CTX_CREATE response validation category")


def check_synchronous_2d_control_transactions() -> None:
    queue_header = canonical_code(QUEUE_HEADER_CODE)
    method_names = (
        "CreateResource2DSynchronous",
        "AttachBackingSynchronous",
        "DetachBackingSynchronous",
        "UnrefResourceSynchronous",
        "SetScanoutSynchronous",
        "TransferToHost2DSynchronous",
        "FlushResourceSynchronous",
    )
    for method_name in method_names:
        if queue_header.count(f"VIOGPU_HOST_CONTEXT_RESULT{method_name}(") != 1:
            fail(f"full-WDDM 2D control API must declare exactly one {method_name} transaction")

    helper_body = function_body("CtrlQueue::SubmitSynchronousNoDataLocked", QUEUE_CODE)
    helper = canonical_code(helper_body)
    helper_requirements = (
        "BOOLEANreleaseBuffer=TRUE;",
        "BOOLEANsubmitted=FALSE;",
        "BOOLEANcompleted=SubmitSynchronousLocked(buf,&releaseBuffer,&submitted);",
        "VIOGPU_HOST_CONTEXT_RESULTresult=VioGpuHostContextUnknown;",
        "if(!submitted){result=VioGpuHostContextNotSubmitted;}",
        "elseif(completed&&buf->response_size==sizeof(GPU_CTRL_HDR))",
        "if(IsPlainControlResponse(response,VIRTIO_GPU_RESP_OK_NODATA))",
        "result=VioGpuHostContextConfirmed;",
        "elseif(IsPlainControlErrorResponse(response))",
        "result=VioGpuHostContextRejected;",
        "if(releaseBuffer){ReleaseBuffer(buf);}",
        "returnresult;",
    )
    for fragment in helper_requirements:
        if helper.count(fragment) != 1:
            fail(f"synchronous 2D response classification must retain exactly one: {fragment}")
    if helper.count("PoisonSynchronousRequests();") != 2:
        fail("synchronous 2D responses must poison both malformed-header and malformed-size completions")
    if helper.find("if(!submitted)") > helper.find("elseif(completed&&buf->response_size==sizeof(GPU_CTRL_HDR))"):
        fail("synchronous 2D response classification must resolve non-submission before reading a response")

    command_contracts = {
        "CreateResource2DSynchronous": (
            "VIRTIO_GPU_CMD_RESOURCE_CREATE_2D",
            "command->resource_id=resource_id;",
            "command->format=format;",
            "command->width=width;",
            "command->height=height;",
        ),
        "DetachBackingSynchronous": (
            "VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING",
            "command->resource_id=resource_id;",
        ),
        "UnrefResourceSynchronous": (
            "VIRTIO_GPU_CMD_RESOURCE_UNREF",
            "command->resource_id=resource_id;",
        ),
        "SetScanoutSynchronous": (
            "VIRTIO_GPU_CMD_SET_SCANOUT",
            "command->resource_id=resource_id;",
            "command->scanout_id=scanout_id;",
            "command->r.width=width;",
            "command->r.height=height;",
            "command->r.x=x;",
            "command->r.y=y;",
        ),
        "TransferToHost2DSynchronous": (
            "VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D",
            "command->resource_id=resource_id;",
            "command->offset=offset;",
            "command->r.width=width;",
            "command->r.height=height;",
            "command->r.x=x;",
            "command->r.y=y;",
        ),
        "FlushResourceSynchronous": (
            "VIRTIO_GPU_CMD_RESOURCE_FLUSH",
            "command->resource_id=resource_id;",
            "command->r.width=width;",
            "command->r.height=height;",
            "command->r.x=x;",
            "command->r.y=y;",
        ),
    }
    for method_name, required_fragments in command_contracts.items():
        body = canonical_code(function_body(f"CtrlQueue::{method_name}", QUEUE_CODE))
        if body.count("BeginSynchronousRequest()") != 1 or body.count("EndSynchronousRequest();") != 2:
            fail(f"{method_name} must hold one synchronous epoch and release it on allocation failure and completion")
        if body.count("SubmitSynchronousNoDataLocked(vbuf)") != 1:
            fail(f"{method_name} must classify exactly one synchronous no-data transaction")
        if "IsStandard2DResourceId(resource_id)" not in body:
            fail(f"{method_name} must keep standard 2D IDs disjoint from Native Context resource IDs")
        for fragment in required_fragments:
            if body.count(fragment) != 1:
                fail(f"{method_name} must retain exactly one wire field: {fragment}")

    attach = canonical_code(function_body("CtrlQueue::AttachBackingSynchronous", QUEUE_CODE))
    attach_sequence = (
        attach.find("entry_count>VIOGPU_MAX_BACKING_ENTRIES"),
        attach.find("PGPU_MEM_ENTRYownedEntries="),
        attach.find("RtlCopyMemory(ownedEntries,entries,entriesSize);"),
        attach.find("command->hdr.type=VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;"),
        attach.find("vbuf->data_buf=ownedEntries;"),
        attach.find("SubmitSynchronousNoDataLocked(vbuf)"),
    )
    if min(attach_sequence) < 0 or list(attach_sequence) != sorted(attach_sequence):
        fail("synchronous 2D attach must submit one bounded queue-owned backing table")
    if attach.count("BeginSynchronousRequest()") != 1 or attach.count("EndSynchronousRequest();") != 3:
        fail("synchronous 2D attach must release its epoch on both allocation failures and completion")
    for fragment in (
        "entries[index].addr==0",
        "entries[index].length==0",
        "entries[index].padding!=0",
        "entries[index].addr>MAXULONGLONG-(entries[index].length-1)",
        "command->nr_entries=entry_count;",
        "vbuf->data_size=(UINT)entriesSize;",
    ):
        if attach.count(fragment) != 1:
            fail(f"synchronous 2D attach must validate and retain exact backing data: {fragment}")

    scanout = canonical_code(function_body("CtrlQueue::SetScanoutSynchronous", QUEUE_CODE))
    disable = "BOOLEANdisable=resource_id==0&&width==0&&height==0&&x==0&&y==0;"
    if scanout.count(disable) != 1 or "scanout_id>=VIRTIO_GPU_MAX_SCANOUTS" not in scanout:
        fail("synchronous scanout must allow only an all-zero disable or a bounded standard 2D resource")


def check_wddm_2d_resource_ownership() -> None:
    queue_header = canonical_code(QUEUE_HEADER_CODE)
    expected_states = (
        "VioGpu2DResourceNone=0,"
        "VioGpu2DResourceCreated,"
        "VioGpu2DResourceBackingAttached,"
        "VioGpu2DResourceUnknown,"
    )
    if queue_header.count(expected_states) != 1:
        fail("2D primary resources must retain explicit none, created, attached, and unknown Host states")

    allocate_id = canonical_code(function_body("VioGpuAdapter::Allocate2DResourceId", VIOGPU_CODE))
    for fragment in (
        "KeGetCurrentIrql()!=PASSIVE_LEVEL",
        "!m_CtrlQueue.IsSynchronousRequestsHealthy()",
        "UINTresourceId=m_Idr.GetId();",
        "returnresourceId<VIOGPU_NATIVE_RESOURCE_ID_START?resourceId:0;",
    ):
        if allocate_id.count(fragment) != 1:
            fail(f"2D primary ID allocation must remain in the standard resource range: {fragment}")

    create_host = canonical_code(function_body("VioGpuAdapter::Create2DResourceBacking", VIOGPU_CODE))
    create_sequence = (
        create_host.find("entries==NULL||entryCount==0"),
        create_host.find("m_CtrlQueue.CreateResource2DSynchronous(resourceId,format,width,height)"),
        create_host.find("*resourceState=VioGpu2DResourceCreated;"),
        create_host.find("m_CtrlQueue.AttachBackingSynchronous(resourceId,entries,entryCount)"),
        create_host.find("*resourceState=VioGpu2DResourceBackingAttached;"),
    )
    if min(create_sequence) < 0 or list(create_sequence) != sorted(create_sequence):
        fail("2D primary creation must validate VidMm SG backing before ordered create and attach ownership")
    for fragment in (
        "backingSize>MAXULONG",
        "(backingSize&(PAGE_SIZE-1))!=0",
        "rollback=m_CtrlQueue.UnrefResourceSynchronous(resourceId);",
        "if(rollback==VioGpuHostContextConfirmed)",
        "*resourceState=VioGpu2DResourceNone;",
        "*resourceState=VioGpu2DResourceUnknown;",
        "FailNativeContextAtAnyIrql();",
    ):
        if fragment not in create_host:
            fail(f"2D primary creation must retain transactional Host ownership: {fragment}")
    rollback_call = create_host.find("rollback=m_CtrlQueue.UnrefResourceSynchronous(resourceId);")
    rollback_confirmed = create_host.find("if(rollback==VioGpuHostContextConfirmed)", rollback_call)
    rollback_none = create_host.find("*resourceState=VioGpu2DResourceNone;", rollback_confirmed)
    if min(rollback_call, rollback_confirmed, rollback_none) < 0 or not rollback_call < rollback_confirmed < rollback_none:
        fail("2D primary rollback may release ownership only after a confirmed UNREF")

    destroy_host = canonical_code(function_body("VioGpuAdapter::Destroy2DResource", VIOGPU_CODE))
    for fragment in (
        "result=m_CtrlQueue.UnrefResourceSynchronous(resourceId);",
        "if(result==VioGpuHostContextConfirmed)",
        "*resourceState=VioGpu2DResourceNone;",
        "*resourceResetGeneration=0;",
        "elseif(result==VioGpuHostContextUnknown||result==VioGpuHostContextRejected)",
    ):
        if destroy_host.count(fragment) != 1:
            fail(f"2D primary teardown must retain confirmed-only UNREF ownership: {fragment}")
    if destroy_host.count("*resourceState=VioGpu2DResourceUnknown;") != 2:
        fail("2D primary teardown must quarantine generation mismatch and uncertain UNREF ownership")
    if destroy_host.count("*released=TRUE;") != 2:
        fail("2D primary teardown may release only an already-empty or confirmed-UNREF owner")
    if destroy_host.count("FailNativeContextAtAnyIrql();") != 3:
        fail("2D primary teardown must quarantine preexisting, generation-mismatched, and response-derived unknown ownership")
    if "result==VioGpuHostContextConfirmed||result==VioGpuHostContextRejected" in destroy_host:
        fail("2D primary teardown must not interpret INVALID_RESOURCE_ID as released ownership")

    allocation_header = canonical_code(WDDM_DDI_HEADER_CODE)
    if allocation_header.count("VIOGPU_2D_RESOURCE_STATEResource2DState;") != 1:
        fail("each WDDM allocation must retain its exact 2D Host ownership state")
    if allocation_header.count("ULONGLONGResource2DResetGeneration;") != 1:
        fail("each WDDM allocation must retain the reset generation that owns its 2D Host state")
    create_allocation = canonical_code(function_body("VioGpuWddmCreateAllocation", WDDM_DDI_CODE))
    for fragment in (
        "else{standardResourceId=adapter->Allocate2DResourceId();",
        "standardResourceId=adapter->Allocate2DResourceId();",
        "allocation->ResourceId=nativeContext!=NULL?nativeResourceId:standardResourceId;",
        "allocation->BlobId=nativeResourceId;",
        "allocation->Resource2DState=VioGpu2DResourceNone;",
        "allocation->Resource2DResetGeneration=0;",
    ):
        if create_allocation.count(fragment) != 1:
            fail(f"every standard allocation must publish one disjoint local 2D identity: {fragment}")

    rollback = canonical_code(function_body("DestroyCreatedAllocations", WDDM_DDI_CODE))
    if rollback.count("allocation->Adapter->Release2DResourceId(allocation->ResourceId)") != 1:
        fail("CreateAllocation batch rollback must return every unpublished standard 2D ID")

    destroy_allocation = canonical_code(function_body("VioGpuWddmDestroyAllocation", WDDM_DDI_CODE))
    destroy_call = destroy_allocation.find("result=adapter->Destroy2DResource(allocation->ResourceId,")
    release_call = destroy_allocation.find("adapter->Release2DResourceId(allocation->ResourceId)")
    clear_id = destroy_allocation.find("allocation->ResourceId=0;", release_call)
    delete_allocation = destroy_allocation.find("deleteallocation;")
    if min(destroy_call, release_call, clear_id, delete_allocation) < 0 or not (
        destroy_call < release_call < clear_id < delete_allocation
    ):
        fail("DestroyAllocation must release Host 2D ownership and its local ID before deleting the allocation")

    reconcile = canonical_code(function_body("VioGpuAdapter::Reconcile2DResourceAfterReset", VIOGPU_CODE))
    for fragment in (
        "InterlockedCompareExchange64(&m_2DRetiredResetGeneration,0,0)",
        "if(*resourceResetGeneration<=retiredGeneration)",
        "*resourceState=VioGpu2DResourceNone;",
        "*resourceResetGeneration=0;",
        "*retired=TRUE;",
    ):
        if fragment not in reconcile:
            fail(f"2D Host ownership must retire only across a confirmed reset generation: {fragment}")

    native_reset_retired = canonical_code(function_body("VioGpuAdapter::IsNativeContextResetRetired", VIOGPU_CODE))
    for fragment in (
        "KeGetCurrentIrql()!=PASSIVE_LEVEL",
        "InterlockedCompareExchange64(&m_2DRetiredResetGeneration,0,0)",
        "retiredGeneration!=0&&resetGeneration<=retiredGeneration",
    ):
        if fragment not in native_reset_retired:
            fail(f"Native Context reset retirement must use the confirmed adapter generation: {fragment}")

    native_reset_retired_wrapper = canonical_code(
        function_body("VioGpuDod::IsNativeContextResetRetired", VIOGPU_CODE)
    )
    for fragment in (
        "ExAcquireRundownProtection(&m_HardwareOperations)",
        "VioGpuAdapter*adapter=m_pHWDevice",
        "adapter->IsNativeContextResetRetired(resetGeneration)",
        "ExReleaseRundownProtection(&m_HardwareOperations)",
    ):
        if fragment not in native_reset_retired_wrapper:
            fail(f"Native Context reset retirement wrapper must protect the adapter lifetime: {fragment}")

    publish = canonical_code(function_body("VioGpuAdapter::Publish2DResetRetirementLocked", VIOGPU_CODE))
    require_order(
        publish,
        (
            "InterlockedCompareExchange64(&m_NativeContextResetGeneration,0,0)",
            "InterlockedCompareExchange64(&m_2DRetiredResetGeneration,0,0)",
            "if(resetGeneration==0||resetGeneration<retiredGeneration)",
            "InterlockedExchange64(&m_2DRetiredResetGeneration,static_cast<LONG64>(resetGeneration));",
            "KeWaitForSingleObject(&m_2DScanoutMutex,Executive,KernelMode,FALSE,NULL)",
            "Reconcile2DScanoutAfterResetLocked();",
            "KeReleaseMutex(&m_2DScanoutMutex,FALSE);",
        ),
        "2D reset retirement must publish one monotonic confirmed generation before reconciling scanout under its mutex",
    )

    scanout_reconcile = canonical_code(
        function_body("VioGpuAdapter::Reconcile2DScanoutAfterResetLocked", VIOGPU_CODE)
    )
    for fragment in (
        "InterlockedCompareExchange64(&m_2DRetiredResetGeneration,0,0)",
        "m_2DScanoutResetGeneration!=0&&m_2DScanoutResetGeneration<=retiredGeneration",
        "m_2DScanoutResourceId=0;",
        "m_2DScanoutUnknown=FALSE;",
        "m_2DScanoutResetGeneration=0;",
    ):
        if scanout_reconcile.count(fragment) != 1:
            fail(f"scanout reset reconciliation must clear one retired Host identity: {fragment}")

    stop = canonical_code(function_body("VioGpuAdapter::StopNativeContextTransportLocked", VIOGPU_CODE))
    rundown = stop.find("CompleteNativeSubmitRundown();")
    reset = stop.find("virtio_device_reset_checked(&m_VioDev)", rundown)
    publish = stop.find("Publish2DResetRetirementLocked();", reset)
    retire = stop.find("RetireAllNativeContextOwnersLocked();", publish)
    final_barrier = stop.find("SynchronizeInterruptMessages();", retire)
    delete_queues = stop.find("virtio_delete_queues(&m_VioDev)", final_barrier)
    if min(rundown, reset, publish, retire, final_barrier, delete_queues) < 0 or not (
        rundown < reset < publish < retire < final_barrier < delete_queues
    ):
        fail("confirmed VirtIO reset must retire 2D and native owners before the final barrier and queue deletion")


def check_wddm_standard_paging() -> None:
    allocation_info = canonical_code(function_body("InitializeAllocationInfo", WDDM_DDI_CODE))
    for fragment in (
        "BOOLEANcpuVisible=(allocation->Flags&VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE)!=0;",
        "allocationInfo->Flags.CpuVisible=cpuVisible;",
        "allocationInfo->Flags.Cached=cpuVisible;",
        "allocationInfo->Flags.SynchronousPaging=TRUE;",
    ):
        if allocation_info.count(fragment) != 1:
            fail(f"ordinary VidMm allocation backing must retain its exact visibility contract: {fragment}")
    if "PermanentSysMem" in allocation_info:
        fail("CPU-visible Present sources must remain pageable into the aperture segment")

    query_segment = canonical_code(function_body("QuerySegment", WDDM_DDI_CODE))
    for fragment in (
        "descriptor->BaseAddress.QuadPart=0;",
        "descriptor->CpuTranslatedAddress.QuadPart=0;",
        "descriptor->Size=VIOGPU_WDDM_APERTURE_SIZE;",
        "descriptor->CommitLimit=VIOGPU_WDDM_APERTURE_SIZE;",
        "descriptor->Flags.CpuVisible=TRUE;",
        "descriptor->Flags.Aperture=TRUE;",
        "descriptor->Flags.CacheCoherent=TRUE;",
    ):
        if query_segment.count(fragment) != 1:
            fail(f"VidMm must see one CPU-visible cache-coherent aperture: {fragment}")

    placement = canonical_code(
        function_body_with_parameters(
            "ValidateNativePlacement",
            "VIOGPU_WDDM_ALLOCATION *allocation, LARGE_INTEGER segmentAddress, ULONGLONG *offset",
            WDDM_DDI_CODE,
        )
    )
    if "allocation->BackingSize==0" not in placement:
        fail("placement validation must reject zero-sized allocations before its end-address subtraction")

    validator = canonical_code(function_body("ValidatePagingDmaPacket", WDDM_DDI_CODE))
    for fragment in (
        "BOOLEANhasContext=packet->ContextId!=0;",
        "if(hasContext?packet->ResourceId<VIOGPU_NATIVE_RESOURCE_ID_START:packet->ResourceId>=VIOGPU_NATIVE_RESOURCE_ID_START)",
        "packet->ResourceId==0",
        "packet->ResourceId==MAXUINT",
    ):
        if validator.count(fragment) != 1:
            fail(f"paging packets must keep standard and native resource identity disjoint: {fragment}")
    if "(!pageIn&&!pageOut)||!hasContext" in validator:
        fail("paging packet validation must allow context-zero standard transfers")
    if "VioGpuWddmPagingFlagAllocationIdle)==0&&hasContext" in validator:
        fail("paging packet validation must allow context-zero standard fills")

    build = canonical_code(function_body("VioGpuWddmBuildPagingBuffer", WDDM_DDI_CODE))
    for fragment in (
        "pagingBuffer->Operation==DXGK_OPERATION_MAP_APERTURE_SEGMENT",
        "pagingBuffer->MapApertureSegment.SegmentId!=VIOGPU_WDDM_SEGMENT_ID",
        "(pagingBuffer->MapApertureSegment.Flags.Value&~1)!=0",
        "MapApertureAllocation(adapter,allocation,",
        "pagingBuffer->MapApertureSegment.pMdl",
        "pagingBuffer->MapApertureSegment.MdlOffset",
        "pagingBuffer->Operation==DXGK_OPERATION_UNMAP_APERTURE_SEGMENT",
        "NTSTATUSstatus=UnmapApertureAllocation(adapter,allocation,",
        "pagingBuffer->UnmapApertureSegment.DummyPage",
        "adapter->RequestHardwareResetAtAnyIrql();",
        "returnSTATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;",
        "(pagingBuffer->Transfer.Flags.Value&~0x1C)!=0",
        "(pagingBuffer->DiscardContent.Flags.Value&~1)!=0",
        "returnBuildSoftwarePagingTransaction(adapter,",
    ):
        if fragment not in build:
            fail(f"aperture paging must use VidMm MDLs and the shared software transaction path: {fragment}")
    unmap_dispatch_start = build.find("if(pagingBuffer->Operation==DXGK_OPERATION_UNMAP_APERTURE_SEGMENT)")
    unmap_dispatch_end = build.find("if(pagingBuffer->pDmaBuffer==NULL", unmap_dispatch_start)
    if min(unmap_dispatch_start, unmap_dispatch_end) < 0 or \
       "STATUS_GRAPHICS_ALLOCATION_BUSY" in build[unmap_dispatch_start:unmap_dispatch_end]:
        fail("UNMAP_APERTURE must return only success or a retryable paging-buffer status")

    map_allocation = canonical_code(function_body("MapApertureAllocation", WDDM_DDI_CODE))
    for aperture_name in ("MapApertureAllocation", "UnmapApertureAllocation"):
        aperture_body = canonical_code(function_body(aperture_name, WDDM_DDI_CODE))
        require_order(
            aperture_body,
            (
                "status=AcquireAllocationLifecycle(allocation);",
                "BOOLEANnativeAllocation=IsNativeAllocation(allocation);",
                "BOOLEANsnapshotAcquired=nativeAllocation&&AcquireAllocationNativeContextSnapshot(allocation,&snapshot);",
            ),
            f"{aperture_name} must serialize allocation teardown before taking a Native Context snapshot",
        )
    for fragment in (
        "ADDRESS_AND_SIZE_TO_SPAN_PAGES(MmGetMdlVirtualAddress(mdl),MmGetMdlByteCount(mdl))",
        "SIZE_TallocationPage=static_cast<SIZE_T>(mdlOffset);",
        "BOOLEANbaseResolved=offsetInPages>=allocationPage;",
        "SIZE_TbasePage=baseResolved?offsetInPages-allocationPage:0;",
        "allocationPageCount<=aperturePageCount-basePage",
        "allocation->ApertureBaseValid?allocation->ApertureBasePage==basePage",
        "BOOLEANhostPlacementConsistent=",
        "allocation->HostState==VioGpuWddmAllocationHostNone&&!allocation->PlacementValid",
        "allocation->HostState==VioGpuWddmAllocationHostLive&&allocation->PlacementValid",
        "PFN_NUMBERpfn=mdlPfns[static_cast<SIZE_T>(mdlOffset)+page];",
        "currentState==VioGpuWddmAperturePageMapped",
        "currentState!=VioGpuWddmAperturePageMapped",
        "allocation->AperturePfns[allocationPageIndex]=mdlPfns[static_cast<SIZE_T>(mdlOffset)+page];",
        "allocation->ApertureMappedPages[allocationPageIndex]=VioGpuWddmAperturePageMapped;",
        "allocation->ApertureMappedPageCount+=pagesBecomingMapped;",
        "allocation->ApertureMappedPageCount!=allocation->AperturePageCount",
        "AllocateApertureBackingEntries(allocation,&entries,&entryCount)",
        "EnsureApertureCpuMapping(allocation)",
        "snapshot.Adapter->CreateNativeGuestAllocation(",
        "adapter->Create2DResourceBacking(",
    ):
        if fragment not in map_allocation:
            fail(f"aperture map must attach ordinary VidMm page backing: {fragment}")
    if "if(nativeAllocation&&!snapshotAcquired)" not in map_allocation or "resetRetired" in map_allocation:
        fail("aperture map must remain fail-closed when a Native Context snapshot is unavailable")
    if "RtlMoveMemory(allocation->AperturePfns" in map_allocation or "rebasePages" in map_allocation:
        fail("aperture page identity must derive from MdlOffset, not mutate with partial-map call order")
    sg = canonical_code(function_body("BuildPfnEntries", WDDM_DDI_CODE))
    for fragment in (
        "static_cast<ULONGLONG>(pfn)>(MAXULONGLONG>>PAGE_SHIFT)",
        "ULONGLONGphysicalAddress=static_cast<ULONGLONG>(pfn)<<PAGE_SHIFT;",
        "physicalAddress>MAXULONGLONG-(PAGE_SIZE-1)",
        "previous->addr+previous->length==physicalAddress",
        "entries[*entryCount].addr=physicalAddress;",
        "entries[*entryCount].length=PAGE_SIZE;",
        "entries[*entryCount].padding=0;",
    ):
        if fragment not in sg:
            fail(f"VidMm PFN backing must become bounded VirtIO SG entries: {fragment}")

    page_state = canonical_code(
        function_body_with_parameters(
            "ValidateAperturePageState",
            "_In_ const VIOGPU_WDDM_ALLOCATION *allocation, _In_ BOOLEAN requireComplete",
            WDDM_DDI_CODE,
        )
    )
    for fragment in (
        "allocation->AperturePfns==NULL||allocation->ApertureMappedPages==NULL",
        "allocation->ApertureMappedPageCount>allocation->AperturePageCount",
        "pageState>VioGpuWddmAperturePageDummy",
        "static_cast<ULONGLONG>(pfn)>(MAXULONGLONG>>PAGE_SHIFT)",
        "observedMappedPages!=allocation->ApertureMappedPageCount",
        "!requireComplete||observedMappedPages==allocation->AperturePageCount",
    ):
        if fragment not in page_state:
            fail(f"aperture page-state validation must reject inconsistent PFN metadata: {fragment}")
    if canonical_code(WDDM_DDI_CODE).count("ValidateAperturePageState(allocation,TRUE)") != 2 or \
       canonical_code(WDDM_DDI_CODE).count("ValidateAperturePageState(allocation,FALSE)") != 1:
        fail("full backing consumers and partial unmap must use the shared aperture page-state validator")

    allocate_sg = canonical_code(function_body("AllocateApertureBackingEntries", WDDM_DDI_CODE))
    for fragment in (
        "allocation->ApertureMappedPageCount!=allocation->AperturePageCount",
        "allocation->ApertureMappedPages[page]!=VioGpuWddmAperturePageMapped",
        "VIOGPU_MAX_BACKING_ENTRIES",
        "BuildPfnEntries(allocation->AperturePfns,allocation->AperturePageCount,newEntries,entryCapacity,entryCount)",
        "ExFreePoolWithTag(newEntries,);",
    ):
        if fragment not in allocate_sg:
            fail(f"allocation SG ownership must remain bounded and complete: {fragment}")

    cpu_mapping = canonical_code(function_body("EnsureApertureCpuMapping", WDDM_DDI_CODE))
    for fragment in (
        "IoAllocateMdl(NULL,static_cast<ULONG>(allocation->BackingSize),FALSE,FALSE,NULL)",
        "RtlCopyMemory(MmGetMdlPfnArray(mdl),allocation->AperturePfns,",
        "mdl->MdlFlags|=MDL_PAGES_LOCKED;",
        "MmMapLockedPagesSpecifyCache(mdl,KernelMode,MmCached,NULL,FALSE,NormalPagePriority|MdlMappingNoExecute)",
        "allocation->ApertureMdl=mdl;",
        "allocation->ApertureAddress=address;",
    ):
        if fragment not in cpu_mapping:
            fail(f"CPU aperture access must use one driver-owned PFN MDL mapping: {fragment}")

    release_cpu = canonical_code(function_body("ReleaseApertureCpuMapping", WDDM_DDI_CODE))
    require_order(
        release_cpu,
        (
            "MmUnmapLockedPages(allocation->ApertureAddress,allocation->ApertureMdl);",
            "allocation->ApertureMdl->MdlFlags&=~MDL_PAGES_LOCKED;",
            "IoFreeMdl(allocation->ApertureMdl);",
        ),
        "driver-owned CPU aperture mapping must unmap before freeing its synthetic MDL",
    )

    unmap_allocation = canonical_code(function_body("UnmapApertureAllocation", WDDM_DDI_CODE))
    for fragment in (
        "dummyPage.QuadPart<0",
        "numberOfPages>(VIOGPU_WDDM_APERTURE_SIZE>>PAGE_SHIFT)-offsetInPages",
        "snapshot.Adapter->DestroyNativeGuestAllocation(",
        "adapter->Destroy2DResource(",
        "adapter->Detach2DScanoutResource(allocation->ResourceId,&detached)",
        "nativeAllocation&&allocation->HostState!=VioGpuWddmAllocationHostNone&&!snapshotAcquired",
        "BOOLEANresetRetired=nativeAllocation&&AllocationResetRetired(allocation);",
        "nativeAllocation&&allocation->HostState!=VioGpuWddmAllocationHostNone&&!snapshotAcquired&&!resetRetired",
        "BOOLEANreleased=FALSE;",
        "if(allocation->HostState==VioGpuWddmAllocationHostNone){released=TRUE;}",
        "elseif(resetRetired){ClearAllocationHostBinding(allocation);released=TRUE;}",
        "if(allocation->Resource2DState==VioGpu2DResourceNone){released=TRUE;}",
        "ReleaseApertureCpuMapping(allocation);",
        "static_cast<ULONGLONG>(dummyPage.QuadPart)>>PAGE_SHIFT",
        "allocation->ApertureMappedPages[allocationPageIndex]=VioGpuWddmAperturePageDummy;",
        "allocation->ApertureMappedPageCount-=mappedPagesToRemove;",
        "allocation->AperturePfns[allocationPageIndex]=dummyPfn;",
        "allocation->ApertureMappedPageCount==0",
        "allocation->ApertureBaseValid=FALSE;",
    ):
        if fragment not in unmap_allocation:
            fail(f"partial aperture unmap must retire host ownership and install DummyPage state: {fragment}")
    require_order(
        unmap_allocation,
        (
            "BOOLEANsnapshotAcquired=nativeAllocation&&AcquireAllocationNativeContextSnapshot(allocation,&snapshot);",
            "BOOLEANresetRetired=nativeAllocation&&AllocationResetRetired(allocation);",
            "if(nativeAllocation&&!snapshotAcquired&&!resetRetired)",
        ),
        "aperture unmap must evaluate confirmed reset retirement before rejecting a dead registration",
    )

    allocation_retirement = canonical_code(
        function_body_with_parameters(
            "AllocationResetRetired",
            "VIOGPU_WDDM_ALLOCATION *allocation",
            WDDM_DDI_CODE,
        )
    )
    for fragment in (
        "VioGpuAdapter::IsNativeContextAllocationBindingRetired(allocation->NativeContext)",
        "allocation->Adapter->IsNativeContextResetRetired(allocation->ContextResetGeneration)",
    ):
        if fragment not in allocation_retirement:
            fail(f"Native allocation reset retirement must require confirmed generation proof: {fragment}")
    host_release = min(
        unmap_allocation.find("snapshot.Adapter->DestroyNativeGuestAllocation("),
        unmap_allocation.find("adapter->Destroy2DResource("),
    )
    release_mapping = unmap_allocation.find("ReleaseApertureCpuMapping(allocation);", host_release)
    replace_pfn = unmap_allocation.find("allocation->AperturePfns[allocationPageIndex]=dummyPfn;", release_mapping)
    if min(host_release, release_mapping, replace_pfn) < 0 or not host_release < release_mapping < replace_pfn:
        fail("aperture unmap must retire host ownership before changing retained PFNs")
    if "BOOLEANreleased=NT_SUCCESS(status);" in unmap_allocation:
        fail("aperture unmap must not publish DummyPage state after a failed Host retirement")

    destroy_allocation = canonical_code(function_body("VioGpuWddmDestroyAllocation", WDDM_DDI_CODE))
    destroy_host = destroy_allocation.find(
        "status=ReleaseAllocationHostOwnership(allocation,&snapshot,snapshotAcquired);"
    )
    destroy_placement = destroy_allocation.find("ClearNativePlacement(allocation);", destroy_host)
    destroy_mapping = destroy_allocation.find("ReleaseApertureMapping(allocation);", destroy_placement)
    if min(destroy_host, destroy_placement, destroy_mapping) < 0 or not (
        destroy_host < destroy_placement < destroy_mapping
    ):
        fail("native allocation destroy must release Host ownership, placement, and retained MDL state in order")

    software = canonical_code(function_body("BuildSoftwarePagingTransaction", WDDM_DDI_CODE))
    for fragment in (
        "ResolveTransferMdlAddress(transferMdl,mdlOffset,transferSize,&systemAddress);",
        "CopyAperturePlacement(allocation,",
        "FillAperturePlacement(allocation,transferSize,fillPattern);",
        "packet->ContextId=nativeAllocation?allocation->ContextId:0;",
        "transaction->ContextId=packet->ContextId;",
        "transaction->TransferDataComplete=transferDataComplete;",
        "AcquireAllocationSubmissionReference(allocation,adapter);",
        "InterlockedExchange(&transaction->ReferenceHeld,1);",
        "InterlockedExchange(&transaction->State,VioGpuWddmPagingTransactionBuilt);",
    ):
        if fragment not in software:
            fail(f"software paging must retain exact MDL transfer ownership: {fragment}")
    require_order(
        software,
        (
            "AcquireAllocationSubmissionReference(allocation,adapter);",
            "InterlockedExchange(&transaction->ReferenceHeld,1);",
            "InterlockedExchange(&transaction->State,VioGpuWddmPagingTransactionBuilt);",
        ),
        "a paging record must acquire its allocation owner before publishing Built",
    )


def check_wddm_standard_primary_scanout() -> None:
    queue_source = QUEUE_SOURCE_PATH.read_text(encoding="utf-8")
    queue = canonical_code(QUEUE_CODE)
    set_scanout_start = queue_source.find("VIOGPU_HOST_CONTEXT_RESULT CtrlQueue::SetScanoutSynchronous")
    if set_scanout_start < 0:
        fail("standard primary scanout must expose one synchronous control transaction")
    previous_page_end = queue_source.rfind("PAGED_CODE_SEG_END", 0, set_scanout_start)
    previous_page_begin = queue_source.rfind("PAGED_CODE_SEG_BEGIN", 0, set_scanout_start)
    next_page_begin = queue_source.find("PAGED_CODE_SEG_BEGIN", set_scanout_start)
    if previous_page_end < previous_page_begin or next_page_begin < set_scanout_start:
        fail("SetScanoutSynchronous must stay in the nonpaged code segment")
    nonpaged_bodies = {
        "SubmitSynchronousLocked(three arguments)": function_body_with_parameters(
            "CtrlQueue::SubmitSynchronousLocked",
            "PGPU_VBUFFER buf, _Out_ PBOOLEAN release_buffer, _Out_ PBOOLEAN submitted",
            QUEUE_CODE,
        ),
        "SubmitSynchronousNoDataLocked": function_body("CtrlQueue::SubmitSynchronousNoDataLocked", QUEUE_CODE),
        "SetScanoutSynchronous": function_body("CtrlQueue::SetScanoutSynchronous", QUEUE_CODE),
    }
    for method_name, body in nonpaged_bodies.items():
        if "PAGED_CODE();" in body:
            fail(f"nonpaged source-address path must not call a pageable queue method: {method_name}")
    set_queue = canonical_code(function_body("CtrlQueue::SetScanoutSynchronous", QUEUE_CODE))
    if set_queue.count("SubmitSynchronousNoDataLocked(vbuf)") != 1:
        fail("nonpaged scanout must retain exact synchronous response classification")

    adapter_header = canonical_code(VIOGPU_HEADER_CODE)
    for fragment in (
        "KMUTEXm_2DScanoutMutex;",
        "BOOLEANm_2DResourceIdsInitialized;",
        "UINTm_2DScanoutResourceId;",
        "BOOLEANm_2DScanoutUnknown;",
        "ULONGLONGm_2DScanoutResetGeneration;",
        "volatileLONG64m_2DRetiredResetGeneration;",
    ):
        if adapter_header.count(fragment) != 1:
            fail(f"adapter must retain one serialized 2D scanout owner: {fragment}")

    set_host = canonical_code(function_body("VioGpuAdapter::Set2DScanout", VIOGPU_CODE))
    for fragment in (
        "KeGetCurrentIrql()!=PASSIVE_LEVEL",
        "KeWaitForSingleObject(&m_2DScanoutMutex,Executive,KernelMode,FALSE,&timeout)",
        "Reconcile2DScanoutAfterResetLocked();",
        "*previousResourceId=m_2DScanoutResourceId;",
        "if(m_2DScanoutUnknown)",
        "m_CtrlQueue.SetScanoutSynchronous(scanoutId,resourceId,width,height,0,0)",
        "if(result==VioGpuHostContextConfirmed)",
        "m_2DScanoutResourceId=resourceId;",
        "m_2DScanoutResetGeneration=resourceId==0?0:operationGeneration;",
        "elseif(result==VioGpuHostContextUnknown)",
        "m_2DScanoutUnknown=TRUE;",
        "m_2DScanoutResetGeneration=operationGeneration;",
        "FailNativeContextAtAnyIrql();",
        "KeReleaseMutex(&m_2DScanoutMutex,FALSE);",
    ):
        if fragment not in set_host:
            fail(f"2D scanout switch must retain confirmed-only serialized ownership: {fragment}")
    if "m_2DScanoutResourceId=resourceId;" in set_host.split("if(result==VioGpuHostContextConfirmed)", 1)[0]:
        fail("2D scanout ownership must not publish before Host confirmation")

    detach_host = canonical_code(function_body("VioGpuAdapter::Detach2DScanoutResource", VIOGPU_CODE))
    for fragment in (
        "KeWaitForSingleObject(&m_2DScanoutMutex,Executive,KernelMode,FALSE,&timeout)",
        "Reconcile2DScanoutAfterResetLocked();",
        "if(m_2DScanoutResourceId!=resourceId)",
        "m_CtrlQueue.SetScanoutSynchronous(0,0,0,0,0,0)",
        "if(result==VioGpuHostContextConfirmed)",
        "m_2DScanoutResourceId=0;",
        "m_2DScanoutResetGeneration=0;",
        "*detached=TRUE;",
        "elseif(result==VioGpuHostContextUnknown)",
        "m_2DScanoutUnknown=TRUE;",
        "FailNativeContextAtAnyIrql();",
        "KeReleaseMutex(&m_2DScanoutMutex,FALSE);",
    ):
        if fragment not in detach_host:
            fail(f"primary retirement must atomically detach only its own scanout: {fragment}")
    detach_decision = detach_host.find("if(m_2DScanoutResourceId!=resourceId)")
    detach_command = detach_host.find("m_CtrlQueue.SetScanoutSynchronous(0,0,0,0,0,0)")
    if min(detach_decision, detach_command) < 0 or detach_decision > detach_command:
        fail("primary retirement must identify the current owner before disabling the scanout")

    detach_dod = canonical_code(function_body("VioGpuDod::Detach2DScanoutResource", VIOGPU_CODE))
    for fragment in (
        "*detached=FALSE;",
        "AcquireNativeSubmissionOperation()",
        "adapter->Detach2DScanoutResource(resourceId,detached)",
        "ReleaseNativeSubmissionOperation();",
    ):
        if fragment not in detach_dod:
            fail(f"primary detach must retain the outer transport rundown: {fragment}")

    visibility = canonical_code(function_body("VioGpuDod::SetVidPnSourceVisibility", VIOGPU_CODE))
    visibility_publish = visibility.find("m_CurrentMode.Flags.SourceNotVisible=!(pSetVidPnSourceVisibility->Visible);")
    if visibility_publish < 0 or "#if!defined(VIOGPU_NATIVE_CONTEXT)" not in visibility:
        fail("Native Context visibility changes must preserve the programmed primary scanout")
    if "Set2DScanout(" in visibility or "Detach2DScanoutResource(" in visibility:
        fail("visibility changes must not retire the primary scanout programmed by SetVidPnSourceAddress")

    query_host = canonical_code(function_body("VioGpuAdapter::Query2DScanoutResource", VIOGPU_CODE))
    for fragment in (
        "Reconcile2DScanoutAfterResetLocked();",
        "BOOLEANvalid=!m_2DScanoutUnknown;",
        "*active=m_2DScanoutResourceId==resourceId;",
        "KeReleaseMutex(&m_2DScanoutMutex,FALSE);",
    ):
        if query_host.count(fragment) != 1:
            fail(f"2D scanout ownership query must fail closed: {fragment}")

    set_ddi = canonical_code(function_body("VioGpuWddmSetVidPnSourceAddress", WDDM_DDI_CODE))
    for fragment in (
        "KeGetCurrentIrql()!=PASSIVE_LEVEL",
        "setVidPnSourceAddress->VidPnSourceId!=0",
        "setVidPnSourceAddress->ContextCount!=0",
        "setVidPnSourceAddress->Flags.Value!=1",
        "setVidPnSourceAddress->PrimarySegment!=VIOGPU_WDDM_SEGMENT_ID",
        "IsStandardPrimaryAllocation(allocation)",
        "EnsureStandard2DAllocationBacking(allocation)",
        "allocation->Resource2DState!=VioGpu2DResourceBackingAttached",
        "!allocation->PlacementValid",
        "setVidPnSourceAddress->PrimaryAddress.QuadPart)!=allocation->PlacementOffset",
        "adapter->Set2DScanout(0,allocation->ResourceId,allocation->Width,allocation->Height,&previousResourceId)",
        "result==VioGpuHostContextConfirmed?STATUS_SUCCESS:STATUS_DEVICE_NOT_READY",
    ):
        if fragment not in set_ddi:
            fail(f"SetVidPnSourceAddress must retain the exact mode-change primary contract: {fragment}")
    if "STATUS_NOT_SUPPORTED" in set_ddi:
        fail("SetVidPnSourceAddress must no longer reject the completed standard primary mode-change path")

    destroy_allocation = canonical_code(function_body("VioGpuWddmDestroyAllocation", WDDM_DDI_CODE))
    unmap_allocation = canonical_code(function_body("UnmapApertureAllocation", WDDM_DDI_CODE))
    if destroy_allocation.count("Detach2DScanoutResource(allocation->ResourceId,&detached)") != 1:
        fail("DestroyAllocation must detach its exact scanout before unref")
    if unmap_allocation.count("Detach2DScanoutResource(allocation->ResourceId,&detached)") != 1:
        fail("standard primary aperture unmap must detach its exact scanout before unref")
    if "Query2DScanoutResource(allocation->ResourceId" in destroy_allocation or \
       "Query2DScanoutResource(allocation->ResourceId" in unmap_allocation:
        fail("primary retirement must not use a racy query-then-disable scanout sequence")

    query_caps = canonical_code(function_body("VioGpuDod::QueryAdapterInfo", VIOGPU_CODE))
    wddm_query_caps = canonical_code(function_body("VioGpuWddmQueryAdapterInfo", WDDM_DDI_CODE))
    if "RtlZeroMemory(pDriverCaps,pQueryAdapterInfo->OutputDataSize);" not in query_caps or \
       "FlipOnVSyncMmIo" in query_caps or "FlipOnVSyncMmIo" in wddm_query_caps:
        fail("the synchronous PASSIVE_LEVEL scanout path must not advertise MMIO flip capability")


def check_wddm_present_contract() -> None:
    header = canonical_code(WDDM_DDI_HEADER_CODE)
    present_display = canonical_code(function_body("VioGpuAdapter::ExecutePresentDisplayOnly", VIOGPU_CODE))
    for fragment in (
        "if(!m_CtrlQueue.TransferToHost2D",
        "if(!m_CtrlQueue.ResFlush",
        "returnSTATUS_DEVICE_NOT_READY;",
    ):
        if present_display.count(fragment) != (2 if fragment == "returnSTATUS_DEVICE_NOT_READY;" else 1):
            fail(f"display-only Present must propagate 2D queue failure: {fragment}")
    packet_matches = re.findall(
        r"\bstruct\s+VIOGPU_WDDM_PRESENT_DMA_PACKET\s*\{(.*?)\}\s*;",
        WDDM_DDI_HEADER_CODE,
        re.DOTALL,
    )
    transaction_matches = re.findall(
        r"\bstruct\s+VIOGPU_WDDM_PRESENT_TRANSACTION\s*\{(.*?)\}\s*;",
        WDDM_DDI_HEADER_CODE,
        re.DOTALL,
    )
    if len(packet_matches) != 1 or len(transaction_matches) != 1:
        fail("Present must expose exactly one DMA packet and one transaction record")
    packet = canonical_code(packet_matches[0])
    transaction = canonical_code(transaction_matches[0])
    for fragment in (
        "VioGpuWddmDmaKindPresent=3,",
        "static_assert(sizeof(VIOGPU_WDDM_PRESENT_DMA_PACKET)==56,",
        "VioGpuWddmContextNative=0,VioGpuWddmContextSystem,VioGpuWddmContextGdi,",
        "VioGpuWddmPresentInvalid=0,VioGpuWddmPresentBuilt,VioGpuWddmPresentPatched,"
        "VioGpuWddmPresentQueued,VioGpuWddmPresentExecuting,VioGpuWddmPresentFinished,"
        "VioGpuWddmPresentCancelled,",
    ):
        if header.count(fragment) != 1:
            fail(f"Present ABI must retain its exact kind, context, and state values: {fragment}")
    for field in (
        "ULONGSignature;",
        "USHORTVersion;",
        "USHORTSize;",
        "UINTFlags;",
        "UINTSourceResourceId;",
        "UINTDestinationResourceId;",
        "UINTRectCount;",
        "UINTReserved;",
        "ULONGLONGSourcePlacementOffset;",
        "ULONGLONGDestinationPlacementOffset;",
        "ULONGLONGDestinationResetGeneration;",
    ):
        if packet.count(field) != 1:
            fail(f"Present DMA packet must retain one exact field: {field}")
    for field in (
        "volatileLONGReferenceCount;",
        "volatileLONGState;",
        "volatileLONGCancelRequested;",
        "volatileLONGWorkReferenceHeld;",
        "VIOGPU_WDDM_CONTEXT_SUBMISSION_ENTRYContextEntry;",
        "LIST_ENTRYAdapterLink;",
        "VIOGPU_NATIVE_PASSIVE_WORKWork;",
        "VIOGPU_WDDM_CONTEXT*Context;",
        "VIOGPU_WDDM_ALLOCATION*Source;",
        "VIOGPU_WDDM_ALLOCATION*Destination;",
        "RECT*DestinationSubRects;",
        "UINTRectCount;",
        "ULONGLONGDestinationResetGeneration;",
        "UINTFenceId;",
        "BOOLEANFullyPrepatched;",
    ):
        if transaction.count(field) != 1:
            fail(f"Present transaction must retain one exact ownership field: {field}")

    validate_packet = canonical_code(
        function_body_with_parameters(
            "ValidatePresentDmaPacket",
            "_In_ const VIOGPU_WDDM_KMD_DMA_PRIVATE *privateData, "
            "_In_ const VIOGPU_WDDM_PRESENT_DMA_PACKET *packet, "
            "_In_ const VIOGPU_WDDM_PRESENT_TRANSACTION *transaction",
            WDDM_DDI_CODE,
        )
    )
    for fragment in (
        "privateData->Kind!=VioGpuWddmDmaKindPresent",
        "privateData->Submission!=transaction",
        "transaction->Source==transaction->Destination",
        "state!=VioGpuWddmPresentBuilt&&state!=VioGpuWddmPresentPatched&&"
        "state!=VioGpuWddmPresentQueued&&state!=VioGpuWddmPresentExecuting",
        "packet->Signature!=VIOGPU_WDDM_PRESENT_DMA_SIGNATURE",
        "packet->Version!=VioGpuWddmDmaPrivateVersion",
        "packet->Size!=sizeof(*packet)",
        "packet->SourceResourceId!=transaction->Source->ResourceId",
        "packet->DestinationResourceId!=transaction->Destination->ResourceId",
        "packet->RectCount!=transaction->RectCount",
        "packet->SourcePlacementOffset!=transaction->SourcePlacementOffset",
        "packet->DestinationPlacementOffset!=transaction->DestinationPlacementOffset",
        "packet->DestinationResetGeneration!=transaction->DestinationResetGeneration",
    ):
        if fragment not in validate_packet:
            fail(f"Present DMA validation must bind the packet to one live transaction: {fragment}")

    gdi_source = canonical_code(
        function_body_with_parameters(
            "IsGdiSourceAllocation",
            "const VIOGPU_WDDM_ALLOCATION *allocation",
            WDDM_DDI_CODE,
        )
    )
    if gdi_source != (
        "returnIsStandardAllocation(allocation)&&!IsStandardPrimaryAllocation(allocation)&&"
        "(allocation->Flags&VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE)!=0;"
    ):
        fail("GDI Present must accept only a CPU-visible non-primary standard allocation")
    native_identity = canonical_code(function_body("HasLiveNativePresentIdentity", WDDM_DDI_CODE))
    for fragment in (
        "context->Type==VioGpuWddmContextNative",
        "IsNativeAllocation(allocation)",
        "allocation->NativeContext==&context->NativeContext",
        "allocation->HostState==VioGpuWddmAllocationHostLive",
        "allocation->BlobId==allocation->ResourceId",
        "allocation->BoundContextId==allocation->ContextId",
        "allocation->BoundGeneration==allocation->ContextGeneration",
        "allocation->BoundResetGeneration==allocation->ContextResetGeneration",
    ):
        if native_identity.count(fragment) != 1:
            fail(f"Native Present source must retain its exact live identity: {fragment}")
    gdi_identity = canonical_code(function_body("HasGdiPresentIdentity", WDDM_DDI_CODE))
    for fragment in (
        "context->Type==VioGpuWddmContextGdi",
        "IsGdiSourceAllocation(allocation)",
        "allocation->HostState==VioGpuWddmAllocationHostNone",
        "allocation->BlobId==0",
        "allocation->ResourceId!=0",
        "allocation->ResourceId<VIOGPU_NATIVE_RESOURCE_ID_START",
        "allocation->ContextId==0",
        "allocation->ContextGeneration==0",
        "allocation->ContextResetGeneration==0",
    ):
        if gdi_identity.count(fragment) != 1:
            fail(f"GDI Present source must retain its exact allocation identity: {fragment}")
    live_gdi_identity = canonical_code(function_body("HasLiveGdiPresentIdentity", WDDM_DDI_CODE))
    for fragment in (
        "HasGdiPresentIdentity(allocation,context,adapter)",
        "allocation->Resource2DState==VioGpu2DResourceBackingAttached",
        "allocation->Resource2DResetGeneration!=0",
        "allocation->PlacementValid",
        "allocation->ApertureMdl!=NULL",
        "allocation->ApertureAddress!=NULL",
        "allocation->ApertureMappedPageCount==allocation->AperturePageCount",
    ):
        if live_gdi_identity.count(fragment) != 1:
            fail(f"GDI Present source must retain its exact live standard-2D identity: {fragment}")
    for retired_fragment in (
        "allocation->Resource2DState==VioGpu2DResourceNone",
        "allocation->Resource2DResetGeneration==0",
    ):
        if retired_fragment in live_gdi_identity:
            fail(f"paged-in GDI Present source must not require retired 2D identity: {retired_fragment}")

    allocation_parameter = "VIOGPU_WDDM_ALLOCATION *allocation"
    ensure_primary = canonical_code(
        function_body_with_parameters("EnsureStandard2DAllocationBacking", allocation_parameter, WDDM_DDI_CODE)
    )
    reconcile_gdi = canonical_code(
        function_body_with_parameters("ReconcileGdiSourcePlacementAfterReset", allocation_parameter, WDDM_DDI_CODE)
    )
    if "allocation->ApertureMdl!=NULL" not in ensure_primary or \
       "allocation->ApertureAddress!=NULL" not in ensure_primary or \
       "allocation->ApertureMdl!=NULL" not in reconcile_gdi or \
       "allocation->ApertureAddress!=NULL" not in reconcile_gdi or \
       "EnsureStandard2DAllocationBacking(allocation)" not in reconcile_gdi or \
       "allocation->Resource2DState==VioGpu2DResourceBackingAttached" not in reconcile_gdi or \
       "allocation->Resource2DResetGeneration!=0" not in reconcile_gdi:
        fail("Present reset reconciliation must retain the current attached VidMm-backed 2D identity")

    geometry = canonical_code(
        function_body_with_parameters(
            "ValidatePresentGeometry",
            "_In_ const VIOGPU_WDDM_ALLOCATION *source, _In_ const VIOGPU_WDDM_ALLOCATION *destination, "
            "_In_ const RECT *sourceRect, _In_ const RECT *destinationRect, "
            "_In_reads_(rectCount) const RECT *destinationSubRects, _In_ UINT rectCount",
            WDDM_DDI_CODE,
        )
    )
    prepatch_entry = canonical_code(
        function_body_with_parameters(
            "ValidatePresentPrepatchEntry",
            "_In_ const DXGK_ALLOCATIONLIST *entry, _In_ const VIOGPU_WDDM_ALLOCATION *allocation, "
            "_In_ BOOLEAN writeOperation, _Out_ BOOLEAN *prepatched",
            WDDM_DDI_CODE,
        )
    )
    if canonical_code(WDDM_DDI_CODE).count("constUINTVIOGPU_WDDM_PRESENT_RECTS_PER_PASS=256;") != 1:
        fail("Present multipass must retain one bounded subrectangle chunk size")
    for fragment in (
        "rectCount==0||rectCount>VIOGPU_WDDM_PRESENT_RECTS_PER_PASS",
        "source->Format!=destination->Format",
        "static_cast<ULONGLONG>(source->Pitch)*source->Height>source->BackingSize",
        "static_cast<ULONGLONG>(destination->Pitch)*destination->Height>destination->BackingSize",
        "sourceRect->right-sourceRect->left!=destinationRect->right-destinationRect->left",
        "LONGLONGsourceLeft=",
        "rect->left<destinationRect->left",
        "sourceRight>sourceRect->right",
        "sourceBottom>sourceRect->bottom",
    ):
        if fragment not in geometry:
            fail(f"Present geometry must remain bounded before any aperture copy: {fragment}")
    for fragment in (
        "*prepatched=FALSE;",
        "entry->Reserved!=0",
        "(entry->WriteOperation!=0)!=(writeOperation!=FALSE)",
        "entry->SegmentId==0",
        "entry->SegmentId!=VIOGPU_WDDM_SEGMENT_ID",
        "static_cast<ULONGLONG>(entry->PhysicalAddress.QuadPart)!=allocation->PlacementOffset",
        "*prepatched=TRUE;",
    ):
        if fragment not in prepatch_entry:
            fail(f"Present prepatch input must retain its exact placement gate: {fragment}")

    dereference = canonical_code(function_body("DereferencePresentTransaction", WDDM_DDI_CODE))
    require_order(
        dereference,
        (
            "LONGreferences=InterlockedDecrement(&transaction->ReferenceCount);",
            "VIOGPU_WDDM_CONTEXT*context=transaction->Context;",
            "ReleaseAllocationSubmissionReference(transaction->Source);",
            "ReleaseAllocationSubmissionReference(transaction->Destination);",
            "ReleaseContextSubmissionReference(context);",
            "InterlockedCompareExchange(&transaction->WorkReferenceHeld,0,0)==0",
            "deletetransaction;",
        ),
        "final Present release must drop allocation, context, and work owners exactly once",
    )
    register = canonical_code(function_body("RegisterPresentTransaction", WDDM_DDI_CODE))
    unregister = canonical_code(function_body("UnregisterPresentTransaction", WDDM_DDI_CODE))
    if register.count("ReferencePresentTransaction(transaction)") != 1 or \
       register.count("RegisterWddmPresentTransaction(&transaction->AdapterLink)") != 1 or \
       register.count("DereferencePresentTransaction(transaction);") != 1:
        fail("Present registry insertion must reserve and roll back one adapter-list reference")
    require_order(
        unregister,
        (
            "UnregisterWddmPresentTransaction(&transaction->AdapterLink)",
            "DereferencePresentTransaction(transaction);",
        ),
        "Present registry removal must release its transferred list reference",
    )

    resolve = canonical_code(
        function_body_with_parameters(
            "ResolvePresentTransaction",
            "PVOID privateDataBase, UINT privateDataSize, UINT submissionStart, UINT submissionEnd, "
            "VioGpuDod *adapter, HANDLE runtimeContext, LONG expectedState, "
            "VIOGPU_WDDM_PRESENT_TRANSACTION **transactionOut",
            WDDM_DDI_CODE,
        )
    )
    for fragment in (
        "KeAcquireSpinLock(&context->SubmissionLock,&oldIrql);",
        "entry->Kind==VioGpuWddmContextSubmissionPresent",
        "entry->Owner==privateData->Submission",
        "ReferencePresentTransaction(candidate)",
        "HasLiveNativePresentIdentity(transaction->Source,context,adapter)",
        "HasGdiPresentIdentity(transaction->Source,context,adapter)",
        "privateData->ContextId==0&&privateData->Generation==0&&privateData->ResetGeneration==0",
        "transaction->Source->ContextId==0",
        "transaction->Source->ContextGeneration==0",
        "transaction->Source->ContextResetGeneration==0",
        "ValidatePresentDmaPacket(privateData,packet,transaction)",
        "DereferencePresentTransaction(transaction);",
    ):
        if fragment not in resolve:
            fail(f"Present resolution must retain exact linked and Native/GDI identity: {fragment}")

    retire = canonical_code(function_body("RetirePresentTransaction", WDDM_DDI_CODE))
    require_order(
        retire,
        (
            "KeAcquireSpinLock(&context->SubmissionLock,&oldIrql);",
            "InterlockedCompareExchange(&transaction->State,finalState,expectedState)==expectedState",
            "RemoveEntryList(&transaction->ContextEntry.Link);",
            "transaction->PrivateData->Submission=NULL;",
            "KeReleaseSpinLock(&context->SubmissionLock,oldIrql);",
            "UnregisterPresentTransaction(transaction);",
            "DereferencePresentTransaction(transaction);",
        ),
        "Present retirement must detach context and adapter owners before its terminal release",
    )

    adapter_header = canonical_code(VIOGPU_HEADER_CODE)
    for field in (
        "KSPIN_LOCKm_WddmPresentLock;",
        "LIST_ENTRYm_WddmPresentTransactions;",
        "volatileLONGm_WddmPresentClosing;",
        "WORK_QUEUE_ITEMm_WddmDrainWorkItem;",
        "volatileLONGm_WddmDrainWorkerQueued;",
        "volatileLONGm_WddmDrainRequested;",
        "KEVENTm_WddmDrainIdleEvent;",
    ):
        if adapter_header.count(field) != 1:
            fail(f"adapter must retain one Present reset registry field: {field}")
    constructor = canonical_code(function_body("VioGpuDod::VioGpuDod", VIOGPU_CODE))
    destructor = canonical_code(function_body("VioGpuDod::~VioGpuDod", VIOGPU_CODE))
    for fragment in (
        "KeInitializeSpinLock(&m_WddmPresentLock);",
        "InitializeListHead(&m_WddmPresentTransactions);",
        "m_WddmPresentClosing=TRUE;",
        "ExInitializeWorkItem(&m_WddmDrainWorkItem,WddmSubmissionDrainWorker,this);",
        "m_WddmDrainWorkerQueued=FALSE;",
        "m_WddmDrainRequested=FALSE;",
        "KeInitializeEvent(&m_WddmDrainIdleEvent,NotificationEvent,TRUE);",
    ):
        if constructor.count(fragment) != 1:
            fail(f"adapter construction must start with Present publication closed: {fragment}")
    if "NT_ASSERT(IsListEmpty(&m_WddmPresentTransactions));" not in destructor or \
       "NT_ASSERT(m_WddmPresentClosing);" not in destructor or \
       "NT_ASSERT(InterlockedCompareExchange(&m_WddmDrainWorkerQueued,0,0)==0);" not in destructor or \
       "NT_ASSERT(InterlockedCompareExchange(&m_WddmDrainRequested,0,0)==0);" not in destructor:
        fail("adapter destruction must require a closed and empty Present registry")

    open_present = canonical_code(function_body("VioGpuDod::OpenWddmPresentTransactions", VIOGPU_CODE))
    register_present = canonical_code(function_body("VioGpuDod::RegisterWddmPresentTransaction", VIOGPU_CODE))
    pop_present = canonical_code(function_body("VioGpuDod::PopWddmPresentTransactionForReset", VIOGPU_CODE))
    for fragment in (
        "!IsHardwareResetRequested()&&InterlockedCompareExchange(&m_WddmDrainRequested,0,0)==0&&"
        "InterlockedCompareExchange(&m_WddmDrainWorkerQueued,0,0)==0",
        "InterlockedCompareExchange(&m_WddmPresentClosing,0,0)==0",
        "IsListEmpty(&m_WddmPresentTransactions)",
        "InterlockedExchange(&m_WddmPresentClosing,FALSE);",
    ):
        if open_present.count(fragment) != 1:
            fail(f"Present publication must be idempotent when open and may leave closed only in an empty Active epoch: {fragment}")
    if register_present.count(
        "InterlockedCompareExchange(&m_WddmPresentClosing,0,0)==0&&!IsHardwareResetRequested()&&"
        "link->Flink==link&&link->Blink==link"
    ) != 1:
        fail("Present registry insertion must atomically reject close and reset epochs")
    require_order(
        pop_present,
        ("RemoveHeadList(&m_WddmPresentTransactions)", "InitializeListHead(link);"),
        "reset drain must transfer one detached adapter-list reference",
    )

    drain = canonical_code(function_body("VioGpuWddmDrainPresentTransactions", WDDM_DDI_CODE))
    require_order(
        drain,
        (
            "adapter->CloseWddmPresentTransactions();",
            "adapter->PopWddmPresentTransactionForReset();",
            "InterlockedExchange(&transaction->CancelRequested,1);",
            "adapter->CancelNativePassiveWork(&transaction->Work);",
            "RetirePresentTransaction(transaction,VioGpuWddmPresentQueued,VioGpuWddmPresentCancelled);",
            "!adapter->IsHardwareResetRequested()",
            "adapter->NotifyNativeSubmissionFault(",
            "DereferencePresentTransaction(transaction);",
        ),
        "Present reset drain must close publication, cancel work, and release the transferred registry owner",
    )
    if drain.count("state==VioGpuWddmPresentBuilt||state==VioGpuWddmPresentPatched") != 1:
        fail("Present reset drain must retire both scheduler-unsubmitted states")

    request_drain = canonical_code(function_body("VioGpuDod::RequestWddmSubmissionDrainAtAnyIrql", VIOGPU_CODE))
    require_order(
        request_drain,
        (
            "InterlockedExchange(&m_NativePassiveClosing,TRUE);",
            "InterlockedExchange(&m_WddmPresentClosing,TRUE);",
            "InterlockedExchange(&m_WddmDrainRequested,TRUE);",
            "if(KeGetCurrentIrql()<=DISPATCH_LEVEL)",
            "QueueWddmSubmissionDrainWorker();",
            "elseif(m_DxgkInterface.DxgkCbQueueDpc!=NULL)",
            "m_DxgkInterface.DxgkCbQueueDpc(m_DxgkInterface.DeviceHandle);",
        ),
        "any-IRQL submission drain must close both publication gates before deferring passive cleanup",
    )
    queue_drain = canonical_code(function_body("VioGpuDod::QueueWddmSubmissionDrainWorker", VIOGPU_CODE))
    require_order(
        queue_drain,
        (
            "KeGetCurrentIrql()>DISPATCH_LEVEL",
            "InterlockedCompareExchange(&m_WddmDrainWorkerQueued,TRUE,FALSE)!=FALSE",
            "KeClearEvent(&m_WddmDrainIdleEvent);",
            "ExAcquireRundownProtection(&m_HardwareOperations)",
            "ExQueueWorkItem(&m_WddmDrainWorkItem,DelayedWorkQueue);",
        ),
        "deferred drain publication must reserve one worker and one hardware-rundown reference before queueing",
    )
    rundown_failure_blocks = [
        canonical_code(body)
        for condition, body, _, _ in if_blocks(function_body("VioGpuDod::QueueWddmSubmissionDrainWorker", VIOGPU_CODE))
        if canonical_code(condition) == "!ExAcquireRundownProtection(&m_HardwareOperations)"
    ]
    if rundown_failure_blocks != [
        "InterlockedExchange(&m_WddmDrainRequested,FALSE);"
        "InterlockedExchange(&m_WddmDrainWorkerQueued,FALSE);"
        "KeSetEvent(&m_WddmDrainIdleEvent,IO_NO_INCREMENT,FALSE);return;"
    ]:
        fail("a deferred-drain rundown failure must release the worker reservation and wake passive waiters")
    drain_worker = canonical_code(function_body("VioGpuDod::RunWddmSubmissionDrainWorker", VIOGPU_CODE))
    require_order(
        drain_worker,
        (
            "InterlockedExchange(&m_WddmDrainRequested,FALSE);",
            "CloseNativePassiveQueue();",
            "VioGpuWddmDrainPresentTransactions(this);",
            "if(IsHardwareResetRequested())",
            "InvalidateNativeFenceTracker();",
            "InterlockedCompareExchange(&m_WddmDrainRequested,0,0)!=0",
            "KeSetEvent(&m_WddmDrainIdleEvent,IO_NO_INCREMENT,FALSE);",
            "InterlockedExchange(&m_WddmDrainWorkerQueued,FALSE);",
            "ExReleaseRundownProtection(&m_HardwareOperations);",
        ),
        "the passive drain worker must close the queue, retire Present, publish idle, and release rundown ownership",
    )
    for fragment in (
        "if(InterlockedCompareExchange(&m_WddmDrainRequested,0,0)==0){break;}",
        "if(InterlockedCompareExchange(&m_WddmDrainWorkerQueued,TRUE,FALSE)==FALSE)",
        "KeClearEvent(&m_WddmDrainIdleEvent);continue;",
        "ExReleaseRundownProtection(&m_HardwareOperations);",
    ):
        if fragment not in drain_worker:
            fail(f"the passive drain worker must close the final request/publication race: {fragment}")
    wait_drain = canonical_code(function_body("VioGpuDod::WaitForWddmSubmissionDrain", VIOGPU_CODE))
    for fragment in (
        "KeGetCurrentIrql()!=PASSIVE_LEVEL",
        "InterlockedCompareExchange(&m_WddmDrainRequested,0,0)!=0&&"
        "InterlockedCompareExchange(&m_WddmDrainWorkerQueued,0,0)==0",
        "QueueWddmSubmissionDrainWorker();",
        "InterlockedCompareExchange(&m_WddmDrainRequested,0,0)==0&&"
        "InterlockedCompareExchange(&m_WddmDrainWorkerQueued,0,0)==0",
        "KeWaitForSingleObject(&m_WddmDrainIdleEvent,Executive,KernelMode,FALSE,NULL);",
    ):
        if fragment not in wait_drain:
            fail(f"passive submission-drain wait must retain its request/worker handshake: {fragment}")
    reset_declaration = "__declspec(noinline)VOIDRequestHardwareResetAtAnyIrql(void);"
    if canonical_code(VIOGPU_HEADER_CODE).count(reset_declaration) != 1:
        fail("the any-IRQL reset request entry must remain an out-of-line noinline provenance boundary")
    if len(re.findall(r"\b__declspec\s*\(\s*noinline\s*\)\s+VOID\s+"
                      r"VioGpuDod::RequestHardwareResetAtAnyIrql\s*\(", VIOGPU_SOURCE)) != 1:
        fail("the any-IRQL reset request definition must remain noinline")
    reset_request = canonical_code(function_body("VioGpuDod::RequestHardwareResetAtAnyIrql", VIOGPU_CODE))
    require_order(
        reset_request,
        (
            "ULONG_PTRimageBase=reinterpret_cast<ULONG_PTR>(&__ImageBase);",
            "ULONG_PTRreturnAddress=reinterpret_cast<ULONG_PTR>(_ReturnAddress());",
            "ULONG_PTRcallerRva=returnAddress>=imageBase?returnAddress-imageBase:0;",
            "InterlockedExchange(&m_HardwareResetState,VioGpuHardwareResetRequested);",
            "previousState==VioGpuHardwareActive&&callerRva!=0&&callerRva<=MAXULONG",
            "InterlockedCompareExchange(&m_HardwareResetCallerRva,static_cast<LONG>(callerRva),0);",
            "RequestWddmSubmissionDrainAtAnyIrql();",
        ),
        "every hardware reset request must capture first-active-epoch provenance before deferred drain",
    )
    if reset_request.count("InterlockedExchange(&m_HardwareResetState,VioGpuHardwareResetRequested);") != 1 or \
       reset_request.count("_ReturnAddress()") != 1 or reset_request.count("&__ImageBase") != 1:
        fail("reset provenance must retain one state publication and one module-relative caller sample")
    dpc = canonical_code(function_body("VioGpuDod::DpcRoutine", VIOGPU_CODE))
    if (
        "InterlockedCompareExchange(&m_WddmDrainRequested,0,0)!=0" not in dpc
        or "QueueWddmSubmissionDrainWorker();" not in dpc
    ):
        fail("the display DPC must convert a DIRQL drain request into a passive worker")

    present_body = function_body("VioGpuWddmPresent", WDDM_DDI_CODE)
    present = canonical_code(present_body)
    present_lifecycle = canonical_code(function_body("AcquirePresentAllocationLifecycles", WDDM_DDI_CODE))
    require_order(
        present_lifecycle,
        (
            "source==NULL||destination==NULL||source==destination||sourceLocked==NULL||destinationLocked==NULL||KeGetCurrentIrql()!=PASSIVE_LEVEL",
            "BOOLEANsourceFirst=reinterpret_cast<ULONG_PTR>(source)<reinterpret_cast<ULONG_PTR>(destination);",
            "VIOGPU_WDDM_ALLOCATION*first=sourceFirst?source:destination;",
            "VIOGPU_WDDM_ALLOCATION*second=sourceFirst?destination:source;",
            "status=AcquireAllocationLifecycle(first);",
            "status=AcquireAllocationLifecycle(second);",
        ),
        "Present allocation lifetimes must be acquired in one deterministic address order",
    )
    if present_lifecycle.count("returnstatus;") != 2:
        fail("Present allocation lifetime helper must retain both failure exits with the first lock owned by its caller")
    for fragment in (
        "KeGetCurrentIrql()!=PASSIVE_LEVEL",
        "present->SubRectCnt==0&&present->MultipassOffset!=0",
        "present->MultipassOffset>=present->SubRectCnt",
        "present->DmaSize<sizeof(VIOGPU_WDDM_PRESENT_DMA_PACKET)||present->PatchLocationListOutSize<2",
        "returnSTATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;",
        "UINTrectOffset=present->SubRectCnt==0?0:present->MultipassOffset;",
        "present->SubRectCnt-rectOffset",
        "remainingRectCount<VIOGPU_WDDM_PRESENT_RECTS_PER_PASS",
        "present->pDstSubRects+rectOffset",
        "context->Type!=VioGpuWddmContextNative&&context->Type!=VioGpuWddmContextGdi",
        "ExAcquireRundownProtection(&context->Operations)",
        "AcquireContextSubmissionReference(context)",
        "AcquireAllocationSubmissionReference(source,context->Device->Adapter)",
        "AcquireAllocationSubmissionReference(destination,context->Device->Adapter)",
        "AcquirePresentAllocationLifecycles(source,destination,&sourceLocked,&destinationLocked)",
        "HasLiveNativePresentIdentity(source,context,context->Device->Adapter)",
        "gdiCandidate&&HasGdiPresentIdentity(source,context,context->Device->Adapter)",
        "IsStandardPrimaryAllocation(destination)",
        "EnsureStandard2DAllocationBacking(destination)",
        "ValidatePresentGeometry(source,destination,",
        "destinationOpen->ReadOnly",
        "ValidatePresentPrepatchEntry(&present->pAllocationList[DXGK_PRESENT_SOURCE_INDEX],source,FALSE,&sourcePrepatched)",
        "ValidatePresentPrepatchEntry(&present->pAllocationList[DXGK_PRESENT_DESTINATION_INDEX],destination,TRUE,&destinationPrepatched)",
        "BOOLEANsourcePrepatchValid=ValidatePresentPrepatchEntry(",
        "BOOLEANdestinationPrepatchValid=ValidatePresentPrepatchEntry(",
        "sourcePrepatchValid&&sourcePrepatched&&gdiSource",
        "gdiSourcePrepatchLive=ReconcileGdiSourcePlacementAfterReset(source)&&"
        "HasLiveGdiPresentIdentity(source,context,context->Device->Adapter);",
        "sourcePrepatched&&gdiSource&&!gdiSourcePrepatchLive",
        "sourcePrepatched&&(!source->PlacementValid",
        "destinationPrepatched&&(!destination->PlacementValid",
        "destinationPrepatched&&(!EnsureStandard2DAllocationBacking(destination)",
        "transaction->State=VioGpuWddmPresentBuilt;",
        "transaction->FullyPrepatched=sourcePrepatched&&destinationPrepatched;",
        "packet->SourcePlacementOffset=transaction->SourcePlacementOffset;",
        "packet->DestinationPlacementOffset=transaction->DestinationPlacementOffset;",
        "privateData->Kind=VioGpuWddmDmaKindPresent;",
        "privateData->ContextId=source->ContextId;",
        "privateData->Generation=source->ContextGeneration;",
        "privateData->ResetGeneration=source->ContextResetGeneration;",
        "RegisterPresentTransaction(transaction)",
        "InsertTailList(&context->PendingSubmissions,&transaction->ContextEntry.Link);",
        "present->pDmaBuffer=static_cast<BYTE*>(present->pDmaBuffer)+sizeof(VIOGPU_WDDM_PRESENT_DMA_PACKET);",
        "present->pDmaBufferPrivateData=static_cast<BYTE*>(present->pDmaBufferPrivateData)+sizeof(VIOGPU_WDDM_KMD_DMA_PRIVATE);",
        "present->DmaBufferPrivateDataSize-=sizeof(VIOGPU_WDDM_KMD_DMA_PRIVATE);",
        "present->MultipassOffset=present->SubRectCnt==0?0:rectOffset+rectCount;",
        "present->MultipassOffset<present->SubRectCnt",
        "RetirePresentTransaction(transaction,VioGpuWddmPresentBuilt,VioGpuWddmPresentCancelled);",
        "ExReleaseRundownProtection(&context->Operations);",
    ):
        if fragment not in present:
            fail(f"Present build must retain its exact Native/GDI and ownership contract: {fragment}")
    require_order(
        present,
        (
            "BOOLEANsourcePrepatchValid=ValidatePresentPrepatchEntry(",
            "BOOLEANdestinationPrepatchValid=ValidatePresentPrepatchEntry(",
            "gdiSourcePrepatchLive=ReconcileGdiSourcePlacementAfterReset(source)&&"
            "HasLiveGdiPresentIdentity(source,context,context->Device->Adapter);",
            "destinationPrepatched&&(!EnsureStandard2DAllocationBacking(destination)",
        ),
        "Present build must defer SegmentId-zero residency and Host-backing checks to Patch",
    )
    private_zero = present.find("RtlZeroMemory(privateData,sizeof(*privateData));")
    if private_zero < 0 or "privateData->Submission" in present[:private_zero]:
        fail("Present must treat KMD private data as an uninitialized output until its success publication")
    private_pointer_writes = variable_write_offsets(present_body, "present->pDmaBufferPrivateData")
    private_size_writes = variable_write_offsets(present_body, "present->DmaBufferPrivateDataSize")
    publication = present_body.find("if (published)")
    if len(private_pointer_writes) != 1 or len(private_size_writes) != 1 or publication < 0 or \
       private_pointer_writes[0] < publication or private_size_writes[0] < publication:
        fail("Present must consume exactly one private-data record only after publishing its transaction")

    patch_body = function_body("VioGpuWddmPatch", WDDM_DDI_CODE)
    patch = canonical_code(patch_body)
    if "NotifyPatchSubmissionFault" in WDDM_DDI_CODE or "NotifyNativeSubmission" in patch or \
       "NotifyNativeSoftwareCompletion" in patch or "NotifyNativeSchedulerInterrupt" in patch:
        fail("Patch must not notify the scheduler before SubmitCommand accepts the submission fence")
    non_success_patch_returns = [
        match.group(0)
        for match in re.finditer(r"return\s+[^;]+;", patch_body)
        if canonical_code(match.group(0)) != "returnSTATUS_SUCCESS;"
    ]
    if non_success_patch_returns:
        fail("Patch must never return an error because Dxgkrnl converts every Patch failure into bugcheck 0x119/3")
    require_order(
        patch,
        (
            "candidatePrivate->Kind==VioGpuWddmDmaKindPresent",
            "ResolvePresentTransaction(",
            "VioGpuWddmPresentBuilt,",
            "candidatePrivateLength!=transaction->PrivateDataSize",
            "AcquirePresentAllocationLifecycles(source,destination,&sourceLocked,&destinationLocked)",
            "HasLiveNativePresentIdentity(source,transaction->Context,adapter)",
            "ReconcileGdiSourcePlacementAfterReset(source)",
            "HasLiveGdiPresentIdentity(source,transaction->Context,adapter)",
            "!destinationOpen->ReadOnly",
            "IsStandardPrimaryAllocation(destination)",
            "EnsureStandard2DAllocationBacking(destination)",
            "transaction->FenceId=patchArguments->SubmissionFenceId;",
            "KeMemoryBarrier();",
            "InterlockedCompareExchange(&transaction->State,VioGpuWddmPresentPatched,VioGpuWddmPresentBuilt)",
            "InterlockedCompareExchange(&transaction->CancelRequested,0,0)!=0",
        ),
        "Present Patch must revalidate placement before publishing fence and Patched state",
    )
    if patch.count("RetirePatchDmaOwner(adapter,patchArguments);") != 4:
        fail("Patch must retire every recoverable Built DMA owner on validation or operation-acquisition failure")
    retire_patch_owner = canonical_code(function_body("RetirePatchDmaOwner", WDDM_DDI_CODE))
    if "HANDLEruntimeContext=patchArguments->Flags.Paging?NULL:patchArguments->hContext;" not in retire_patch_owner:
        fail("Patch retirement must not reinterpret a paging hDevice as a render context")

    retire_owner = canonical_code(function_body("RetireDmaOwner", WDDM_DDI_CODE))
    for fragment in (
        "privateData->Kind==VioGpuWddmDmaKindPaging",
        "if(!resolved&&count==0)",
        "CancelRecognizedPagingTransaction(pagingPrivate,adapter);",
        "if(!cancelled)",
        "privateData->Kind==VioGpuWddmDmaKindPresent",
        "state==VioGpuWddmPresentBuilt||state==VioGpuWddmPresentPatched",
        "transaction->PrivateDataSize==privateEnd-privateStart",
        "dmaBuffer==NULL?ValidatePresentSubmitDmaRange(transaction,dmaBufferSize,dmaStart,dmaEnd):"
        "ValidatePresentDmaSubmissionRange(transaction,dmaBuffer,dmaBufferSize,dmaStart,dmaEnd)",
        "RetirePresentTransaction(transaction,state,VioGpuWddmPresentCancelled);",
        "if(!retired)",
        "privateData->Kind==VioGpuWddmDmaKindRender",
        "dmaBuffer==NULL?ValidateRenderSubmitDmaRange(submission,dmaBufferSize,dmaStart,dmaEnd):"
        "ValidateRenderDmaSubmissionRange(submission,dmaBuffer,dmaBufferSize,dmaStart,dmaEnd)",
        "submission->DmaPrivateDataSize==privateEnd-privateStart",
        "QuarantineSubmission(submission,state,TRUE);",
        "if(!quarantined)",
    ):
        if fragment not in retire_owner:
            fail(f"generic DMA retirement must cover each pre-submit owner: {fragment}")

    submit = canonical_code(function_body("VioGpuWddmSubmitCommand", WDDM_DDI_CODE))
    retire_submit_owner = canonical_code(function_body("RetireUnsubmittedDmaOwner", WDDM_DDI_CODE))
    if "RetireDmaOwner(adapter,NULL,submitCommand->DmaBufferSize" not in retire_submit_owner:
        fail("Submit retirement must use private owner state without a nonexistent CPU DMA base")
    if submit.count("RetireUnsubmittedDmaOwner(adapter,submitCommand);") != 3:
        fail("SubmitCommand must retire every recoverable owner on validation, operation, or Render publication failure")
    operation_gate = submit.find("if(!adapter->AcquireNativeSubmissionOperation())")
    operation_retire = submit.find("RetireUnsubmittedDmaOwner(adapter,submitCommand);", operation_gate)
    present_start = submit.find("elseif(NT_SUCCESS(status)&&privateData->Kind==VioGpuWddmDmaKindPresent)")
    present_end = submit.find("elseif(NT_SUCCESS(status)&&privateData->Kind!=VioGpuWddmDmaKindRender)", present_start)
    if min(operation_gate, operation_retire, present_start, present_end) < 0 or not operation_gate < operation_retire < present_start:
        fail("SubmitCommand must retire its DMA owner before entering kind-specific dispatch")
    present_submit = submit[present_start:present_end]
    require_order(
        present_submit,
        (
            "privateData->Kind==VioGpuWddmDmaKindPresent",
            "-1,&transaction);",
            "transaction->FullyPrepatched",
            "ValidatePresentSubmitDmaRange(transaction,",
            "transaction->FenceId=submitCommand->SubmissionFenceId;",
            "InterlockedCompareExchange(&transaction->State,VioGpuWddmPresentPatched,VioGpuWddmPresentBuilt)",
            "AcquirePresentWorkReference(transaction)",
            "InterlockedCompareExchange(&transaction->State,VioGpuWddmPresentQueued,VioGpuWddmPresentPatched)",
            "adapter->QueueNativePassiveWork(&transaction->Work,submitCommand->SubmissionFenceId)",
            "RetirePresentTransaction(transaction,state,VioGpuWddmPresentCancelled);",
            "adapter->NotifyNativeSubmissionFault(",
            "ReleasePresentWorkReference(transaction);",
        ),
        "Present Submit must promote only a fully prepatched Built owner before Queued publication",
    )
    expected_submit_stages = {
        "VioGpuWddmPresentSubmitNone": "0",
        "VioGpuWddmPresentSubmitResolveTransaction": "1",
        "VioGpuWddmPresentSubmitContract": "2",
        "VioGpuWddmPresentSubmitPrepatchTransition": "3",
        "VioGpuWddmPresentSubmitCancelled": "4",
        "VioGpuWddmPresentSubmitWorkReference": "5",
        "VioGpuWddmPresentSubmitQueueTransition": "6",
        "VioGpuWddmPresentSubmitPassiveQueue": "7",
        "VioGpuWddmPresentSubmitUnexpected": "0x0FFF",
    }
    for stage, value in expected_submit_stages.items():
        if len(re.findall(rf"\b{stage}\s*=\s*{value}\s*[,}}]", WDDM_DDI_HEADER_SOURCE)) != 1:
            fail(f"Present Submit diagnostic stage ABI drifted: {stage}")
    for fragment in (
        "submitFailureStage=VioGpuWddmPresentSubmitResolveTransaction;",
        "submitFailureStage=VioGpuWddmPresentSubmitContract;",
        "submitFailureStage=VioGpuWddmPresentSubmitPrepatchTransition;",
        "submitFailureStage=VioGpuWddmPresentSubmitCancelled;",
        "submitFailureStage=VioGpuWddmPresentSubmitWorkReference;",
        "submitFailureStage=VioGpuWddmPresentSubmitQueueTransition;",
        "submitFailureStage=VioGpuWddmPresentSubmitPassiveQueue;",
        "submitFailureStage=VioGpuWddmPresentSubmitUnexpected;",
        "static_cast<DWORD>(submitFailureStage),status,submitFailureDetail);",
    ):
        if present_submit.count(fragment) != 1:
            fail(f"Present Submit must publish one exact pre-worker failure stage: {fragment}")
    for bit in range(12):
        if len(re.findall(rf"1<<{bit}(?![0-9])", present_submit)) != 1:
            fail(f"Present Submit contract failure mask must retain bit {bit}")
    if "constUINTpresentSubmitFlags=0x6;" not in present_submit or \
       "BOOLEANpresentFlagsValid=submitCommand->Flags.Value==0||(submitCommand->Flags.Present!=0&&" not in present_submit or \
       "(submitCommand->Flags.Value&~presentSubmitFlags)==0)" not in present_submit or \
       "if(submitFailureDetail!=0){submitFailureDetail|=(submitCommand->Flags.Value&0xFFFF)<<16;" not in present_submit or \
       "submitFailureDetail|=!presentFlagsValid?1<<1:0" not in present_submit:
        fail("Present Submit must accept legacy zero, Present, or Present|RedirectedPresent flags only")

    worker = canonical_code(function_body("NativePresentWorker", WDDM_DDI_CODE))
    executing_claim = worker.find(
        "InterlockedCompareExchange(&transaction->State,VioGpuWddmPresentExecuting,VioGpuWddmPresentQueued)"
    )
    worker_main_start = worker.find("BOOLEANoperationAcquired=adapter->AcquireNativeSubmissionOperation();")
    if min(executing_claim, worker_main_start) < 0 or executing_claim >= worker_main_start:
        fail("Present worker must claim Queued-to-Executing before entering its Host operation")
    worker_main = worker[worker_main_start:]
    require_order(
        worker_main,
        (
            "operationAcquired=adapter->AcquireNativeSubmissionOperation();",
            "ExecutePresentTransaction(transaction,&failureStage,&failureDetail,&executionDiagnostic)",
            "RetirePresentTransaction(transaction,VioGpuWddmPresentExecuting,finalState);",
            "adapter->NotifyNativeSoftwareCompletion(",
            "adapter->CompleteNativePassiveWork(&transaction->Work);",
            "adapter->ReleaseNativeSubmissionOperation();",
            "ReleasePresentWorkReference(transaction);",
        ),
        "Present worker must own Executing through terminal retirement and scheduler notification",
    )
    for fragment in (
        "VIOGPU_WDDM_PRESENT_EXECUTION_STAGEfailureStage=VioGpuWddmPresentExecuteSubmissionOperation;",
        "InitializePresentExecutionDiagnostic(transaction,failureStage,status,failureDetail,&executionDiagnostic);",
        "executionDiagnostic.Stage=VioGpuWddmPresentExecuteTransactionRetire;",
        "executionDiagnostic.Status=static_cast<DWORD>(STATUS_DEVICE_NOT_READY);",
        "executionDiagnostic.TransactionState="
        "static_cast<DWORD>(InterlockedCompareExchange(&transaction->State,0,0));",
        "adapter->RequestHardwareResetAtAnyIrql();",
        "adapter->NotifyNativeSubmissionFault(",
        "adapter->ClaimNativePresentExecutionDiagnostic();",
        "adapter->RecordNativePresentExecutionDiagnostic(&executionDiagnostic);",
        "adapter->RecordNativePresentExecutionResetProvenance();",
    ):
        if fragment not in worker_main:
            fail(f"Present worker must persist its first execution failure before reset notification: {fragment}")
    if worker_main.count("adapter->RecordNativePresentExecutionDiagnostic(&executionDiagnostic);") != 2:
        fail("Present worker must record exactly the retire and execution-fault failure paths")
    if worker_main.count("adapter->ClaimNativePresentExecutionDiagnostic();") != 2:
        fail("Present worker must claim exactly the retire and execution-fault diagnostic paths")
    retire_stage = worker_main.find("executionDiagnostic.Stage=VioGpuWddmPresentExecuteTransactionRetire;")
    retire_claim = worker_main.find("adapter->ClaimNativePresentExecutionDiagnostic();", retire_stage)
    retire_record = worker_main.find(
        "adapter->RecordNativePresentExecutionDiagnostic(&executionDiagnostic);",
        retire_claim,
    )
    retire_reset = worker_main.find("adapter->RequestHardwareResetAtAnyIrql();", retire_record)
    retire_provenance = worker_main.find("adapter->RecordNativePresentExecutionResetProvenance();", retire_reset)
    fault_claim = worker_main.find("adapter->ClaimNativePresentExecutionDiagnostic();", retire_claim + 1)
    fault_record = worker_main.find("adapter->RecordNativePresentExecutionDiagnostic(&executionDiagnostic);", retire_record + 1)
    fault_notify = worker_main.find("adapter->NotifyNativeSubmissionFault(", fault_record)
    fault_provenance = worker_main.find("adapter->RecordNativePresentExecutionResetProvenance();", fault_notify)
    if min(retire_stage,
           retire_claim,
           retire_record,
           retire_reset,
           retire_provenance,
           fault_claim,
           fault_record,
           fault_notify,
           fault_provenance) < 0 or \
       not retire_stage < retire_claim < retire_record < retire_reset < retire_provenance < fault_claim < fault_record < \
       fault_notify < fault_provenance:
        fail("Present execution core diagnostics must commit before reset and optional reset provenance after return")

    worker_prefix = worker[:worker_main_start]
    for fragment in (
        "VioGpuWddmPresentExecuteInvalidTransaction",
        "VioGpuWddmPresentExecuteStateTransition",
        "InitializePresentExecutionDiagnostic(transaction,",
        "RecordNativePresentExecutionDiagnostic(&executionDiagnostic);",
        "RequestHardwareResetAtAnyIrql();",
        "RecordNativePresentExecutionResetProvenance();",
    ):
        if fragment not in worker_prefix:
            fail(f"Present worker early reset paths must retain pre-reset execution evidence: {fragment}")
    if worker.count("RecordNativePresentExecutionDiagnostic(&executionDiagnostic);") != 4 or \
       worker.count("ClaimNativePresentExecutionDiagnostic();") != 4 or \
       worker.count("RecordNativePresentExecutionResetProvenance();") != 4:
        fail("Present worker must diagnose exactly its two early and two terminal reset paths")

    expected_execution_stages = {
        "VioGpuWddmPresentExecuteNone": "0",
        "VioGpuWddmPresentExecuteInvalidTransaction": "1",
        "VioGpuWddmPresentExecuteSourceLifecycle": "2",
        "VioGpuWddmPresentExecuteDestinationLifecycle": "3",
        "VioGpuWddmPresentExecuteGdiSourceReconcile": "4",
        "VioGpuWddmPresentExecuteSourceIdentity": "5",
        "VioGpuWddmPresentExecuteSourceObject": "6",
        "VioGpuWddmPresentExecuteDestinationObject": "7",
        "VioGpuWddmPresentExecuteAliasedAllocations": "8",
        "VioGpuWddmPresentExecuteDestinationPrimary": "9",
        "VioGpuWddmPresentExecuteSourcePlacement": "10",
        "VioGpuWddmPresentExecuteDestinationBacking": "11",
        "VioGpuWddmPresentExecuteDestinationPlacement": "12",
        "VioGpuWddmPresentExecuteGeometry": "13",
        "VioGpuWddmPresentExecuteSourcePlacementOffset": "14",
        "VioGpuWddmPresentExecuteDestinationPlacementOffset": "15",
        "VioGpuWddmPresentExecuteDestinationResetGeneration": "16",
        "VioGpuWddmPresentExecuteCopyAddress": "17",
        "VioGpuWddmPresentExecuteCancelled": "18",
        "VioGpuWddmPresentExecuteHostPresent": "19",
        "VioGpuWddmPresentExecuteSubmissionOperation": "20",
        "VioGpuWddmPresentExecuteTransactionRetire": "21",
        "VioGpuWddmPresentExecuteStateTransition": "22",
        "VioGpuWddmPresentExecuteComplete": "0x0FFF",
    }
    execution_stage_match = re.search(
        r"\benum\s+VIOGPU_WDDM_PRESENT_EXECUTION_STAGE\s*:\s*DWORD\s*\{(?P<body>.*?)\}\s*;",
        WDDM_DDI_HEADER_SOURCE,
        re.DOTALL,
    )
    if execution_stage_match is None:
        fail("Present execution stage ABI is missing")
    execution_stage_body = canonical_code(execution_stage_match.group("body"))
    for stage_name, stage_value in expected_execution_stages.items():
        if execution_stage_body.count(f"{stage_name}={stage_value}") != 1:
            fail(f"Present execution stage ABI drifted at {stage_name}")

    execute_body = function_body("ExecutePresentTransaction", WDDM_DDI_CODE)
    execute = canonical_code(execute_body)
    for fragment in (
        "source==destination",
        "AcquirePresentAllocationLifecycles(source,destination,&sourceLocked,&destinationLocked)",
        "ReconcileGdiSourcePlacementAfterReset(source)",
        "HasLiveGdiPresentIdentity(source,transaction->Context,transaction->Adapter)",
        "source->ApertureAddress==NULL||destination->ApertureAddress==NULL",
        "RtlCopyMemory(destinationBase+destinationOffset,sourceBase+sourceOffset,rowBytes);",
        "ProbePresentCopy(transaction,&copyProbe);",
        "transaction->Adapter->Present2DResource(destination->ResourceId,0,destination->Width,"
        "destination->Height,0,0,",
        "result!=VioGpuHostContextConfirmed",
        "InterlockedCompareExchange(&transaction->CancelRequested,0,0)!=0",
        "BuildPresentExecutionDiagnostic(transaction,*failureStage,status,*failureDetail,executionDiagnostic);",
        "if(sourceLocked){KeReleaseMutex(&source->LifecycleMutex,FALSE);}",
    ):
        if fragment not in execute:
            fail(f"Present execution must keep the copy and Host flush in one validated ownership epoch: {fragment}")
    for stage_name in tuple(expected_execution_stages)[1:]:
        expected_count = 3 if stage_name == "VioGpuWddmPresentExecuteCancelled" else 1
        observed_count = len(re.findall(rf"\b{re.escape(stage_name)}\b", execute_body))
        if observed_count != expected_count and stage_name not in (
            "VioGpuWddmPresentExecuteSubmissionOperation",
            "VioGpuWddmPresentExecuteTransactionRetire",
            "VioGpuWddmPresentExecuteStateTransition",
        ):
            fail(f"Present execution must classify its first failure at {stage_name}")
    require_order(
        execute,
        (
            "RtlCopyMemory(destinationBase+destinationOffset,sourceBase+sourceOffset,rowBytes);",
            "KeMemoryBarrier();",
            "KeFlushIoBuffers(destination->ApertureMdl,FALSE,TRUE);",
            "ProbePresentCopy(transaction,&copyProbe);",
            "transaction->Adapter->Present2DResource(",
            "transaction->Adapter->RecordNativePresentCopyProbe(&copyProbe);",
            "BuildPresentExecutionDiagnostic(transaction,*failureStage,status,*failureDetail,executionDiagnostic);",
            "KeReleaseMutex(&destination->LifecycleMutex,FALSE);",
        ),
        "Present must publish CPU row writes, notify Host, and snapshot execution while allocation state remains locked",
    )
    if execute.count("KeMemoryBarrier();") != 1:
        fail("Present must retain exactly one CPU-to-Host ordering barrier after its row-copy batch")
    if execute.count("KeFlushIoBuffers(destination->ApertureMdl,FALSE,TRUE);") != 1:
        fail("Present must flush the CPU-written primary backing before Host transfer")
    if "transferOffset" in execute_body or execute.count("Present2DResource(") != 1:
        fail("Present must publish exactly one full-primary transfer for classic virglrenderer")

    probe_copy = canonical_code(function_body("ProbePresentCopy", WDDM_DDI_CODE))
    for fragment in (
        "probe->SourceHash=2166136261;",
        "probe->DestinationHash=2166136261;",
        "probe->SampleCount<256",
        "RtlCopyMemory(&sourcePixel,sourceBase+sourceOffset,sizeof(sourcePixel));",
        "RtlCopyMemory(&destinationPixel,destinationBase+destinationOffset,sizeof(destinationPixel));",
        "probe->SourceRgbNonzero+=(sourcePixel&0x00FFFFFF)!=0?1:0;",
        "probe->DestinationRgbNonzero+=(destinationPixel&0x00FFFFFF)!=0?1:0;",
    ):
        if fragment not in probe_copy:
            fail(f"Present copy probe must compare bounded source and destination samples: {fragment}")

    cancel = canonical_code(function_body("VioGpuWddmCancelCommand", WDDM_DDI_CODE))
    dma_range_parameters = (
        "_In_ const {owner_type} *{owner}, _In_ PVOID dmaBuffer, _In_ UINT dmaBufferSize, "
        "_In_ UINT submissionStart, _In_ UINT submissionEnd"
    )
    exact_present_range = canonical_code(
        function_body_with_parameters(
            "ValidatePresentDmaSubmissionRange",
            dma_range_parameters.format(
                owner_type="VIOGPU_WDDM_PRESENT_TRANSACTION",
                owner="transaction",
            ),
            WDDM_DDI_CODE,
        )
    )
    exact_render_range = canonical_code(
        function_body_with_parameters(
            "ValidateRenderDmaSubmissionRange",
            dma_range_parameters.format(
                owner_type="VIOGPU_WDDM_SUBMISSION",
                owner="submission",
            ),
            WDDM_DDI_CODE,
        )
    )
    submit_range_parameters = (
        "_In_ const {owner_type} *{owner}, _In_ UINT dmaBufferSize, "
        "_In_ UINT submissionStart, _In_ UINT submissionEnd"
    )
    submit_present_range = canonical_code(
        function_body_with_parameters(
            "ValidatePresentSubmitDmaRange",
            submit_range_parameters.format(
                owner_type="VIOGPU_WDDM_PRESENT_TRANSACTION",
                owner="transaction",
            ),
            WDDM_DDI_CODE,
        )
    )
    submit_render_range = canonical_code(
        function_body_with_parameters(
            "ValidateRenderSubmitDmaRange",
            submit_range_parameters.format(
                owner_type="VIOGPU_WDDM_SUBMISSION",
                owner="submission",
            ),
            WDDM_DDI_CODE,
        )
    )
    for fragment in (
        "submissionEnd-submissionStart==sizeof(VIOGPU_WDDM_PRESENT_DMA_PACKET)",
        "transaction->DmaBuffer==static_cast<BYTE*>(dmaBuffer)+submissionStart",
        "transaction->DmaBufferSize==dmaBufferSize-submissionStart",
    ):
        if fragment not in exact_present_range:
            fail(f"Present Cancel DMA range validation must remain exact: {fragment}")
    for fragment in (
        "submissionEnd-submissionStart==submission->CommandLength",
        "submission->DmaBuffer==static_cast<BYTE*>(dmaBuffer)+submissionStart",
        "submission->DmaBufferSize==dmaBufferSize-submissionStart",
    ):
        if fragment not in exact_render_range:
            fail(f"Render Cancel DMA range validation must remain exact: {fragment}")
    for fragment in (
        "transaction->DmaBuffer!=NULL",
        "submissionEnd-submissionStart==sizeof(VIOGPU_WDDM_PRESENT_DMA_PACKET)",
        "transaction->DmaBufferSize==dmaBufferSize-submissionStart",
    ):
        if fragment not in submit_present_range:
            fail(f"Present Submit DMA range validation must not require a missing CPU base: {fragment}")
    for fragment in (
        "submission->DmaBuffer!=NULL",
        "submissionEnd-submissionStart==submission->CommandLength",
        "submission->DmaBufferSize==dmaBufferSize-submissionStart",
    ):
        if fragment not in submit_render_range:
            fail(f"Render Submit DMA range validation must not require a missing CPU base: {fragment}")
    for fragment in (
        "privateData->Kind==VioGpuWddmDmaKindRender",
        "submission->DmaPrivateDataSize==privateLength",
        "ValidateRenderDmaSubmissionRange(submission,",
        "state==VioGpuWddmSubmissionPrepared||state==VioGpuWddmSubmissionPatched",
        "QuarantineSubmission(submission,state,TRUE)",
        "privateData->Kind==VioGpuWddmDmaKindPresent",
        "transaction->PrivateDataSize==privateLength",
        "ValidatePresentDmaSubmissionRange(transaction,",
        "state==VioGpuWddmPresentBuilt||state==VioGpuWddmPresentPatched",
        "state==VioGpuWddmPresentQueued||state==VioGpuWddmPresentExecuting",
        "InterlockedExchange(&transaction->CancelRequested,1);",
        "adapter->CancelNativePassiveWork(&transaction->Work)",
        "ownership==VioGpuNativePassiveWorkRemoved",
        "adapter->NotifyNativeSubmissionFault(",
    ):
        if fragment not in cancel:
            fail(f"CancelCommand must retain Present terminal ownership semantics: {fragment}")

    stop = canonical_code(function_body("VioGpuDod::StopDevice", VIOGPU_CODE))
    require_order(
        stop,
        (
            "InterlockedExchange(&m_HardwareResetState,VioGpuHardwareResetRequested);",
            "RequestWddmSubmissionDrainAtAnyIrql();",
            "WaitForWddmSubmissionDrain()",
            "ExWaitForRundownProtectionRelease(&m_HardwareOperations);",
            "m_pHWDevice->HWClose();",
        ),
        "StopDevice must drain Present before transport teardown",
    )
    power = canonical_code(function_body("VioGpuDod::SetPowerState", VIOGPU_CODE))
    reset_recovery = power.find("if(resetState==VioGpuHardwareResetRequested)")
    d0_reset_drain = power.find("RequestWddmSubmissionDrainAtAnyIrql();", reset_recovery)
    d0_reset_wait = power.find("WaitForWddmSubmissionDrain()", d0_reset_drain)
    d0_reset_adapter = power.find("m_pHWDevice->ResetDevice();", d0_reset_wait)
    d_state_condition = power.find(
        "if(DevicePowerState==PowerDeviceD1||DevicePowerState==PowerDeviceD2||"
        "DevicePowerState==PowerDeviceD3)",
        d0_reset_adapter,
    )
    d_state_drain = power.find("RequestHardwareResetAtAnyIrql();", d_state_condition)
    d_state_wait = power.find("WaitForWddmSubmissionDrain()", d_state_drain)
    transport_transition = power.find(
        "Status=m_pHWDevice->SetPowerState(&m_DeviceInfo,DevicePowerState,&m_CurrentMode);"
    )
    d_state_failure = power.find(
        "if(!NT_SUCCESS(Status)&&(DevicePowerState==PowerDeviceD1||DevicePowerState==PowerDeviceD2||"
        "DevicePowerState==PowerDeviceD3))",
        transport_transition,
    )
    d_state_failure_reset = power.find("RequestHardwareResetAtAnyIrql();", d_state_failure)
    d0_passive_idle = power.find("WaitForNativePassiveQueueIdle()", d_state_failure_reset)
    d0_active = power.find("VioGpuHardwareActive,VioGpuHardwareRecovering", d_state_failure_reset)
    d0_recheck = power.find("VioGpuHardwareActive,VioGpuHardwareActive", d0_active)
    d0_reset_fence = power.find("CompleteNativeFenceReset();", d0_recheck)
    d0_open = power.find("OpenWddmPresentTransactions()", d0_reset_fence)
    d0_open_failure_reset = power.find("RequestHardwareResetAtAnyIrql();", d0_open)
    publish_power = power.find("m_AdapterPowerState=DevicePowerState;", d0_open)
    power_stages = (
        reset_recovery,
        d0_reset_drain,
        d0_reset_wait,
        d0_reset_adapter,
        d_state_condition,
        d_state_drain,
        d_state_wait,
        transport_transition,
        d_state_failure,
        d_state_failure_reset,
        d0_passive_idle,
        d0_active,
        d0_recheck,
        d0_reset_fence,
        d0_open,
        d0_open_failure_reset,
        publish_power,
    )
    if min(power_stages) < 0 or tuple(sorted(power_stages)) != power_stages:
        fail("D0 recovery must drain before reset; failed D-state teardown must close the reset gate; reopen must fail closed")
    idle_wait_blocks = [
        canonical_code(body)
        for condition, body, _, _ in if_blocks(function_body("VioGpuDod::SetPowerState", VIOGPU_CODE))
        if canonical_code(condition) == "!WaitForNativePassiveQueueIdle()"
    ]
    if idle_wait_blocks != [
        "InterlockedCompareExchange(&m_HardwareResetState,VioGpuHardwareResetRequested,"
        "VioGpuHardwareRecovering);returnSTATUS_DEVICE_NOT_READY;"
    ]:
        fail("D0 recovery must keep the reset gate closed when the passive queue cannot prove idle")
    open_failure_blocks = [
        canonical_code(body)
        for condition, body, _, _ in if_blocks(function_body("VioGpuDod::SetPowerState", VIOGPU_CODE))
        if canonical_code(condition) == "!OpenNativePassiveQueue()||!OpenWddmPresentTransactions()"
    ]
    if open_failure_blocks != [
        "RequestWddmSubmissionDrainAtAnyIrql();RequestHardwareResetAtAnyIrql();returnSTATUS_DEVICE_NOT_READY;"
    ]:
        fail("D0 must restore the outer reset gate when Present publication cannot reopen")
    reset_timeout = canonical_code(function_body("VioGpuDod::ResetFromTimeout", VIOGPU_CODE))
    require_order(
        reset_timeout,
        (
            "InterlockedExchange(&m_HardwareResetState,VioGpuHardwareResetRequested);",
            "RequestWddmSubmissionDrainAtAnyIrql();",
            "WaitForWddmSubmissionDrain()",
            "ExAcquireRundownProtection(&m_HardwareOperations)",
            "adapter->SetPowerState(&m_DeviceInfo,PowerDeviceD3,&m_CurrentMode);",
        ),
        "TDR must drain Present before stopping the transport",
    )
    system_display = canonical_code(function_body("VioGpuDod::SystemDisplayEnable", VIOGPU_CODE))
    require_order(
        system_display,
        (
            "InterlockedExchange(&m_HardwareResetState,VioGpuHardwareResetRequested);",
            "RequestWddmSubmissionDrainAtAnyIrql();",
            "WaitForWddmSubmissionDrain()",
            "ExAcquireRundownProtection(&m_HardwareOperations)",
            "adapter->ResetToVgaMode();",
        ),
        "system-display takeover must drain Present before VGA reset",
    )


def check_native_context_ownership() -> None:
    queue_header = canonical_code(strip_cpp_comments_and_literals(QUEUE_HEADER_SOURCE))
    wire_header = canonical_code(WIRE_HEADER_CODE)
    expected_results = (
        "VioGpuHostContextNotSubmitted=0,"
        "VioGpuHostContextConfirmed,"
        "VioGpuHostContextRejected,"
        "VioGpuHostContextUnknown,"
    )
    if queue_header.count(expected_results) != 1:
        fail("Host context queue results must keep the four distinct ownership outcomes")
    if queue_header.count("VIOGPU_NATIVE_MAP_RESPONSE_DIAGNOSTIC") != 4:
        fail("Host map response diagnostics must have one type declaration, storage, and getter surface")
    if queue_header.count("GetLastNativeMapResponseDiagnostic") != 1:
        fail("Host map response diagnostics must expose one const queue getter")

    create_queue = canonical_code(function_body("CtrlQueue::CreateNativeContext", QUEUE_CODE))
    destroy_queue = canonical_code(function_body("CtrlQueue::DestroyNativeContext", QUEUE_CODE))
    create_required = (
        "BOOLEANsubmitted=FALSE;",
        "SubmitSynchronousLocked(vbuf,&releaseBuffer,&submitted)",
        "VioGpuValidatePlainControlResponse(",
        "VIOGPU_HOST_CONTEXT_RESULTresult=VioGpuHostContextUnknown;",
        "if(output->Validation==VioGpuHostResponseNotSubmitted){result=VioGpuHostContextNotSubmitted;}",
        "elseif(output->Validation==VioGpuHostResponseConfirmed){result=VioGpuHostContextConfirmed;}",
        "elseif(output->Validation==VioGpuHostResponseRejected){result=VioGpuHostContextRejected;}",
        "else{PoisonSynchronousRequests();}",
        "returnresult;",
    )
    if any(create_queue.count(fragment) != 1 for fragment in create_required):
        fail("Host context create must map each focused response classification to one ownership outcome")
    if create_queue.find("VioGpuHostContextUnknown") > create_queue.find("VioGpuValidatePlainControlResponse("):
        fail("Host context create must default to Unknown before interpreting focused validation")

    destroy_required = (
        "BOOLEANsubmitted=FALSE;",
        "SubmitSynchronousLocked(vbuf,&releaseBuffer,&submitted)",
        "VIOGPU_HOST_CONTEXT_RESULTresult=VioGpuHostContextUnknown;",
        "if(!submitted){result=VioGpuHostContextNotSubmitted;}",
        "result=VioGpuHostContextConfirmed;",
        "returnresult;",
    )
    if any(destroy_queue.count(fragment) != 1 for fragment in destroy_required):
        fail("Host context destroy must classify NotSubmitted, Confirmed, and Unknown separately")
    if destroy_queue.find("VioGpuHostContextUnknown") > destroy_queue.find("if(!submitted)"):
        fail("Host context destroy must default to Unknown before interpreting completion")
    if destroy_queue.find("if(!submitted)") > destroy_queue.find(
        "elseif(completed&&vbuf->response_size==sizeof(GPU_CTRL_HDR))"
    ):
        fail("Host context destroy must classify non-submission before any response")

    destroy_rejected = (
        "elseif(IsPlainControlErrorResponse(response)&&"
        "response->type==VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID)"
        "{result=VioGpuHostContextRejected;}"
    )
    if destroy_queue.count(destroy_rejected) != 1:
        fail("Host destroy may classify Rejected only from exact INVALID_CONTEXT_ID acknowledgement")
    if destroy_queue.count("VioGpuHostContextRejected") != 1:
        fail("Host destroy must leave every other submitted non-success response Unknown")

    wire_requirements = (
        "#defineVIRTIO_GPU_MAP_INFO_RESPONSE_WIRE_SIZE32",
        "VioGpuValidateMapInfoResponse(",
        "map_info!=VIRTIO_GPU_MAP_CACHE_CACHED||map_padding!=0",
        "#defineMSM_PIPE_3D00x10",
        "#defineMSM_PARAM_VA_START0x0eU",
        "#defineMSM_PARAM_VA_SIZE0x0fU",
        "#defineDRM_IOCTL_MSM_GET_PARAM0xc0186440",
        "#defineMSM_SUBMITQUEUE_ALLOW_PREEMPT0x00000001",
        "#defineDRM_IOCTL_MSM_SUBMITQUEUE_NEW0xc00c644aU",
        "#defineDRM_IOCTL_MSM_SUBMITQUEUE_CLOSE0x4004644bU",
        "VIOGPU_WIRE_ASSERT_SIZE(drm_msm_param,24);",
        "VIOGPU_WIRE_ASSERT_SIZE(msm_ccmd_ioctl_simple_get_param_req,44);",
        "VIOGPU_WIRE_ASSERT_SIZE(msm_ccmd_ioctl_simple_get_param_rsp,32);",
        "VIOGPU_WIRE_ASSERT_OFFSET(msm_ccmd_ioctl_simple_get_param_rsp,ret,4);",
        "VIOGPU_WIRE_ASSERT_OFFSET(msm_ccmd_ioctl_simple_get_param_rsp,param,8);",
        "VIOGPU_WIRE_ASSERT_SIZE(drm_msm_submitqueue,12);",
        "VIOGPU_WIRE_ASSERT_OFFSET(drm_msm_submitqueue,flags,0);",
        "VIOGPU_WIRE_ASSERT_OFFSET(drm_msm_submitqueue,prio,4);",
        "VIOGPU_WIRE_ASSERT_OFFSET(drm_msm_submitqueue,id,8);",
        "VIOGPU_WIRE_ASSERT_SIZE(msm_ccmd_ioctl_simple_submitqueue_new_req,32);",
        "VIOGPU_WIRE_ASSERT_OFFSET(msm_ccmd_ioctl_simple_submitqueue_new_req,ioctl_cmd,16);",
        "VIOGPU_WIRE_ASSERT_OFFSET(msm_ccmd_ioctl_simple_submitqueue_new_req,submitqueue,20);",
        "VIOGPU_WIRE_ASSERT_SIZE(msm_ccmd_ioctl_simple_submitqueue_new_rsp,20);",
        "VIOGPU_WIRE_ASSERT_OFFSET(msm_ccmd_ioctl_simple_submitqueue_new_rsp,ret,4);",
        "VIOGPU_WIRE_ASSERT_OFFSET(msm_ccmd_ioctl_simple_submitqueue_new_rsp,submitqueue,8);",
        "VIOGPU_WIRE_ASSERT_SIZE(msm_ccmd_ioctl_simple_submitqueue_close_req,24);",
        "VIOGPU_WIRE_ASSERT_OFFSET(msm_ccmd_ioctl_simple_submitqueue_close_req,ioctl_cmd,16);",
        "VIOGPU_WIRE_ASSERT_OFFSET(msm_ccmd_ioctl_simple_submitqueue_close_req,queue_id,20);",
        "VIOGPU_WIRE_ASSERT_SIZE(msm_ccmd_ioctl_simple_submitqueue_close_rsp,8);",
        "VIOGPU_WIRE_ASSERT_OFFSET(msm_ccmd_ioctl_simple_submitqueue_close_rsp,ret,4);",
    )
    for fragment in wire_requirements:
        if wire_header.count(fragment) != 1:
            fail(f"native GET_PARAM wire contract is missing its exact ABI assertion: {fragment}")

    create_blob = canonical_code(function_body("CtrlQueue::CreateNativeControlBlob", QUEUE_CODE))
    for fragment in (
        "command->hdr.type=VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB;",
        "command->hdr.ctx_id=context_id;",
        "command->resource_id=resource_id;",
        "command->blob_mem=VIRTIO_GPU_BLOB_MEM_HOST3D;",
        "command->blob_flags=VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE;",
        "command->blob_id=0;",
        "command->size=VIOGPU_NATIVE_CONTROL_BLOB_SIZE;",
    ):
        if create_blob.count(fragment) != 1:
            fail(f"control blob create must use the exact blob-0 HOST3D contract: {fragment}")

    map_blob = canonical_code(function_body("CtrlQueue::MapNativeControlBlob", QUEUE_CODE))
    for fragment in (
        "command->offset=offset;",
        "VIOGPU_NATIVE_MAP_RESPONSE_DIAGNOSTICcaptured={};",
        "captured.ResponseSize=vbuf->response_size;",
        "captured.MapInfo=response->map_info;",
        "captured.MapPadding=response->padding;",
        "captured.Validation=VioGpuValidateMapInfoResponse(",
        "if(captured.Validation==VioGpuHostResponseNotSubmitted){result=VioGpuHostContextNotSubmitted;}",
        "elseif(captured.Validation==VioGpuHostResponseConfirmed){result=VioGpuHostContextConfirmed;}",
        "elseif(captured.Validation==VioGpuHostResponseRejected){result=VioGpuHostContextRejected;}",
        "elseif(completed){PoisonSynchronousRequests();}",
    ):
        if map_blob.count(fragment) != 1:
            fail(f"control blob map must retain and classify its complete host response: {fragment}")
    if map_blob.count("RtlCopyMemory(&m_LastNativeMapResponseDiagnostic,&captured,sizeof(m_LastNativeMapResponseDiagnostic));") != 2:
        fail("control blob map must initialize and publish one complete diagnostic snapshot")
    if map_blob.find("captured.Validation=VioGpuValidateMapInfoResponse(") < map_blob.find(
        "SubmitSynchronousLocked(vbuf,&releaseBuffer,&submitted)"
    ):
        fail("control blob map must validate the response only after synchronous completion")

    resource_header = canonical_code(RESOURCE_HEADER_CODE)
    for fragment in ("ULONGmap_info;ULONGpadding;}GPU_RESP_MAP_INFO",):
        if resource_header.count(fragment) != 1:
            fail(f"control blob map response must retain its reserved response word: {fragment}")

    native_header = canonical_code(VIOGPU_HEADER_CODE)
    for field in (
        "UINTControlResourceId;",
        "ULONGLONGControlBarOffset;",
        "PVOIDControlAddress;",
        "ULONGControlBlobSize;",
        "ULONGLastControlSeqno;",
        "BOOLEANControlResourceCreated;",
        "BOOLEANControlMapped;",
        "BOOLEANSubmitQueueCreated;",
    ):
        if native_header.count(field) != 1:
            fail(f"native owner must retain one control-resource field: {field}")
    if native_header.count("VIOGPU_NATIVE_MAP_RESPONSE_DIAGNOSTIC") != 1:
        fail("native map diagnostics must be declared once on the DOD recorder surface")
    if native_header.count("UINTm_NextNativeResourceId;") != 1:
        fail("native control resources must use one adapter-owned high-range allocator")

    if resource_header.count("#defineVIOGPU_NATIVE_RESOURCE_ID_START0x80000000") != 1:
        fail("VirtIO resource IDs must define one exact low/high namespace boundary")
    idr_header = canonical_code(IDR_HEADER_CODE)
    for fragment in ("BOOLEANInit(_In_ULONGstart,_In_ULONGend);", "ULONGm_endId;"):
        if idr_header.count(fragment) != 1:
            fail(f"legacy resource ID allocator must retain its exclusive upper bound: {fragment}")
    idr_init = canonical_code(function_body("VioGpuIdr::Init", IDR_CODE))
    for fragment in (
        "if(start==0||start>=end){returnFALSE;}",
        "m_nextId=start;",
        "m_endId=end;",
    ):
        if idr_init.count(fragment) != 1:
            fail(f"legacy resource ID initialization must validate and publish its bounded range: {fragment}")
    idr_get = canonical_code(function_body("VioGpuIdr::GetId", IDR_CODE))
    idr_get_sequence = (
        idr_get.find("KeAcquireSpinLock(&m_lock,&oldIrql);"),
        idr_get.find("elseif(m_nextId!=0&&m_nextId<m_endId)"),
        idr_get.find("id=m_nextId++;"),
        idr_get.find("KeReleaseSpinLock(&m_lock,oldIrql);"),
    )
    if min(idr_get_sequence) < 0 or list(idr_get_sequence) != sorted(idr_get_sequence):
        fail("legacy resource ID allocation must increment under lock and stop before the native high range")
    if "ExInterlocked" in IDR_CODE:
        fail("legacy resource ID list and sequential counter must use one shared locking domain")
    native_resource_allocator = canonical_code(
        function_body("VioGpuAdapter::AllocateNativeResourceIdLocked", VIOGPU_CODE)
    )
    for fragment in (
        "UINTresourceId=m_NextNativeResourceId;",
        "if(resourceId<VIOGPU_NATIVE_RESOURCE_ID_START||resourceId==MAXUINT){return0;}",
        "++m_NextNativeResourceId;",
    ):
        if native_resource_allocator.count(fragment) != 1:
            fail(f"native resource ID allocation must remain inside the reserved high range: {fragment}")
    if canonical_code(VIOGPU_CODE).count("m_NextNativeResourceId=VIOGPU_NATIVE_RESOURCE_ID_START;") != 1:
        fail("native resource ID allocation must begin exactly at the reserved high-range boundary")
    native_context_allocator = canonical_code(
        function_body("VioGpuAdapter::AllocateNativeContextIdLocked", VIOGPU_CODE)
    )
    if native_context_allocator.count("if(contextId==0||contextId==MAXUINT){return0;}") != 1:
        fail("native context ID allocation must reject the exhausted MAXUINT sentinel before incrementing")

    seed = canonical_code(
        function_body_with_parameters(
            "VioGpuSeedNativeControlResponse",
            "_In_ VioGpuAdapter *adapter, _Inout_ VIOGPU_NATIVE_CONTEXT_OWNER *owner, _In_ ULONG sequence, "
            "_In_ ULONG responseSize, _Inout_opt_ PVIOGPU_NATIVE_CONTEXT_PARAMETER_DIAGNOSTIC diagnostic = NULL",
            VIOGPU_CODE,
        )
    )
    copy = canonical_code(
        function_body_with_parameters(
            "VioGpuCopyNativeControlResponse",
            "_In_ VioGpuAdapter *adapter, _In_ const VIOGPU_NATIVE_CONTEXT_OWNER *owner, _In_ ULONG sequence, "
            "_Out_ PVOID response, _In_ ULONG responseSize, "
            "_Inout_opt_ PVIOGPU_NATIVE_CONTEXT_PARAMETER_DIAGNOSTIC diagnostic = NULL",
            VIOGPU_CODE,
        )
    )
    consume = canonical_code(
        function_body_with_parameters(
            "VioGpuConsumeNativeControlResponse",
            "_In_ VioGpuAdapter *adapter, _In_ const VIOGPU_NATIVE_CONTEXT_OWNER *owner, _In_ ULONG sequence, "
            "_In_ ULONG parameter, _Out_ PULONGLONG value, "
            "_Inout_opt_ PVIOGPU_NATIVE_CONTEXT_PARAMETER_DIAGNOSTIC diagnostic = NULL",
            VIOGPU_CODE,
        )
    )
    faults_clear = canonical_code(
        function_body_with_parameters(
            "VioGpuNativeControlFaultsClear",
            "_In_ VioGpuAdapter *adapter, _In_ const VIOGPU_NATIVE_CONTEXT_OWNER *owner",
            VIOGPU_CODE,
        )
    )
    for owner, body in (("seed", seed), ("copy", copy), ("fault snapshot", faults_clear)):
        if body.count("VioGpuResolveNativeControlWindow(owner,") != 1:
            fail(f"native response {owner} must resolve exactly one mapped host-visible BAR window")
        if "KeWaitForSingleObject" in body or "SubmitNativeControl" in body or "PAGED_CODE" in body:
            fail(f"native response {owner} must remain non-waiting and nonpageable")
    for bulk_clear in ("RtlZeroMemory(", "RtlFillMemory(", "memset("):
        if bulk_clear in seed:
            fail("native response seed must not use bulk memory operations on the mapped BAR window")
    for fragment in (
        "(responseSize&(sizeof(ULONG)-1))!=0",
        "(reinterpret_cast<ULONG_PTR>(response)&(sizeof(ULONG)-1))!=0",
        "volatileULONG*responseWords=reinterpret_cast<volatileULONG*>(response);",
        "ULONGresponseWordCount=responseSize/sizeof(ULONG);",
        "for(ULONGwordIndex=0;wordIndex<responseWordCount;++wordIndex){VioGpuWriteSharedU32(&responseWords[wordIndex],0);}",
    ):
        if seed.count(fragment) != 1:
            fail(f"native response seed must use aligned volatile 32-bit BAR writes: {fragment}")
    seed_order = (
        seed.find("for(ULONGwordIndex=0;wordIndex<responseWordCount;++wordIndex)"),
        seed.find("VioGpuWriteSharedU32(reinterpret_cast<volatileULONG*>(&responseHeader->ret),MAXLONG);"),
        seed.find("VioGpuWriteSharedU32(reinterpret_cast<volatileULONG*>(&responseHeader->hdr.len),responseSize);"),
    )
    if min(seed_order) < 0 or list(seed_order) != sorted(seed_order):
        fail("native response seed must install an invalid return sentinel before publishing the exact length")
    if re.search(r"(?:RtlCopyMemory|RtlMoveMemory|memcpy|memmove)\([^;]*,sharedResponse[^;]*\)", copy):
        fail("native response copy must not use a bulk read from the mapped BAR window")
    for fragment in (
        "(responseSize&(sizeof(ULONG)-1))!=0",
        "(reinterpret_cast<ULONG_PTR>(response)&(sizeof(ULONG)-1))!=0",
        "(reinterpret_cast<ULONG_PTR>(sharedResponse)&(sizeof(ULONG)-1))!=0",
        "volatileconstULONG*sharedResponseWords=reinterpret_cast<volatileconstULONG*>(sharedResponse);",
        "PULONGresponseWords=static_cast<PULONG>(response);",
        "ULONGresponseWordCount=responseSize/sizeof(ULONG);",
        "for(ULONGwordIndex=0;wordIndex<responseWordCount;++wordIndex){responseWords[wordIndex]=VioGpuReadSharedU32(&sharedResponseWords[wordIndex]);}",
    ):
        if copy.count(fragment) != 1:
            fail(f"native response copy must use aligned scalar 32-bit BAR reads: {fragment}")
    copy_sequence = (
        copy.find("(responseSize&(sizeof(ULONG)-1))!=0"),
        copy.find("(reinterpret_cast<ULONG_PTR>(response)&(sizeof(ULONG)-1))!=0"),
        copy.find("RtlZeroMemory(response,responseSize);"),
        copy.find("(reinterpret_cast<ULONG_PTR>(sharedResponse)&(sizeof(ULONG)-1))!=0"),
        copy.find("ULONGsharedSeqno=VioGpuReadSharedU32(&shmem->base.seqno);"),
        copy.find("if(responseSize>responseCapacity)"),
        copy.find("if(sharedSeqno!=sequence)"),
        copy.find("KeMemoryBarrier();"),
        copy.find("volatileconstULONG*sharedResponseWords=reinterpret_cast<volatileconstULONG*>(sharedResponse);"),
        copy.find("PULONGresponseWords=static_cast<PULONG>(response);"),
        copy.find("ULONGresponseWordCount=responseSize/sizeof(ULONG);"),
        copy.find("for(ULONGwordIndex=0;wordIndex<responseWordCount;++wordIndex){responseWords[wordIndex]=VioGpuReadSharedU32(&sharedResponseWords[wordIndex]);}"),
        copy.find("if(responseHeader->len!=responseSize||sharedAsyncError!=0||sharedGlobalFaults!=0)"),
    )
    if min(copy_sequence) < 0 or list(copy_sequence) != sorted(copy_sequence):
        fail("native response copy must acquire seqno before copying and validating the response")
    for fragment in (
        "VioGpuReadSharedU32(&shmem->async_error)==0",
        "VioGpuReadSharedU32(&shmem->global_faults)==0",
    ):
        if faults_clear.count(fragment) != 1:
            fail(f"native fault snapshot must validate its mapped control state: {fragment}")

    resolve = canonical_code(function_body("VioGpuResolveNativeControlWindow", VIOGPU_CODE))
    for fragment in (
        "owner->ControlAddress==NULL",
        "(owner->ControlBarOffset&(PAGE_SIZE-1))!=0",
        "PUCHARblob=static_cast<PUCHAR>(owner->ControlAddress);",
    ):
        if resolve.count(fragment) != 1:
            fail(f"control responses must stay inside the assigned host-visible BAR slot: {fragment}")

    shared_read = canonical_code(
        function_body_with_parameters(
            "VioGpuReadSharedU32", "_In_ const volatile ULONG *value", VIOGPU_CODE
        )
    )
    shared_write = canonical_code(
        function_body_with_parameters(
            "VioGpuWriteSharedU32", "_Out_ volatile ULONG *value, _In_ ULONG newValue", VIOGPU_CODE
        )
    )
    if "Interlocked" in shared_read or "Interlocked" in shared_write or "Interlocked" in seed:
        fail("native control BAR accesses must not issue ARM64 exclusive atomic operations")
    if shared_read != "ULONGresult=*value;KeMemoryBarrier();returnresult;":
        fail("native control BAR reads must use one volatile load followed by an ordering barrier")
    if shared_write != "KeMemoryBarrier();*value=newValue;KeMemoryBarrier();":
        fail("native control BAR writes must bracket one volatile store with ordering barriers")

    query_param = canonical_code(function_body("VioGpuAdapter::QueryNativeContextParameterLocked", VIOGPU_CODE))
    query_sequence = (
        query_param.find(
            "VioGpuSeedNativeControlResponse(this,owner,sequence,sizeof(MSM_CCMD_IOCTL_SIMPLE_GET_PARAM_RSP),&diagnostic)"
        ),
        query_param.find(
            "m_CtrlQueue.SubmitNativeControl(owner->ContextId,&request,sizeof(request),&diagnostic)"
        ),
        query_param.find("VioGpuConsumeNativeControlResponse(this,owner,sequence,parameter,value,&diagnostic)"),
    )
    if min(query_sequence) < 0 or list(query_sequence) != sorted(query_sequence):
        fail("GET_PARAM must seed the BAR response before submit and consume it only after VirtIO completion")
    if query_param.count("m_CtrlQueue.PoisonSynchronousRequests();") != 2:
        fail("GET_PARAM must poison malformed shared-memory state")
    for fragment in (
        "request.hdr.cmd=MSM_CCMD_IOCTL_SIMPLE;",
        "request.hdr.len=sizeof(request);",
        "request.hdr.seqno=sequence;",
        "request.hdr.rsp_off=0;",
        "request.ioctl_cmd=DRM_IOCTL_MSM_GET_PARAM;",
        "request.param.pipe=MSM_PIPE_3D0;",
        "request.param.param=parameter;",
    ):
        if query_param.count(fragment) != 1:
            fail(f"GET_PARAM request must use its exact 44-byte IOCTL_SIMPLE encoding: {fragment}")

    create_queue = canonical_code(
        function_body_with_parameters(
            "VioGpuAdapter::CreateNativeSubmitQueueLocked",
            "_Inout_ VIOGPU_NATIVE_CONTEXT_OWNER *owner, _Out_ PUINT queueId",
            VIOGPU_CODE,
        )
    )
    close_queue = canonical_code(
        function_body_with_parameters(
            "VioGpuAdapter::CloseNativeSubmitQueueLocked",
            "_Inout_ VIOGPU_NATIVE_CONTEXT_OWNER *owner",
            VIOGPU_CODE,
        )
    )
    new_request = (
        "MSM_CCMD_IOCTL_SIMPLE_SUBMITQUEUE_NEW_REQrequest={};",
        "request.hdr.cmd=MSM_CCMD_IOCTL_SIMPLE;",
        "request.hdr.len=sizeof(request);",
        "request.hdr.seqno=sequence;",
        "request.hdr.rsp_off=0;",
        "request.ioctl_cmd=DRM_IOCTL_MSM_SUBMITQUEUE_NEW;",
    )
    for fragment in new_request:
        if create_queue.count(fragment) != 1:
            fail(f"submitqueue NEW must use its exact 32-byte IOCTL_SIMPLE request: {fragment}")
    new_sequence = (
        create_queue.find(
            "VioGpuSeedNativeControlResponse(this,owner,sequence,sizeof(MSM_CCMD_IOCTL_SIMPLE_SUBMITQUEUE_NEW_RSP))"
        ),
        create_queue.find("owner->LastControlSeqno=sequence;"),
        create_queue.find("m_CtrlQueue.SubmitNativeControl(owner->ContextId,&request,sizeof(request))"),
        create_queue.find(
            "VioGpuCopyNativeControlResponse(this,owner,sequence,&response,sizeof(response))"
        ),
    )
    if min(new_sequence) < 0 or list(new_sequence) != sorted(new_sequence):
        fail("submitqueue NEW must release the response lease before VirtIO submit and reacquire it after completion")
    if create_queue.count("if(response.ret!=0){returnVioGpuHostContextRejected;}") != 1:
        fail("submitqueue NEW must classify only a complete nonzero return as rejected")
    response_shape = "response.submitqueue.flags!=0||response.submitqueue.prio!=0||response.submitqueue.id==0"
    if create_queue.count(response_shape) != 1:
        fail("submitqueue NEW must accept only flags=0, prio=0, and a nonzero returned queue id")
    ret_check = create_queue.find("if(response.ret!=0){returnVioGpuHostContextRejected;}")
    shape_check = create_queue.find(response_shape)
    publish_id = create_queue.find("owner->SubmitQueueId=response.submitqueue.id;")
    publish_created = create_queue.find("owner->SubmitQueueCreated=TRUE;")
    publish_output = create_queue.find("*queueId=owner->SubmitQueueId;")
    if min(ret_check, shape_check, publish_id, publish_created, publish_output) < 0 or not (
        ret_check < shape_check < publish_id < publish_created < publish_output
    ):
        fail("submitqueue NEW must publish ownership only after ret and returned-shape validation")

    close_request = (
        "MSM_CCMD_IOCTL_SIMPLE_SUBMITQUEUE_CLOSE_REQrequest={};",
        "request.hdr.cmd=MSM_CCMD_IOCTL_SIMPLE;",
        "request.hdr.len=sizeof(request);",
        "request.hdr.seqno=sequence;",
        "request.hdr.rsp_off=0;",
        "request.ioctl_cmd=DRM_IOCTL_MSM_SUBMITQUEUE_CLOSE;",
        "request.queue_id=owner->SubmitQueueId;",
    )
    for fragment in close_request:
        if close_queue.count(fragment) != 1:
            fail(f"submitqueue CLOSE must use its exact 24-byte request and owner queue id: {fragment}")
    close_sequence = (
        close_queue.find(
            "VioGpuSeedNativeControlResponse(this,owner,sequence,sizeof(MSM_CCMD_IOCTL_SIMPLE_SUBMITQUEUE_CLOSE_RSP))"
        ),
        close_queue.find("owner->LastControlSeqno=sequence;"),
        close_queue.find("m_CtrlQueue.SubmitNativeControl(owner->ContextId,&request,sizeof(request))"),
        close_queue.find(
            "VioGpuCopyNativeControlResponse(this,owner,sequence,&response,sizeof(response))"
        ),
    )
    if min(close_sequence) < 0 or list(close_sequence) != sorted(close_sequence):
        fail("submitqueue CLOSE must release the response lease before VirtIO submit and reacquire it after completion")
    close_guard = (
        "if(!VioGpuCopyNativeControlResponse(this,owner,sequence,&response,sizeof(response))||response.ret!=0)"
        "{m_CtrlQueue.PoisonSynchronousRequests();returnVioGpuHostContextUnknown;}"
    )
    if close_queue.count(close_guard) != 1:
        fail("submitqueue CLOSE must poison and retain ownership until a complete zero-return response")
    close_guard_end = close_queue.find(close_guard)
    clear_created = close_queue.find("owner->SubmitQueueCreated=FALSE;")
    clear_id = close_queue.find("owner->SubmitQueueId=0;")
    if min(close_guard_end, clear_created, clear_id) < 0 or not close_guard_end < clear_created < clear_id:
        fail("submitqueue CLOSE must clear ownership only after successful response validation")

    create = canonical_code(function_body("VioGpuAdapter::CreateNativeContext", VIOGPU_CODE))
    registry_insert = create.find("InsertTailList(&m_NativeContextRegistry,&owner->AdapterLink);")
    host_create = create.find("m_CtrlQueue.CreateNativeContext(contextId)")
    if registry_insert < 0 or host_create < 0 or registry_insert > host_create:
        fail("adapter must record Creating Host ownership before submitting CTX_CREATE")
    if create.count("owner->State=VioGpuNativeContextOwnerCreating;") != 1 or create.count(
        "owner->State=VioGpuNativeContextOwnerLive;"
    ) != 1:
        fail("adapter create must publish one Creating-to-Live Host-owner transition")
    creation_sequence = (
        create.find("m_CtrlQueue.CreateNativeContext(contextId)"),
        create.find("m_CtrlQueue.CreateNativeControlBlob(contextId,resourceId)"),
        create.find("AllocateNativeControlSlotLocked(&controlOffset,&controlAddress)"),
        create.find("owner->ControlBarOffset=controlOffset;"),
        create.find("m_CtrlQueue.MapNativeControlBlob(resourceId,controlOffset)"),
        create.find("m_CtrlQueue.GetLastNativeMapResponseDiagnostic(&mapResponse)"),
        create.find("m_pVioGpuDod->RecordNativeContextMapResponseDiagnostic(&mapResponse)"),
        create.find("owner->ControlMapped=TRUE;"),
        create.find("m_PciResources.MapHostVisibleAddress(controlOffset,VIOGPU_NATIVE_CONTROL_BLOB_SIZE,&controlAddress)"),
        create.find("m_PciResources.QueryHostVisibleMapping(&mappedPhysicalAddress,&mappedRegionOffset,&mappedLength)"),
        create.find("m_pVioGpuDod->RecordNativeContextMapMemoryDiagnostic("),
        create.find("owner->ControlAddress=controlAddress;"),
        create.find("QueryNativeContextParameterLocked(owner,MSM_PARAM_VA_START,&vaStart)"),
        create.find("QueryNativeContextParameterLocked(owner,MSM_PARAM_VA_SIZE,&vaSize)"),
        create.find("CreateNativeSubmitQueueLocked(owner,&submitQueueId)"),
        create.find("context->VaStart=vaStart;"),
        create.find("context->VaSize=vaSize;"),
        create.find("context->SubmitQueueId=submitQueueId;"),
        create.find("context->Registered=TRUE;"),
    )
    if min(creation_sequence) < 0 or list(creation_sequence) != sorted(creation_sequence):
        fail("native create must create/map blob-0, query both VA parameters, then publish the live registration")
    if create.count("m_pVioGpuDod->RecordNativeContextMapMemoryDiagnostic(STATUS_DEVICE_NOT_READY,0,0,hostVisibleBar,controlOffset,FALSE,FALSE);") != 1:
        fail("native create must invalidate stale BAR mapping diagnostics when the host map is rejected")

    map_slot = canonical_code(function_body("VioGpuAdapter::AllocateNativeControlSlotLocked", VIOGPU_CODE))
    for fragment in (
        "*offset=0;",
        "*address=NULL;",
        "regionSize<=VIOGPU_NATIVE_CONTROL_BAR_GUARD_SIZE||regionSize-VIOGPU_NATIVE_CONTROL_BAR_GUARD_SIZE<VIOGPU_NATIVE_CONTROL_BLOB_SIZE",
        "ULONGLONGslotCount=(regionSize-VIOGPU_NATIVE_CONTROL_BAR_GUARD_SIZE)/VIOGPU_NATIVE_CONTROL_BLOB_SIZE;",
        "owner->ControlAddress!=NULL&&owner->ControlBarOffset==candidate",
        "*offset=candidate;",
        "*address=NULL;",
    ):
        if map_slot.count(fragment) != 1:
            fail(f"native control BAR allocator must select one free mapped slot: {fragment}")
    if map_slot.count(
        "ULONGLONGcandidate=VIOGPU_NATIVE_CONTROL_BAR_GUARD_SIZE+slot*VIOGPU_NATIVE_CONTROL_BLOB_SIZE;"
    ) != 1:
        fail("native control BAR allocator must skip crosvm's drm2kgsl base guard")
    if create.count("owner->Registration=NULL;FailNativeContextAtAnyIrql();") != 1:
        fail("native create must retain every unresolved Host owner until reset")
    va_validation = (
        "vaStart==0||vaSize==0||(vaStart&(PAGE_SIZE-1))!=0||(vaSize&(PAGE_SIZE-1))!=0||"
        "vaStart>MAXULONGLONG-vaSize"
    )
    if create.count(va_validation) != 1:
        fail("native create must reject zero, unaligned, or overflowing Host VA ranges")

    for field in ("VaStart", "VaSize"):
        if native_header.count(f"ULONGLONG{field};") != 2:
            fail(f"native registration and protected snapshot must each retain one Host address-space field: {field}")
        assignments = re.findall(rf"\bcontext\s*->\s*{field}\s*=(?!=)\s*([^;]+);", VIOGPU_SOURCE)
        published_value = "vaStart" if field == "VaStart" else "vaSize"
        if [canonical_code(value) for value in assignments] != ["0", published_value, "0", "0"]:
            fail(f"{field} must have one success-only Host publisher and three lifecycle clears")
        acquire_snapshot = canonical_code(function_body("VioGpuAdapter::AcquireNativeContextSnapshot", VIOGPU_CODE))
        acquire_allocation_snapshot = canonical_code(
            function_body("VioGpuAdapter::AcquireNativeContextSnapshotForAllocation", VIOGPU_CODE)
        )
        if acquire_snapshot.count(f"snapshot->{field}=context->{field};") != 1:
            fail(f"native-context snapshot must copy {field} once under lifecycle protection")
        if acquire_allocation_snapshot.count(f"snapshot->{field}=context->{field};") != 1:
            fail(f"allocation lookup must copy {field} once from its unique matching context")
    if create.count("context->VaStart!=0") != 1 or create.count("context->VaSize!=0") != 1:
        fail("native context creation must reject stale Host address-space state")
    if create.count("context->SubmitQueueId!=0") != 1:
        fail("native context creation must reject stale submit-queue state")
    if native_header.count("UINTSubmitQueueId;") != 3:
        fail("native owner, registration, and protected snapshot must each retain one submit-queue identity")
    submit_queue_assignments = re.findall(r"\bcontext\s*->\s*SubmitQueueId\s*=(?!=)\s*([^;]+);", VIOGPU_SOURCE)
    if [canonical_code(value) for value in submit_queue_assignments] != ["0", "submitQueueId", "0", "0"]:
        fail("SubmitQueueId must have one success-only publisher and three lifecycle clears")
    for snapshot_function in ("VioGpuAdapter::AcquireNativeContextSnapshot", "VioGpuAdapter::AcquireNativeContextSnapshotForAllocation"):
        snapshot_body = canonical_code(function_body(snapshot_function, VIOGPU_CODE))
        if snapshot_body.count("snapshot->SubmitQueueId=context->SubmitQueueId;") != 1:
            fail("native snapshots must copy the submit-queue identity under lifecycle protection")

    released = canonical_code(function_body("VioGpuAdapter::IsNativeContextReleased", VIOGPU_CODE))
    if released.count("context->VaStart==0") != 1 or released.count("context->VaSize==0") != 1:
        fail("native context release proof must require both Host address-space fields to be cleared")
    if released.count("context->SubmitQueueId==0") != 1:
        fail("native context release proof must require submit-queue identity to be cleared")

    destroy = canonical_code(function_body("VioGpuAdapter::DestroyNativeContext", VIOGPU_CODE))
    mark_destroying = destroy.find("owner->State=VioGpuNativeContextOwnerDestroying;")
    host_destroy = destroy.find("DestroyNativeContextHostObjectsLocked(owner)")
    if mark_destroying < 0 or host_destroy < 0 or mark_destroying > host_destroy:
        fail("adapter must retain and mark Host ownership Destroying before resource/context teardown")

    cleanup = canonical_code(function_body("VioGpuAdapter::DestroyNativeContextHostObjectsLocked", VIOGPU_CODE))
    cleanup_sequence = (
        cleanup.find("CloseNativeSubmitQueueLocked(owner)"),
        cleanup.find("m_CtrlQueue.UnmapNativeControlBlob(owner->ControlResourceId)"),
        cleanup.find("m_PciResources.UnmapHostVisibleAddress(owner->ControlAddress)"),
        cleanup.find("m_CtrlQueue.UnrefNativeResource(owner->ControlResourceId)"),
        cleanup.find("m_CtrlQueue.DestroyNativeContext(owner->ContextId)"),
    )
    if min(cleanup_sequence) < 0 or list(cleanup_sequence) != sorted(cleanup_sequence):
        fail("normal native teardown must close submitqueue, unmap blob and BAR, unref, then destroy the Host context")
    if cleanup.count("returnVioGpuHostContextUnknown;") != 7 or cleanup.count(
        "returnVioGpuHostContextConfirmed;"
    ) != 1:
        fail("native teardown must retain ownership after every non-confirmed cleanup stage")
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


def check_native_map_diagnostics() -> None:
    dod_header = canonical_code(VIOGPU_HEADER_SOURCE)
    for fragment in (
        "VOIDRecordNativeContextMapResponseDiagnostic(_In_constVIOGPU_NATIVE_MAP_RESPONSE_DIAGNOSTIC*diagnostic);",
        "VOIDRecordNativeContextMapMemoryDiagnostic(_In_NTSTATUSstatus,_In_ULONGLONGphysicalAddress,_In_ULONGLONGlength,"
        "_In_UINTbar,_In_ULONGLONGregionOffset,_In_BOOLEANattempted,_In_BOOLEANmapped);",
    ):
        if dod_header.count(fragment) != 1:
            fail(f"DOD must expose one native map diagnostic recorder: {fragment}")

    source = VIOGPU_SOURCE
    response_names = (
        "NativeContextCreateMapResponseSize",
        "NativeContextCreateMapResponseType",
        "NativeContextCreateMapResponseFlags",
        "NativeContextCreateMapResponseFenceLow",
        "NativeContextCreateMapResponseFenceHigh",
        "NativeContextCreateMapResponseContextId",
        "NativeContextCreateMapResponseRingIndex",
        "NativeContextCreateMapResponseHeaderPadding",
        "NativeContextCreateMapResponseMapInfo",
        "NativeContextCreateMapResponseMapPadding",
        "NativeContextCreateMapResponseSubmitted",
        "NativeContextCreateMapResponseCompleted",
    )
    for name in response_names:
        if source.count(f'L"{name}"') != 1:
            fail(f"native map response diagnostic must write one {name} value")
    if source.count('L"NativeContextCreateMapResponseValidation"') != 2:
        fail("native map response validation must be cleared and then committed")

    memory_names = (
        "NativeContextCreateMapMemoryStatus",
        "NativeContextCreateMapMemoryPhysicalLow",
        "NativeContextCreateMapMemoryPhysicalHigh",
        "NativeContextCreateMapMemoryLengthLow",
        "NativeContextCreateMapMemoryLengthHigh",
        "NativeContextCreateMapMemoryBar",
        "NativeContextCreateMapMemoryRegionOffsetLow",
        "NativeContextCreateMapMemoryRegionOffsetHigh",
        "NativeContextCreateMapMemoryMapped",
    )
    for name in memory_names:
        if source.count(f'L"{name}"') != 1:
            fail(f"native map memory diagnostic must write one {name} value")
    if source.count('L"NativeContextCreateMapMemoryAttempted"') != 2:
        fail("native map memory attempted must be cleared and then committed")

    response = canonical_code(function_body("VioGpuDod::RecordNativeContextMapResponseDiagnostic", VIOGPU_CODE))
    response_sequence = (
        response.find("NTSTATUSmarkerClear=WriteRegistryDWORD(deviceKey,L,&zero)"),
        response.find("NTSTATUSwriteStatus=markerClear;"),
        response.find("for(UINTindex=0;index<ARRAYSIZE(values);++index)"),
        response.find("DWORDvalue=values[index].Value;"),
        response.find("WriteRegistryDWORD(deviceKey,values[index].Name,&value)"),
        response.find("ZwClose(deviceKey);"),
    )
    if min(response_sequence) < 0 or list(response_sequence) != sorted(response_sequence):
        fail("native map response diagnostics must clear, publish, and close one ordered snapshot")

    memory = canonical_code(function_body("VioGpuDod::RecordNativeContextMapMemoryDiagnostic", VIOGPU_CODE))
    memory_sequence = (
        memory.find("NTSTATUSmarkerClear=WriteRegistryDWORD(deviceKey,L,&zero)"),
        memory.find("NTSTATUSwrites[]={"),
        memory.find("BOOLEANwritesSucceeded=NT_SUCCESS(markerClear);"),
        memory.find("for(constNTSTATUSwrite:writes)"),
        memory.find("markerWrite=WriteRegistryDWORD(deviceKey,L,&attemptedValue)"),
        memory.find("ZwClose(deviceKey);"),
    )
    if min(memory_sequence) < 0 or list(memory_sequence) != sorted(memory_sequence):
        fail("native map memory diagnostics must commit the attempted marker after all fields")


def check_native_parameter_diagnostics() -> None:
    """Keep the bounded GET_PARAM diagnostic snapshot transactional."""
    queue_header = canonical_code(strip_cpp_comments_and_literals(QUEUE_HEADER_SOURCE))
    expected_phases = (
        "VioGpuNativeContextParameterNotStarted=0,"
        "VioGpuNativeContextParameterPreconditions=1,"
        "VioGpuNativeContextParameterSeed=2,"
        "VioGpuNativeContextParameterSubmitted=3,"
        "VioGpuNativeContextParameterCopy=4,"
        "VioGpuNativeContextParameterValidated=5,"
        "VioGpuNativeContextParameterComplete=6,"
    )
    if queue_header.count(expected_phases) != 1:
        fail("GET_PARAM diagnostics must keep one stable phase ABI")
    for enum_fragment in (
        "VioGpuNativeContextParameterWindowInvalidOwner=1,",
        "VioGpuNativeContextParameterWindowInvalidResponseOffset=6,",
        "VioGpuNativeContextParameterWindowReady=7,",
        "VioGpuNativeContextParameterSeedResponseOutOfBounds=3,",
        "VioGpuNativeContextParameterSeedWritten=5,",
        "VioGpuNativeContextParameterCopySequenceMismatch=3,",
        "VioGpuNativeContextParameterCopyCompleted=5,",
    ):
        if queue_header.count(enum_fragment) != 1:
            fail(f"GET_PARAM diagnostics must keep one stable result ABI: {enum_fragment}")

    diagnostic_struct_match = re.search(
        r"typedef\s+struct\s+viogpu_native_context_parameter_diagnostic\s*\{(?P<body>.*?)\}\s*"
        r"VIOGPU_NATIVE_CONTEXT_PARAMETER_DIAGNOSTIC(?:\s*,\s*\*\w+)?\s*;",
        QUEUE_HEADER_SOURCE,
        re.DOTALL,
    )
    if diagnostic_struct_match is None:
        fail("GET_PARAM diagnostic struct definition is missing")
    diagnostic_struct = diagnostic_struct_match.group("body")
    fields = (
        "ContextId",
        "Parameter",
        "Sequence",
        "PhysicalDeviceValid",
        "RegistryOpenStatus",
        "WindowStatus",
        "ControlBarOffset",
        "ControlAddress",
        "ControlBlobSize",
        "ResponseOffset",
        "ResponseCapacity",
        "SeedResult",
        "SeedSharedSeqno",
        "SharedSeqno",
        "SharedAsyncError",
        "SharedGlobalFaults",
        "OuterResponseSize",
        "OuterType",
        "OuterSubmitted",
        "OuterCompleted",
        "OuterValidation",
        "SubmitResult",
        "CopyResult",
        "InnerResponseLength",
        "InnerRet",
        "InnerPipe",
        "InnerParameter",
        "InnerValue",
        "InnerValueLength",
        "InnerPadding",
        "Validation",
        "Result",
        "RegistryWriteStatus",
        "Phase",
    )
    for field in fields:
        if len(re.findall(rf"\b{re.escape(field)}\b", diagnostic_struct)) != 1:
            fail(f"GET_PARAM diagnostic must declare one {field} field")

    dod_header = canonical_code(VIOGPU_HEADER_SOURCE)
    declaration = (
        "VOIDRecordNativeContextParameterDiagnostic"
        "(_Inout_PVIOGPU_NATIVE_CONTEXT_PARAMETER_DIAGNOSTICdiagnostic);"
    )
    if dod_header.count(declaration) != 1:
        fail("DOD must expose exactly one GET_PARAM diagnostic recorder")

    recorder_source = function_body("VioGpuDod::RecordNativeContextParameterDiagnostic", VIOGPU_SOURCE)
    recorder = canonical_code(function_body("VioGpuDod::RecordNativeContextParameterDiagnostic", VIOGPU_CODE))
    registry_names = (
        "NativeContextGetParamContextId",
        "NativeContextGetParamParameter",
        "NativeContextGetParamSequence",
        "NativeContextGetParamPhysicalDevice",
        "NativeContextGetParamRegistryOpenStatus",
        "NativeContextGetParamWindowStatus",
        "NativeContextGetParamControlBarOffsetLow",
        "NativeContextGetParamControlBarOffsetHigh",
        "NativeContextGetParamControlAddressLow",
        "NativeContextGetParamControlAddressHigh",
        "NativeContextGetParamControlBlobSize",
        "NativeContextGetParamResponseOffset",
        "NativeContextGetParamResponseCapacity",
        "NativeContextGetParamSeedResult",
        "NativeContextGetParamSeedSharedSeqno",
        "NativeContextGetParamSharedSeqno",
        "NativeContextGetParamSharedAsyncError",
        "NativeContextGetParamSharedGlobalFaults",
        "NativeContextGetParamOuterResponseSize",
        "NativeContextGetParamOuterType",
        "NativeContextGetParamOuterSubmitted",
        "NativeContextGetParamOuterCompleted",
        "NativeContextGetParamOuterValidation",
        "NativeContextGetParamSubmitResult",
        "NativeContextGetParamCopyResult",
        "NativeContextGetParamInnerResponseLength",
        "NativeContextGetParamInnerRet",
        "NativeContextGetParamInnerPipe",
        "NativeContextGetParamInnerParameter",
        "NativeContextGetParamInnerValueLow",
        "NativeContextGetParamInnerValueHigh",
        "NativeContextGetParamInnerValueLength",
        "NativeContextGetParamInnerPadding",
        "NativeContextGetParamValidation",
        "NativeContextGetParamResult",
        "NativeContextGetParamWriteStatus",
    )
    literal_stream = "".join(re.findall(r'L\"([^\"\\]*(?:\\.[^\"\\]*)*)\"', recorder_source))
    for name in registry_names:
        if literal_stream.count(name) != 1:
            fail(f"GET_PARAM diagnostic must write one {name} value")
    if literal_stream.count("NativeContextGetParamPhase") != 2:
        fail("GET_PARAM phase marker must be cleared and committed exactly once")

    reader = START_DIAGNOSTIC_SCRIPT_PATH.read_text(encoding="utf-8")
    for name in registry_names + ("NativeContextGetParamPhase",):
        if reader.count(name) < 1:
            fail(f"GET_PARAM diagnostic reader must expose {name}")
    if reader.count("foreach ($name in $parameterDiagnosticNames)") != 1 or reader.count(
        "$parameterSnapshot[$name] = ConvertTo-DwordValue $diagnostic.$name"
    ) != 1:
        fail("GET_PARAM diagnostic reader must consume every declared snapshot field")
    if reader.count("contains a partial GET_PARAM diagnostic") != 1:
        fail("GET_PARAM diagnostic reader must reject partial snapshots")
    if reader.count("NativeContextGetParamCommitted = $parameterSnapshotCommitted") != 1:
        fail("GET_PARAM diagnostic reader must expose commit state")
    if reader.count("$parameterPhase -ge 1 -and $parameterPhase -le 6") != 1:
        fail("GET_PARAM diagnostic reader must require a committed phase")
    if reader.count("$parameterWriteStatus -eq 0") != 1:
        fail("GET_PARAM diagnostic reader must reject registry-write failures")
    if reader.count("NativeContextGetParam = $parameterSnapshot") != 1:
        fail("GET_PARAM diagnostic reader must return one complete snapshot object")

    recorder_sequence = (
        recorder.find("NTSTATUSmarkerClear=WriteRegistryDWORD(deviceKey,L,&zero)"),
        recorder.find("NTSTATUSwriteStatus=markerClear;"),
        recorder.find("for(UINTindex=0;index<ARRAYSIZE(values);++index)"),
        recorder.find("DWORDregistryWriteStatus=NT_SUCCESS(writeStatus)?"),
        recorder.find("NTSTATUSstatusWrite=WriteRegistryDWORD(deviceKey,L,&registryWriteStatus)"),
        recorder.find("markerWrite=WriteRegistryDWORD(deviceKey,L,&phase)"),
        recorder.find("ZwClose(deviceKey);"),
    )
    if min(recorder_sequence) < 0 or list(recorder_sequence) != sorted(recorder_sequence):
        fail("GET_PARAM diagnostics must clear, publish, write status, commit, and close in order")

    submit = canonical_code(function_body("CtrlQueue::SubmitNativeControl", QUEUE_CODE))
    submit_sequence = (
        submit.find("diagnostic->OuterResponseSize=0;"),
        submit.find("SubmitSynchronousLocked(vbuf,&releaseBuffer,&submitted)"),
        submit.find("diagnostic->OuterResponseSize=vbuf->response_size;"),
        submit.find("ReleaseBuffer(vbuf);"),
        submit.find("diagnostic->SubmitResult=result;"),
    )
    if min(submit_sequence) < 0 or list(submit_sequence) != sorted(submit_sequence):
        fail("GET_PARAM outer diagnostics must capture completion before buffer release and publish the result last")
    for fragment in (
        "diagnostic->OuterSubmitted=submitted?1:0;",
        "diagnostic->OuterCompleted=completed?1:0;",
        "diagnostic->OuterValidation=VioGpuValidatePlainControlResponse(",
    ):
        if submit.count(fragment) != 1:
            fail(f"GET_PARAM outer diagnostics must retain {fragment}")

    query = canonical_code(function_body("VioGpuAdapter::QueryNativeContextParameterLocked", VIOGPU_CODE))
    query_sequence = (
        query.find("diagnostic.Phase=VioGpuNativeContextParameterPreconditions;"),
        query.find("diagnostic.Phase=VioGpuNativeContextParameterSeed;"),
        query.find("diagnostic.Phase=VioGpuNativeContextParameterSubmitted;"),
        query.find("diagnostic.Phase=VioGpuNativeContextParameterCopy;"),
        query.find("diagnostic.Phase=VioGpuNativeContextParameterValidated;"),
        query.find("diagnostic.Phase=VioGpuNativeContextParameterComplete;"),
    )
    if min(query_sequence) < 0 or list(query_sequence) != sorted(query_sequence):
        fail("GET_PARAM diagnostic phases must advance monotonically through the lifecycle")
    if query.count("m_pVioGpuDod->RecordNativeContextParameterDiagnostic(&diagnostic)") != 9:
        fail("GET_PARAM must persist each precondition/seed/submit/copy/failure/complete phase")
    if query.count("diagnostic.Result=VioGpuHostContextNotSubmitted;") != 2:
        fail("GET_PARAM must classify the precondition path as not submitted")
    if query.count("diagnostic.Result=VioGpuHostContextUnknown;") != 2:
        fail("GET_PARAM must classify seed and shared-response failures as unknown")
    if query.count("diagnostic.Result=VioGpuHostContextConfirmed;") != 1:
        fail("GET_PARAM must publish one confirmed result")

    start = canonical_code(function_body("VioGpuDod::RecordNativeStartDiagnostic", VIOGPU_CODE))
    invalidate_sequence = (
        start.find("if(stage==VioGpuNativeStartEntered)"),
        start.find("parameterPhaseInvalidateWrite=WriteRegistryDWORD(deviceKey,L,&parameterPhaseZero)"),
        start.find("parameterWriteStatusInvalidateWrite=WriteRegistryDWORD(deviceKey,L,&parameterWriteStatus)"),
    )
    if min(invalidate_sequence) < 0 or list(invalidate_sequence) != sorted(invalidate_sequence):
        fail("GET_PARAM diagnostics must be invalidated at the beginning of each native start")
    if "!NT_SUCCESS(parameterPhaseInvalidateWrite)" not in start or "!NT_SUCCESS(parameterWriteStatusInvalidateWrite)" not in start:
        fail("native start must surface GET_PARAM diagnostic invalidation failures")


def check_wddm_private_abi(root: ET.Element) -> None:
    abi = canonical_code(WDDM_ABI_HEADER_CODE)
    require_integer_define(WDDM_ABI_HEADER_CODE, "VIOGPU_WDDM_ABI_MAGIC", 0x504D5644, "WDDM private ABI")
    require_integer_define(WDDM_ABI_HEADER_CODE, "VIOGPU_WDDM_ABI_VERSION", 0, "WDDM private ABI")
    require_integer_define(WDDM_ABI_HEADER_CODE, "VIOGPU_WDDM_CAPABILITIES_NONE", 0, "WDDM private ABI")
    require_integer_define(WDDM_ABI_HEADER_CODE, "VIOGPU_WDDM_ESCAPE_FLAGS_NONE", 0, "WDDM private ABI")

    if "Experimental pre-v1 snapshot" not in WDDM_ABI_HEADER_SOURCE or (
        "Version 1 must not be published until the" not in WDDM_ABI_HEADER_SOURCE
    ):
        fail("WDDM private ABI must remain explicitly experimental pre-v1")

    if WDDM_ABI_HEADER_SOURCE.count("#pragma pack(push, 4)") != 1 or WDDM_ABI_HEADER_SOURCE.count(
        "#pragma pack(pop)"
    ) != 1:
        fail("WDDM private ABI must use one balanced pack(4) region")
    if re.search(r"\b[A-Z0-9_]*(?:MIN|MAX|FULL|FORWARD|COMPAT)[A-Z0-9_]*VERSION\b", WDDM_ABI_HEADER_CODE):
        fail("WDDM private ABI must use exact current-version matching without compatibility macros")
    for integer_type in (
        "typedefunsignedintVIOGPU_WDDM_UINT32;",
        "typedefunsignedlonglongVIOGPU_WDDM_UINT64;",
    ):
        if abi.count(integer_type) != 1:
            fail(f"WDDM private ABI must declare its WDK-independent integer type exactly once: {integer_type}")
    if "#include<stdint.h>" in abi:
        fail("WDDM private ABI must not pull user CRT integer headers into the WDK kernel build")

    expected_structs = {
        "VIOGPU_WDDM_ABI_HEADER": """
            VIOGPU_WDDM_UINT32 Magic;
            VIOGPU_WDDM_UINT32 Version;
            VIOGPU_WDDM_UINT32 Size;
            VIOGPU_WDDM_UINT32 Reserved;
        """,
        "VIOGPU_WDDM_ADAPTER_INFO": """
            VIOGPU_WDDM_ABI_HEADER Header;
            VIOGPU_WDDM_UINT64 Capabilities;
            VIOGPU_WDDM_UINT64 ResetGeneration;
            VIOGPU_WDDM_UINT32 MsmMajorVersion;
            VIOGPU_WDDM_UINT32 MsmMinorVersion;
            VIOGPU_WDDM_UINT32 MsmPatchVersion;
            VIOGPU_WDDM_UINT32 GpuId;
            VIOGPU_WDDM_UINT64 ChipId;
            VIOGPU_WDDM_UINT32 GmemSize;
            VIOGPU_WDDM_UINT32 PriorityCount;
            VIOGPU_WDDM_UINT64 GmemBase;
            VIOGPU_WDDM_UINT32 HighestBankBit;
            VIOGPU_WDDM_UINT32 HasCachedCoherentMemory;
            VIOGPU_WDDM_UINT64 UbwcSwizzle;
            VIOGPU_WDDM_UINT64 MacrotileMode;
            VIOGPU_WDDM_UINT64 UcheTrapBase;
            VIOGPU_WDDM_UINT32 HasRayTracing;
            VIOGPU_WDDM_UINT32 MaxFrequency;
            VIOGPU_WDDM_UINT64 Reserved[2];
        """,
        "VIOGPU_WDDM_ALLOCATION_INFO": """
            VIOGPU_WDDM_ABI_HEADER Header;
            VIOGPU_WDDM_UINT64 Size;
            VIOGPU_WDDM_UINT64 Alignment;
            VIOGPU_WDDM_UINT64 RequestedIova;
            VIOGPU_WDDM_UINT64 ExpectedResetGeneration;
            VIOGPU_WDDM_UINT32 Flags;
            VIOGPU_WDDM_UINT32 Format;
            VIOGPU_WDDM_UINT32 Width;
            VIOGPU_WDDM_UINT32 Height;
            VIOGPU_WDDM_UINT32 Pitch;
            VIOGPU_WDDM_UINT32 RefreshRateNumerator;
            VIOGPU_WDDM_UINT32 RefreshRateDenominator;
            VIOGPU_WDDM_UINT32 ContextId;
        """,
        "VIOGPU_WDDM_CONTEXT_CREATE": """
            VIOGPU_WDDM_ABI_HEADER Header;
            VIOGPU_WDDM_UINT64 ExpectedResetGeneration;
            VIOGPU_WDDM_UINT32 Flags;
            VIOGPU_WDDM_UINT32 Reserved;
        """,
        "VIOGPU_WDDM_CONTEXT_INFO": """
            VIOGPU_WDDM_ABI_HEADER Header;
            VIOGPU_WDDM_UINT32 Opcode;
            VIOGPU_WDDM_UINT32 Flags;
            VIOGPU_WDDM_UINT64 ExpectedResetGeneration;
            VIOGPU_WDDM_UINT64 VaStart;
            VIOGPU_WDDM_UINT64 VaSize;
            VIOGPU_WDDM_UINT64 ResetGeneration;
            VIOGPU_WDDM_UINT32 ContextId;
            VIOGPU_WDDM_UINT32 SubmitQueueId;
        """,
        "VIOGPU_WDDM_FENCE_INFO": """
            VIOGPU_WDDM_ABI_HEADER Header;
            VIOGPU_WDDM_UINT32 Opcode;
            VIOGPU_WDDM_UINT32 Flags;
            VIOGPU_WDDM_UINT64 ExpectedResetGeneration;
            VIOGPU_WDDM_UINT64 CompletedFence;
            VIOGPU_WDDM_UINT64 ResetGeneration;
            VIOGPU_WDDM_UINT32 ContextId;
            VIOGPU_WDDM_UINT32 Reserved;
        """,
        "VIOGPU_WDDM_RENDER_COMMAND": """
            VIOGPU_WDDM_ABI_HEADER Header;
            VIOGPU_WDDM_UINT32 Opcode;
            VIOGPU_WDDM_UINT32 Flags;
            VIOGPU_WDDM_UINT64 ExpectedResetGeneration;
            VIOGPU_WDDM_UINT32 AllocationReferencesOffset;
            VIOGPU_WDDM_UINT32 AllocationReferenceCount;
            VIOGPU_WDDM_UINT32 CommandStreamOffset;
            VIOGPU_WDDM_UINT32 CommandStreamSize;
            VIOGPU_WDDM_UINT32 Reserved[4];
        """,
        "VIOGPU_WDDM_ALLOCATION_REFERENCE": """
            VIOGPU_WDDM_UINT32 AllocationIndex;
            VIOGPU_WDDM_UINT32 Flags;
            VIOGPU_WDDM_UINT64 AllocationOffset;
            VIOGPU_WDDM_UINT64 Length;
            VIOGPU_WDDM_UINT32 PatchOffset;
            VIOGPU_WDDM_UINT32 Reserved;
        """,
    }
    declared_structs = re.findall(
        r"\btypedef\s+struct\s+(VIOGPU_WDDM_[A-Z0-9_]+)\s*\{.*?\}\s*\1\s*;",
        WDDM_ABI_HEADER_CODE,
        re.DOTALL,
    )
    if sorted(declared_structs) != sorted(expected_structs):
        fail("WDDM private ABI must match the current pre-v1 validation snapshot")
    for name, expected_body in expected_structs.items():
        matches = re.findall(
            rf"\btypedef\s+struct\s+{re.escape(name)}\s*\{{(.*?)\}}\s*{re.escape(name)}\s*;",
            WDDM_ABI_HEADER_CODE,
            re.DOTALL,
        )
        if len(matches) != 1 or canonical_code(matches[0]) != canonical_code(expected_body):
            fail(f"WDDM private ABI structure {name} must keep its current pre-v1 field contract")

    forbidden_identifiers = (
        "HANDLE",
        "PVOID",
        "PHYSICAL_ADDRESS",
        "GPA",
        "IOVA",
        "VIRTIO",
        "KGSL",
        "KMD_CONTEXT",
        "ResourceId",
        "BlobId",
    )
    if "*" in abi or any(re.search(rf"\b{re.escape(token)}\b", WDDM_ABI_HEADER_CODE) for token in forbidden_identifiers):
        fail("WDDM private ABI must not expose pointers, Windows handles, physical addresses, or transport/KMD identities")

    expected_fixture_files = {
        ".gitattributes",
        "README.md",
        "abi_manifest.cpp",
        "abi_manifest_entries.h",
        "expected-pre-v1.txt",
        "run-local.sh",
        "run-msvc.cmd",
    }
    if not WDDM_ABI_FIXTURE_DIR.is_dir() or {
        path.name for path in WDDM_ABI_FIXTURE_DIR.iterdir() if path.is_file()
    } != expected_fixture_files:
        fail("WDDM private ABI fixture must contain the exact dual-endpoint manifest file set")
    fixture_attributes = (WDDM_ABI_FIXTURE_DIR / ".gitattributes").read_text(encoding="utf-8")
    if fixture_attributes != "expected-pre-v1.txt -text\n":
        fail("WDDM private ABI fixture must preserve byte-exact LF manifest data on Windows checkout")
    manifest = (WDDM_ABI_FIXTURE_DIR / "abi_manifest.cpp").read_text(encoding="utf-8")
    manifest_entries = (WDDM_ABI_FIXTURE_DIR / "abi_manifest_entries.h").read_text(encoding="utf-8")
    for width_assertion in (
        "ABI_SIZE(size.uint32, VIOGPU_WDDM_UINT32, 4);",
        "ABI_SIZE(size.uint64, VIOGPU_WDDM_UINT64, 8);",
    ):
        if manifest_entries.count(width_assertion) != 1:
            fail(f"WDDM private ABI fixture must assert each local integer width exactly once: {width_assertion}")
    for context_info_assertion in (
        "ABI_VALUE(escape.opcode.get_context_info, VIOGPU_WDDM_ESCAPE_GET_CONTEXT_INFO, 1);",
        "ABI_VALUE(escape.flags.none, VIOGPU_WDDM_ESCAPE_FLAGS_NONE, 0);",
        "ABI_SIZE(size.context_info, VIOGPU_WDDM_CONTEXT_INFO, 64);",
        "ABI_OFFSET(offset.context_info.va_start, VIOGPU_WDDM_CONTEXT_INFO, VaStart, 32);",
        "ABI_OFFSET(offset.context_info.va_size, VIOGPU_WDDM_CONTEXT_INFO, VaSize, 40);",
        "ABI_OFFSET(offset.context_info.reset_generation, VIOGPU_WDDM_CONTEXT_INFO, ResetGeneration, 48);",
        "ABI_OFFSET(offset.context_info.context_id, VIOGPU_WDDM_CONTEXT_INFO, ContextId, 56);",
        "ABI_OFFSET(offset.context_info.submit_queue_id, VIOGPU_WDDM_CONTEXT_INFO, SubmitQueueId, 60);",
    ):
        if manifest_entries.count(context_info_assertion) != 1:
            fail(f"WDDM private ABI fixture must lock the context-info contract: {context_info_assertion}")
    for fence_info_assertion in (
        "ABI_VALUE(escape.opcode.get_completed_fence, VIOGPU_WDDM_ESCAPE_GET_COMPLETED_FENCE, 2);",
        "ABI_SIZE(size.fence_info, VIOGPU_WDDM_FENCE_INFO, 56);",
        "ABI_OFFSET(offset.fence_info.completed_fence, VIOGPU_WDDM_FENCE_INFO, CompletedFence, 32);",
        "ABI_OFFSET(offset.fence_info.reset_generation, VIOGPU_WDDM_FENCE_INFO, ResetGeneration, 40);",
        "ABI_OFFSET(offset.fence_info.context_id, VIOGPU_WDDM_FENCE_INFO, ContextId, 48);",
    ):
        if manifest_entries.count(fence_info_assertion) != 1:
            fail(f"WDDM private ABI fixture must lock the fence-info contract: {fence_info_assertion}")
    local_runner_path = WDDM_ABI_FIXTURE_DIR / "run-local.sh"
    local_runner = local_runner_path.read_text(encoding="utf-8")
    msvc_runner = (WDDM_ABI_FIXTURE_DIR / "run-msvc.cmd").read_text(encoding="utf-8")
    if os.name != "nt" and local_runner_path.stat().st_mode & 0o111 == 0:
        fail("local WDDM private ABI runner must be executable")
    if msvc_runner.count(r'fc /b "%SCRIPT_DIR%\expected-pre-v1.txt" "%OUT_DIR%\%%E.txt" >nul') != 1 or (
        r'fc "%SCRIPT_DIR%\expected-pre-v1.txt" "%OUT_DIR%\%%E.txt"' not in msvc_runner
    ):
        fail("MSVC WDDM private ABI runner must compare and diagnose each endpoint inside its loop")
    for endpoint in ("KMD", "UMD"):
        if manifest.count(f"ABI_ENDPOINT_{endpoint}") != 2:
            fail(f"WDDM private ABI manifest must enforce one {endpoint} endpoint selection")
        if local_runner.count(f"-DABI_ENDPOINT_{endpoint}") != 2:
            fail(f"local WDDM private ABI runner must compile the {endpoint} endpoint once with GCC and Clang")
        if msvc_runner.count(f"/DABI_ENDPOINT_%%E") != 1:
            fail("MSVC WDDM private ABI runner must compile each endpoint through one shared loop")

    project_headers = [
        element.attrib.get("Include", "").replace("\\", "/")
        for element in root.findall(".//msbuild:ClInclude[@Include]", NAMESPACE)
    ]
    if project_headers.count("../shared/viogpu_wddm_abi.h") != 1:
        fail("full WDDM project must track the shared private ABI header exactly once")
    workflow = WORKFLOW_PATH.read_text(encoding="utf-8")
    if workflow.count('"viogpu/shared/viogpu_wddm_abi.h"') != 2 or workflow.count(
        '"viogpu/tests/wddm-private-abi/**"'
    ) != 2:
        fail("ARM64 workflow must trigger on the shared WDDM ABI and its fixture")
    if workflow.count(r"viogpu\tests\wddm-private-abi\run-msvc.cmd") != 1:
        fail("ARM64 workflow must run the WDDM private ABI MSVC endpoint gate exactly once")

    query = canonical_code(function_body("QueryUmdPrivateInfo", WDDM_DDI_CODE))
    query_guard = (
        "adapter==NULL||queryAdapterInfo->pInputData!=NULL||queryAdapterInfo->InputDataSize!=0||"
        "queryAdapterInfo->pOutputData==NULL||"
        "queryAdapterInfo->OutputDataSize!=sizeof(VIOGPU_WDDM_ADAPTER_INFO)"
    )
    if query.count(query_guard) != 1 or query.count("adapterInfo->Capabilities=VIOGPU_WDDM_CAPABILITIES_NONE;") != 1:
        fail("UMDRIVERPRIVATE must require zero input, exact output size, and zero capabilities")
    zero_output = query.find("RtlZeroMemory(adapterInfo,sizeof(*adapterInfo));")
    initialize_header = query.find("InitializeAbiHeader(&adapterInfo->Header,sizeof(*adapterInfo));")
    if min(zero_output, initialize_header) < 0 or zero_output > initialize_header:
        fail("UMDRIVERPRIVATE must initialize and zero the complete current pre-v1 output")
    if query.count("adapterInfo->ResetGeneration=resetGeneration;") != 1 or query.count(
        "adapter->QueryNativeContextReadiness(&capset,NULL,NULL,&resetGeneration)"
    ) != 1:
        fail("UMDRIVERPRIVATE must publish the stable 64-bit readiness reset generation")
    if (
        query.count("adapterInfo->PriorityCount=1;") != 1
        or "adapterInfo->PriorityCount=capset.msm.priorities;" in query
    ):
        fail("UMDRIVERPRIVATE must expose only the context-owned priority-zero submitqueue")
    for fragment in (
        "switch(capset.msm.has_raytracing)",
        "caseVIRTGPU_CAP_BOOL_TRUE:hasRayTracing=1;break;",
        "caseVIRTGPU_CAP_BOOL_UNSUPPORTED_BY_HOST:caseVIRTGPU_CAP_BOOL_FALSE:break;",
        "default:returnSTATUS_GRAPHICS_DRIVER_MISMATCH;",
        "adapterInfo->HasRayTracing=hasRayTracing;",
    ):
        if query.count(fragment) != 1:
            fail(f"UMDRIVERPRIVATE must normalize the optional ray-tracing wire boolean: {fragment}")
    if "adapterInfo->HasRayTracing=capset.msm.has_raytracing;" in query:
        fail("UMDRIVERPRIVATE must not publish the optional wire boolean without normalization")
    if any(token in query for token in ("va_start", "va_size", "ContextId", "ResourceId", "PhysicalAddress")):
        fail("UMDRIVERPRIVATE must not expose VA ranges or KMD/transport identities")
    query_dispatch = canonical_code(function_body("VioGpuWddmQueryAdapterInfo", WDDM_DDI_CODE))
    if query_dispatch.count("if(pQueryAdapterInfo->Type==DXGKQAITYPE_UMDRIVERPRIVATE)") != 1 or query_dispatch.count(
        "status=QueryUmdPrivateInfo(adapter,pQueryAdapterInfo);"
    ) != 1:
        fail("QueryAdapterInfo must dispatch UMDRIVERPRIVATE through the current pre-v1 endpoint")

    if canonical_code(WDDM_DDI_HEADER_CODE).count("DXGKDDI_ESCAPEVioGpuWddmEscape;") != 1:
        fail("full WDDM must declare one dedicated Escape wrapper")
    escape_dispatch = canonical_code(function_body("VioGpuWddmEscape", WDDM_DDI_CODE))
    for fragment in (
        "hAdapter==NULL",
        "escape==NULL",
        "KeGetCurrentIrql()!=PASSIVE_LEVEL",
        "escape->hContext!=NULL||escape->PrivateDriverDataSize==sizeof(VIOGPU_WDDM_CONTEXT_INFO)",
        "returnQueryContextInfo(reinterpret_cast<VioGpuDod*>(hAdapter),escape);",
        "returnVioGpuDodEscape(hAdapter,escape);",
    ):
        if escape_dispatch.count(fragment) != 1:
            fail(f"full WDDM Escape dispatch must keep its context/private fallback boundary: {fragment}")
    for fragment in (
        "escape->PrivateDriverDataSize==sizeof(VIOGPU_WDDM_FENCE_INFO)",
        "returnQueryCompletedFenceInfo(reinterpret_cast<VioGpuDod*>(hAdapter),escape);",
    ):
        if escape_dispatch.count(fragment) != 1:
            fail(f"Escape must dispatch the exact context completion endpoint: {fragment}")
    completed_fence_query = canonical_code(function_body("QueryCompletedFenceInfo", WDDM_DDI_CODE))
    for fragment in (
        "escape->hDevice==NULL",
        "escape->hContext==NULL",
        "escape->Flags.Value!=0",
        "escape->PrivateDriverDataSize!=sizeof(VIOGPU_WDDM_FENCE_INFO)",
        "RtlCopyMemory(&request,escape->pPrivateDriverData,sizeof(request));",
        "request.Opcode!=VIOGPU_WDDM_ESCAPE_GET_COMPLETED_FENCE",
        "request.CompletedFence!=0",
        "request.ResetGeneration!=0",
        "request.ContextId!=0",
        "!ExAcquireRundownProtection(&context->Operations)",
        "context->Device!=device",
        "!adapter->IsDriverActive()",
        "VioGpuAdapter::AcquireNativeContextSnapshot(&context->NativeContext,&snapshot)",
        "snapshot.ResetGeneration!=request.ExpectedResetGeneration",
        "response.CompletedFence=QueryContextCompletedUmdFence(context);",
        "RtlCopyMemory(escape->pPrivateDriverData,&response,sizeof(response));",
        "VioGpuAdapter::ReleaseNativeContextSnapshot(&snapshot);",
        "ExReleaseRundownProtection(&context->Operations);",
    ):
        if fragment not in completed_fence_query:
            fail(f"completed-fence Escape must keep its exact context/generation contract: {fragment}")
    completion_order = (
        "ExAcquireRundownProtection(&context->Operations)",
        "VioGpuAdapter::AcquireNativeContextSnapshot(&context->NativeContext,&snapshot)",
        "response.CompletedFence=QueryContextCompletedUmdFence(context);",
        "RtlCopyMemory(escape->pPrivateDriverData,&response,sizeof(response));",
        "VioGpuAdapter::ReleaseNativeContextSnapshot(&snapshot);",
        "ExReleaseRundownProtection(&context->Operations);",
    )
    completion_positions = [completed_fence_query.find(fragment) for fragment in completion_order]
    if any(position < 0 for position in completion_positions) or completion_positions != sorted(completion_positions):
        fail("completed-fence query must publish only while context and Native Context ownership are held")

    context_query_body = function_body("QueryContextInfo", WDDM_DDI_CODE)
    context_query = canonical_code(context_query_body)
    query_requirements = (
        "KeGetCurrentIrql()!=PASSIVE_LEVEL",
        "escape->hDevice==NULL",
        "escape->hContext==NULL",
        "escape->Flags.Value!=0",
        "escape->pPrivateDriverData==NULL",
        "escape->PrivateDriverDataSize!=sizeof(VIOGPU_WDDM_CONTEXT_INFO)",
        "RtlCopyMemory(&request,escape->pPrivateDriverData,sizeof(request));",
        "!IsCurrentAbiHeader(&request.Header,sizeof(request))",
        "request.Opcode!=VIOGPU_WDDM_ESCAPE_GET_CONTEXT_INFO",
        "request.Flags!=VIOGPU_WDDM_ESCAPE_FLAGS_NONE",
        "request.ExpectedResetGeneration==0",
        "request.VaStart!=0",
        "request.VaSize!=0",
        "request.ResetGeneration!=0",
        "request.ContextId!=0",
        "context->Device!=device",
        "device->Adapter!=adapter",
        "!adapter->IsDriverActive()",
        "snapshot.ResetGeneration!=request.ExpectedResetGeneration",
        "snapshot.VaStart==0",
        "snapshot.VaSize==0",
        "(snapshot.VaStart&(PAGE_SIZE-1))!=0",
        "(snapshot.VaSize&(PAGE_SIZE-1))!=0",
        "vaEnd<snapshot.VaStart",
    )
    for fragment in query_requirements:
        if context_query.count(fragment) != 1:
            fail(f"GET_CONTEXT_INFO must enforce its exact context-scoped input/output contract: {fragment}")
    if any(
        token in context_query
        for token in (
            "QueryNativeContextReadiness",
            "capset",
            "ResourceId",
            "BlobId",
            "PhysicalAddress",
        )
    ):
        fail("GET_CONTEXT_INFO must not substitute adapter-wide data or expose KMD/transport identities")

    acquire_context = context_query.find("if(!ExAcquireRundownProtection(&context->Operations))")
    validate_identity = context_query.find("context->Signature!=VIOGPU_WDDM_CONTEXT_SIGNATURE")
    acquire_snapshot = context_query.find(
        "VioGpuAdapter::AcquireNativeContextSnapshot(&context->NativeContext,&snapshot)"
    )
    validate_va = context_query.find("snapshot.ResetGeneration!=request.ExpectedResetGeneration")
    publish = context_query.find("RtlCopyMemory(escape->pPrivateDriverData,&response,sizeof(response));")
    release_snapshot = context_query.find("VioGpuAdapter::ReleaseNativeContextSnapshot(&snapshot);")
    release_context = context_query.find("ExReleaseRundownProtection(&context->Operations);")
    query_sequence = (
        acquire_context,
        validate_identity,
        acquire_snapshot,
        validate_va,
        publish,
        release_snapshot,
        release_context,
    )
    if min(query_sequence) < 0 or list(query_sequence) != sorted(query_sequence):
        fail("GET_CONTEXT_INFO must hold context and native snapshots through final response publication")
    if context_query.count("ExAcquireRundownProtection(&context->Operations)") != 1 or context_query.count(
        "ExReleaseRundownProtection(&context->Operations);"
    ) != 1:
        fail("GET_CONTEXT_INFO must use one balanced context rundown interval")
    if context_query.count("VioGpuAdapter::AcquireNativeContextSnapshot(") != 1 or context_query.count(
        "VioGpuAdapter::ReleaseNativeContextSnapshot(&snapshot);"
    ) != 1:
        fail("GET_CONTEXT_INFO must use one balanced native-context snapshot")
    response_assignments = (
        "InitializeAbiHeader(&response.Header,sizeof(response));",
        "response.Opcode=VIOGPU_WDDM_ESCAPE_GET_CONTEXT_INFO;",
        "response.Flags=VIOGPU_WDDM_ESCAPE_FLAGS_NONE;",
        "response.ExpectedResetGeneration=request.ExpectedResetGeneration;",
        "response.VaStart=snapshot.VaStart;",
        "response.VaSize=snapshot.VaSize;",
        "response.ResetGeneration=snapshot.ResetGeneration;",
        "response.ContextId=snapshot.ContextId;",
    )
    for fragment in response_assignments:
        if context_query.count(fragment) != 1:
            fail(f"GET_CONTEXT_INFO may publish only the exact zero-initialized response: {fragment}")
    if context_query.count("RtlCopyMemory(escape->pPrivateDriverData,&response,sizeof(response));") != 1:
        fail("GET_CONTEXT_INFO must publish one immutable local response snapshot")

    create = canonical_code(function_body("VioGpuWddmCreateContext", WDDM_DDI_CODE))
    create_requirements = (
        "KeGetCurrentIrql()!=PASSIVE_LEVEL",
        "createContext->EngineAffinity!=1",
        "constULONGcontextClassFlags=createContext->Flags.Value&~4;",
        "if(!createContext->Flags.SystemContext&&!createContext->Flags.GdiContext&&contextClassFlags==0){contextType=VioGpuWddmContextNative;}",
        "elseif(createContext->Flags.SystemContext&&!createContext->Flags.GdiContext&&contextClassFlags==1)",
        "contextType=VioGpuWddmContextSystem;",
        "elseif(createContext->Flags.GdiContext&&!createContext->Flags.SystemContext&&contextClassFlags==2)",
        "contextType=VioGpuWddmContextGdi;",
        "if(contextType==VioGpuWddmContextNative)",
        "createContext->pPrivateDriverData==NULL",
        "createContext->PrivateDriverDataSize!=sizeof(VIOGPU_WDDM_CONTEXT_CREATE)",
        "RtlCopyMemory(&privateData,createContext->pPrivateDriverData,sizeof(privateData));",
        "!IsCurrentAbiHeader(&privateData.Header,sizeof(privateData))",
        "privateData.ExpectedResetGeneration==0",
        "privateData.Flags!=VIOGPU_WDDM_CONTEXT_FLAGS_NONE",
        "privateData.Reserved!=0",
        "elseif(createContext->pPrivateDriverData!=NULL||createContext->PrivateDriverDataSize!=0)",
        "context->RuntimeContext=NULL;",
        "context->Type=contextType;",
        "context->NativeContext.State=contextType==VioGpuWddmContextNative?VioGpuNativeContextAllocated:VioGpuNativeContextDead;",
        "device->Adapter->CreateNativeContext(&context->NativeContext,privateData.ExpectedResetGeneration)",
        "contextType==VioGpuWddmContextGdi?256:VIOGPU_WDDM_ALLOCATION_LIST_SIZE",
        "contextType==VioGpuWddmContextGdi?256:VIOGPU_WDDM_PATCH_LIST_SIZE",
    )
    for fragment in create_requirements:
        if create.count(fragment) != 1:
            fail(f"CreateContext must enforce exact native, system, and GDI context contracts: {fragment}")
    if "ProbeForRead(" in create:
        fail("CreateContext must snapshot dxgkrnl-owned private data without probing it as a user address")
    create_snapshot = create.find("RtlCopyMemory(&privateData,createContext->pPrivateDriverData,sizeof(privateData));")
    create_validation = create.find("!IsCurrentAbiHeader(&privateData.Header,sizeof(privateData))")
    if create_snapshot < 0 or create_validation < 0 or create_snapshot > create_validation:
        fail("CreateContext must snapshot its private data before validating the current pre-v1 contract")

    allocation_header = canonical_code(WDDM_DDI_HEADER_CODE)
    if allocation_header.count("VIOGPU_WDDM_ALLOCATION_INFOPrivateData;") != 1:
        fail("each KMD allocation must retain one exact private-data snapshot")
    if allocation_header.count("SIZE_TBackingSize;") != 1 or "SIZE_TSize;" in allocation_header:
        fail("each KMD allocation must distinguish its page-aligned backing from the logical private-data size")
    open_wrapper_matches = re.findall(
        r"\bstruct\s+VIOGPU_WDDM_OPEN_ALLOCATION\s*\{(.*?)\}\s*;",
        WDDM_DDI_HEADER_CODE,
        re.DOTALL,
    )
    open_wrapper = canonical_code(open_wrapper_matches[0]) if len(open_wrapper_matches) == 1 else ""
    for required in (
        "VIOGPU_WDDM_DEVICE*Device;",
        "BOOLEANReadOnly;",
    ):
        if open_wrapper.count(required) != 1:
            fail(f"each open allocation must retain its owning device and access contract: {required}")

    create_allocation = canonical_code(function_body("VioGpuWddmCreateAllocation", WDDM_DDI_CODE))
    create_resource_guard = (
        "createAllocation->pPrivateDriverData!=NULL||createAllocation->PrivateDriverDataSize!=0"
    )
    if create_allocation.count(create_resource_guard) != 1:
        fail("CreateAllocation must reject resource-private data in the current pre-v1 contract")
    create_private_copy_guard = (
        "__try{RtlCopyMemory(&privateData,allocationInfo->pPrivateDriverData,sizeof(privateData));}"
        "__except(EXCEPTION_EXECUTE_HANDLER){status=STATUS_INVALID_USER_BUFFER;break;}"
    )
    if create_allocation.count(create_private_copy_guard) != 1:
        fail("CreateAllocation must convert an invalid UMD private-data pointer into STATUS_INVALID_USER_BUFFER")
    create_allocation_sequence = (
        "allocationInfo->PrivateDriverDataSize!=sizeof(VIOGPU_WDDM_ALLOCATION_INFO)",
        "VIOGPU_WDDM_ALLOCATION_INFOprivateData={};",
        "RtlCopyMemory(&privateData,allocationInfo->pPrivateDriverData,sizeof(privateData));",
        "status=ValidateAllocationPrivate(&privateData,&alignedSize);",
        "allocation->PrivateData=privateData;",
        "allocation->BackingSize=alignedSize;",
        "InitializeAllocationInfo(allocationInfo,allocation,alignedSize);",
    )
    create_allocation_offsets = [create_allocation.find(fragment) for fragment in create_allocation_sequence]
    if min(create_allocation_offsets) < 0 or create_allocation_offsets != sorted(create_allocation_offsets):
        fail("CreateAllocation must validate and retain the exact UMD private-data snapshot before publication")
    for fragment in create_allocation_sequence:
        if create_allocation.count(fragment) != 1:
            fail(f"CreateAllocation private-data identity must be unique: {fragment}")

    open_allocation = canonical_code(function_body("VioGpuWddmOpenAllocation", WDDM_DDI_CODE))
    open_guard = (
        "openAllocation->SubresourceIndex!=0||(openAllocation->Flags.Value&~3)!=0||"
        "openAllocation->pPrivateDriverData!=NULL||openAllocation->PrivateDriverSize!=0"
    )
    if open_allocation.count(open_guard) != 1:
        fail("OpenAllocation must reject unknown flags and resource-private data in the current pre-v1 contract")
    open_private_copy_guard = (
        "__try{RtlCopyMemory(&privateData,openInfo->pPrivateDriverData,sizeof(privateData));}"
        "__except(EXCEPTION_EXECUTE_HANDLER){status=STATUS_INVALID_USER_BUFFER;break;}"
    )
    if open_allocation.count(open_private_copy_guard) != 1:
        fail("OpenAllocation must convert an invalid UMD private-data pointer into STATUS_INVALID_USER_BUFFER")
    open_allocation_sequence = (
        "openInfo->PrivateDriverDataSize!=sizeof(VIOGPU_WDDM_ALLOCATION_INFO)",
        "VIOGPU_WDDM_ALLOCATION_INFOprivateData={};",
        "RtlCopyMemory(&privateData,openInfo->pPrivateDriverData,sizeof(privateData));",
        "allocation=static_cast<VIOGPU_WDDM_ALLOCATION*>(dxgkInterface->DxgkCbGetHandleData(&getHandleData));",
        "RtlCompareMemory(&privateData,&allocation->PrivateData,sizeof(privateData))!=sizeof(privateData)",
        "if(!ReferenceDevice(device))",
        "deviceAllocation->Device=device;",
        "deviceAllocation->ReadOnly=openAllocation->Flags.ReadOnly;",
        "openInfo->hDeviceSpecificAllocation=deviceAllocation;",
    )
    open_allocation_offsets = [open_allocation.find(fragment) for fragment in open_allocation_sequence]
    if min(open_allocation_offsets) < 0 or open_allocation_offsets != sorted(open_allocation_offsets):
        fail("OpenAllocation must resolve the KMD object and compare its exact private-data snapshot before publication")
    for fragment in open_allocation_sequence:
        if open_allocation.count(fragment) != 1:
            fail(f"OpenAllocation private-data identity must be unique: {fragment}")
    if re.search(r"(?:openAllocation|mutableOpenAllocation)->(?:SubresourceOffset|Pitch)=", open_allocation):
        fail("OpenAllocation must not publish conditional GDI aperture outputs for the current segment")
    rollback_sequence = (
        "deviceAllocation->Signature=0;",
        "DereferenceDevice(deviceAllocation->Device);",
        "deletedeviceAllocation;",
        "openAllocation->pOpenAllocation[index].hDeviceSpecificAllocation=NULL;",
    )
    rollback_start = open_allocation.find("if(!NT_SUCCESS(status))")
    rollback = open_allocation[rollback_start:] if rollback_start >= 0 else ""
    rollback_offsets = [rollback.find(fragment) for fragment in rollback_sequence]
    if min(rollback_offsets) < 0 or rollback_offsets != sorted(rollback_offsets):
        fail("OpenAllocation rollback must release every published wrapper and its device reference symmetrically")
    if open_allocation.count("ReferenceDevice(device)") != 1 or open_allocation.count(
        "DereferenceDevice(deviceAllocation->Device);"
    ) != 1:
        fail("OpenAllocation must hold exactly one device reference per published wrapper")

    close_body = function_body("VioGpuWddmCloseAllocation", WDDM_DDI_CODE)
    close_allocation = canonical_code(close_body)
    close_owner_guard = (
        "deviceAllocation==NULL||deviceAllocation->Signature!=VIOGPU_WDDM_OPEN_ALLOCATION_SIGNATURE||"
        "deviceAllocation->Device!=device"
    )
    close_duplicate_guard = (
        "closeAllocation->pOpenHandleList[previousIndex]==closeAllocation->pOpenHandleList[index]"
    )
    close_validate = close_allocation.find(close_owner_guard)
    close_duplicate = close_allocation.find(close_duplicate_guard)
    close_validation_loop_end = close_allocation.find(
        "for(UINTindex=0;index<closeAllocation->NumAllocations;++index)",
        close_duplicate + len(close_duplicate_guard),
    )
    close_invalidate = close_allocation.find("deviceAllocation->Signature=0;", close_validation_loop_end)
    close_release = close_allocation.find("DereferenceDevice(deviceAllocation->Device);", close_invalidate)
    close_delete = close_allocation.find("deletedeviceAllocation;", close_release)
    if min(close_validate, close_duplicate, close_validation_loop_end, close_invalidate, close_release, close_delete) < 0 or not (
        close_validate < close_duplicate < close_validation_loop_end < close_invalidate < close_release < close_delete
    ):
        fail("CloseAllocation must validate ownership and uniqueness before releasing wrappers and device references")
    if close_allocation.count(close_duplicate_guard) != 1 or close_allocation.count(
        "DereferenceDevice(deviceAllocation->Device);"
    ) != 1:
        fail("CloseAllocation must reject duplicate handles and release each wrapper reference exactly once")
    if close_allocation.count(
        "deviceAllocation->Device!=device||!IsOwnedAllocation(deviceAllocation->Allocation,device->Adapter)"
    ) != 1:
        fail("CloseAllocation must validate the wrapped allocation and resource ownership before releasing it")

    destroy_allocation = canonical_code(function_body("VioGpuWddmDestroyAllocation", WDDM_DDI_CODE))
    if destroy_allocation.count("!IsOwnedAllocation(allocation,adapter)") != 1:
        fail("DestroyAllocation must validate allocation resource ownership before using the batch")
    detach_allocation = canonical_code(function_body("DetachAllocationNativeContext", WDDM_DDI_CODE))
    if (
        destroy_allocation.count("DetachAllocationNativeContext(allocation)") != 1
        or "UnregisterNativeAllocationRange(allocation)" in destroy_allocation
        or "UnregisterNativeAllocationRange(allocation)" in detach_allocation
        or "DereferenceNativeContextAllocation" in detach_allocation
    ):
        fail("DestroyAllocation must route each native owner detach through one atomic retry-safe helper")
    destroy_duplicate_guard = (
        "destroyAllocation->pAllocationList[previousIndex]==destroyAllocation->pAllocationList[index]"
    )
    destroy_duplicate = destroy_allocation.find(destroy_duplicate_guard)
    destroy_validation_loop_end = destroy_allocation.find(
        "VIOGPU_WDDM_RESOURCE*resource=NULL;",
        destroy_duplicate + len(destroy_duplicate_guard),
    )
    destroy_delete = destroy_allocation.find("deleteallocation;", destroy_validation_loop_end)
    if (
        destroy_allocation.count(destroy_duplicate_guard) != 1
        or min(destroy_duplicate, destroy_validation_loop_end, destroy_delete) < 0
        or not destroy_duplicate < destroy_validation_loop_end < destroy_delete
    ):
        fail("DestroyAllocation must reject duplicate handles before deleting any allocation")

    validate = canonical_code(function_body("ValidateCommandHeader", WDDM_DDI_CODE))
    validate_requirements = (
        "header->ExpectedResetGeneration!=resetGeneration",
        "header->AllocationReferenceCount==0",
        "header->AllocationReferenceCount!=patchListSize",
        "header->AllocationReferencesOffset!=sizeof(*header)",
        "header->CommandStreamOffset!=referencesEnd",
        "header->CommandStreamSize<sizeof(ULONGLONG)",
        "reference->AllocationIndex>=allocationListSize",
        "reference->AllocationIndex!=patch->AllocationIndex",
        "reference->AllocationOffset+reference->Length<reference->AllocationOffset",
        "reference->AllocationOffset!=patch->AllocationOffset",
        "reference->PatchOffset!=patch->PatchOffset-header->CommandStreamOffset",
        "(reference->PatchOffset&(sizeof(ULONG)-1))!=0",
        "patch->Reserved!=0",
        "deviceAllocation->Device!=device",
        "allocation->NativeContext==nativeContext->Registration",
        "allocation->ContextGeneration==nativeContext->Generation",
        "allocation->ContextResetGeneration==nativeContext->ResetGeneration",
        "allocation->ContextId==nativeContext->ContextId",
        "reference->AllocationOffset>deviceAllocation->Allocation->PrivateData.Size",
        "reference->Length>deviceAllocation->Allocation->PrivateData.Size-reference->AllocationOffset",
        "allocationEntry->Reserved!=0",
        "deviceAllocation->ReadOnly&&(reference->Flags&VIOGPU_WDDM_REFERENCE_WRITE)!=0",
    )
    for fragment in validate_requirements:
        if validate.count(fragment) != 1:
            fail(f"Render private ABI validation is missing its bounded identity check: {fragment}")
    for forbidden in (
        "HostState",
        "PlacementValid",
        "ResourceId",
        "BlobId",
        "BoundGeneration",
        "BoundResetGeneration",
        "BoundContextId",
        "SegmentId",
        "PhysicalAddress",
    ):
        if forbidden in validate:
            fail(f"Render must not require pre-Patch residency or placement state: {forbidden}")
    if "patch->Value" in validate:
        fail("Render must accept UMD-selected SlotId values while rejecting the reserved patch bits")
    if validate.find("header->CommandStreamSize<sizeof(ULONGLONG)") > validate.find(
        "header->CommandStreamSize-sizeof(ULONGLONG)"
    ):
        fail("Render must prove the command stream can hold a patch before subtracting patch width")

    overlap_condition = (
        "patch->PatchOffset<previousPatch->PatchOffset+sizeof(ULONGLONG)&&"
        "previousPatch->PatchOffset<patch->PatchOffset+sizeof(ULONGLONG)"
    )
    overlap_blocks = [
        canonical_code(body)
        for condition, body, _, _ in if_blocks(function_body("ValidateCommandHeader", WDDM_DDI_CODE))
        if canonical_code(condition) == overlap_condition
    ]
    if validate.count("for(UINTpreviousIndex=0;previousIndex<index;++previousIndex)") != 1 or overlap_blocks != [
        "returnSTATUS_INVALID_PARAMETER;"
    ]:
        fail("Render must reject duplicate or overlapping 8-byte patch slots")

    render_body = function_body("VioGpuWddmRender", WDDM_DDI_CODE)
    render = canonical_code(render_body)
    render_requirements = (
        "render->CommandLength>VIOGPU_WDDM_DMA_BUFFER_SIZE",
        "BYTE*commandSnapshot=NULL;",
        "D3DDDI_PATCHLOCATIONLIST*patchSnapshot=NULL;",
        "commandSnapshot=new(NonPagedPoolNx)BYTE[render->CommandLength];",
        "patchSnapshot=new(NonPagedPoolNx)D3DDDI_PATCHLOCATIONLIST[render->PatchLocationListInSize];",
    )
    for fragment in render_requirements:
        if render.count(fragment) != 1:
            fail(f"Render must bound and allocate its nonpaged input snapshots exactly once: {fragment}")

    probe_command = render.find("ProbeForRead(const_cast<PVOID>(render->pCommand),render->CommandLength,1);")
    copy_command = render.find("RtlCopyMemory(commandSnapshot,render->pCommand,render->CommandLength);")
    probe_patch = render.find("ProbeForRead(render->pPatchLocationListIn,patchBytes,__alignof(D3DDDI_PATCHLOCATIONLIST));")
    copy_patch = render.find("RtlCopyMemory(patchSnapshot,render->pPatchLocationListIn,patchBytes);")
    snapshot_command = render.find(
        "constVIOGPU_WDDM_RENDER_COMMAND*command="
        "reinterpret_cast<constVIOGPU_WDDM_RENDER_COMMAND*>(commandSnapshot);"
    )
    validate_call = render.find(
        "status=ValidateCommandHeader(command,render->CommandLength,context->Device,render->pAllocationList,"
        "render->AllocationListSize,patchSnapshot,render->PatchLocationListInSize,&snapshot);"
    )
    snapshot_sequence = (probe_command, copy_command, probe_patch, copy_patch, snapshot_command, validate_call)
    if min(snapshot_sequence) < 0 or list(snapshot_sequence) != sorted(snapshot_sequence):
        fail("Render must snapshot command and patch inputs before validating their shared identity")
    for fragment in (
        "RtlCopyMemory(commandSnapshot,render->pCommand,render->CommandLength);",
        "RtlCopyMemory(patchSnapshot,render->pPatchLocationListIn,patchBytes);",
    ):
        if render.count(fragment) != 1:
            fail(f"Render input snapshot must be unique: {fragment}")

    generation_check = render.find(
        "if(NT_SUCCESS(status)&&"
        "!snapshot.Adapter->IsNativeContextGenerationCurrent(snapshot.Generation,snapshot.ResetGeneration))"
    )
    if generation_check < 0 or generation_check < validate_call:
        fail("Render must revalidate its reset generation after private ABI validation")

    publication_blocks = [
        (canonical_code(condition), canonical_code(body), start)
        for condition, body, start, _ in if_blocks(render_body)
        if "RtlCopyMemory(dmaBuffer,commandSnapshot,render->CommandLength);" in canonical_code(body)
    ]
    if len(publication_blocks) != 1 or publication_blocks[0][0] != "NT_SUCCESS(status)":
        fail("Render must publish all outputs through one success-only block")
    publication, publication_start = publication_blocks[0][1:]
    publication_sequence = (
        "PVOIDdmaBuffer=render->pDmaBuffer;",
        "D3DDDI_PATCHLOCATIONLIST*patchOutput=render->pPatchLocationListOut;",
        "RtlCopyMemory(dmaBuffer,commandSnapshot,render->CommandLength);",
        "RtlCopyMemory(patchOutput,patchSnapshot,patchBytes);",
        "RtlZeroMemory(privateData,sizeof(*privateData));",
        "privateData->Signature=VIOGPU_WDDM_DMA_SIGNATURE;",
        "privateData->DmaBuffer=dmaBuffer;",
        "privateData->ResetGeneration=snapshot.ResetGeneration;",
        "render->pDmaBuffer=static_cast<BYTE*>(dmaBuffer)+render->CommandLength;",
        "render->pPatchLocationListOut=patchOutput+render->PatchLocationListInSize;",
        "render->MultipassOffset=render->CommandLength;",
    )
    publication_offsets = [publication.find(fragment) for fragment in publication_sequence]
    if min(publication_offsets) < 0 or publication_offsets != sorted(publication_offsets):
        fail("Render success publication must keep DMA, patch, metadata, and cursor updates ordered")
    for fragment in publication_sequence:
        if render.count(fragment) != 1:
            fail(f"Render output publication must be unique and success-only: {fragment}")
    publication_absolute = len(canonical_code(render_body[:publication_start]))
    if publication_absolute <= generation_check:
        fail("Render must complete the final reset-generation check before publishing outputs")
    if any(
        fragment in render
        for fragment in (
            "RtlCopyMemory(render->pDmaBuffer,render->pCommand",
            "RtlCopyMemory(render->pPatchLocationListOut,render->pPatchLocationListIn",
        )
    ):
        fail("Render must never publish directly from mutable UMD input")
    if render.count("snapshot.ResetGeneration") != 2 or render.count(
        "privateData->ResetGeneration=snapshot.ResetGeneration;"
    ) != 1:
        fail("Render must validate and retain the exact context reset generation")


def check_wddm_paging_transaction_gate() -> None:
    """Native paging is reachable only through an owned, cancellable transaction."""
    namespace_end = WDDM_DDI_SOURCE.find("} // namespace")
    cancel_forward = WDDM_DDI_SOURCE.find("VOID NativePagingBatchCancelled(_In_ PVOID callbackContext);")
    cancel_definition = WDDM_DDI_SOURCE.find("_Use_decl_annotations_ VOID NativePagingBatchCancelled(PVOID callbackContext)")
    if min(namespace_end, cancel_forward, cancel_definition) < 0 or not namespace_end < cancel_forward < cancel_definition:
        fail("paging cancel callback declaration and definition must share global linkage")
    create = canonical_code(function_body("VioGpuWddmCreateAllocation", WDDM_DDI_CODE))
    gate = "if((privateData.Flags&VIOGPU_WDDM_ALLOCATION_NATIVE)!=0){status=STATUS_GRAPHICS_DRIVER_MISMATCH;break;}"
    if gate in create:
        fail("CreateAllocation must not retain the old native paging gate after transaction ownership exists")
    if "AcquireNativeContextSnapshotForAllocation(" not in create:
        fail("CreateAllocation must retain native context ownership after removing the paging gate")
    paging_header = canonical_code(WDDM_DDI_HEADER_CODE)
    for fragment in (
        "VIOGPU_WDDM_PAGING_TRANSACTION_STATE",
        "VIOGPU_WDDM_PAGING_TRANSACTION",
        "VIOGPU_WDDM_PAGING_PRIVATE",
        "VioGpuWddmPagingTransactionBuilt",
        "VioGpuWddmPagingTransactionQueued",
        "VioGpuWddmPagingTransactionExecuting",
        "volatileLONGReferenceHeld;",
        "volatileLONGExecutionStarted;",
        "volatileLONGCancelRequested;",
    ):
        if fragment not in paging_header:
            fail(f"paging transaction ABI must expose {fragment}")
    build_dispatch = canonical_code(function_body("VioGpuWddmBuildPagingBuffer", WDDM_DDI_CODE))
    if "returnBuildSoftwarePagingTransaction(adapter," not in build_dispatch:
        fail("BuildPagingBuffer must route standard and native software records through one ownership helper")
    build = canonical_code(function_body("BuildSoftwarePagingTransaction", WDDM_DDI_CODE))
    if build.count("AcquireAllocationSubmissionReference(allocation,adapter)") != 1:
        fail("native BuildPagingBuffer must acquire one allocation submission reference before publication")
    if "InterlockedExchange(&transaction->State,VioGpuWddmPagingTransactionBuilt);" not in build:
        fail("BuildPagingBuffer must publish the Built transaction state only after initialization")
    build_state_sequence = (
        build.find("AcquireAllocationSubmissionReference(allocation,adapter)"),
        build.find("InterlockedExchange(&transaction->ReferenceHeld,1);"),
        build.find("InterlockedExchange(&transaction->State,VioGpuWddmPagingTransactionBuilt);"),
    )
    if min(build_state_sequence) < 0 or list(build_state_sequence) != sorted(build_state_sequence):
        fail("BuildPagingBuffer must acquire and initialize ownership before publishing Built")
    if "NativePagingBatchWorker" not in build:
        fail("BuildPagingBuffer must initialize the passive paging worker owner")
    build_copy = build.find("CopyAperturePlacement(allocation,")
    build_publish = build.find("InterlockedExchange(&transaction->State,VioGpuWddmPagingTransactionBuilt);")
    if min(build_copy, build_publish) < 0 or build_copy > build_publish:
        fail("BuildPagingBuffer must consume the transfer MDL before publishing Built")
    if "ResolveTransferMdlAddress(" in canonical_code(function_body("ExecutePagingTransaction", WDDM_DDI_CODE)):
        fail("passive paging execution must not dereference a BuildPagingBuffer-owned MDL")
    cancel = canonical_code(function_body("VioGpuWddmCancelCommand", WDDM_DDI_CODE))
    for fragment in (
        "CancelRecognizedPagingTransaction(pagingPrivate,adapter)",
        "adapter->CancelNativePassiveWork(&firstPrivate->Work)",
        "state==VioGpuWddmSubmissionPrepared||state==VioGpuWddmSubmissionPatched",
        "QuarantineSubmission(submission,state,TRUE)",
        "returnSTATUS_SUCCESS;",
    ):
        if fragment not in cancel:
            fail(f"CancelCommand must retain safe cleanup and success semantics: {fragment}")
    paging_cancel_end = cancel.find("elseif(cancelCommand->hContext!=NULL")
    if paging_cancel_end < 0:
        fail("CancelCommand must separate paging cleanup from client-context cleanup")
    paging_cancel = cancel[:paging_cancel_end]
    if paging_cancel.count("adapter->CompleteNativeSystemSubmission(") != 2 or \
       paging_cancel.count("adapter->RequestHardwareResetAtAnyIrql();") != 2 or \
       "NotifyNativeSubmissionFault(" in paging_cancel:
        fail("removed paging work must retire its system fence and request reset without DMA_FAULTED")
    submit = canonical_code(function_body("VioGpuWddmSubmitCommand", WDDM_DDI_CODE))
    for fragment in (
        "ResolvePagingBatch(",
        "ValidatePagingTransactionReference(&pagingPrivate->Transaction,adapter)",
        "InterlockedCompareExchange(&pagingPrivate->Transaction.State,VioGpuWddmPagingTransactionQueued,VioGpuWddmPagingTransactionBuilt)",
        "adapter->QueueNativePassiveWork(&firstPrivate->Work,submitCommand->SubmissionFenceId)",
        "CancelRecognizedPagingTransaction(pagingPrivate,adapter)",
    ):
        if fragment not in submit:
            fail(f"SubmitCommand must transfer paging ownership to a passive worker: {fragment}")
    paging_submit_start = submit.find("if(pagingSubmission)", submit.find("VIOGPU_WDDM_KMD_DMA_PRIVATE*privateData="))
    paging_submit_end = submit.find("elseif(NT_SUCCESS(status)&&privateData->Kind==VioGpuWddmDmaKindPresent)", paging_submit_start)
    if min(paging_submit_start, paging_submit_end) < 0 or "NotifyNativeSubmissionFault(" in submit[paging_submit_start:paging_submit_end]:
        fail("nonempty paging SubmitCommand must never fault a scheduler system command")
    require_order(
        submit,
        (
            "ValidatePagingTransactionReference(&pagingPrivate->Transaction,adapter)",
            "InterlockedCompareExchange(&pagingPrivate->Transaction.State,VioGpuWddmPagingTransactionQueued,"
            "VioGpuWddmPagingTransactionBuilt)",
            "adapter->QueueNativePassiveWork(&firstPrivate->Work,submitCommand->SubmissionFenceId)",
        ),
        "SubmitCommand must validate every Build-owned paging allocation before publishing the batch to the passive FIFO",
    )
    validate_reference = canonical_code(
        function_body_with_parameters(
            "ValidatePagingTransactionReference",
            "_Inout_ VIOGPU_WDDM_PAGING_TRANSACTION *transaction, _In_ VioGpuDod *adapter",
            WDDM_DDI_CODE,
        )
    )
    for fragment in (
        "transaction->Signature!=VIOGPU_WDDM_PAGING_TRANSACTION_SIGNATURE",
        "transaction->Adapter!=adapter",
        "transaction->Allocation==NULL",
        "InterlockedCompareExchange(&transaction->State,0,0)!=VioGpuWddmPagingTransactionBuilt",
        "InterlockedCompareExchange(&transaction->ReferenceHeld,0,0)!=1",
    ):
        if fragment not in validate_reference:
            fail(f"Submit paging reference validation must retain the Built owner gate: {fragment}")
    driver_caps = canonical_code(function_body("VioGpuWddmQueryAdapterInfo", WDDM_DDI_CODE))
    if driver_caps.count("driverCaps->SchedulingCaps.CancelCommandAware=0;") != 1:
        fail("the Win7 registration contract must not advertise the unregistered Win8 CancelCommand callback")

    finish = canonical_code(
        function_body_with_parameters(
            "FinishPagingTransaction",
            "_Inout_ VIOGPU_WDDM_PAGING_TRANSACTION *transaction, "
            "_In_ VIOGPU_WDDM_PAGING_TRANSACTION_STATE expectedState, "
            "_In_ VIOGPU_WDDM_PAGING_TRANSACTION_STATE finalState",
            WDDM_DDI_CODE,
        )
    )
    if "ReleasePagingTransactionReference(" in finish:
        fail("an executed paging record must retain its allocation reference until worker terminal handling")
    release = canonical_code(function_body("ReleasePagingTransactionReference", WDDM_DDI_CODE))
    if "InterlockedCompareExchange(&transaction->ReferenceHeld,0,1)==1" not in release:
        fail("paging terminal paths must converge on one allocation-reference release")
    cancel_transaction = canonical_code(function_body("CancelPagingTransaction", WDDM_DDI_CODE))
    for fragment in (
        "state==VioGpuWddmPagingTransactionExecuting",
        "InterlockedExchange(&transaction->CancelRequested,1);",
        "state!=VioGpuWddmPagingTransactionBuilt&&state!=VioGpuWddmPagingTransactionQueued",
        "ReleasePagingTransactionReference(transaction);",
    ):
        if fragment not in cancel_transaction:
            fail(f"paging cancellation must cover built, queued, and executing ownership: {fragment}")

    recognized_owner = canonical_code(
        function_body_with_parameters(
            "IsRecognizedPagingOwner",
            "_In_ const VIOGPU_WDDM_PAGING_PRIVATE *pagingPrivate, _In_ VioGpuDod *adapter",
            WDDM_DDI_CODE,
        )
    )
    for fragment in (
        "pagingPrivate->Header.Signature==VIOGPU_WDDM_DMA_SIGNATURE",
        "pagingPrivate->Header.Version==VioGpuWddmDmaPrivateVersion",
        "pagingPrivate->Header.Kind==VioGpuWddmDmaKindPaging",
        "pagingPrivate->Header.Submission==pagingPrivate",
        "pagingPrivate->Transaction.Signature==VIOGPU_WDDM_PAGING_TRANSACTION_SIGNATURE",
        "pagingPrivate->Transaction.Adapter==adapter",
        "pagingPrivate->Work.Routine==NativePagingBatchWorker",
        "pagingPrivate->Work.Context==pagingPrivate",
    ):
        if fragment not in recognized_owner:
            fail(f"malformed paging cleanup must prove KMD owner identity before cancellation: {fragment}")
    cancel_recognized = canonical_code(
        function_body_with_parameters(
            "CancelRecognizedPagingTransaction",
            "_Inout_ VIOGPU_WDDM_PAGING_PRIVATE *pagingPrivate, _In_ VioGpuDod *adapter",
            WDDM_DDI_CODE,
        )
    )
    if (
        "IsRecognizedPagingOwner(pagingPrivate,adapter)" not in cancel_recognized
        or "CancelPagingTransaction(&pagingPrivate->Transaction)" not in cancel_recognized
    ):
        fail("recognized paging cleanup must gate the terminal reference release on owner identity")

    batch_worker = canonical_code(function_body("NativePagingBatchWorker", WDDM_DDI_CODE))
    for fragment in (
        "VioGpuWddmPagingTransactionExecuting,VioGpuWddmPagingTransactionQueued",
        "InterlockedExchange(&pagingPrivate->Transaction.ExecutionStarted,1);",
        "VioGpuWddmPagingTransactionExecuting,VioGpuWddmPagingTransactionCancelled",
        "VioGpuWddmPagingTransactionExecuting,VioGpuWddmPagingTransactionFinished",
        "ReleasePagingTransactionReference(&pagingPrivate->Transaction);",
        "adapter->CompleteNativePassiveWork(&first->Work);",
        "adapter->ReleaseNativeSubmissionOperation();",
    ):
        if fragment not in batch_worker:
            fail(f"paging worker must retain claim and terminal ownership: {fragment}")
    if batch_worker.count(
        "InterlockedCompareExchange(&pagingPrivate->Transaction.CancelRequested,0,0)!=0"
    ) != 2:
        fail("paging worker must observe cancellation both before and after passive execution")
    worker_claim = batch_worker.find(
        "VioGpuWddmPagingTransactionExecuting,VioGpuWddmPagingTransactionQueued"
    )
    worker_execute = batch_worker.find("status=ExecutePagingTransaction(&pagingPrivate->Transaction);")
    worker_release = batch_worker.find("ReleasePagingTransactionReference(&pagingPrivate->Transaction);")
    if min(worker_claim, worker_execute, worker_release) < 0 or not worker_claim < worker_execute < worker_release:
        fail("paging worker must claim before execution and retain references through terminal cleanup")
    resolve = canonical_code(
        function_body_with_parameters(
            "ResolvePagingBatch",
            "_In_opt_ PVOID dmaBuffer, _In_ UINT dmaBufferSize, _In_ UINT dmaStart, _In_ UINT dmaEnd, "
            "_In_ PVOID privateBuffer, _In_ UINT privateBufferSize, _In_ UINT privateStart, "
            "_In_ UINT privateEnd, _In_ VioGpuDod *adapter, "
            "_In_ VIOGPU_WDDM_PAGING_TRANSACTION_STATE expectedState, "
            "_Out_ VIOGPU_WDDM_PAGING_PRIVATE **firstPrivate, _Out_ UINT *recordCount",
            WDDM_DDI_CODE,
        )
    )
    resolve_offset = canonical_code(
        function_body_with_parameters(
            "ResolvePagingBatchOffset",
            "_In_ UINT base, _In_ UINT index, _In_ SIZE_T stride, _In_ UINT limit, _Out_ UINT *offset",
            WDDM_DDI_CODE,
        )
    )
    if resolve.count("ResolvePagingBatchOffset(") != 2 or "ULONGLONGcandidate=" not in resolve_offset:
        fail("paging batch offsets must use checked wide arithmetic for DMA and private records")
    count_publish = resolve.find("*recordCount=count;")
    deep_validation = resolve.find("for(UINTindex=0;index<count;++index)")
    if resolve.count("*recordCount=count;") != 1 or min(count_publish, deep_validation) < 0 or count_publish > deep_validation:
        fail("paging batch shape must publish its unwind count before per-record validation")
    if "expectedState==VioGpuWddmPagingTransactionAny" not in resolve:
        fail("CancelCommand must be able to resolve a batch across ownership-state races")
    if "dmaBuffer==NULL?static_cast<VIOGPU_WDDM_PAGING_DMA_PACKET*>(header->Packet)" not in resolve:
        fail("SubmitCommand paging resolution must use private packet ownership without a CPU DMA pointer")
    if re.search(r"submitCommand->pDmaBuffer(?!PrivateData)", submit):
        fail("SubmitCommand must not read a nonexistent pDmaBuffer member")
    if "ResolvePagingBatch(NULL,submitCommand->DmaBufferSize" not in submit:
        fail("SubmitCommand paging resolution must use its private packet pointer")
    if "VioGpuWddmPagingTransactionAny" not in cancel:
        fail("CancelCommand must resolve both Built and Queued paging records")
    if "ownership!=VioGpuNativePassiveOwnershipWorkerOwned" in cancel:
        fail("worker-owned paging cancellation must still publish transaction cancel state")
    if cancel.count("CancelRecognizedPagingTransaction(pagingPrivate,adapter)") != 2:
        fail("CancelCommand must cancel recognized records in both exact and fallback paging resolution")
    exact_work_cancel = cancel.find("adapter->CancelNativePassiveWork(&firstPrivate->Work)")
    exact_transaction_cancel = cancel.find(
        "CancelRecognizedPagingTransaction(pagingPrivate,adapter)", exact_work_cancel
    )
    exact_removed = cancel.find("if(ownership==VioGpuNativePassiveWorkRemoved)", exact_transaction_cancel)
    fallback_owner = cancel.find("IsRecognizedPagingOwner(fallbackFirstPrivate,adapter)", exact_removed)
    fallback_work_cancel = cancel.find(
        "adapter->CancelNativePassiveWork(&fallbackFirstPrivate->Work)", fallback_owner
    )
    fallback_transaction_cancel = cancel.find(
        "CancelRecognizedPagingTransaction(pagingPrivate,adapter)", fallback_work_cancel
    )
    fallback_removed = cancel.find("if(ownership==VioGpuNativePassiveWorkRemoved)", fallback_transaction_cancel)
    cancel_stages = (
        exact_work_cancel,
        exact_transaction_cancel,
        exact_removed,
        fallback_owner,
        fallback_work_cancel,
        fallback_transaction_cancel,
        fallback_removed,
    )
    if min(cancel_stages) < 0 or tuple(sorted(cancel_stages)) != cancel_stages:
        fail("paging cancellation must signal every recognized transaction before removed-work notification and release")

    retire_owner = canonical_code(function_body("RetireDmaOwner", WDDM_DDI_CODE))
    retire_resolve = retire_owner.find("ResolvePagingBatch(")
    retire_cancel = retire_owner.find("CancelRecognizedPagingTransaction(pagingPrivate,adapter)", retire_resolve)
    if min(retire_resolve, retire_cancel) < 0 or retire_resolve > retire_cancel or "if(ResolvePagingBatch(" in retire_owner:
        fail("generic paging retirement must unwind recognized records after full batch validation fails")

    passive_header = canonical_code(VIOGPU_HEADER_CODE)
    deferred_fence_states = (
        "enumVIOGPU_NATIVE_FENCE_STATE:LONG{VioGpuNativeFenceFree=0,VioGpuNativeFencePending,"
        "VioGpuNativeFenceSoftwarePending,VioGpuNativeFenceRetired,};"
    )
    if passive_header.count(deferred_fence_states) != 1:
        fail("native fence state must distinguish deferred software completion from Host-pending work")
    for fragment in (
        "volatileLONGState;",
        "volatileLONGRetired;",
        "UINTFenceId;",
        "VIOGPU_NATIVE_PASSIVE_ROUTINECancelRoutine;",
        "volatileLONG*CancelRequested;",
        "VIOGPU_NATIVE_PASSIVE_WORK_OWNERSHIPCancelNativePassiveWork",
    ):
        if fragment not in passive_header:
            fail(f"passive work ownership ABI must expose {fragment}")
    passive_queue = canonical_code(function_body("QueueNativePassiveWork", VIOGPU_CODE))
    software_submission = canonical_code(
        function_body("VioGpuDod::QueueNativeSoftwareSubmissionCompletion", VIOGPU_CODE)
    )
    software_drain = canonical_code(
        function_body("VioGpuDod::DrainNativeSoftwareSubmissionCompletionsFromDpc", VIOGPU_CODE)
    )
    system_submission = canonical_code(function_body("CompleteNativeSystemSubmission", VIOGPU_CODE))
    completed_fence = canonical_code(function_body("NotifyNativeCompletedFence", VIOGPU_CODE))
    passive_cancel = canonical_code(function_body("CancelNativePassiveWork", VIOGPU_CODE))
    passive_worker = canonical_code(function_body("RunNativePassiveWorker", VIOGPU_CODE))
    for fragment in (
        "InterlockedExchange(&work->State,VioGpuNativePassiveWorkQueued);",
        "InterlockedCompareExchange(&work->Retired,0,0)==0",
        "RecordNativeSubmissionFence(fenceId)",
        "work->FenceId=fenceId;",
    ):
        if fragment not in passive_queue:
            fail(f"passive queueing must retain retirement ownership: {fragment}")
    require_order(
        passive_queue,
        (
            "KeAcquireSpinLock(&m_NativePassiveLock,&oldIrql);",
            "BOOLEANneedsWorker=m_NativePassiveActiveWork==NULL&&!m_NativePassiveWorkerQueued;",
            "BOOLEANworkerReference=!needsWorker||ExAcquireRundownProtection(&m_HardwareOperations);",
            "if(workerReference&&RecordNativeSubmissionFence(fenceId))",
            "KeClearEvent(&m_NativePassiveIdleEvent);",
            "work->FenceId=fenceId;",
            "InterlockedExchange(&work->State,VioGpuNativePassiveWorkQueued);",
            "InsertTailList(&m_NativePassiveQueue,&work->Link);",
            "inserted=TRUE;",
            "KeReleaseSpinLock(&m_NativePassiveLock,oldIrql);",
            "if(releaseWorkerReference)",
            "ExReleaseRundownProtection(&m_HardwareOperations);",
            "if(queueWorker)",
            "ExQueueWorkItem(&m_NativePassiveWorkItem,DelayedWorkQueue);",
            "returninserted;",
        ),
        "passive FIFO publication must atomically reserve rundown and fence ownership before linking work",
    )
    if passive_queue.count("RecordNativeSubmissionFence(fenceId)") != 1 or \
       "RecordNativeSubmissionFence(" in software_submission or \
       "RecordNativeSubmissionFence(" in WDDM_DDI_CODE:
        fail("passive work must reserve a hardware fence while deferred software completion owns its tracker insertion")
    require_order(
        software_submission,
        (
            "KeGetCurrentIrql()!=DISPATCH_LEVEL",
            "m_DxgkInterface.DxgkCbQueueDpc==NULL",
            "KeAcquireSpinLock(&m_NativeFenceLock,&oldIrql);",
            "m_NativeFenceCount<VioGpuNativeFenceTrackerCapacity",
            "static_cast<LONG>(fenceId-submitted)>0",
            "m_NativeFences[tail].State=VioGpuNativeFenceSoftwarePending;",
            "InterlockedExchange(&m_NativeSubmittedFence,static_cast<LONG>(fenceId));",
            "KeReleaseSpinLock(&m_NativeFenceLock,oldIrql);",
            "m_DxgkInterface.DxgkCbQueueDpc(m_DxgkInterface.DeviceHandle);",
            "returnvalid;",
        ),
        "software completion must publish a pending fence before requesting the asynchronous Dxgk DPC",
    )
    if any(
        fragment in software_submission
        for fragment in (
            "VioGpuNativeFenceRetired",
            "NotifyNativeCompletedFence(",
            "NotifyNativeSchedulerInterrupt(",
            "DxgkCbSynchronizeExecution(",
            "NotifyNativeSubmissionFault(",
        )
    ):
        fail("SubmitCommand software completion must only publish SoftwarePending state and queue a DPC")
    require_order(
        software_drain,
        (
            "KeGetCurrentIrql()!=DISPATCH_LEVEL",
            "KeAcquireSpinLock(&m_NativeFenceLock,&oldIrql);",
            "m_NativeFences[index].State==VioGpuNativeFenceSoftwarePending",
            "m_NativeFences[index].State=VioGpuNativeFenceRetired;",
            "m_NativeFences[m_NativeFenceHead].State==VioGpuNativeFenceRetired",
            "InterlockedExchange(&m_NativeCompletedFence,static_cast<LONG>(completedFence));",
            "KeReleaseSpinLock(&m_NativeFenceLock,oldIrql);",
            "NotifyNativeCompletedFence(completedFence,0,0,FALSE)",
        ),
        "the Dxgk DPC must retire software fences and publish only the contiguous completed prefix",
    )
    if "DxgkCbQueueDpc(" in software_drain or "NotifyNativeSubmissionFault(" in software_drain:
        fail("software fence DPC drain must neither recursively queue itself nor emit DMA_FAULTED")
    require_order(
        system_submission,
        (
            "RetireNativeSubmissionFence(fenceId,&completedFence)",
            "NotifyNativeCompletedFence(completedFence,nodeOrdinal,engineOrdinal,TRUE)",
        ),
        "queued system completion must retire its pre-recorded fence without using the generic fault path",
    )
    if "NotifyNativeSubmissionFault(" in system_submission or "NotifyNativeSubmissionFault(" in completed_fence:
        fail("system-command fence retirement and completion publication must never emit DMA_FAULTED")
    for fragment in (
        "notify.InterruptType=DXGK_INTERRUPT_DMA_COMPLETED;",
        "notify.DmaCompleted.SubmissionFenceId=completedFence;",
        "NotifyNativeSchedulerInterrupt(&notify,queueDpc)",
        "RequestHardwareResetAtAnyIrql();",
    ):
        if fragment not in completed_fence:
            fail(f"system-safe completion publication must retain: {fragment}")
    for fragment in (
        "InterlockedExchange(&work->Retired,1);",
        "RemoveEntryList(&work->Link);",
        "ownership=VioGpuNativePassiveWorkRemoved;",
        "ownership=VioGpuNativePassiveOwnershipWorkerOwned;",
    ):
        if fragment not in passive_cancel:
            fail(f"passive cancellation must remove queued work or transfer worker ownership: {fragment}")
    if "InterlockedExchange(&work->State,VioGpuNativePassiveWorkWorkerOwned);" not in passive_worker:
        fail("passive worker dequeue must publish worker ownership before callback execution")

    validation = create.find("status=ValidateAllocationPrivate(&privateData,&alignedSize);")
    native_lookup = create.find("AcquireNativeContextSnapshotForAllocation(")
    if min(validation, native_lookup) < 0 or not validation < native_lookup:
        fail("native allocation context ownership must follow private-data validation")
    if "DestroyCreatedAllocations(createAllocation->pAllocationInfo,createdCount);" not in create:
        fail("CreateAllocation must retain batch rollback for earlier allocations")


def check_wddm_guest_allocation_lifecycle() -> None:
    allocation_matches = re.findall(
        r"\bstruct\s+VIOGPU_WDDM_ALLOCATION\s*\{(.*?)\}\s*;",
        WDDM_DDI_HEADER_CODE,
        re.DOTALL,
    )
    allocation_header = canonical_code(allocation_matches[0]) if len(allocation_matches) == 1 else ""
    for field in (
        "VIOGPU_NATIVE_CONTEXT_REGISTRATION*NativeContext;",
        "LONGContextGeneration;",
        "ULONGLONGContextResetGeneration;",
        "UINTContextId;",
        "PMDLApertureMdl;",
        "PVOIDApertureAddress;",
        "VIOGPU_WDDM_ALLOCATION_RANGE*ContextRange;",
    ):
        if allocation_header.count(field) != 1:
            fail(f"native allocation must retain one fixed context/paging identity field: {field}")

    owner_matches = re.findall(
        r"\bstruct\s+VIOGPU_NATIVE_CONTEXT_OWNER\s*\{(.*?)\}\s*;",
        VIOGPU_HEADER_CODE,
        re.DOTALL,
    )
    owner_header = canonical_code(owner_matches[0]) if len(owner_matches) == 1 else ""
    if owner_header.count("volatileLONGAllocationCount;") != 1:
        fail("native context Host ownership must use one volatile atomic allocation count")
    for helper_name, fragments in (
        (
            "TryReferenceNativeAllocationCount",
            (
                "observed<MAXLONG",
                "InterlockedCompareExchange(&owner->AllocationCount,observed+1,observed)",
            ),
        ),
        (
            "ReleaseNativeAllocationCount",
            (
                "observed>0",
                "InterlockedCompareExchange(&owner->AllocationCount,observed-1,observed)",
            ),
        ),
    ):
        helper_parameters = {
            "TryReferenceNativeAllocationCount": "_In_ VIOGPU_NATIVE_CONTEXT_OWNER *owner",
            "ReleaseNativeAllocationCount": "_In_ VIOGPU_NATIVE_CONTEXT_OWNER *owner",
        }[helper_name]
        helper = canonical_code(function_body_with_parameters(helper_name, helper_parameters, VIOGPU_CODE))
        for fragment in fragments:
            if helper.count(fragment) != 1:
                fail(f"{helper_name} must retain its bounded atomic count transition: {fragment}")

    registration_matches = re.findall(
        r"\bstruct\s+VIOGPU_NATIVE_CONTEXT_REGISTRATION\s*\{(.*?)\}\s*;",
        VIOGPU_HEADER_CODE,
        re.DOTALL,
    )
    snapshot_matches = re.findall(
        r"\bstruct\s+VIOGPU_NATIVE_CONTEXT_SNAPSHOT\s*\{(.*?)\}\s*;",
        VIOGPU_HEADER_CODE,
        re.DOTALL,
    )
    registration_header = canonical_code(registration_matches[0]) if len(registration_matches) == 1 else ""
    snapshot_header = canonical_code(snapshot_matches[0]) if len(snapshot_matches) == 1 else ""
    if registration_header.count("ULONGAllocationReferences;") != 1:
        fail("native registration must retain one KMD allocation reference count")
    if snapshot_header.count("VIOGPU_NATIVE_CONTEXT_REGISTRATION*Registration;") != 1:
        fail("protected context snapshots must retain their exact registration identity")
    if registration_header.count("LIST_ENTRYAllocationRanges;") != 1:
        fail("native registration must own the per-context IOVA range registry")

    resource_matches = re.findall(
        r"\bstruct\s+VIOGPU_WDDM_RESOURCE\s*\{(.*?)\}\s*;",
        WDDM_DDI_HEADER_CODE,
        re.DOTALL,
    )
    resource_header = canonical_code(resource_matches[0]) if len(resource_matches) == 1 else ""
    if resource_header.count("volatileLONGAllocationCount;") != 1:
        fail("WDDM resource ownership must use one volatile atomic allocation count")
    if canonical_code(WDDM_DDI_CODE).count("ReadResourceAllocationCount(resource)") < 3:
        fail("WDDM resource allocation count reads must use the atomic snapshot helper")

    validate_allocation = canonical_code(function_body("ValidateAllocationPrivate", WDDM_DDI_CODE))
    for fragment in (
        "localAlignedSize>MAXULONG",
        "privateData->RequestedIova==0",
        "(privateData->RequestedIova&(PAGE_SIZE-1))!=0",
        "privateData->ExpectedResetGeneration==0",
        "privateData->ContextId==0",
        "privateData->RequestedIova>MAXULONGLONG-((ULONGLONG)localAlignedSize-1)",
    ):
        if validate_allocation.count(fragment) != 1:
            fail(f"native allocation validation must reject an unrepresentable IOVA/backing: {fragment}")

    create = canonical_code(function_body("VioGpuWddmCreateAllocation", WDDM_DDI_CODE))
    context_generation = create.find("contextGeneration=snapshot.Generation;")
    create_sequence = (
        create.find("adapter->AcquireNativeContextSnapshotForAllocation("),
        create.find("VioGpuAdapter::ReferenceNativeContextAllocation(&snapshot,&nativeContext)"),
        context_generation,
        create.find("VioGpuAdapter::ReleaseNativeContextSnapshot(&snapshot);", context_generation),
        create.find("nativeResourceId=adapter->AllocateNativeResourceId(privateData.ExpectedResetGeneration);"),
        create.find("allocation->NativeContext=nativeContext;"),
        create.find("allocation->ContextId=contextId;"),
        create.find("InitializeAllocationInfo(allocationInfo,allocation,alignedSize);"),
    )
    if min(create_sequence) < 0 or list(create_sequence) != sorted(create_sequence):
        fail("CreateAllocation must pin its unique context before releasing the lookup snapshot and publishing the allocation")
    if canonical_code(WDDM_DDI_CODE).count("AcquireNativeContextSnapshotForAllocation(") != 1:
        fail("range-based context lookup is allowed only while initially creating an allocation")
    if "privateData.ContextId" not in create:
        fail("native allocation creation must pass its explicit context identity to the range lookup")
    if create.count("RegisterNativeAllocationRange(allocation)") != 1:
        fail("native allocation creation must reserve one non-overlapping context IOVA range")
    allocation_lookup = canonical_code(function_body("VioGpuAdapter::AcquireNativeContextSnapshotForAllocation", VIOGPU_CODE))
    for fragment in ("expectedContextId==0", "owner->ContextId!=expectedContextId"):
        if allocation_lookup.count(fragment) != 1:
            fail(f"range-based allocation lookup must require the caller's context identity: {fragment}")

    acquire_bound = canonical_code(function_body("AcquireAllocationNativeContextSnapshot", WDDM_DDI_CODE))
    for fragment in (
        "VioGpuAdapter::AcquireNativeContextSnapshot(allocation->NativeContext,snapshot)",
        "snapshot->Registration==allocation->NativeContext",
        "snapshot->Generation==allocation->ContextGeneration",
        "snapshot->ResetGeneration==allocation->ContextResetGeneration",
        "snapshot->ContextId==allocation->ContextId",
        "snapshot->ResetGeneration==allocation->PrivateData.ExpectedResetGeneration",
    ):
        if acquire_bound.count(fragment) != 1:
            fail(f"resident allocation lookup must use its immutable context binding: {fragment}")

    reference = canonical_code(function_body("VioGpuAdapter::ReferenceNativeContextAllocation", VIOGPU_CODE))
    for fragment in (
        "snapshot->Owner->Registration==context",
        "context->Generation==snapshot->Generation",
        "context->ResetGeneration==snapshot->ResetGeneration",
        "context->ContextId==snapshot->ContextId",
        "context->AllocationReferences!=MAXULONG",
        "++context->AllocationReferences;",
    ):
        if reference.count(fragment) != 1:
            fail(f"allocation context pin must validate and increment one live registration: {fragment}")

    destroy_context = canonical_code(function_body("VioGpuAdapter::DestroyNativeContext", VIOGPU_CODE))
    released_context = canonical_code(function_body("VioGpuAdapter::IsNativeContextReleased", VIOGPU_CODE))
    if destroy_context.count("context->AllocationReferences!=0") != 2:
        fail("context destroy must reject both live and reset-retired registrations with allocation references")
    dead_state = destroy_context.find("if(objectState==VioGpuNativeContextDead&&context->Adapter==NULL&&!context->Registered)")
    dead_state_guard = destroy_context.find(
        "if(context->Owner!=NULL||context->Generation!=0||context->ResetGeneration!=0||context->ContextId!=0||"
        "context->VaStart!=0||context->VaSize!=0||context->SubmitQueueId!=0)",
        dead_state,
    )
    dead_state_failure = destroy_context.find("FailNativeContextAtAnyIrql();returnSTATUS_INVALID_DEVICE_STATE;", dead_state_guard)
    dead_state_success = destroy_context.find("*released=TRUE;", dead_state_failure)
    if min(dead_state, dead_state_guard, dead_state_failure, dead_state_success) < 0 or not (
        dead_state < dead_state_guard < dead_state_failure < dead_state_success
    ):
        fail("dead Native Context destroy must prove every identity field is cleared before reporting release")
    if released_context.count("context->AllocationReferences==0") != 1:
        fail("context release proof must wait for every KMD allocation reference")
    range_register = canonical_code(function_body("RegisterNativeAllocationRange", WDDM_DDI_CODE))
    if range_register.count("InsertTailList(&registration->AllocationRanges,&range->Link)") != 1 or range_register.count(
        "range->Iova<=existingEnd&&existing->Iova<=rangeEnd"
    ) != 1:
        fail("native allocation range registration must reject overlapping context IOVA intervals")
    for fragment in (
        "existing->Registration!=registration",
        "!existing->Linked",
        "existing->Iova==0",
        "existing->Length==0",
        "existing->Iova>MAXULONGLONG-((ULONGLONG)existing->Length-1)",
    ):
        if range_register.count(fragment) != 1:
            fail(f"native allocation range registration must reject malformed existing metadata: {fragment}")
    range_unregister = canonical_code(function_body("UnregisterNativeAllocationRange", WDDM_DDI_CODE))
    if range_unregister.count("RemoveEntryList(&range->Link)") != 1:
        fail("native allocation teardown must unregister its context IOVA range")

    destroy_allocation = canonical_code(function_body("VioGpuWddmDestroyAllocation", WDDM_DDI_CODE))
    detach_allocation = canonical_code(function_body("DetachAllocationNativeContext", WDDM_DDI_CODE))
    unlink_binding = detach_allocation.find("RemoveEntryList(&range->Link)")
    release_binding = detach_allocation.find("--registration->AllocationReferences;", unlink_binding)
    clear_binding = detach_allocation.find("allocation->NativeContext=NULL;", release_binding)
    detach_call = destroy_allocation.find("DetachAllocationNativeContext(allocation)")
    delete_allocation = destroy_allocation.find("deleteallocation;", detach_call)
    if min(unlink_binding, release_binding, clear_binding, detach_call, delete_allocation) < 0 or not (
        unlink_binding < release_binding < clear_binding and detach_call < delete_allocation
    ):
        fail("DestroyAllocation must drop its context pin before deleting the KMD allocation")

    create_host = canonical_code(function_body("VioGpuAdapter::CreateNativeGuestAllocation", VIOGPU_CODE))
    host_sequence = (
        create_host.find("entries==NULL||entryCount==0"),
        create_host.find("request.flags=msmFlags|MSM_BO_GUEST_ALLOC;"),
        create_host.find("TryReferenceNativeAllocationCount(snapshot->Owner)"),
        create_host.find("m_CtrlQueue.SubmitNativeControl(snapshot->ContextId,&request,sizeof(request))"),
        create_host.find("m_CtrlQueue.CreateNativeGuestBlob("),
    )
    if min(host_sequence) < 0 or list(host_sequence) != sorted(host_sequence):
        fail("guest-backed BO creation must bind GEM_NEW ownership before creating its guest-memory blob")
    for fragment in (
        "resourceId!=blobId",
        "resourceId==MAXUINT",
        "backingSize>MAXULONG",
        "backingSize<PAGE_SIZE",
        "logicalSize>MAXULONGLONG-(PAGE_SIZE-1)",
        "constULONGLONGlogicalAlignedSize=(logicalSize+PAGE_SIZE-1)&~((ULONGLONG)PAGE_SIZE-1);",
        "logicalAlignedSize!=(ULONGLONG)backingSize",
        "entries==NULL||entryCount==0",
        "capset.msm.has_cached_coherent==0",
        "(msmFlags&MSM_BO_CACHED_COHERENT)==0",
        "(blobFlags&VIRTIO_GPU_BLOB_FLAG_CREATE_GUEST_HANDLE)==0",
    ):
        if create_host.count(fragment) != 1:
            fail(f"guest-backed BO creation must preserve its WB/SG/guest-handle contract: {fragment}")

    create_blob_call = create_host.find("result=m_CtrlQueue.CreateNativeGuestBlob(")
    blob_failure = create_host.find("if(result!=VioGpuHostContextConfirmed)", create_blob_call)
    rollback_gate = create_host.find("if(result!=VioGpuHostContextUnknown)", blob_failure)
    rollback_unref = create_host.find("rollback=m_CtrlQueue.UnrefNativeResource(resourceId);", rollback_gate)
    rollback_proof = create_host.find(
        "if(rollback==VioGpuHostContextConfirmed&&ReleaseNativeAllocationCount(snapshot->Owner))", rollback_unref
    )
    release_count = rollback_proof
    release_ownership = create_host.find("*ownershipRetained=FALSE;", release_count)
    preserve_result = create_host.find("returnresult;", release_ownership)
    poison = create_host.find("FailNativeContextAtAnyIrql();", preserve_result)
    unknown_result = create_host.find("returnVioGpuHostContextUnknown;", poison)
    rollback_sequence = (
        create_blob_call,
        blob_failure,
        rollback_gate,
        rollback_unref,
        rollback_proof,
        release_count,
        release_ownership,
        preserve_result,
        poison,
        unknown_result,
    )
    if min(rollback_sequence) < 0 or list(rollback_sequence) != sorted(rollback_sequence):
        fail("failed blob creation must roll back confirmed GEM_NEW ownership or poison an unknowable transport")
    if create_host.count("m_CtrlQueue.UnrefNativeResource(resourceId)") != 1:
        fail("failed blob creation must attempt exactly one GEM_NEW resource rollback")
    if "rollback==VioGpuHostContextRejected" in create_host:
        fail("INVALID_RESOURCE_ID cannot prove that an unattached GEM_NEW blob object was released")
    destroy_host = canonical_code(function_body("VioGpuAdapter::DestroyNativeGuestAllocation", VIOGPU_CODE))
    if "result==VioGpuHostContextConfirmed||result==VioGpuHostContextRejected" in destroy_host:
        fail("guest allocation teardown cannot treat INVALID_RESOURCE_ID as released ownership")
    for fragment in (
        "if(result==VioGpuHostContextConfirmed)",
        "resourceId==MAXUINT",
        "ReleaseNativeAllocationCount(snapshot->Owner)",
        "elseif(result==VioGpuHostContextUnknown||result==VioGpuHostContextRejected)",
        "returnVioGpuHostContextUnknown;",
    ):
        if fragment == "returnVioGpuHostContextUnknown;":
            if destroy_host.count(fragment) < 1:
                fail(f"guest allocation teardown must quarantine unproven UNREF ownership: {fragment}")
        elif destroy_host.count(fragment) != 1:
            fail(f"guest allocation teardown must quarantine unproven UNREF ownership: {fragment}")
    if destroy_host.count("FailNativeContextAtAnyIrql();") < 1:
        fail("guest allocation teardown must quarantine unproven UNREF ownership: FailNativeContextAtAnyIrql();")

    release_ownership = canonical_code(function_body("ReleaseAllocationHostOwnership", WDDM_DDI_CODE))
    for fragment in (
        "allocation==NULL||!IsNativeAllocation(allocation)",
        "allocation->ResourceId<VIOGPU_NATIVE_RESOURCE_ID_START",
        "allocation->ResourceId==MAXUINT",
        "allocation->BlobId!=allocation->ResourceId",
    ):
        if fragment not in release_ownership:
            fail(f"Host ownership release must reject an unpaired Native resource identity: {fragment}")

    create_blob = canonical_code(function_body("CtrlQueue::CreateNativeGuestBlob", QUEUE_CODE))
    for fragment in (
        "resource_id==MAXUINT",
        "resource_id!=blob_id",
        "entries[index].addr==0",
        "entries[index].addr>MAXULONGLONG-(entries[index].length-1)",
    ):
        if create_blob.count(fragment) != 1:
            fail(f"guest blob creation must reject an unpaired or exhausted resource identity: {fragment}")
    blob_sequence = (
        create_blob.find("for(UINTindex=0;index<entry_count;++index)"),
        create_blob.find("entryBytes+=entries[index].length;"),
        create_blob.find("if(entryBytes!=size)"),
        create_blob.find("PGPU_MEM_ENTRYownedEntries="),
        create_blob.find("RtlCopyMemory(ownedEntries,entries,entriesSize);"),
        create_blob.find("command->blob_mem=VIRTIO_GPU_BLOB_MEM_HOST3D_GUEST;"),
        create_blob.find("command->blob_flags=blob_flags;"),
        create_blob.find("command->nr_entries=entry_count;"),
        create_blob.find("vbuf->data_buf=ownedEntries;"),
        create_blob.find("SubmitSynchronousLocked(vbuf,&releaseBuffer,&submitted)"),
    )
    if min(blob_sequence) < 0 or list(blob_sequence) != sorted(blob_sequence):
        fail("RESOURCE_CREATE_BLOB must validate and submit the complete queue-owned HOST3D_GUEST SG table")

    paging = canonical_code(function_body("VioGpuWddmBuildPagingBuffer", WDDM_DDI_CODE))
    if "pagingBuffer->Transfer.TransferOffset!=0" in paging or "TransferSize!=allocation->BackingSize" in paging:
        fail("BuildPagingBuffer must accept VidMm sub-transfers instead of requiring a whole allocation")
    worker = canonical_code(function_body("ExecutePagingTransaction", WDDM_DDI_CODE))
    for fragment in (
        "allocation->ApertureMdl!=NULL",
        "allocation->ApertureAddress!=NULL",
        "EnsureStandard2DAllocationBacking(allocation)",
        "allocation->Resource2DState==VioGpu2DResourceBackingAttached",
        "allocation->Resource2DResetGeneration!=0",
        "!transaction->Adapter->IsHardwareResetRequested()",
        "!transaction->TransferDataComplete",
        "transaction->Adapter->IsHardwareResetRequested()",
        "allocation->ResourceId>=VIOGPU_NATIVE_RESOURCE_ID_START",
        "allocation->ResourceId!=MAXUINT",
        "allocation->BlobId==allocation->ResourceId",
    ):
        if fragment not in worker:
            fail(f"passive paging execution must validate the retained VidMm backing: {fragment}")
    if "pagingBuffer->MultipassOffset!=0" in paging or "pagingBuffer->MultipassOffset=" in paging:
        fail("atomic paging markers must leave MultipassOffset unchanged")
    software_paging = canonical_code(function_body("BuildSoftwarePagingTransaction", WDDM_DDI_CODE))
    if "ResolveTransferMdlAddress(transferMdl,mdlOffset,transferSize,&systemAddress);" not in software_paging or \
       "CopyAperturePlacement(allocation," not in software_paging:
        fail("BuildPagingBuffer transfers must copy through the retained VidMm MDL mapping")

    transfer_mdl = canonical_code(function_body("ResolveTransferMdlAddress", WDDM_DDI_CODE))
    for fragment in (
        "MmGetMdlByteOffset(mdl)!=0",
        "(mdl->MdlFlags&MDL_PAGES_LOCKED)==0",
    ):
        if fragment not in transfer_mdl:
            fail(f"paging transfers must reject non-page-locked MDLs before system mapping: {fragment}")

    map_aperture = canonical_code(function_body("MapApertureAllocation", WDDM_DDI_CODE))
    for fragment in (
        "MmGetMdlByteOffset(mdl)!=0",
        "(mdl->MdlFlags&MDL_PAGES_LOCKED)==0",
    ):
        if fragment not in map_aperture:
            fail(f"aperture mapping must reject non-page-locked MDLs before treating PFNs as guest backing: {fragment}")

    private_matches = re.findall(
        r"\bstruct\s+VIOGPU_WDDM_KMD_DMA_PRIVATE\s*\{(.*?)\}\s*;",
        WDDM_DDI_HEADER_CODE,
        re.DOTALL,
    )
    private_header = canonical_code(private_matches[0]) if len(private_matches) == 1 else ""
    for field in (
        "ULONGSignature;",
        "USHORTVersion;",
        "USHORTKind;",
        "PVOIDDmaBuffer;",
        "UINTDmaBufferSize;",
        "UINTCommandLength;",
        "UINTContextId;",
        "LONGGeneration;",
        "ULONGLONGResetGeneration;",
        "UINTFlags;",
        "PVOIDPacket;",
        "UINTPacketLength;",
        "UINTReserved;",
    ):
        if private_header.count(field) != 1:
            fail(f"WDDM DMA private data must retain one exact field: {field}")

    packet_matches = re.findall(
        r"\bstruct\s+VIOGPU_WDDM_PAGING_DMA_PACKET\s*\{(.*?)\}\s*;",
        WDDM_DDI_HEADER_CODE,
        re.DOTALL,
    )
    packet_header = canonical_code(packet_matches[0]) if len(packet_matches) == 1 else ""
    for field in (
        "ULONGSignature;",
        "USHORTVersion;",
        "USHORTSize;",
        "UINTOperation;",
        "UINTFlags;",
        "UINTResourceId;",
        "UINTContextId;",
        "LONGContextGeneration;",
        "UINTReserved;",
        "ULONGLONGResetGeneration;",
        "ULONGLONGPlacementOffset;",
        "ULONGLONGTransferOffset;",
        "ULONGLONGTransferSize;",
    ):
        if packet_header.count(field) != 1:
            fail(f"paging DMA packet must retain one exact field: {field}")
    for fragment in (
        "VioGpuWddmDmaPrivateVersion=1",
        "VioGpuWddmDmaKindRender=1",
        "VioGpuWddmDmaKindPaging=2",
        "VioGpuWddmPagingFlagSoftwareCompleted=1<<7",
    ):
        if canonical_code(WDDM_DDI_HEADER_CODE).count(fragment) != 1:
            fail(f"paging DMA contract must expose one fixed revision/kind marker: {fragment}")

    paging_validator = canonical_code(function_body("ValidatePagingDmaPacket", WDDM_DDI_CODE))
    for fragment in (
        "privateData->Kind!=VioGpuWddmDmaKindPaging",
        "privateData->Packet!=privateData->DmaBuffer",
        "privateData->Submission!=pagingPrivate",
        "BOOLEANhasContext=packet->ContextId!=0;",
        "packet->ResourceId<VIOGPU_NATIVE_RESOURCE_ID_START",
        "packet->ResourceId>=VIOGPU_NATIVE_RESOURCE_ID_START",
        "packet->ResourceId==MAXUINT",
        "packet->TransferOffset>MAXUINT",
        "packet->TransferSize>MAXULONG",
        "(packet->PlacementOffset&(PAGE_SIZE-1))!=0",
        "(packet->Flags&~allowedFlags)!=0",
        "(packet->Flags&VioGpuWddmPagingFlagSoftwareCompleted)==0",
        "packet->Operation==DXGK_OPERATION_TRANSFER",
        "operationFlags==VioGpuWddmPagingFlagPageIn",
        "operationFlags==VioGpuWddmPagingFlagPageOut",
        "pageOut&&(packet->Flags&VioGpuWddmPagingFlagAllocationIdle)==0",
        "packet->Operation==DXGK_OPERATION_FILL",
        "operationFlags==VioGpuWddmPagingFlagFill",
        "packet->Operation==DXGK_OPERATION_DISCARD_CONTENT",
        "operationFlags==VioGpuWddmPagingFlagDiscard",
        "packet->TransferOffset>MAXULONGLONG-packet->TransferSize",
        "packet->PlacementOffset>MAXULONGLONG-packet->TransferOffset",
    ):
        if fragment not in paging_validator:
            fail(f"paging DMA validation must reject stale or malformed packet state: {fragment}")
    if paging_validator.count("packet->TransferSize-1<=MAXULONGLONG-") != 2:
        fail("paging DMA validation must bound both transfer and fill placement extents")

    if "pagingBuffer->MultipassOffset!=0" in paging or "pagingBuffer->MultipassOffset=" in paging:
        fail("atomic paging markers must leave MultipassOffset unchanged")
    capacity = paging.find(
        "pagingBuffer->DmaSize<sizeof(VIOGPU_WDDM_PAGING_DMA_PACKET)||"
        "pagingBuffer->DmaBufferPrivateDataSize<sizeof(VIOGPU_WDDM_PAGING_PRIVATE)"
    )
    if capacity < 0:
        fail("BuildPagingBuffer must snapshot and check both DMA capacities")
    publication = software_paging.find("RtlZeroMemory(packet,sizeof(*packet));")
    private_publication = software_paging.find("RtlZeroMemory(pagingPrivate,sizeof(*pagingPrivate));", publication)
    cursor_publication = software_paging.find(
        "pagingBuffer->pDmaBuffer=static_cast<BYTE*>(dmaBuffer)+sizeof(*packet);", private_publication
    )
    private_cursor = software_paging.find(
        "pagingBuffer->pDmaBufferPrivateData=static_cast<BYTE*>(privateBuffer)+sizeof(*pagingPrivate);",
        cursor_publication,
    )
    if min(publication, private_publication, cursor_publication, private_cursor) < 0:
        fail("BuildPagingBuffer must publish one packet/private record and advance both cursors")
    for fragment in (
        "packet->Signature=VIOGPU_WDDM_PAGING_DMA_SIGNATURE;",
        "packet->Version=VioGpuWddmDmaPrivateVersion;",
        "packet->Size=static_cast<USHORT>(sizeof(*packet));",
        "packet->Operation=static_cast<UINT>(pagingBuffer->Operation);",
        "packet->Flags=packetFlags;",
        "packet->ResourceId=allocation->ResourceId;",
        "packet->Reserved=0;",
        "privateData->Signature=VIOGPU_WDDM_DMA_SIGNATURE;",
        "privateData->Version=VioGpuWddmDmaPrivateVersion;",
        "privateData->Kind=VioGpuWddmDmaKindPaging;",
        "privateData->Packet=packet;",
        "privateData->PacketLength=sizeof(*packet);",
        "privateData->Reserved=0;",
        "pagingBuffer->DmaSize=dmaSize-sizeof(*packet);",
        "pagingBuffer->DmaBufferPrivateDataSize=privateSize-sizeof(*pagingPrivate);",
    ):
        if software_paging.count(fragment) != 1:
            fail(f"BuildPagingBuffer packet publication must retain exact metadata/cursors: {fragment}")
    if not (0 <= capacity and 0 <= publication < private_publication < cursor_publication < private_cursor):
        fail("BuildPagingBuffer must publish metadata only after operation success and in cursor order")

    render_body = function_body("VioGpuWddmRender", WDDM_DDI_CODE)
    render = canonical_code(render_body)
    for fragment in (
        "privateData->Version=VioGpuWddmDmaPrivateVersion;",
        "privateData->Kind=VioGpuWddmDmaKindRender;",
        "privateData->Packet=dmaBuffer;",
        "privateData->PacketLength=render->CommandLength;",
        "privateData->Reserved=0;",
        "privateData,sizeof(*privateData),render->CommandLength",
        "render->pDmaBufferPrivateData=static_cast<BYTE*>(render->pDmaBufferPrivateData)+sizeof(*privateData);",
        "render->DmaBufferPrivateDataSize-=sizeof(*privateData);",
    ):
        if render.count(fragment) != 1:
            fail(f"Render private data must identify its exact DMA kind and payload: {fragment}")
    render_private_pointer_writes = variable_write_offsets(render_body, "render->pDmaBufferPrivateData")
    render_private_size_writes = variable_write_offsets(render_body, "render->DmaBufferPrivateDataSize")
    render_publication = render_body.find("submissionPublished = TRUE;")
    if len(render_private_pointer_writes) != 1 or len(render_private_size_writes) != 1 or render_publication < 0 or \
       render_private_pointer_writes[0] < render_publication or render_private_size_writes[0] < render_publication:
        fail("Render must consume exactly one private-data record only after publishing its submission")


def check_native_guest_allocation_extent_math() -> None:
    page_size = 4096
    max_u64 = (1 << 64) - 1

    def page_round(logical_size: int) -> Optional[int]:
        if logical_size == 0 or logical_size > max_u64 - (page_size - 1):
            return None
        return (logical_size + page_size - 1) & ~(page_size - 1)

    for logical_size, backing_size in (
        (page_size, page_size),
        (page_size + 1, page_size * 2),
        (page_size * 2 - 1, page_size * 2),
    ):
        rounded = page_round(logical_size)
        if rounded != backing_size:
            fail(f"page-rounded logical extent must accept matching backing: {logical_size}, {backing_size}")

    for logical_size, backing_size in (
        (0, page_size),
        (page_size, page_size * 2),
        (page_size + 1, page_size),
        (max_u64 - (page_size - 2), max_u64),
    ):
        rounded = page_round(logical_size)
        if rounded is not None and rounded == backing_size:
            fail(f"page-rounded logical extent must reject mismatched backing: {logical_size}, {backing_size}")


def check_allocation_lifecycle_wait_status_contract() -> None:
    """Every mutex wait must require the exact STATUS_SUCCESS result.

    KeWaitForSingleObject can return a positive STATUS_TIMEOUT.  NT_SUCCESS
    therefore cannot be used as the ownership test for these lifecycle
    wrappers; accepting it would execute callback code without holding the
    allocation mutex and could release an unowned mutex on the unwind path.
    """

    present_helper = canonical_code(function_body("AcquirePresentAllocationLifecycles", WDDM_DDI_CODE))
    if present_helper.count("if(status!=STATUS_SUCCESS)") != 2 or present_helper.count(
        "if(NT_SUCCESS(status)){status=STATUS_GRAPHICS_ALLOCATION_BUSY;}"
    ) != 2:
        fail("Present lifecycle waits must reject positive wait statuses and normalize them before returning")

    exact_guards = (
        ("ValidateCommandHeader", "allocationStatus=AcquireAllocationLifecycle(allocation);", "if(allocationStatus!=STATUS_SUCCESS)"),
        ("ApplyRenderPrepatches", "status=AcquireAllocationLifecycle(allocation);", "if(status!=STATUS_SUCCESS)"),
        ("VioGpuWddmOpenAllocation", "status=AcquireAllocationLifecycle(allocation);", "if(status==STATUS_SUCCESS)"),
        ("ExecutePagingTransaction", "status=AcquireAllocationLifecycle(allocation);", "if(status!=STATUS_SUCCESS)"),
        ("MapApertureAllocation", "status=AcquireAllocationLifecycle(allocation);", "if(status!=STATUS_SUCCESS)"),
        ("UnmapApertureAllocation", "status=AcquireAllocationLifecycle(allocation);", "if(status!=STATUS_SUCCESS)"),
        ("VioGpuWddmPatch", "status=AcquireAllocationLifecycle(allocation);", "if(status!=STATUS_SUCCESS)"),
        ("VioGpuWddmSetVidPnSourceAddress", "status=AcquireAllocationLifecycle(allocation);", "if(status!=STATUS_SUCCESS)"),
    )
    for function_name, acquisition, guard in exact_guards:
        body = canonical_code(function_body(function_name, WDDM_DDI_CODE))
        call = body.find(acquisition)
        guarded = body.find(guard, call + len(acquisition)) if call >= 0 else -1
        if call < 0 or guarded < 0:
            fail(f"{function_name} must test lifecycle acquisition with exact STATUS_SUCCESS")

    software = canonical_code(function_body("BuildSoftwarePagingTransaction", WDDM_DDI_CODE))
    for fragment in (
        "BOOLEANlifecycleAcquired=status==STATUS_SUCCESS;",
        "if(status==STATUS_SUCCESS)",
    ):
        if software.count(fragment) == 0:
            fail(f"BuildSoftwarePagingTransaction must keep mutex ownership exact: {fragment}")

    for function_name in ("ExecutePresentTransaction", "VioGpuWddmPresent"):
        body = canonical_code(function_body(function_name, WDDM_DDI_CODE))
        call = body.find("AcquirePresentAllocationLifecycles(")
        if call < 0 or body.find("status!=STATUS_SUCCESS", call) < 0:
            fail(f"{function_name} must reject a positive Present lifecycle wait status")

    render_references = canonical_code(function_body("AcquireRenderAllocationReferences", WDDM_DDI_CODE))
    call = render_references.find("NTSTATUSstatus=AcquireAllocationLifecycle(allocation);")
    if call < 0:
        fail("AcquireRenderAllocationReferences must acquire each allocation lifecycle")
    if render_references.find("if(status==STATUS_SUCCESS)", call) < 0:
        fail("AcquireRenderAllocationReferences must release the mutex only after exact acquisition")
    if render_references.find("elseif(NT_SUCCESS(status)){status=STATUS_GRAPHICS_ALLOCATION_BUSY;}", call) < 0:
        fail("AcquireRenderAllocationReferences must normalize a positive wait result")
    if render_references.find("if(status!=STATUS_SUCCESS)", call) < 0:
        fail("AcquireRenderAllocationReferences must reject any non-success lifecycle result")

    software_return = canonical_code(function_body("BuildSoftwarePagingTransaction", WDDM_DDI_CODE))
    if "returnstatus==STATUS_SUCCESS?STATUS_SUCCESS:STATUS_GRAPHICS_ALLOCATION_BUSY;" not in software_return:
        fail("BuildSoftwarePagingTransaction must not convert a positive wait result into success")


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
        "UNREFERENCED_PARAMETER(state);"
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
    publish_type = create.find("context->Type=contextType;")
    publish_native_state = create.find(
        "context->NativeContext.State=contextType==VioGpuWddmContextNative?"
        "VioGpuNativeContextAllocated:VioGpuNativeContextDead;"
    )
    host_create = create.find(
        "NTSTATUSstatus=contextType==VioGpuWddmContextNative?"
        "device->Adapter->CreateNativeContext(&context->NativeContext,privateData.ExpectedResetGeneration):"
        "STATUS_SUCCESS;"
    )
    publish = create.find("createContext->hContext=context;")
    if min(
        reserve,
        allocate,
        initialize_rundown,
        publish_open,
        binding_lock,
        publish_type,
        publish_native_state,
        host_create,
        publish,
    ) < 0 or not (
        reserve < allocate < initialize_rundown < publish_open < publish_type < binding_lock <
        publish_native_state < host_create < publish
    ):
        fail("CreateContext must initialize the typed context before conditional Host creation and publication")
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
    release_default = destroy.find("BOOLEANreleased=context->Type!=VioGpuWddmContextNative;")
    native_destroy = destroy.find("context->Device->Adapter->DestroyNativeContext(")
    rundown_close = destroy.find(expected_rundown_close)
    release_guard = destroy.find("if(!released){returnNT_SUCCESS(status)?STATUS_DEVICE_NOT_READY:status;}", native_destroy)
    if min(rundown_close, release_default, native_destroy, release_guard) < 0 or not (
        rundown_close < release_default < native_destroy < release_guard
    ):
        fail("DestroyContext must close operations, bypass Host teardown for non-Native contexts, and prove release")
    if destroy.count("context->Type!=VioGpuWddmContextNative") != 1 or destroy.count(
        "if(!released&&context->Device!=NULL&&context->Device->Adapter!=NULL)"
    ) != 1:
        fail("DestroyContext must issue Host teardown only for an unreleased Native context")
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
    render_context_gate = (
        "context->Signature!=VIOGPU_WDDM_CONTEXT_SIGNATURE||context->Type!=VioGpuWddmContextNative"
    )
    signature_check = render.find(f"if({render_context_gate})")
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
        if canonical_code(condition) == render_context_gate
    ]
    if acquire_failure != ["returnSTATUS_DEVICE_NOT_READY;"] or signature_failure != [
        "ExReleaseRundownProtection(&context->Operations);returnSTATUS_INVALID_HANDLE;"
    ]:
        fail("Render must fail closed on rundown acquisition and release it after a bad signature")
    if render.count("VioGpuAdapter::ReleaseNativeContextSnapshot(&snapshot);") != 1:
        fail("Render must release its native-context snapshot exactly once through unified cleanup")
    if render.count("ExReleaseRundownProtection(&context->Operations);") != 4:
        fail("Render must release context operations on both acquisition failures and unified cleanup")
    cleanup_start = render.find("NTSTATUSstatus=STATUS_SUCCESS;")
    render_cleanup = (
        "if(allocationSubmissionReferences!=0){"
        "ReleaseRenderAllocationReferences(validatedCommand,render->pAllocationList,render->AllocationListSize,"
        "allocationSubmissionReferences);}"
        "if(contextSubmissionReference){ReleaseContextSubmissionReference(context);}"
        "delete[]patchSnapshot;"
        "delete[]commandSnapshot;"
        "VioGpuAdapter::ReleaseNativeContextSnapshot(&snapshot);"
        "if(hardwareOperation){context->Device->Adapter->ReleaseNativeSubmissionOperation();}"
        "ExReleaseRundownProtection(&context->Operations);returnstatus;"
    )
    if cleanup_start < acquire or render[cleanup_start:].count("return") != 1 or not render.endswith(render_cleanup):
        fail("Render must free both snapshots and release both lifetime guards through one tail path")


def check_wddm_submission_lifetime() -> None:
    resolve_submission_parameters = (
        "PVOID privateDataBase, UINT privateDataSize, UINT submissionStart, UINT submissionEnd, "
        "VioGpuDod *adapter, HANDLE runtimeContext, VIOGPU_WDDM_SUBMISSION **submissionOut"
    )
    allocation_matches = re.findall(
        r"\bstruct\s+VIOGPU_WDDM_ALLOCATION\s*\{(.*?)\}\s*;", WDDM_DDI_HEADER_CODE, re.DOTALL
    )
    context_matches = re.findall(
        r"\bstruct\s+VIOGPU_WDDM_CONTEXT\s*\{(.*?)\}\s*;", WDDM_DDI_HEADER_CODE, re.DOTALL
    )
    submission_matches = re.findall(
        r"\bstruct\s+VIOGPU_WDDM_SUBMISSION\s*\{(.*?)\}\s*;", WDDM_DDI_HEADER_CODE, re.DOTALL
    )
    allocation_header = canonical_code(allocation_matches[0]) if len(allocation_matches) == 1 else ""
    context_header = canonical_code(context_matches[0]) if len(context_matches) == 1 else ""
    submission_header = canonical_code(submission_matches[0]) if len(submission_matches) == 1 else ""
    for field in (
        "KSPIN_LOCKSubmissionLock;",
        "volatileLONGSubmissionReferences;",
        "volatileLONGOpenReferences;",
        "BOOLEANDestroying;",
    ):
        if allocation_header.count(field) != 1:
            fail(f"allocation lifetime must retain exactly one internal ownership field: {field}")
    for field in (
        "KSPIN_LOCKSubmissionLock;",
        "volatileLONGSubmissionReferences;",
        "BOOLEANSubmissionClosing;",
        "LIST_ENTRYPendingSubmissions;",
        "UINTUmdFenceHead;",
        "UINTUmdFenceCount;",
        "VIOGPU_WDDM_CONTEXT_FENCE_ENTRYUmdFences[VioGpuWddmContextFenceTrackerCapacity];",
    ):
        if context_header.count(field) != 1:
            fail(f"context lifetime must retain exactly one internal submission field: {field}")
    for field in (
        "ULONGSignature;",
        "volatileLONGReferenceCount;",
        "volatileLONGCancelRequested;",
        "volatileLONGWorkReferenceHeld;",
        "VIOGPU_WDDM_CONTEXT_SUBMISSION_ENTRYContextEntry;",
        "VIOGPU_NATIVE_PASSIVE_WORKWork;",
        "VIOGPU_WDDM_CONTEXT*Context;",
        "PVOIDDmaBuffer;",
        "PVOIDDmaPrivateData;",
        "UINTContextId;",
        "LONGGeneration;",
        "ULONGLONGResetGeneration;",
        "ULONGLONGFenceId;",
        "PGPU_VBUFFERVirtioBuffer;",
        "VioGpuDod*Adapter;",
        "PVOIDCommandStream;",
        "BOOLEANPatchApplied;",
        "BOOLEANFullyPrepatched;",
        "volatileLONGState;",
        "VIOGPU_WDDM_SUBMISSION_REFERENCE*References;",
    ):
        if submission_header.count(field) != 1:
            fail(f"submission record must retain exactly one nonpaged ownership field: {field}")
    header = canonical_code(WDDM_DDI_HEADER_CODE)
    if header.count("VioGpuWddmSubmissionAllocationLimit=1024,") != 1:
        fail("submission record must use the bounded allocation-reference limit")
    source = canonical_code(WDDM_DDI_SOURCE)
    if source.count(
        "constUINTVIOGPU_WDDM_ALLOCATION_LIST_SIZE=VioGpuWddmSubmissionAllocationLimit;"
    ) != 1:
        fail("the VidSch allocation list must match the bounded submission-reference limit")
    if source.count(
        "constUINTVIOGPU_WDDM_PATCH_LIST_SIZE=VioGpuWddmSubmissionAllocationLimit;"
    ) != 1:
        fail("the VidSch patch list must match the bounded submission-reference limit")
    if WDDM_DDI_SOURCE.count("const ULONG VIOGPU_WDDM_SUBMISSION_SIGNATURE = 'sWGV';") != 1:
        fail("submission record must have one private signature")

    queue_header = canonical_code(QUEUE_HEADER_CODE)
    for field in (
        "void(*cancel_cb)(void*ctx);",
        "void*cancel_ctx;",
        "void(*queue_error_cb)(void*ctx);",
        "void*queue_error_ctx;",
        "volatileLONGterminal_callback_state;",
        "KEVENTterminal_callback_event;",
        "LIST_ENTRYnative_submit_link;",
    ):
        if queue_header.count(field) != 1:
            fail(f"native queue buffer must retain one reset/backlog ownership field: {field}")

    create_allocation = canonical_code(function_body("VioGpuWddmCreateAllocation", WDDM_DDI_CODE))
    require_order(
        create_allocation,
        (
            "KeInitializeMutex(&allocation->LifecycleMutex,0);",
            "KeInitializeSpinLock(&allocation->SubmissionLock);",
            "allocation->SubmissionReferences=0;",
            "allocation->OpenReferences=0;",
            "allocation->Destroying=FALSE;",
        ),
        "CreateAllocation must initialize internal ownership before publishing the allocation",
    )
    wddm_ddi = canonical_code(WDDM_DDI_CODE)
    if wddm_ddi.count("allocation->Destroying=FALSE;") != 1 or "CancelAllocationDestroy(" in wddm_ddi:
        fail("allocation destruction must never reopen a published allocation")
    create_context = canonical_code(function_body("VioGpuWddmCreateContext", WDDM_DDI_CODE))
    require_order(
        create_context,
        (
            "ExInitializeRundownProtection(&context->Operations);",
            "KeInitializeSpinLock(&context->SubmissionLock);",
            "context->SubmissionReferences=0;",
            "context->SubmissionClosing=FALSE;",
            "InitializeListHead(&context->PendingSubmissions);",
        ),
        "CreateContext must initialize submission ownership before Host context creation",
    )

    acquire_context = canonical_code(function_body("AcquireContextSubmissionReference", WDDM_DDI_CODE))
    if acquire_context.count("!context->SubmissionClosing") != 1 or acquire_context.count(
        "++context->SubmissionReferences;"
    ) != 1:
        fail("Render context ownership must reject closing and increment exactly once")
    begin_context = canonical_code(function_body("BeginContextSubmissionRundown", WDDM_DDI_CODE))
    if begin_context.count("context->SubmissionClosing=TRUE;") != 1 or \
       "context->SubmissionReferences" in begin_context or \
       "STATUS_GRAPHICS_ALLOCATION_BUSY" in begin_context:
        fail("context destruction must close publication before the DestroyContext owner drains live submissions")
    context_header = canonical_code(WDDM_DDI_HEADER_CODE)
    for field in ("volatileLONGSubmittedUmdFence;", "volatileLONGCompletedUmdFence;"):
        if context_header.count(field) != 1:
            fail(f"each Native Context must retain one UMD fence endpoint: {field}")
    record_context_fence = canonical_code(function_body("RecordContextUmdFence", WDDM_DDI_CODE))
    for fragment in (
        "fenceId==0",
        "KeAcquireSpinLock(&context->SubmissionLock,&oldIrql);",
        "!context->SubmissionClosing",
        "static_cast<LONG>(fenceId-submitted)>0",
        "InterlockedExchange(&context->SubmittedUmdFence,static_cast<LONG>(fenceId));",
    ):
        if fragment not in record_context_fence:
            fail(f"UMD submit fence recording must be context-scoped and monotonic: {fragment}")
    retire_context_fence = canonical_code(function_body("RetireContextUmdFence", WDDM_DDI_CODE))
    require_order(
        retire_context_fence,
        (
            "KeAcquireSpinLock(&context->SubmissionLock,&oldIrql);",
            "context->UmdFenceCount!=0",
            "context->UmdFences[index].State==VioGpuWddmContextFencePending",
            "match->State=VioGpuWddmContextFenceRetired;",
            "context->UmdFences[context->UmdFenceHead].State==VioGpuWddmContextFenceRetired",
            "--context->UmdFenceCount;",
            "InterlockedExchange(&context->CompletedUmdFence,static_cast<LONG>(completed));",
            "KeReleaseSpinLock(&context->SubmissionLock,oldIrql);",
        ),
        "UMD completion must retire only tracked context fences and publish a contiguous prefix",
    )
    query_context_fence = canonical_code(function_body("QueryContextCompletedUmdFence", WDDM_DDI_CODE))
    if "context->Signature==VIOGPU_WDDM_CONTEXT_SIGNATURE" not in query_context_fence or "context->CompletedUmdFence" not in query_context_fence:
        fail("completed-fence query must read the context-owned endpoint under its lock")
    invalidate_context_fence = canonical_code(function_body("InvalidateContextUmdFenceTracker", WDDM_DDI_CODE))
    require_order(
        invalidate_context_fence,
        (
            "context->UmdFenceHead=0;",
            "context->UmdFenceCount=0;",
            "RtlZeroMemory(context->UmdFences,sizeof(context->UmdFences));",
            "InterlockedExchange(&context->SubmittedUmdFence,static_cast<LONG>(completed));",
        ),
        "a failed native enqueue must invalidate unaccepted context UMD fences without publishing completion",
    )
    acquire_allocation = canonical_code(
        function_body_with_parameters(
            "AcquireAllocationSubmissionReference",
            "VIOGPU_WDDM_ALLOCATION *allocation, VioGpuDod *adapter",
            WDDM_DDI_CODE,
        )
    )
    for fragment in (
        "allocation->Adapter==adapter",
        "!allocation->Destroying",
        "++allocation->SubmissionReferences;",
    ):
        if acquire_allocation.count(fragment) != 1:
            fail(f"allocation submission acquisition must retain its destroy gate: {fragment}")
    acquire_destroy_lifecycle = canonical_code(
        function_body("AcquireAllocationLifecycleForDestroy", WDDM_DDI_CODE)
    )
    require_order(
        acquire_destroy_lifecycle,
        (
            "allocation==NULL||KeGetCurrentIrql()!=PASSIVE_LEVEL",
            "timeout.QuadPart=-10LL*10*1000*1000;",
            "returnKeWaitForSingleObject(&allocation->LifecycleMutex,Executive,KernelMode,FALSE,&timeout);",
        ),
        "destroy retry must acquire the allocation lifecycle mutex at PASSIVE_LEVEL",
    )
    if "allocation->Destroying" in acquire_destroy_lifecycle or "KeReleaseMutex(" in acquire_destroy_lifecycle:
        fail("destroy retry must preserve the close gate and retain the lifecycle mutex")
    acquire_lifecycle = canonical_code(function_body("AcquireAllocationLifecycle", WDDM_DDI_CODE))
    require_order(
        acquire_lifecycle,
        (
            "status=AcquireAllocationLifecycleForDestroy(allocation);",
            "if(status!=STATUS_SUCCESS)",
            "KeAcquireSpinLock(&allocation->SubmissionLock,&oldIrql);",
            "destroying=allocation->Destroying;",
            "KeReleaseSpinLock(&allocation->SubmissionLock,oldIrql);",
            "if(destroying)",
            "KeReleaseMutex(&allocation->LifecycleMutex,FALSE);",
            "returnSTATUS_GRAPHICS_ALLOCATION_BUSY;",
        ),
        "ordinary allocation lifecycle acquisition must reject the closed destroy state",
    )
    begin_allocation = canonical_code(function_body("BeginAllocationDestroy", WDDM_DDI_CODE))
    require_order(
        begin_allocation,
        ("allocation->Destroying=TRUE;", "allocation->SubmissionReferences!=0||allocation->OpenReferences!=0"),
        "allocation destruction must close new references before reporting busy",
    )
    validate_destroy_state = canonical_code(
        function_body("ValidateNativeAllocationDestroyState", WDDM_DDI_CODE)
    )
    require_order(
        validate_destroy_state,
        (
            "if(!IsNativeAllocation(allocation))",
            "allocation->NativeContext!=NULL&&allocation->ContextRange!=NULL",
            "ValidateNativeAllocationRange(allocation)",
            "allocation->Destroying&&allocation->NativeContext==NULL&&allocation->ContextRange==NULL&&allocation->HostState==VioGpuWddmAllocationHostNone",
        ),
        "destroy retry must accept only a live range or a closed fully detached Native allocation",
    )
    detach_native = canonical_code(function_body("DetachAllocationNativeContext", WDDM_DDI_CODE))
    require_order(
        detach_native,
        (
            "registration=allocation->NativeContext;",
            "range=allocation->ContextRange;",
            "if(registration==NULL||range==NULL)",
            "registration==NULL&&range==NULL?STATUS_SUCCESS:STATUS_DEVICE_NOT_READY",
            "KeAcquireSpinLock(&registration->BindingLock,&oldIrql);",
            "range->Registration==registration&&range->Linked&&registration->AllocationReferences!=0",
            "RemoveEntryList(&range->Link);",
            "allocation->ContextRange=NULL;",
            "--registration->AllocationReferences;",
            "allocation->NativeContext=NULL;",
            "KeReleaseSpinLock(&registration->BindingLock,oldIrql);",
            "deleterange;",
        ),
        "Native allocation detach must atomically release its range and context reference",
    )

    open_allocation = canonical_code(function_body("VioGpuWddmOpenAllocation", WDDM_DDI_CODE))
    require_order(
        open_allocation,
        (
            "status=AcquireAllocationLifecycle(allocation);",
            "status=ReferenceAllocationOpen(allocation,device->Adapter);",
            "KeReleaseMutex(&allocation->LifecycleMutex,FALSE);",
            "openInfo->hDeviceSpecificAllocation=deviceAllocation;",
        ),
        "OpenAllocation must acquire a serialized strong reference before publishing its wrapper",
    )
    close_allocation = canonical_code(function_body("VioGpuWddmCloseAllocation", WDDM_DDI_CODE))
    require_order(
        close_allocation,
        ("ReleaseAllocationOpen(deviceAllocation->Allocation);", "DereferenceDevice(deviceAllocation->Device);"),
        "CloseAllocation must release allocation ownership before device ownership",
    )
    destroy_allocation = canonical_code(function_body("VioGpuWddmDestroyAllocation", WDDM_DDI_CODE))
    if (
        wddm_ddi.count("AcquireAllocationLifecycleForDestroy(") != 3
        or acquire_lifecycle.count("AcquireAllocationLifecycleForDestroy(allocation)") != 1
        or destroy_allocation.count("AcquireAllocationLifecycleForDestroy(allocation)") != 1
    ):
        fail("the raw destroy-retry lifecycle acquisition must only serve the gated wrapper and DestroyAllocation")
    require_order(
        destroy_allocation,
        (
            "status=AcquireAllocationLifecycleForDestroy(allocation);",
            "if(status==STATUS_SUCCESS)",
            "ValidateNativeAllocationDestroyState(allocation)",
            "snapshotAcquired=AcquireAllocationNativeContextSnapshot(allocation,&snapshot);",
            "status=BeginAllocationDestroy(allocation);",
            "status=ReleaseAllocationHostOwnership(allocation,&snapshot,snapshotAcquired);",
            "if(status!=STATUS_SUCCESS)",
            "status=DetachAllocationNativeContext(allocation);",
            "InterlockedDecrement(&allocation->Resource->AllocationCount);",
            "deleteallocation;",
        ),
        "DestroyAllocation must complete all fallible detaches before deleting any allocation",
    )
    detach_call = destroy_allocation.find("status=DetachAllocationNativeContext(allocation);")
    detach_failure = destroy_allocation.find("if(status!=STATUS_SUCCESS)", detach_call)
    delete_allocation = destroy_allocation.find("deleteallocation;", detach_failure)
    if min(detach_call, detach_failure, delete_allocation) < 0 or not detach_call < detach_failure < delete_allocation:
        fail("DestroyAllocation must retain the whole allocation list after any detach failure")
    if "NT_SUCCESS(status)" in destroy_allocation:
        fail("DestroyAllocation must not treat positive wait statuses as successful mutex acquisition")
    destroy_context = canonical_code(function_body("VioGpuWddmDestroyContext", WDDM_DDI_CODE))
    require_order(
        destroy_context,
        (
            "BeginContextSubmissionRundown(context);",
            "if(!context->OperationsRundownCompleted)",
            "for(PLIST_ENTRYlink=context->PendingSubmissions.Flink;",
        ),
        "DestroyContext must close submissions and drain callback operations before retiring pending owners",
    )

    publish = canonical_code(function_body("PublishPreparedSubmission", WDDM_DDI_CODE))
    require_order(
        publish,
        (
            "RtlZeroMemory(submission,sizeof(*submission));",
            "submission->Signature=VIOGPU_WDDM_SUBMISSION_SIGNATURE;",
            "submission->ReferenceCount=1;",
            "submission->State=VioGpuWddmSubmissionPrepared;",
            "submission->WorkReferenceHeld=0;",
            "InitializeListHead(&submission->ContextEntry.Link);",
            "submission->ContextEntry.Kind=VioGpuWddmContextSubmissionRender;",
            "submission->FullyPrepatched=fullyPrepatched;",
            "submission->References=new(NonPagedPoolNx)VIOGPU_WDDM_SUBMISSION_REFERENCE[allocationCount];",
            "for(UINTindex=0;index<allocationCount;++index)",
            "KeAcquireSpinLock(&context->SubmissionLock,&oldIrql);",
            "privateData->Submission=submission;",
            "virtioBuffer->complete_cb=NativeSubmissionComplete;",
            "virtioBuffer->cancel_cb=NativeSubmissionCancelled;",
            "virtioBuffer->queue_error_cb=NativeSubmissionQueueFailed;",
            "virtioBuffer->auto_release=FALSE;",
            "VioGpuArmVbufferTerminalCallbacks(virtioBuffer)",
            "InsertTailList(&context->PendingSubmissions,&submission->ContextEntry.Link);",
        ),
        "prepared submission must publish private-data identity and all callbacks under the context lock",
    )
    publish_body = function_body("PublishPreparedSubmission", WDDM_DDI_CODE)
    free_submission_storage = canonical_code(function_body("FreeRenderSubmissionStorage", WDDM_DDI_CODE))
    require_order(
        free_submission_storage,
        (
            "delete[]submission->References;",
            "submission->References=NULL;",
        ),
        "Render submission variable storage must have one idempotent cleanup helper",
    )
    if publish.count("FreeRenderSubmissionStorage(submission);") != 4:
        fail("prepared submission publication must release variable storage on every post-allocation failure")
    resolve_submission = canonical_code(
        function_body_with_parameters(
            "ResolveSubmissionPrivateData", resolve_submission_parameters, WDDM_DDI_CODE
        )
    )
    for fragment in ("submission->References==NULL",):
        if fragment not in resolve_submission:
            fail(f"submission resolution must validate variable reference storage: {fragment}")
    arm_blocks = []
    for condition, body, _, block_end in if_blocks(publish_body):
        if canonical_code(condition) != "VioGpuArmVbufferTerminalCallbacks(virtioBuffer)":
            continue
        else_body = else_block_after(publish_body, block_end)
        if else_body is not None:
            arm_blocks.append((canonical_code(body), canonical_code(else_body)))
    if arm_blocks != [
        (
            "InsertTailList(&context->PendingSubmissions,&submission->ContextEntry.Link);",
            "VioGpuDetachVbufferTerminalCallbacks(virtioBuffer);privateData->Submission=NULL;valid=FALSE;",
        )
    ]:
        fail("prepared Render publication must link only an armed terminal owner and clear identity on arm failure")
    for fragment in (
        "header->CommandStreamSize<sizeof(MSM_CCMD_GEM_SUBMIT_REQ)",
        "submission->UmdFenceId=submitRequest->fence;",
        "submission->UmdFenceId==0",
    ):
        if fragment not in publish:
            fail(f"prepared Native submission must retain its validated UMD fence token: {fragment}")
    quarantine = canonical_code(
        function_body_with_parameters(
            "QuarantineSubmission",
            "VIOGPU_WDDM_SUBMISSION *submission, LONG expectedState, BOOLEAN releaseBuffer, "
            "BOOLEAN terminalCallbackOwned = FALSE",
            WDDM_DDI_CODE,
        )
    )
    require_order(
        quarantine,
        (
            "InterlockedCompareExchange(&submission->State,expectedState,expectedState)",
            "BOOLEANterminalOwned=virtioBuffer==NULL||terminalCallbackOwned;",
            "if(linked&&!terminalOwned)",
            "VioGpuClaimVbufferTerminalCallbacks(virtioBuffer)",
            "==VioGpuVbufferTerminalClaimWon",
            "if(linked&&terminalOwned)",
            "RemoveEntryList(&submission->ContextEntry.Link);",
            "InterlockedExchange(&submission->State,VioGpuWddmSubmissionQuarantined);",
            "VioGpuDetachVbufferTerminalCallbacks(virtioBuffer);",
            "elseif(linked)",
            "linked=FALSE;",
            "if(!linked)",
            "privateData->Submission=NULL;",
            "adapter->ReleaseNativeSubmitBuffer(virtioBuffer);",
            "VioGpuCompleteVbufferTerminalCallbacks(virtioBuffer);",
            "DereferenceRenderSubmission(submission);",
        ),
        "submission quarantine must unpublish one terminal owner before dropping its registry reference",
    )
    not_linked = quarantine.find("if(!linked)")
    not_linked_return = quarantine.find("returnFALSE;", not_linked)
    private_unpublish = quarantine.find("privateData->Submission=NULL;", not_linked_return)
    if min(not_linked, not_linked_return, private_unpublish) < 0 or not (
        not_linked < not_linked_return < private_unpublish
    ):
        fail("submission quarantine must reject an owner it could not unlink before clearing private identity")
    for forbidden in (
        "ReleaseAllocationSubmissionReference(",
        "ReleaseContextSubmissionReference(",
        "submission->Context=NULL;",
        "submission->Signature=0;",
        "deletesubmission;",
    ):
        if forbidden in quarantine:
            fail(f"submission quarantine must leave dependencies alive for temporary references: {forbidden}")
    terminal_quarantine = canonical_code(
        function_body_with_parameters(
            "QuarantineTerminalSubmission",
            "VIOGPU_WDDM_SUBMISSION *submission, BOOLEAN releaseBuffer",
            WDDM_DDI_CODE,
        )
    )
    for fragment in (
        "state>=VioGpuWddmSubmissionPrepared",
        "state<=VioGpuWddmSubmissionHostIssued",
        "QuarantineSubmission(submission,state,releaseBuffer,TRUE)",
    ):
        if fragment not in terminal_quarantine:
            fail(f"terminal Render callbacks must claim only the published state range: {fragment}")
    render_submission_parameter = "_Inout_ VIOGPU_WDDM_SUBMISSION *submission"
    dereference_render = canonical_code(
        function_body_with_parameters("DereferenceRenderSubmission", render_submission_parameter, WDDM_DDI_CODE)
    )
    require_order(
        dereference_render,
        (
            "LONGreferences=InterlockedDecrement(&submission->ReferenceCount);",
            "references==0",
            "VioGpuWddmSubmissionQuarantined",
            "ReleaseAllocationSubmissionReference(submission->References[index].Allocation);",
            "FreeRenderSubmissionStorage(submission);",
            "ReleaseContextSubmissionReference(context);",
            "submission->Context=NULL;",
            "submission->Signature=0;",
            "deletesubmission;",
        ),
        "final Render dereference must retain Context and allocations until every temporary owner exits",
    )
    reference_render = canonical_code(
        function_body_with_parameters("ReferenceRenderSubmission", render_submission_parameter, WDDM_DDI_CODE)
    )
    require_order(
        reference_render,
        (
            "LONGreferences=InterlockedCompareExchange(&submission->ReferenceCount,0,0);",
            "while(references>0&&references<MAXLONG)",
            "InterlockedCompareExchange(&submission->ReferenceCount,references+1,references);",
            "if(observed==references)",
            "returnTRUE;",
        ),
        "temporary Render owners must increment only a live bounded reference count",
    )
    acquire_render_work = canonical_code(
        function_body_with_parameters("AcquireRenderWorkReference", render_submission_parameter, WDDM_DDI_CODE)
    )
    require_order(
        acquire_render_work,
        (
            "if(!ReferenceRenderSubmission(submission))",
            "InterlockedCompareExchange(&submission->WorkReferenceHeld,1,0)==0",
            "DereferenceRenderSubmission(submission);",
        ),
        "Render worker ownership must transfer one reference through the 0-to-1 work flag",
    )
    release_render_work = canonical_code(
        function_body_with_parameters("ReleaseRenderWorkReference", render_submission_parameter, WDDM_DDI_CODE)
    )
    if (
        "InterlockedCompareExchange(&submission->WorkReferenceHeld,0,1)==1" not in release_render_work
        or release_render_work.count("DereferenceRenderSubmission(submission);") != 1
    ):
        fail("Render worker release must drop exactly the reference whose work flag changes from 1 to 0")
    resolve_render = canonical_code(
        function_body_with_parameters(
            "ResolveSubmissionPrivateData",
            resolve_submission_parameters,
            WDDM_DDI_CODE,
        )
    )
    require_order(
        resolve_render,
        (
            "KeAcquireSpinLock(&context->SubmissionLock,&oldIrql);",
            "entry->Kind==VioGpuWddmContextSubmissionRender",
            "entry->Owner==privateData->Submission",
            "ReferenceRenderSubmission(candidate)",
            "KeReleaseSpinLock(&context->SubmissionLock,oldIrql);",
            "if(submission==NULL)",
            "DereferenceRenderSubmission(submission);",
            "*submissionOut=submission;",
        ),
        "Render private-data resolution must acquire its temporary owner while the context registry is locked",
    )

    validate_packet = canonical_code(function_body("ValidateNativeSubmitPacket", WDDM_DDI_CODE))
    for fragment in (
        "request->hdr.cmd!=MSM_CCMD_GEM_SUBMIT",
        "request->hdr.len!=header->CommandStreamSize",
        "request->queue_id!=nativeContext->SubmitQueueId",
        "request->nr_bos!=header->AllocationReferenceCount",
        "expectedSize!=header->CommandStreamSize",
        "bos[index].Handle!=0",
        "bos[index].Presumed!=0",
        "previousOpen->Allocation==allocation",
        "command->SubmitIndex>=request->nr_bos",
        "command->RelocationCount!=0",
        "command->Iova!=0",
        "command->Size>allocation->PrivateData.Size-command->SubmitOffset",
    ):
        if fragment not in validate_packet:
            fail(f"Render must validate the exact bounded MSM GEM_SUBMIT packet: {fragment}")
    if "bos[index].Handle!=allocation->ResourceId" in validate_packet:
        fail("Render must not require the UMD to know a KMD-owned native resource ID")

    render_prepatch = canonical_code(function_body("ApplyRenderPrepatches", WDDM_DDI_CODE))
    for fragment in (
        "*fullyPrepatched=TRUE;",
        "allocationEntry->SegmentId==0",
        "*fullyPrepatched=FALSE;",
        "allocation->NativeContext==nativeContext->Registration",
        "allocation->HostState==VioGpuWddmAllocationHostLive",
        "allocation->PlacementValid",
        "allocation->BoundResetGeneration==nativeContext->ResetGeneration",
        "allocation->ResourceId!=MAXUINT",
        "allocation->BlobId==allocation->ResourceId",
        "allocationEntry->SegmentId==VIOGPU_WDDM_SEGMENT_ID",
        "static_cast<ULONGLONG>(allocationEntry->PhysicalAddress.QuadPart)==allocation->PlacementOffset",
        "allocation->PrivateData.RequestedIova<=MAXULONGLONG-reference->AllocationOffset",
        "(reference->PatchOffset&(sizeof(ULONG)-1))!=0",
        "RtlCopyMemory(&bos[index].Handle,&resourceId,sizeof(resourceId));",
        "RtlCopyMemory(commandStream+reference->PatchOffset,&iova,sizeof(iova));",
    ):
        if fragment not in render_prepatch:
            fail(f"Render prepatch must retain its per-reference residency and payload gate: {fragment}")

    acquire_render = canonical_code(function_body("AcquireRenderAllocationReferences", WDDM_DDI_CODE))
    require_order(
        acquire_render,
        (
            "status=AcquireAllocationLifecycle(allocation);",
            "status=AcquireAllocationSubmissionReference(allocation,device->Adapter);",
            "KeReleaseMutex(&allocation->LifecycleMutex,FALSE);",
        ),
        "Render allocation references must serialize against allocation destruction",
    )
    render = canonical_code(function_body("VioGpuWddmRender", WDDM_DDI_CODE))
    require_order(
        render,
        (
            "ValidateNativeSubmitPacket(command,context->Device,render->pAllocationList,render->AllocationListSize,&snapshot)",
            "!snapshot.Adapter->IsNativeContextGenerationCurrent(snapshot.Generation,snapshot.ResetGeneration)",
            "status=AcquireContextSubmissionReference(context);",
            "status=AcquireRenderAllocationReferences(validatedCommand,",
            "status=ApplyRenderPrepatches(reinterpret_cast<VIOGPU_WDDM_RENDER_COMMAND*>(commandSnapshot),",
            "PrepareNativeSubmit(snapshot.ContextId,commandStream,validatedCommand->CommandStreamSize)",
            "submission=new(NonPagedPoolNx)VIOGPU_WDDM_SUBMISSION;",
            "RtlCopyMemory(dmaBuffer,commandSnapshot,render->CommandLength);",
            "status=PublishPreparedSubmission(submission,",
            "submissionPublished=TRUE;",
            "submission=NULL;virtioBuffer=NULL;allocationSubmissionReferences=0;contextSubmissionReference=FALSE;",
        ),
        "Render must validate, retain, prepare and publish one asynchronously owned submission",
    )
    for cleanup in (
        "ReleasePreparedSubmission(submission);",
        "ReleaseNativeSubmitBuffer(virtioBuffer);",
        "ReleaseRenderAllocationReferences(validatedCommand,",
        "ReleaseContextSubmissionReference(context);",
        "ReleaseNativeContextSnapshot(&snapshot);",
        "ReleaseNativeSubmissionOperation();",
    ):
        if cleanup not in render:
            fail(f"Render unified cleanup must release every untransferred owner: {cleanup}")

    release_buffer = canonical_code(function_body("VioGpuDod::ReleaseNativeSubmitBuffer", VIOGPU_CODE))
    require_order(
        release_buffer,
        (
            "if(buffer==NULL)",
            "if(!ExAcquireRundownProtection(&m_HardwareOperations))",
            "VioGpuCompleteVbufferTerminalCallbacks(buffer);",
            "VioGpuAdapter*adapter=m_pHWDevice;",
            "BOOLEANreleased=adapter!=NULL;",
            "adapter->ReleaseNativeSubmitBuffer(buffer);",
            "ExReleaseRundownProtection(&m_HardwareOperations);",
            "if(!released)",
            "returnreleased;",
        ),
        "buffer release must complete a claimed terminal owner when adapter rundown or ownership is unavailable",
    )
    if release_buffer.count("VioGpuCompleteVbufferTerminalCallbacks(buffer);") != 2:
        fail("buffer release must complete terminal ownership on both rundown and missing-adapter failures")
    rundown_failure = release_buffer.find("if(!ExAcquireRundownProtection(&m_HardwareOperations))")
    rundown_completion = release_buffer.find("VioGpuCompleteVbufferTerminalCallbacks(buffer);", rundown_failure)
    adapter_load = release_buffer.find("VioGpuAdapter*adapter=m_pHWDevice;", rundown_completion)
    missing_adapter = release_buffer.find("if(!released)", adapter_load)
    missing_completion = release_buffer.find("VioGpuCompleteVbufferTerminalCallbacks(buffer);", missing_adapter)
    final_return = release_buffer.find("returnreleased;", missing_completion)
    if min(rundown_failure, rundown_completion, adapter_load, missing_adapter, missing_completion, final_return) < 0 or not (
        rundown_failure < rundown_completion < adapter_load < missing_adapter < missing_completion < final_return
    ):
        fail("buffer release must complete each failed terminal owner before returning")

    patch = canonical_code(function_body("VioGpuWddmPatch", WDDM_DDI_CODE))
    patch_offset_helper = canonical_code(
        function_body_with_parameters(
            "IsPatchOffsetForSubmission",
            "_In_ UINT patchOffset, _In_ UINT expectedRelativeOffset, _In_ UINT submissionStartOffset",
            WDDM_DDI_CODE,
        )
    )
    for fragment in (
        "patchOffset==expectedRelativeOffset",
        "submissionStartOffset<=MAXUINT-expectedRelativeOffset",
        "patchOffset==submissionStartOffset+expectedRelativeOffset",
    ):
        if patch_offset_helper.count(fragment) != 1:
            fail(f"PatchOffset validation must accept one bounded relative or full-DMA representation: {fragment}")
    for fragment in (
        "IsPatchOffsetForSubmission(sourcePatch->PatchOffset,FIELD_OFFSET(VIOGPU_WDDM_PRESENT_DMA_PACKET,SourcePlacementOffset),patchArguments->DmaBufferSubmissionStartOffset)",
        "IsPatchOffsetForSubmission(destinationPatch->PatchOffset,FIELD_OFFSET(VIOGPU_WDDM_PRESENT_DMA_PACKET,DestinationPlacementOffset),patchArguments->DmaBufferSubmissionStartOffset)",
    ):
        if patch.count(fragment) != 1:
            fail(f"Present Patch must use the bounded PatchOffset representation helper: {fragment}")
    for fragment in (
        "allocation->HostState==VioGpuWddmAllocationHostLive",
        "allocation->PlacementValid",
        "allocation->ApertureMdl!=NULL",
        "allocation->ApertureAddress!=NULL",
        "allocation->ResourceId>=VIOGPU_NATIVE_RESOURCE_ID_START",
        "allocation->ResourceId!=MAXUINT",
        "allocation->BlobId==allocation->ResourceId",
        "allocation->BoundContextId==submission->ContextId",
        "allocation->BoundGeneration==submission->Generation",
        "allocation->BoundResetGeneration==submission->ResetGeneration",
        "allocationEntry->SegmentId==VIOGPU_WDDM_SEGMENT_ID",
        "static_cast<ULONGLONG>(allocationEntry->PhysicalAddress.QuadPart)==allocation->PlacementOffset",
    ):
        if patch.count(fragment) != 1:
            fail(f"Patch must retain the post-paging residency and placement gate: {fragment}")
    paging_patch_blocks = [
        canonical_code(body)
        for condition, body, _, _ in if_blocks(function_body("VioGpuWddmPatch", WDDM_DDI_CODE))
        if canonical_code(condition) == "patchArguments->Flags.Value==1"
    ]
    if len(paging_patch_blocks) != 1:
        fail("Patch must expose one exact paging no-op branch")
    paging_patch = paging_patch_blocks[0]
    for fragment in (
        "reinterpret_cast<VIOGPU_WDDM_DEVICE*>(patchArguments->hDevice)",
        "ReferenceDevice(device)",
        "deviceReferenced&&device->Adapter==adapter",
        "patchArguments->DmaBufferSubmissionStartOffset==patchArguments->DmaBufferSubmissionEndOffset",
        "patchArguments->DmaBufferPrivateDataSubmissionStartOffset==patchArguments->DmaBufferPrivateDataSubmissionEndOffset",
        "emptySubmission=emptyDmaRange&&emptyPrivateRange",
        "patchArguments->pAllocationList==NULL",
        "patchArguments->AllocationListSize==0",
        "patchArguments->pPatchLocationList==NULL",
        "patchArguments->PatchLocationListSize==0",
        "patchArguments->PatchLocationListSubmissionStart==0",
        "patchArguments->PatchLocationListSubmissionLength==0",
        "emptySubmission||ResolvePagingBatch(",
        "ResolvePagingBatch(",
        "VioGpuWddmPagingTransactionBuilt",
        "if(deviceReferenced)",
        "DereferenceDevice(device);",
        "if(!exact)",
        "RetirePatchDmaOwner(adapter,patchArguments);",
        "returnSTATUS_SUCCESS;",
    ):
        if fragment not in paging_patch:
            fail(f"paging Patch must validate one exact no-op submission range: {fragment}")
    if "AcquireNativeSubmissionOperation" in paging_patch or "RecordNativeSubmissionFence" in paging_patch or \
       "NotifyNativeSubmission" in paging_patch:
        fail("paging Patch must remain a validation-and-retirement-only no-op")
    require_order(
        paging_patch,
        (
            "deviceReferenced=ReferenceDevice(device);",
            "deviceValid=deviceReferenced&&device->Adapter==adapter;",
            "BOOLEANexact=deviceValid",
            "emptySubmission||ResolvePagingBatch(",
            "if(deviceReferenced)",
            "DereferenceDevice(device);",
            "if(!exact)",
            "RetirePatchDmaOwner(adapter,patchArguments);",
        ),
        "paging Patch must hold the device reference through batch validation and retire invalid KMD ownership afterward",
    )
    require_order(
        patch,
        (
            "ResolveSubmissionPrivateData(",
            "patchedResourceIds=new(NonPagedPoolNx)UINT[submission->AllocationCount];",
            "patchedIovas=new(NonPagedPoolNx)ULONGLONG[submission->AllocationCount];",
            "IsPatchOffsetForSubmission(patch->PatchOffset,expectedPatchOffset,patchArguments->DmaBufferSubmissionStartOffset)",
            "allocation->PrivateData.RequestedIova<=MAXULONGLONG-reference->AllocationOffset",
            "patchedResourceIds[index]=allocation->ResourceId;",
            "patchedIovas[index]=allocation->PrivateData.RequestedIova+reference->AllocationOffset;",
            "RtlCopyMemory(&submitBo->Handle,&patchedResourceIds[index],sizeof(patchedResourceIds[index]));",
            "RtlCopyMemory(patchAddress,&patchedIovas[index],sizeof(patchedIovas[index]));",
            "adapter->RefreshNativeSubmit(submission->VirtioBuffer,submission->CommandStream,submission->CommandStreamSize)",
            "submission->FenceId=patchArguments->SubmissionFenceId;KeMemoryBarrier();submission->PatchApplied=TRUE;",
            "delete[]patchedIovas;",
            "delete[]patchedResourceIds;",
        ),
        "Patch must validate placement, write resource IDs and requested IOVAs, refresh payload and publish the WDDM fence",
    )
    if "ReleasePreparedSubmission(submission);" not in patch:
        fail("Patch failure must quarantine the prepared submission")
    if not patch.endswith(
        "DereferenceRenderSubmission(submission);}delete[]patchedIovas;delete[]patchedResourceIds;"
        "adapter->ReleaseNativeSubmissionOperation();returnSTATUS_SUCCESS;"
    ):
        fail("Patch must release its resolver reference after success or quarantine failure")

    submit_body = function_body("VioGpuWddmSubmitCommand", WDDM_DDI_CODE)
    submit = canonical_code(submit_body)
    if "KeGetCurrentIrql()!=DISPATCH_LEVEL" not in submit:
        fail("SubmitCommand must enforce its DISPATCH_LEVEL contract")
    if "submitCommand->SubmissionFenceId>MAXUINT" not in submit:
        fail("SubmitCommand must reject fence values that the bounded tracker cannot represent")
    for forbidden in ("KeWaitForSingleObject", "PAGED_CODE", "new(", "ExAllocatePool"):
        if forbidden in submit_body:
            fail(f"SubmitCommand must not wait or allocate pageable state at DISPATCH_LEVEL: {forbidden}")
    empty_paging_blocks = [
        canonical_code(body)
        for condition, body, _, _ in if_blocks(submit_body)
        if canonical_code(condition) == "emptyPagingSubmission"
    ]
    if len(empty_paging_blocks) != 1:
        fail("SubmitCommand must expose one empty paging completion path")
    empty_paging = empty_paging_blocks[0]
    for fragment in (
        "pagingSubmission=submitCommand!=NULL&&submitCommand->Flags.Paging!=0",
        "submitCommand->Flags.Value==1",
        "submitCommand->DmaBufferSubmissionStartOffset==submitCommand->DmaBufferSubmissionEndOffset",
        "submitCommand->DmaBufferPrivateDataSubmissionStartOffset==submitCommand->DmaBufferPrivateDataSubmissionEndOffset",
        "adapter->QueueNativeSoftwareSubmissionCompletion(submitCommand->SubmissionFenceId,submitCommand->NodeOrdinal,submitCommand->EngineOrdinal)",
        "adapter->RequestHardwareResetAtAnyIrql();",
        "returnSTATUS_SUCCESS;",
    ):
        if fragment not in submit and fragment not in empty_paging:
            fail(f"empty paging SubmitCommand must publish a system-safe tracked completion: {fragment}")
    validation_failure_blocks = [
        canonical_code(body)
        for condition, body, _, _ in if_blocks(submit_body)
        if "adapter == NULL || submitCommand == NULL" in condition
    ]
    if len(validation_failure_blocks) != 1:
        fail("SubmitCommand must expose one top-level argument validation block")
    validation_failure = validation_failure_blocks[0]
    for fragment in (
        "if(adapter!=NULL&&submitCommand!=NULL&&KeGetCurrentIrql()==DISPATCH_LEVEL&&pagingSubmission)",
        "adapter->QueueNativeSoftwareSubmissionCompletion(submitCommand->SubmissionFenceId,submitCommand->NodeOrdinal,submitCommand->EngineOrdinal);",
        "adapter->RequestHardwareResetAtAnyIrql();",
        "returnSTATUS_SUCCESS;",
        "returnSTATUS_INVALID_PARAMETER;",
    ):
        if fragment not in validation_failure:
            fail(f"malformed paging validation must fail closed without failing a system command: {fragment}")
    if "NotifyNativeSubmissionFault(" in empty_paging or "ReleaseNativeSubmissionOperation" in empty_paging:
        fail("empty paging completion must run before the hardware operation gate and never fault the system command")
    empty_branch = submit.find("if(emptyPagingSubmission)")
    operation_gate = submit.find("if(!adapter->AcquireNativeSubmissionOperation())")
    private_dereference = submit.find("VIOGPU_WDDM_KMD_DMA_PRIVATE*privateData=")
    if min(empty_branch, operation_gate, private_dereference) < 0 or not empty_branch < operation_gate < private_dereference:
        fail("empty paging SubmitCommand must complete before both the hardware gate and private-data dereference")
    operation_gate_blocks = [
        canonical_code(body)
        for condition, body, _, _ in if_blocks(submit_body)
        if canonical_code(condition) == "!adapter->AcquireNativeSubmissionOperation()"
    ]
    if len(operation_gate_blocks) != 1:
        fail("SubmitCommand must expose one hardware operation gate")
    operation_failure = operation_gate_blocks[0]
    for fragment in (
        "RetireUnsubmittedDmaOwner(adapter,submitCommand);",
        "if(pagingSubmission)",
        "adapter->QueueNativeSoftwareSubmissionCompletion(submitCommand->SubmissionFenceId,submitCommand->NodeOrdinal,submitCommand->EngineOrdinal)",
        "adapter->RequestHardwareResetAtAnyIrql();",
        "else{adapter->NotifyNativeSubmissionFault(",
        "returnSTATUS_SUCCESS;",
    ):
        if fragment not in operation_failure:
            fail(f"operation-gate failure must separate system and client command recovery: {fragment}")
    require_order(
        submit,
        (
            "status=ResolveSubmissionPrivateData(",
            "submission->PatchApplied",
            "submission->FullyPrepatched",
            "ValidateRenderSubmitDmaRange(submission,",
            "VioGpuWddmSubmissionSubmitClaimed",
            "submission->FenceId=submitCommand->SubmissionFenceId;KeMemoryBarrier();",
            "RecordContextUmdFence(submission->Context,submission->UmdFenceId)",
            "AcquireRenderWorkReference(submission)",
            "VioGpuWddmSubmissionEngineQueued",
            "adapter->QueueNativePassiveWork(&submission->Work,submitCommand->SubmissionFenceId)",
            "InvalidateContextUmdFenceTracker(submission->Context)",
        ),
        "SubmitCommand must require Patch or a full Render prepatch before FIFO publication",
    )
    paging_worker = canonical_code(function_body("NativePagingBatchWorker", WDDM_DDI_CODE))
    for fragment in (
        "privateData->Kind==VioGpuWddmDmaKindPaging",
        "reinterpret_cast<VIOGPU_WDDM_DEVICE*>(submitCommand->hDevice)",
        "deviceReferenced=ReferenceDevice(device)",
        "deviceReferenced&&device->Adapter==adapter",
        "ResolvePagingBatch(",
        "packet->ContextId!=0&&!adapter->IsNativeContextGenerationCurrent(packet->ContextGeneration,packet->ResetGeneration)",
        "adapter->QueueNativePassiveWork(&firstPrivate->Work,submitCommand->SubmissionFenceId)",
        "adapter->QueueNativeSoftwareSubmissionCompletion(",
        "adapter->CompleteNativeSystemSubmission(",
        "adapter->RequestHardwareResetAtAnyIrql();",
    ):
        if fragment not in submit and fragment not in paging_worker:
            fail(f"SubmitCommand must retain paging completion and reset semantics: {fragment}")
    if "NotifyNativeSubmissionFault(" in paging_worker:
        fail("paging worker must complete the system fence and request reset without emitting DMA_FAULTED")
    if not submit.endswith("returnSTATUS_SUCCESS;") or "returnSTATUS_NOT_SUPPORTED;" in submit:
        fail("SubmitCommand must convert post-validation scheduler failures into fault notification plus success")
    submit_queue = submit.find("adapter->QueueNativePassiveWork(&submission->Work,submitCommand->SubmissionFenceId)")
    submit_success_release = submit.find("DereferenceRenderSubmission(submission);", submit_queue)
    submit_success_return = submit.find("returnSTATUS_SUCCESS;", submit_success_release)
    submit_failure_work_release = submit.find("ReleaseRenderWorkReference(submission);", submit_success_return)
    submit_failure_release = submit.find("DereferenceRenderSubmission(submission);", submit_failure_work_release)
    if min(
        submit_queue,
        submit_success_release,
        submit_success_return,
        submit_failure_work_release,
        submit_failure_release,
    ) < 0 or not (
        submit_queue
        < submit_success_release
        < submit_success_return
        < submit_failure_work_release
        < submit_failure_release
    ):
        fail("SubmitCommand must release its resolver reference on both FIFO publication and every failure unwind")

    complete = canonical_code(function_body("NativeSubmissionComplete", WDDM_DDI_CODE))
    for fragment in (
        "response->type==VIRTIO_GPU_RESP_OK_NODATA",
        "response->flags==expectedFlags",
        "response->fence_id==submission->FenceId",
        "response->ctx_id==submission->ContextId",
        "response->ring_idx==1",
        "adapter->IsNativeContextGenerationCurrent(submission->Generation,submission->ResetGeneration)",
        "RetireContextUmdFence(context,submission->UmdFenceId)",
        "QuarantineTerminalSubmission(submission,TRUE)",
        "adapter->NotifyNativeSubmissionCompletion(fenceId,nodeOrdinal,engineOrdinal,FALSE);",
        "adapter->CompleteNativePassiveWork(&submission->Work);",
        "ReleaseRenderWorkReference(submission);",
        "adapter->NotifyNativeSubmissionFault(",
    ):
        if fragment not in complete:
            fail(f"native completion must validate Host retirement before VidSch notification: {fragment}")
    if "!ReferenceRenderSubmission(submission)" not in complete or \
       complete.count("DereferenceRenderSubmission(submission);") != 3 or \
       not complete.endswith("DereferenceRenderSubmission(submission);"):
        fail("native completion callback must retain one temporary reference through every terminal path")
    cancelled = canonical_code(function_body("NativeSubmissionCancelled", WDDM_DDI_CODE))
    if "QuarantineTerminalSubmission(submission,FALSE)" not in cancelled or (
        "NotifyNativeSubmissionCompletion" in cancelled
    ):
        fail("reset cancellation must release ownership without fabricating a completed fence")
    if "ReferenceRenderSubmission(submission)" not in cancelled or \
       cancelled.count("DereferenceRenderSubmission(submission);") != 1 or \
       not cancelled.endswith("DereferenceRenderSubmission(submission);"):
        fail("native cancellation callback must retain and release one temporary Render reference")
    queue_failed = canonical_code(function_body("NativeSubmissionQueueFailed", WDDM_DDI_CODE))
    queue_failed_main_start = queue_failed.find("UINTfenceId=static_cast<UINT>(submission->FenceId);")
    if queue_failed_main_start < 0:
        fail("permanent backlog failure must enter its terminal path only after validating the scheduler fence")
    queue_failed_main = queue_failed[queue_failed_main_start:]
    require_order(
        queue_failed_main,
        (
            "InvalidateContextUmdFenceTracker(submission->Context);",
            "QuarantineTerminalSubmission(submission,FALSE)",
            "adapter->NotifyNativeSubmissionFault(",
            "adapter->CompleteNativePassiveWork(&submission->Work);",
            "ReleaseRenderWorkReference(submission);",
        ),
        "permanent backlog enqueue failure must release ownership and fault the scheduler",
    )
    if "!ReferenceRenderSubmission(submission)" not in queue_failed or \
       queue_failed.count("DereferenceRenderSubmission(submission);") != 2 or \
       not queue_failed.endswith("DereferenceRenderSubmission(submission);"):
        fail("native queue-failure callback must retain one temporary reference through every terminal path")
    render_worker_body = function_body("NativeRenderDispatchWorker", WDDM_DDI_CODE)
    render_worker = canonical_code(render_worker_body)
    render_issue_claim = (
        "InterlockedCompareExchange(&submission->State,VioGpuWddmSubmissionHostIssued,"
        "VioGpuWddmSubmissionEngineQueued)!=VioGpuWddmSubmissionEngineQueued"
    )
    if render_worker.count(render_issue_claim) != 1:
        fail("the Render FIFO worker must claim EngineQueued-to-HostIssued exactly once")
    admission_failure_blocks = [
        canonical_code(body)
        for condition, body, _, _ in if_blocks(render_worker_body)
        if render_issue_claim in canonical_code(condition)
    ]
    if len(admission_failure_blocks) != 1:
        fail("the Render FIFO worker must expose one exact Host-issue claim failure block")
    require_order(
        admission_failure_blocks[0],
        (
            "QuarantineSubmission(submission,VioGpuWddmSubmissionEngineQueued,TRUE);",
            "adapter->RequestHardwareResetAtAnyIrql();",
            "adapter->CompleteNativePassiveWork(&submission->Work);",
            "ReleaseRenderWorkReference(submission);",
        ),
        "a failed Render Host-issue claim must quarantine, reset, complete passive work, and release its work owner",
    )
    for fragment in (
        "QuarantineSubmission(submission,VioGpuWddmSubmissionEngineQueued,TRUE);",
        "adapter->RequestHardwareResetAtAnyIrql();",
        "adapter->AcquireNativeSubmissionOperation();",
        "adapter->QueueNativeSubmit(submission->VirtioBuffer,fenceId)",
        "adapter->ReleaseNativeSubmissionOperation();",
        "if(queueResult>=0){return;}",
        "InvalidateContextUmdFenceTracker(submission->Context);",
        "QuarantineSubmission(submission,VioGpuWddmSubmissionHostIssued,TRUE);",
        "adapter->NotifyNativeSubmissionFault(",
    ):
        if render_worker.count(fragment) != 1:
            fail(f"the Render FIFO worker must retain one exact dispatch operation: {fragment}")
    if render_worker.count("adapter->CompleteNativePassiveWork(&submission->Work);") != 2 or \
       render_worker.count("ReleaseRenderWorkReference(submission);") != 2:
        fail("both Render dispatch failure paths must complete passive work and release the work reference")
    dispatch_failure = render_worker[render_worker.find("InvalidateContextUmdFenceTracker(submission->Context);") :]
    require_order(
        dispatch_failure,
        (
            "InvalidateContextUmdFenceTracker(submission->Context);",
            "QuarantineSubmission(submission,VioGpuWddmSubmissionHostIssued,TRUE);",
            "adapter->NotifyNativeSubmissionFault(",
            "adapter->CompleteNativePassiveWork(&submission->Work);",
            "ReleaseRenderWorkReference(submission);",
        ),
        "a failed Host enqueue must quarantine before fault publication and passive-work retirement",
    )
    render_dispatch_cancelled = canonical_code(function_body("NativeRenderDispatchCancelled", WDDM_DDI_CODE))
    require_order(
        render_dispatch_cancelled,
        (
            "InterlockedExchange(&submission->CancelRequested,1);",
            "InvalidateContextUmdFenceTracker(submission->Context);",
            "QuarantineSubmission(submission,VioGpuWddmSubmissionEngineQueued,TRUE);",
            "ReleaseRenderWorkReference(submission);",
        ),
        "a FIFO-removed Render must quarantine the registry owner before releasing its work reference",
    )
    for fragment in (
        "InterlockedExchange(&submission->CancelRequested,1);",
        "InvalidateContextUmdFenceTracker(submission->Context);",
        "QuarantineSubmission(submission,VioGpuWddmSubmissionEngineQueued,TRUE);",
        "ReleaseRenderWorkReference(submission);",
    ):
        if render_dispatch_cancelled.count(fragment) != 1:
            fail(f"Render FIFO cancellation must perform exactly one terminal operation: {fragment}")
    if "CompleteNativePassiveWork" in render_dispatch_cancelled:
        fail("a FIFO-removed Render must not complete work that no worker owns")
    fault = canonical_code(function_body("VioGpuDod::NotifyNativeSubmissionFault", VIOGPU_CODE))
    if "InvalidateNativeFenceTracker();" not in fault or "ResetNativeFenceTracker();" in fault:
        fail("a native submission fault must clear pending fences while preserving the submitted reset endpoint")
    require_order(
        fault,
        (
            "BOOLEANvalidIdentity=fenceId!=0&&nodeOrdinal==0&&engineOrdinal==0;",
            "RequestHardwareResetAtAnyIrql();",
            "InvalidateNativeFenceTracker();",
            "adapter->FailNativeContextAtAnyIrql();",
            "if(!validIdentity){return;}",
            "notify.InterruptType=DXGK_INTERRUPT_DMA_FAULTED;",
            "NotifyNativeSchedulerInterrupt(&notify,queueDpc);",
        ),
        "a malformed fault identity must still close outer and inner publication before suppressing the scheduler callback",
    )

    invalidate = canonical_code(function_body("VioGpuDod::InvalidateNativeFenceTracker", VIOGPU_CODE))
    if (
        "m_NativeFenceHead=0;" not in invalidate
        or "m_NativeFenceCount=0;" not in invalidate
        or "RtlZeroMemory(m_NativeFences,sizeof(m_NativeFences));" not in invalidate
        or "m_NativeSubmittedFence" in invalidate
        or "m_NativeCompletedFence" in invalidate
    ):
        fail("fault invalidation must discard pending entries without publishing a fence endpoint")

    complete_reset = canonical_code(function_body("VioGpuDod::CompleteNativeFenceReset", VIOGPU_CODE))
    require_order(
        complete_reset,
        (
            "submitted=static_cast<UINT>(InterlockedCompareExchange(&m_NativeSubmittedFence,0,0));",
            "m_NativeFenceCount=0;",
            "InterlockedExchange(&m_NativeCompletedFence,static_cast<LONG>(submitted));",
        ),
        "a successful adapter reset must advance completed fence to the last submitted endpoint",
    )

    preempt = canonical_code(function_body("VioGpuWddmPreemptCommand", WDDM_DDI_CODE))
    if "returnSTATUS_NOT_SUPPORTED;" in preempt or not preempt.endswith("returnSTATUS_SUCCESS;"):
        fail("PreemptCommand must remain a non-failing scheduler callback")
    require_order(
        preempt,
        (
            "preemptCommand->PreemptionFenceId!=0",
            "adapter->IsNativeFenceQueueEmpty()",
            "adapter->ResetDevice();",
            "notify.InterruptType=DXGK_INTERRUPT_DMA_PREEMPTED;",
            "notify.DmaPreempted.PreemptionFenceId=preemptCommand->PreemptionFenceId;",
            "notify.DmaPreempted.LastCompletedFenceId=adapter->QueryNativeCompletedFence();",
            "adapter->NotifyNativeSchedulerInterrupt(&notify,TRUE)",
        ),
        "preemption must reset on an in-flight native queue and notify only an idle queue",
    )
    if "preemptCommand->Flags.Value==0" not in preempt:
        fail("PreemptCommand must reject reserved preemption flags without returning an error")
    preempt_blocks = [
        canonical_code(body)
        for condition, body, _, _ in if_blocks(function_body("VioGpuWddmPreemptCommand", WDDM_DDI_CODE))
        if "!adapter->IsNativeFenceQueueEmpty()" in canonical_code(condition)
    ]
    if len(preempt_blocks) != 1 or "adapter->ResetDevice();" not in preempt_blocks[0]:
        fail("an in-flight preemption request must gate the adapter before TDR")
    notify_failure_blocks = [
        canonical_code(body)
        for condition, body, _, _ in if_blocks(function_body("VioGpuWddmPreemptCommand", WDDM_DDI_CODE))
        if "!adapter->NotifyNativeSchedulerInterrupt(&notify,TRUE)" in canonical_code(condition)
    ]
    if len(notify_failure_blocks) != 1 or "adapter->ResetDevice();" not in notify_failure_blocks[0]:
        fail("a failed preemption interrupt notification must gate the adapter for TDR")
    driver_caps = canonical_code(function_body("VioGpuWddmQueryAdapterInfo", WDDM_DDI_CODE))
    if driver_caps.count("driverCaps->WDDMVersion=DXGKDDI_WDDMv1;") != 1:
        fail("the Native Context target must report the legacy WDDM profile until WDDM 1.2 caps exist")
    if driver_caps.count("driverCaps->SchedulingCaps.PreemptionAware=0;") != 1:
        fail("the legacy Native Context profile must not advertise Windows 8 hardware preemption")
    if "driverCaps->SupportPerEngineTDR" in driver_caps or "driverCaps->SupportSmoothRotation" in driver_caps:
        fail("the Win7 DriverCaps response must not access fields beyond the legacy prefix")

    reset_timeout = canonical_code(function_body("VioGpuDod::ResetFromTimeout", VIOGPU_CODE))
    require_order(
        reset_timeout,
        (
            "InterlockedExchange(&m_HardwareResetState,VioGpuHardwareResetRequested);",
            "RequestWddmSubmissionDrainAtAnyIrql();",
            "if(!WaitForWddmSubmissionDrain())",
            "ExAcquireRundownProtection(&m_HardwareOperations)",
            "adapter->ResetDevice();",
            "adapter->SetPowerState(&m_DeviceInfo,PowerDeviceD3,&m_CurrentMode);",
            "InvalidateNativeFenceTracker();",
            "CompleteNativeFenceReset();",
        ),
        "ResetFromTimeout must gate, drain submitters, tear down, and publish the reset fence only after transport stop",
    )
    reset_success_blocks = [
        canonical_code(body)
        for condition, body, _, _ in if_blocks(function_body("VioGpuDod::ResetFromTimeout", VIOGPU_CODE))
        if is_success_condition(condition, "status")
    ]
    if len(reset_success_blocks) != 1 or "CompleteNativeFenceReset();" not in reset_success_blocks[0]:
        fail("ResetFromTimeout may publish the reset fence only on a successful transport stop")
    set_power_body = function_body("VioGpuDod::SetPowerState", VIOGPU_CODE)
    d0_fence_blocks = [
        canonical_code(body)
        for condition, body, _, _ in if_blocks(set_power_body)
        if canonical_code(condition) == "resetRecovery" and "CompleteNativeFenceReset();" in canonical_code(body)
    ]
    if d0_fence_blocks != ["CompleteNativeFenceReset();"]:
        fail("the D0 recovery path must condition reset-fence publication on its Recovering claim")
    require_order(
        canonical_code(set_power_body),
        (
            "WaitForNativePassiveQueueIdle()",
            "InterlockedCompareExchange(&m_HardwareResetState,VioGpuHardwareActive,VioGpuHardwareRecovering)",
            "CompleteNativeFenceReset();",
            "OpenNativePassiveQueue()",
            "OpenWddmPresentTransactions()",
        ),
        "D0 recovery must claim Active before publishing the reset fence and reopening submission",
    )
    restart_timeout = canonical_code(function_body("VioGpuDod::RestartFromTimeout", VIOGPU_CODE))
    if "SetPowerState(DISPLAY_ADAPTER_HW_ID,PowerDeviceD0,PowerActionNone)" not in restart_timeout:
        fail("RestartFromTimeout must reuse the checked D0 recovery state machine")
    if "ExAcquireRundownProtection(&m_HardwareOperations)" not in restart_timeout:
        fail("RestartFromTimeout must hold hardware rundown while recovering the adapter")
    if "InterlockedExchange(&m_HardwareResetState,VioGpuHardwareResetRequested);" not in restart_timeout:
        fail("failed TDR restart must remain fail-closed")
    reset_ddi = canonical_code(function_body("VioGpuWddmResetFromTimeout", WDDM_DDI_CODE))
    restart_ddi = canonical_code(function_body("VioGpuWddmRestartFromTimeout", WDDM_DDI_CODE))
    if (
        "PAGED_CODE();" not in reset_ddi
        or "adapter->ResetFromTimeout()" not in reset_ddi
        or "STATUS_NOT_SUPPORTED" in reset_ddi
    ):
        fail("the pageable reset DDI must dispatch to the adapter-wide TDR teardown")
    if (
        "PAGED_CODE();" not in restart_ddi
        or "adapter->RestartFromTimeout()" not in restart_ddi
        or "STATUS_NOT_SUPPORTED" in restart_ddi
    ):
        fail("the pageable restart DDI must dispatch to the checked D0 recovery path")

    tdr_header = canonical_code(WDDM_DDI_HEADER_CODE)
    for fragment in (
        "VioGpuWddmDebugSnapshotVersion=1,",
        "structVIOGPU_WDDM_DEBUG_SNAPSHOT",
        "ULONGSignature;USHORTVersion;USHORTSize;UINTReason;LONGHardwareResetState;"
        "UINTSubmittedFence;UINTCompletedFence;UINTCurrentIrql;UINTReserved;",
        "static_assert(sizeof(VIOGPU_WDDM_DEBUG_SNAPSHOT)==32,",
    ):
        if tdr_header.count(fragment) != 1:
            fail(f"TDR debug collection must retain one fixed snapshot contract: {fragment}")

    collect_debug = canonical_code(function_body("VioGpuWddmCollectDbgInfo", WDDM_DDI_CODE))
    for fragment in (
        "if(collectDbgInfo==NULL){returnSTATUS_UNSUCCESSFUL;}",
        "RtlZeroMemory(collectDbgInfo->pExtension,sizeof(*collectDbgInfo->pExtension));",
        "if(collectDbgInfo->pBuffer==NULL||collectDbgInfo->BufferSize==0){returnSTATUS_SUCCESS;}",
        "snapshot.Signature=VIOGPU_WDDM_DEBUG_SIGNATURE;",
        "snapshot.Version=VioGpuWddmDebugSnapshotVersion;",
        "snapshot.Size=static_cast<USHORT>(sizeof(snapshot));",
        "snapshot.Reason=collectDbgInfo->Reason;",
        "snapshot.HardwareResetState=adapter->QueryHardwareResetState();",
        "snapshot.SubmittedFence=adapter->QueryNativeSubmittedFence();",
        "snapshot.CompletedFence=adapter->QueryNativeCompletedFence();",
        "snapshot.CurrentIrql=static_cast<UINT>(KeGetCurrentIrql());",
        "SIZE_TcopySize=collectDbgInfo->BufferSize<sizeof(snapshot)?collectDbgInfo->BufferSize:sizeof(snapshot);",
        "RtlCopyMemory(collectDbgInfo->pBuffer,&snapshot,copySize);",
    ):
        if collect_debug.count(fragment) != 1:
            fail(f"CollectDbgInfo must publish one bounded atomic debug snapshot: {fragment}")

    dependent_group = canonical_code(function_body("VioGpuWddmQueryDependentEngineGroup", WDDM_DDI_CODE))
    for fragment in (
        "PAGED_CODE();",
        "queryDependentEngineGroup->NodeOrdinal==0",
        "queryDependentEngineGroup->EngineOrdinal==0",
        "queryDependentEngineGroup->DependentNodeOrdinalMask=valid?1ULL:0ULL;",
        "returnSTATUS_SUCCESS;",
    ):
        if dependent_group.count(fragment) != 1:
            fail(f"dependent-engine query must retain the single-node fail-closed contract: {fragment}")

    engine_status = canonical_code(function_body("VioGpuWddmQueryEngineStatus", WDDM_DDI_CODE))
    for fragment in (
        "PAGED_CODE();",
        "queryEngineStatus->EngineStatus.Value=0;",
        "queryEngineStatus->NodeOrdinal==0",
        "queryEngineStatus->EngineOrdinal==0",
        "queryEngineStatus->EngineStatus.Responsive=!adapter->IsHardwareResetRequested();",
        "returnSTATUS_SUCCESS;",
    ):
        if engine_status.count(fragment) != 1:
            fail(f"engine status must report only the single adapter reset state: {fragment}")
    if "IsNativeFenceQueueEmpty" in engine_status:
        fail("pending fences must not be misreported as an unresponsive engine")

    reset_engine = canonical_code(function_body("VioGpuWddmResetEngine", WDDM_DDI_CODE))
    require_order(
        reset_engine,
        (
            "PAGED_CODE();",
            "resetEngine->LastAbortedFenceId=adapter==NULL?0:adapter->QueryNativeCompletedFence();",
            "resetEngine->NodeOrdinal!=0||resetEngine->EngineOrdinal!=0",
            "adapter->RequestHardwareResetAtAnyIrql();",
            "returnSTATUS_NOT_SUPPORTED;",
        ),
        "ResetEngine must request adapter-wide recovery and fail to promote TDR when Host cannot reset one engine",
    )

    tracker_header = canonical_code(VIOGPU_HEADER_CODE)
    for field in (
        "VioGpuNativeFenceTrackerCapacity=4096,",
        "KSPIN_LOCKm_NativeFenceLock;",
        "UINTm_NativeFenceHead;",
        "UINTm_NativeFenceCount;",
        "VIOGPU_NATIVE_FENCE_ENTRYm_NativeFences[VioGpuNativeFenceTrackerCapacity];",
    ):
        if tracker_header.count(field) != 1:
            fail(f"node fence retirement must retain one bounded ordered tracker field: {field}")
    record_fence = canonical_code(function_body("VioGpuDod::RecordNativeSubmissionFence", VIOGPU_CODE))
    for fragment in (
        "m_NativeFenceCount<VioGpuNativeFenceTrackerCapacity",
        "static_cast<LONG>(fenceId-submitted)>0",
        "m_NativeFences[index].FenceId==fenceId",
        "m_NativeFences[tail].State=VioGpuNativeFencePending;",
        "++m_NativeFenceCount;",
    ):
        if fragment not in record_fence:
            fail(f"fence publication must reject duplicates and preserve submit order: {fragment}")
    retire_fence = canonical_code(function_body("VioGpuDod::RetireNativeSubmissionFence", VIOGPU_CODE))
    require_order(
        retire_fence,
        (
            "match->State=VioGpuNativeFenceRetired;",
            "m_NativeFences[m_NativeFenceHead].State==VioGpuNativeFenceRetired",
            "*completedFence=head->FenceId;",
            "--m_NativeFenceCount;",
        ),
        "completed fence publication must drain only the contiguous retired prefix",
    )
    if (
        "InterlockedExchange(&m_NativeCompletedFence,static_cast<LONG>(*completedFence));" not in retire_fence
        and "InterlockedExchange(reinterpret_cast<PLONG>(&m_NativeCompletedFence),static_cast<LONG>(*completedFence));"
        not in retire_fence
    ):
        fail("completed fence publication must use an atomic store after draining the contiguous retired prefix")
    notify = canonical_code(function_body("VioGpuDod::NotifyNativeSchedulerInterrupt", VIOGPU_CODE))
    if "DxgkCbSynchronizeExecution(" not in notify or "DxgkCbNotifyInterrupt(" in notify:
        fail("DPC/Submit completion must synchronize the actual interrupt notification to DIRQL")
    dirql = canonical_code(function_body("VioGpuNotifyNativeSchedulerAtDirql", VIOGPU_CODE))
    if dirql.count("DxgkCbNotifyInterrupt(") != 1:
        fail("the synchronized DIRQL callback must own the only native scheduler interrupt call")
    current_fence = canonical_code(function_body("VioGpuWddmQueryCurrentFence", WDDM_DDI_CODE))
    if "currentFence->CurrentFence=adapter->QueryNativeCompletedFence();" not in current_fence:
        fail("QueryCurrentFence must return the contiguous Host-retired node fence")

    queue_submit = canonical_code(function_body("CtrlQueue::QueueNativeSubmit", QUEUE_CODE))
    if queue_submit.count("result==-ENOSPC") != 1 or "InsertTailList(&m_NativeSubmitBacklog,&buf->native_submit_link);" not in queue_submit:
        fail("native submit may backlog only transient virtqueue ENOSPC")
    if "elseif(result<0)" not in queue_submit or "InterlockedExchange(&m_NativeSubmitBacklogPoisoned,1);" not in queue_submit:
        fail("permanent direct enqueue failure must poison the native submit generation")
    drain = canonical_code(function_body("CtrlQueue::DrainNativeSubmitBacklog", QUEUE_CODE))
    require_order(
        drain,
        (
            "if(InterlockedCompareExchange(&m_NativeSubmitBacklogPoisoned,0,0)!=0)",
            "KeReleaseSpinLock(&m_NativeSubmitLock,oldIrql);return;",
            "while(!IsListEmpty(&m_NativeSubmitBacklog))",
            "if(result==-ENOSPC)",
            "InsertHeadList(&m_NativeSubmitBacklog,&buffer->native_submit_link);",
            "if(result<0)",
            "InterlockedExchange(&m_NativeSubmitBacklogPoisoned,1);",
            "while(!IsListEmpty(&m_NativeSubmitBacklog)){PLIST_ENTRYremaining=RemoveHeadList(&m_NativeSubmitBacklog);",
            "while(!IsListEmpty(&quarantined))",
            "errorCallback(errorContext);",
            "ReleaseBuffer(failedBuffer);",
        ),
        "backlog drain must preserve ENOSPC order and quarantine every permanent enqueue failure",
    )
    adapter_submit_acquire = canonical_code(function_body("VioGpuAdapter::AcquireNativeSubmitOperation", VIOGPU_CODE))
    if (
        "KeAcquireSpinLock(&m_NativeSubmitRundownLock,&oldIrql);" not in adapter_submit_acquire
        or "!m_NativeSubmitClosing" not in adapter_submit_acquire
        or "ExAcquireRundownProtection(&m_NativeSubmitRundown)" not in adapter_submit_acquire
        or "KeReleaseSpinLock(&m_NativeSubmitRundownLock,oldIrql);" not in adapter_submit_acquire
    ):
        fail("native submit acquisition must gate rundown admission under the closing lock")
    adapter_submit_release = canonical_code(function_body("VioGpuAdapter::ReleaseNativeSubmitOperation", VIOGPU_CODE))
    if adapter_submit_release.count("ExReleaseRundownProtection(&m_NativeSubmitRundown)") != 1:
        fail("native submit release must drop the independent adapter rundown")
    complete_submit = canonical_code(function_body("VioGpuAdapter::CompleteNativeSubmitRundown", VIOGPU_CODE))
    require_order(
        complete_submit,
        (
            "m_NativeSubmitClosing=TRUE;",
            "ExWaitForRundownProtectionRelease(&m_NativeSubmitRundown);",
            "ExRundownCompleted(&m_NativeSubmitRundown);",
            "m_NativeSubmitRundownCompleted=TRUE;",
        ),
        "transport teardown must close and drain the independent native submit rundown",
    )
    reinit_submit = canonical_code(function_body("VioGpuAdapter::ReinitializeNativeSubmitRundown", VIOGPU_CODE))
    require_order(
        reinit_submit,
        (
            "ExReInitializeRundownProtection(&m_NativeSubmitRundown);",
            "m_NativeSubmitRundownCompleted=FALSE;",
            "m_NativeSubmitClosing=FALSE;",
        ),
        "a new transport generation must reinitialize native submit rundown admission",
    )
    dod_submit_acquire = canonical_code(function_body("VioGpuDod::AcquireNativeSubmissionOperation", VIOGPU_CODE))
    if "adapter->AcquireNativeSubmitOperation()" not in dod_submit_acquire:
        fail("WDDM native operations must acquire the adapter submit rundown after hardware lifetime")
    dod_submit_release = canonical_code(function_body("VioGpuDod::ReleaseNativeSubmissionOperation", VIOGPU_CODE))
    if "adapter->ReleaseNativeSubmitOperation()" not in dod_submit_release:
        fail("WDDM native operations must release the adapter submit rundown before hardware lifetime")
    begin = canonical_code(function_body("VioGpuAdapter::BeginNativeContextInitialization", VIOGPU_CODE))
    if "ReinitializeNativeSubmitRundown()" not in begin:
        fail("transport initialization must reinitialize native submit rundown admission")
    stop = canonical_code(function_body("VioGpuAdapter::StopNativeContextTransportLocked", VIOGPU_CODE))
    poison = canonical_code(function_body("CtrlQueue::PoisonNativeSubmitBacklog", QUEUE_CODE))
    require_order(
        poison,
        (
            "KeAcquireSpinLock(&m_NativeSubmitLock,&oldIrql);",
            "InterlockedExchange(&m_NativeSubmitBacklogPoisoned,1);",
            "KeReleaseSpinLock(&m_NativeSubmitLock,oldIrql);",
        ),
        "transport submit poison must synchronize with active queue/backlog publication",
    )
    require_order(
        stop,
        (
            "m_CtrlQueue.PoisonNativeSubmitBacklog();",
            "CompleteNativeSubmitRundown();",
            "InvalidateNativeContextRegistrationsLocked();",
            "virtio_device_reset_checked(&m_VioDev)",
            "virtio_delete_queues(&m_VioDev);",
        ),
        "teardown must close native submit publication before context and virtqueue teardown",
    )
    require_order(
        stop,
        (
            "m_CtrlQueue.DetachNativeSubmitBacklog();",
            "m_CtrlQueue.Close();",
            "if(!m_GpuBuf.Close())",
        ),
        "teardown must detach software backlog links before buffer cancellation and queue backing close",
    )


def check_deferred_software_fence_model() -> None:
    """Exercise deterministic Host/software retirement interleavings."""

    def run(steps: tuple[tuple[str, int], ...]) -> tuple[list[int], list[tuple[int, str]]]:
        entries: list[list[object]] = []
        notifications: list[int] = []
        submitted = 0

        def drain_prefix() -> None:
            completed = 0
            while entries and entries[0][1] == "retired":
                completed = int(entries.pop(0)[0])
            if completed != 0:
                notifications.append(completed)

        for operation, fence in steps:
            if operation in ("host", "software"):
                if fence == 0 or (submitted != 0 and fence - submitted <= 0):
                    fail("deferred software fence model accepted a non-monotonic submission")
                state = "host-pending" if operation == "host" else "software-pending"
                entries.append([fence, state])
                submitted = fence
            elif operation == "dpc":
                for entry in entries:
                    if entry[1] == "software-pending":
                        entry[1] = "retired"
                drain_prefix()
            elif operation == "complete-host":
                matches = [entry for entry in entries if entry[0] == fence and entry[1] == "host-pending"]
                if len(matches) != 1:
                    fail("deferred software fence model lost Host completion ownership")
                matches[0][1] = "retired"
                drain_prefix()
            else:
                fail(f"deferred software fence model received an unknown operation: {operation}")

        return notifications, [(int(entry[0]), str(entry[1])) for entry in entries]

    cases = (
        ("software-only", (("software", 1), ("dpc", 0)), [1], []),
        (
            "Host-before-software",
            (("host", 1), ("software", 2), ("dpc", 0), ("complete-host", 1)),
            [2],
            [],
        ),
        (
            "software-before-Host",
            (("software", 1), ("host", 2), ("dpc", 0), ("complete-host", 2)),
            [1, 2],
            [],
        ),
        (
            "two-software-behind-Host",
            (
                ("host", 1),
                ("software", 2),
                ("software", 3),
                ("dpc", 0),
                ("complete-host", 1),
            ),
            [3],
            [],
        ),
        (
            "software-around-Host",
            (
                ("software", 1),
                ("host", 2),
                ("software", 3),
                ("dpc", 0),
                ("complete-host", 2),
            ),
            [1, 3],
            [],
        ),
    )
    for name, steps, expected_notifications, expected_entries in cases:
        notifications, entries = run(steps)
        if notifications != expected_notifications or entries != expected_entries:
            fail(
                f"deferred software fence fake-dispatch case {name} produced "
                f"notifications={notifications}, entries={entries}"
            )


def check_dpc_completion_semantics() -> None:
    dpc_body = function_body("VioGpuAdapter::DpcRoutine", VIOGPU_CODE)
    dpc = compact_code(dpc_body)
    if top_level_control_transfers(dpc_body):
        fail("DPC must not return or jump before draining queued completions")
    lost_claim_blocks = [
        canonical_code(body)
        for condition, body, _, _ in if_blocks(dpc_body)
        if canonical_code(condition) == "terminalClaim==VioGpuVbufferTerminalClaimLost"
    ]
    if lost_claim_blocks != ["continue;"]:
        fail("a DPC which lost terminal callback ownership must not touch or release the buffer")
    callback_blocks = [
        (body, start, end)
        for condition, body, start, end in if_blocks(dpc_body)
        if canonical_code(condition) in ("completeCallback!=NULL", "completeCallback")
    ]
    callback_body = canonical_code(callback_blocks[0][0]) if len(callback_blocks) == 1 else ""
    callback_actions = (
        "VioGpuDetachVbufferTerminalCallbacks(pvbuf);"
        "completeCallback(completeContext);"
        "continue;"
    )
    require_order(
        dpc,
        (
            "VIOGPU_VBUFFER_TERMINAL_CLAIMterminalClaim=VioGpuClaimVbufferTerminalCallbacks(pvbuf);",
            "if(terminalClaim==VioGpuVbufferTerminalClaimLost){continue;}",
            "void(*completeCallback)(void*)=pvbuf->complete_cb;",
            "void*completeContext=pvbuf->complete_ctx;",
            "if(completeCallback!=NULL)",
        ),
        "DPC completion must claim terminal ownership before reading callback state",
    )
    if len(callback_blocks) != 1 or callback_actions not in callback_body:
        fail("DPC must claim, detach, and invoke one terminal callback before considering release")
    control_release_calls = method_call_offsets(dpc_body, aliases_of(dpc_body, "m_CtrlQueue"), "ReleaseBuffer")
    release_match = re.search(r"\bm_CtrlQueue\s*\.\s*ReleaseBuffer\s*\(\s*pvbuf\s*\)", dpc_body)
    release_offset = release_match.start() if release_match is not None else -1
    if release_offset < callback_blocks[0][2]:
        fail("DPC must not release a control buffer before the callback path continues")
    if len(control_release_calls) != 2:
        fail("DPC must release only malformed claimed buffers and ordinary post-callback auto-release buffers")
    malformed_terminal = canonical_code(
        """
        if (terminalClaim == VioGpuVbufferTerminalClaimWon)
        {
            VioGpuDetachVbufferTerminalCallbacks(pvbuf);
            m_CtrlQueue.ReleaseBuffer(pvbuf);
            FailNativeContextAtAnyIrql();
            continue;
        }
        """
    )
    if malformed_terminal not in canonical_code(dpc_body):
        fail("a claimed buffer without its terminal callback must be released and fail the transport")


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
    sg_construction = (
        "for(UINTi=0;i<pages;++i){"
        "PHYSICAL_ADDRESSpa={0};"
        "ASSERT(MmIsAddressValid(buf));"
        "pa=MmGetPhysicalAddress(buf);"
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
    pci_header = canonical_code(PCI_HEADER_CODE)
    for member in (
        "USHORTm_InterruptFlags;",
        "ULONGm_InterruptMessageCount;",
        "BOOLEANm_InterruptMessageCountKnown;",
        "UINTm_HostVisibleBar;",
        "ULONGLONGm_HostVisibleOffset;",
        "ULONGLONGm_HostVisibleSize;",
        "PVOIDm_HostVisibleMappedVA;",
        "PHYSICAL_ADDRESSm_HostVisibleMappedPA;",
        "ULONGLONGm_HostVisibleMappedOffset;",
        "ULONGLONGm_HostVisibleMappedSize;",
    ):
        if pci_header.count(member) != 1:
            fail(f"PCI resources must retain exactly one ownership field: {member}")
    constructor = (
        "CPciResources():m_pDxgkInterface(NULL),m_InterruptFlags(0),"
        "m_InterruptMessageCount(0),m_InterruptMessageCountKnown(FALSE),"
        "m_HostVisibleBar(MAXUINT),m_HostVisibleOffset(0),m_HostVisibleSize(0),"
        "m_HostVisibleMappedVA(NULL),m_HostVisibleMappedPA(),m_HostVisibleMappedOffset(0),m_HostVisibleMappedSize(0){}"
    )
    getter = "ULONGGetInterruptMessageCount(){returnm_InterruptMessageCount;}"
    known_getter = "BOOLEANHasKnownInterruptMessageCount(){returnm_InterruptMessageCountKnown;}"
    if (
        pci_header.count(constructor) != 1
        or pci_header.count(getter) != 1
        or pci_header.count(known_getter) != 1
    ):
        fail("PCI resources must initialize and expose retained interrupt count knowledge exactly once")

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
    clear_flags = "m_InterruptFlags=0;"
    clear_messages = "m_InterruptMessageCount=0;"
    clear_message_trust = "m_InterruptMessageCountKnown=FALSE;"
    clear_host_bar = "m_HostVisibleBar=MAXUINT;"
    clear_host_offset = "m_HostVisibleOffset=0;"
    clear_host_size = "m_HostVisibleSize=0;"
    clear_host_mapped_va = "m_HostVisibleMappedVA=NULL;"
    clear_host_mapped_pa = "m_HostVisibleMappedPA=PHYSICAL_ADDRESS();"
    clear_host_mapped_offset = "m_HostVisibleMappedOffset=0;"
    clear_host_mapped_size = "m_HostVisibleMappedSize=0;"
    clear_owner = "m_pDxgkInterface=NULL;"
    loops = list(re.finditer(r"for\(UINTbar=0;bar<PCI_TYPE0_ADDRESSES;(?:\+\+bar|bar\+\+)\)", close))
    loop_offset = loops[0].start() if len(loops) == 2 else -1
    unmap_offset = close.find(unmap_bar, loop_offset)
    record_offset = close.find(record_failure, unmap_offset)
    failure_offset = close.find(failure_return, record_offset)
    reset_offset = close.find(reset_bar, failure_offset)
    flags_offset = close.find(clear_flags, reset_offset)
    messages_offset = close.find(clear_messages, flags_offset)
    message_trust_offset = close.find(clear_message_trust, messages_offset)
    host_bar_offset = close.find(clear_host_bar, message_trust_offset)
    host_offset_offset = close.find(clear_host_offset, host_bar_offset)
    host_size_offset = close.find(clear_host_size, host_offset_offset)
    host_mapped_va_offset = close.find(clear_host_mapped_va, host_size_offset)
    host_mapped_pa_offset = close.find(clear_host_mapped_pa, host_mapped_va_offset)
    host_mapped_offset_offset = close.find(clear_host_mapped_offset, host_mapped_pa_offset)
    host_mapped_size_offset = close.find(clear_host_mapped_size, host_mapped_offset_offset)
    owner_offset = close.find(clear_owner, host_mapped_size_offset)
    if min(
        loop_offset,
        unmap_offset,
        record_offset,
        failure_offset,
        reset_offset,
        flags_offset,
        messages_offset,
        message_trust_offset,
        host_bar_offset,
        host_offset_offset,
        host_size_offset,
        host_mapped_va_offset,
        host_mapped_pa_offset,
        host_mapped_offset_offset,
        host_mapped_size_offset,
        owner_offset,
    ) < 0 or not (
        loop_offset
        < unmap_offset
        < record_offset
        < failure_offset
        < reset_offset
        < flags_offset
        < messages_offset
        < message_trust_offset
        < host_bar_offset
        < host_offset_offset
        < host_size_offset
        < host_mapped_va_offset
        < host_mapped_pa_offset
        < host_mapped_offset_offset
        < host_mapped_size_offset
        < owner_offset
    ):
        fail("PCI close must retain interrupt ownership until every BAR unmap succeeds")
    if (
        close.count(unmap_bar) != 1
        or len(loops) != 2
        or close.count(clear_flags) != 1
        or close.count(clear_messages) != 1
        or close.count(clear_message_trust) != 1
        or close.count(clear_host_bar) != 1
        or close.count(clear_host_offset) != 1
        or close.count(clear_host_size) != 1
        or close.count(clear_host_mapped_va) != 2
        or close.count(clear_host_mapped_pa) != 2
        or close.count(clear_host_mapped_offset) != 2
        or close.count(clear_host_mapped_size) != 2
        or close.count(clear_owner) != 1
    ):
        fail("PCI close must use one full unmap pass followed by one metadata reset pass")
    if len(variable_write_offsets(close_body, "firstFailure")) != 2:
        fail("PCI close must preserve the first BAR unmap failure until it is returned")

    init = function_body("CPciResources::Init", PCI_CODE)
    init_compact = canonical_code(init)
    owner_assign = init_compact.find("m_pDxgkInterface=pDxgkInterface;")
    interrupt_loop = init_compact.find("for(ULONGi=0;i<pResList->Count;++i)", owner_assign)
    interrupt_filter = init_compact.find("if(pResDescriptor->Type!=CmResourceTypeInterrupt)", interrupt_loop)
    first_flags = init_compact.find("m_InterruptFlags=pResDescriptor->Flags;", interrupt_filter)
    mixed_guard = (
        "elseif(messageSignaled!=IsMSIEnabled())"
        "{interrupt_resources_valid=FALSE;}"
    )
    line_guard = (
        "if(!messageSignaled&&m_InterruptMessageCount!=1)"
        "{interrupt_resources_valid=FALSE;}"
    )
    mixed_guard_offset = init_compact.find(mixed_guard, first_flags)
    count_increment = init_compact.find("++m_InterruptMessageCount;", mixed_guard_offset)
    line_guard_offset = init_compact.find(line_guard, count_increment)
    publish_trust = (
        "m_InterruptMessageCountKnown=interrupt_found&&interrupt_resources_valid&&"
        "((!IsMSIEnabled()&&m_InterruptMessageCount==1)||"
        "(IsMSIEnabled()&&m_InterruptMessageCount>=2));"
    )
    known_offset = init_compact.find(publish_trust, line_guard_offset)
    config_read = init_compact.find("Status=m_pDxgkInterface->DxgkCbReadDeviceSpace", known_offset)
    success_return = init_compact.rfind("returntrue;")
    if min(
        owner_assign,
        interrupt_loop,
        interrupt_filter,
        first_flags,
        mixed_guard_offset,
        count_increment,
        line_guard_offset,
        known_offset,
        config_read,
    ) < 0 or not (
        owner_assign
        < interrupt_loop
        < interrupt_filter
        < first_flags
        < mixed_guard_offset
        < count_increment
        < line_guard_offset
        < known_offset
        < config_read
        < success_return
    ):
        fail("PCI initialization must validate and retain interrupt count knowledge before PCI operations can fail")
    failure_tail = init_compact[owner_assign:success_return]
    if failure_tail.count("returnfalse;") != 3 or "Close()" in failure_tail:
        fail("PCI initialization failures must retain interrupt and DXGK ownership for final HWClose")
    if len(variable_write_offsets(init, "m_pDxgkInterface")) != 1:
        fail("PCI initialization must acquire its DXGK owner exactly once")
    if (
        len(variable_write_offsets(init, "m_InterruptFlags")) != 1
        or len(variable_write_offsets(init, "m_InterruptMessageCount")) != 1
        or len(variable_write_offsets(init, "m_InterruptMessageCountKnown")) != 1
        or len(variable_write_offsets(PCI_SOURCE, "m_InterruptFlags")) != 2
        or len(variable_write_offsets(PCI_SOURCE, "m_InterruptMessageCount")) != 2
        or len(variable_write_offsets(PCI_SOURCE, "m_InterruptMessageCountKnown")) != 2
    ):
        fail("interrupt ownership metadata may only be acquired in Init and cleared by successful Close")

    capability_contract = (
        "structvirtio_pci_cap64capability={};",
        "capability.cap.cap_vndr==PCI_CAPABILITY_ID_VENDOR_SPECIFIC",
        "capability.cap.cfg_type==VIRTIO_PCI_CAP_SHARED_MEMORY_CFG",
        "capability.cap.id==1",
        "capability.cap.bar<PCI_TYPE0_ADDRESSES",
        "regionSize>=PAGE_SIZE",
        "regionOffset<=barSize",
        "regionSize<=barSize-regionOffset",
        "m_HostVisibleBar=capability.cap.bar;",
        "m_HostVisibleOffset=regionOffset;",
        "m_HostVisibleSize=regionSize;",
    )
    for fragment in capability_contract:
        if init_compact.count(fragment) != 1:
            fail(f"PCI initialization must discover one bounded standard host-visible region: {fragment}")
    for field in ("m_HostVisibleBar", "m_HostVisibleOffset", "m_HostVisibleSize"):
        if len(variable_write_offsets(init, field)) != 1 or len(variable_write_offsets(PCI_SOURCE, field)) != 2:
            fail(f"host-visible PCI metadata may only be acquired in Init and cleared by successful Close: {field}")

    for fragment in (
        "caseCmResourceTypeMemoryLarge:",
        "pResDescriptor->Flags&CM_RESOURCE_MEMORY_LARGE",
        "caseCM_RESOURCE_MEMORY_LARGE_40:",
        "static_cast<ULONGLONG>(pResDescriptor->u.Memory40.Length40)<<8",
        "caseCM_RESOURCE_MEMORY_LARGE_48:",
        "static_cast<ULONGLONG>(pResDescriptor->u.Memory48.Length48)<<16",
        "caseCM_RESOURCE_MEMORY_LARGE_64:",
        "static_cast<ULONGLONG>(pResDescriptor->u.Memory64.Length64)<<32",
    ):
        if init_compact.count(fragment) != 1:
            fail(f"PCI initialization must preserve each WDK large-memory resource width: {fragment}")

    pci_header = canonical_code(PCI_HEADER_CODE)
    for fragment in (
        "CPciBar(PHYSICAL_ADDRESSBasePA,ULONGLONGuSize,boolbPortSpace,boolbIoMapped)",
        "ULONGLONGGetSize()const",
        "ULONGLONGm_uSize;",
    ):
        if pci_header.count(fragment) != 1:
            fail(f"PCI BAR ownership must retain 64-bit resource lengths: {fragment}")
    get_bar_va = canonical_code(function_body("CPciBar::GetVA", PCI_CODE))
    if "m_uSize==0||m_uSize>MAXULONG" not in get_bar_va:
        fail("legacy whole-BAR mapping must reject a 4 GiB or larger resource instead of truncating it")

    map_host_visible = canonical_code(function_body("CPciResources::MapHostVisibleAddress", PCI_CODE))
    for fragment in (
        "length>MAXULONG",
        "length>m_HostVisibleSize-regionOffset",
        "CPciBar*bar=&m_Bars[m_HostVisibleBar];",
        "barSize>MAXULONG",
        "ULONGLONGmappedSize=m_HostVisibleSize-regionOffset;",
        "PVOIDmappedVA=NULL;",
        "m_pDxgkInterface->DxgkCbMapMemory(m_pDxgkInterface->DeviceHandle",
        "MmNonCached",
        "m_HostVisibleMappedVA=mappedVA;",
        "m_HostVisibleMappedPA=mappedPA;",
        "m_HostVisibleMappedOffset=regionOffset;",
        "m_HostVisibleMappedSize=mappedSize;",
        "*address=static_cast<PUCHAR>(m_HostVisibleMappedVA)+(regionOffset-m_HostVisibleMappedOffset);",
    ):
        if map_host_visible.count(fragment) != 1:
            fail(f"host-visible blob slots must share a bounded suffix mapping: {fragment}")
    unmap_host_visible = canonical_code(function_body("CPciResources::UnmapHostVisibleAddress", PCI_CODE))
    if "DxgkCbUnmapMemory" in unmap_host_visible or "GetMappedVA()" in unmap_host_visible:
        fail("host-visible blob aliases must not independently unmap the suffix mapping")

    query_host_visible = canonical_code(function_body("CPciResources::QueryHostVisibleMapping", PCI_CODE))
    query_contract = (
        "if(physicalAddress==NULL||regionOffset==NULL||size==NULL||m_HostVisibleMappedVA==NULL||"
        "m_HostVisibleMappedSize==0){returnFALSE;}"
        "*physicalAddress=m_HostVisibleMappedPA;"
        "*regionOffset=m_HostVisibleMappedOffset;"
        "*size=m_HostVisibleMappedSize;"
        "returnTRUE;"
    )
    if query_host_visible != query_contract:
        fail("host-visible mapping diagnostics must expose only the active suffix mapping")

    virtio_header = canonical_code(VIRTIO_HEADER_CODE)
    for fragment in (
        "#defineVIRTIO_PCI_CAP_SHARED_MEMORY_CFG8",
        "__u8id;",
        "__u8padding[2];",
        "structvirtio_pci_cap64{structvirtio_pci_capcap;__le32offset_hi;__le32length_hi;};",
    ):
        if virtio_header.count(fragment) != 1:
            fail(f"VirtIO PCI headers must retain the standard 64-bit shared-memory capability layout: {fragment}")

    accepted_layout = (
        "boolinterrupt_layout_accepted=m_InterruptMessageCountKnown&&"
        "(!IsMSIEnabled()||(m_InterruptMessageCount>=3&&m_InterruptMessageCount<=4));"
    )
    resource_guard = "if(bar<0||!interrupt_layout_accepted)"
    if (
        init_compact.count(mixed_guard) != 1
        or init_compact.count(line_guard) != 1
        or len(variable_write_offsets(init, "interrupt_resources_valid")) != 3
        or init_compact.count(accepted_layout) != 1
        or init_compact.count(resource_guard) != 1
    ):
        fail("PCI initialization must reject mixed, multi-line, ordinary-MSI, or unaccepted MSI-X resource shapes")

    vector_helper = canonical_code(function_body("vdev_get_msix_vector", PCI_CODE))
    vector_contract = (
        "IVioGpuPCI*pdev=static_cast<IVioGpuPCI*>(context);"
        "u16vector=VIRTIO_MSI_NO_VECTOR;"
        "if(pdev->IsMSIEnabled()){"
        "if(queue>=0){vector=(u16)(queue+1);}"
        "else{vector=VIRTIO_GPU_MSIX_CONFIG_VECTOR;}}"
        "returnvector;"
    )
    if vector_helper != vector_contract:
        fail("VirtIO vector selection must leave config and queue vectors unprogrammed for line interrupts")


def check_adapter_lifecycle() -> None:
    adapter_header = canonical_code(VIOGPU_HEADER_CODE)
    for required in (
        "mutableEX_RUNDOWN_REFm_HardwareOperations;",
        "BOOLEANm_HardwareRundownCompleted;",
        "mutablevolatileLONGm_HardwareResetState;",
    ):
        if adapter_header.count(required) != 1:
            fail(f"DOD adapter must expose one retry-safe hardware rundown field: {required}")

    start = function_body("VioGpuDod::StartDevice", VIOGPU_CODE)
    start_compact = canonical_code(start)
    allocation = "m_pHWDevice=new(NonPagedPoolNx)VioGpuAdapter(this);"
    active_blocks = [
        (body, start_offset)
        for condition, body, start_offset, _ in if_blocks(start)
        if canonical_code(condition) == "IsDriverActive()"
        and canonical_code(body).endswith("returnSTATUS_ALREADY_INITIALIZED;")
    ]
    retained_blocks = [
        start_offset
        for condition, body, start_offset, _ in if_blocks(start)
        if canonical_code(condition) in ("m_pHWDevice!=NULL", "m_pHWDevice")
        and canonical_code(body).endswith("returnSTATUS_DEVICE_NOT_READY;")
    ]
    if len(active_blocks) != 1 or "VioGpuNativeStartPreconditions" not in active_blocks[0][0]:
        fail("StartDevice must reject reentry while the retained adapter is still active")
    if len(retained_blocks) != 1 or start_compact.count(allocation) != 1:
        fail("StartDevice must reject a retained adapter before allocating its replacement")
    mode_reset = start_compact.find("RtlZeroMemory(&m_CurrentMode,sizeof(m_CurrentMode))")
    interface_copy = start_compact.find("RtlCopyMemory(&m_DxgkInterface")
    allocation_offset = start_compact.find(allocation)
    active_offset = len(canonical_code(start[: active_blocks[0][1]]))
    retained_offset = len(canonical_code(start[: retained_blocks[0]]))
    if min(mode_reset, interface_copy, allocation_offset) < 0 or not (
        active_offset < retained_offset < interface_copy < mode_reset < allocation_offset
    ):
        fail("StartDevice must reject retained ownership before replacing DXGK or mode state")

    initial_recovery = (
        "LONGstartResetState=InterlockedCompareExchange(&m_HardwareResetState,"
        "VioGpuHardwareRecovering,VioGpuHardwareActive);"
    )
    stopped_recovery = (
        "if(startResetState==VioGpuHardwareResetRequested){"
        "startResetState=InterlockedCompareExchange(&m_HardwareResetState,"
        "VioGpuHardwareRecovering,VioGpuHardwareResetRequested);}"
    )
    recovery_reject_condition = (
        "startResetState!=VioGpuHardwareActive&&startResetState!=VioGpuHardwareResetRequested"
    )
    recovery_reject_blocks = [
        (body, start_offset)
        for condition, body, start_offset, _ in if_blocks(start)
        if canonical_code(condition) == recovery_reject_condition
        and canonical_code(body).endswith("returnSTATUS_DEVICE_NOT_READY;")
    ]
    rollback_recovery = (
        "InterlockedCompareExchange(&m_HardwareResetState,startResetState,"
        "VioGpuHardwareRecovering);"
    )
    final_publish = (
        "InterlockedCompareExchange(&m_HardwareResetState,VioGpuHardwareActive,"
        "VioGpuHardwareRecovering)!=VioGpuHardwareRecovering"
    )
    reset_caller_clear = "InterlockedExchange(&m_HardwareResetCallerRva,0);"
    reset_caller_clear_offset = start_compact.find(reset_caller_clear, allocation_offset)
    final_publish_offset = start_compact.find(final_publish, allocation_offset)
    started_offset = start_compact.find("m_Flags.DriverStarted=TRUE;", final_publish_offset)
    initial_recovery_offset = start_compact.find(initial_recovery)
    stopped_recovery_offset = start_compact.find(stopped_recovery, initial_recovery_offset)
    recovery_reject_offset = (
        len(canonical_code(start[: recovery_reject_blocks[0][1]])) if len(recovery_reject_blocks) == 1 else -1
    )
    if (
        start_compact.count(initial_recovery) != 1
        or start_compact.count(stopped_recovery) != 1
        or len(recovery_reject_blocks) != 1
        or start_compact.count(rollback_recovery) != 3
        or start_compact.count(reset_caller_clear) != 1
        or start_compact.count(final_publish) != 1
        or not retained_offset < initial_recovery_offset < stopped_recovery_offset < recovery_reject_offset < interface_copy
        or not allocation_offset < reset_caller_clear_offset < final_publish_offset
        or started_offset < final_publish_offset
    ):
        fail("StartDevice must claim initial or stopped recovery, roll back to its source state, and publish only a complete adapter")

    allocation_failure_end = start.find("Status = GetRegisterInfo()")
    pre_adapter_failures = start[:allocation_failure_end]
    rollback_offsets = [
        match.start()
        for match in re.finditer(
            r"\bInterlockedCompareExchange\s*\(\s*&m_HardwareResetState\s*,\s*startResetState\s*,\s*"
            r"VioGpuHardwareRecovering\s*\)\s*;",
            pre_adapter_failures,
            re.DOTALL,
        )
    ]
    rollback_returns = []
    for offset in rollback_offsets:
        return_match = re.search(
            r"\breturn\s+(?:Status|STATUS_GRAPHICS_DRIVER_MISMATCH)\s*;",
            pre_adapter_failures[offset:],
        )
        if return_match is not None:
            rollback_returns.append(return_match.group(0))
    if len(rollback_offsets) != 3 or len(rollback_returns) != 3:
        fail("every pre-adapter StartDevice failure must roll back only its own Recovering claim")

    failed_start_cleanup = re.findall(
        r"\breturn\s+UnwindFailedStart\s*\(\s*"
        r"(?:Status|STATUS_UNSUCCESSFUL|STATUS_DEVICE_NOT_READY)\s*\)\s*;",
        start,
    )
    if len(failed_start_cleanup) != 5 or start_compact.count("UnwindFailedStart(") != 5:
        fail("every post-allocation StartDevice failure must use the shared ownership-safe unwind")
    require_order(
        start_compact,
        (
            final_publish,
            "OpenNativePassiveQueue()",
            "OpenWddmPresentTransactions()",
            "m_Flags.DriverStarted=TRUE;",
        ),
        "StartDevice must publish Active before opening initial submission and reporting the adapter started",
    )
    initial_open_failure_blocks = [
        canonical_code(body)
        for condition, body, _, _ in if_blocks(start)
        if canonical_code(condition) == "!OpenNativePassiveQueue()||!OpenWddmPresentTransactions()"
    ]
    if initial_open_failure_blocks != [
        "RequestWddmSubmissionDrainAtAnyIrql();"
        "WaitForWddmSubmissionDrain();"
        "VIOGPU_RECORD_NATIVE_START(this,VioGpuNativeStartFinalState,STATUS_DEVICE_NOT_READY,"
        "VioGpuNativeStartDetailNone);returnUnwindFailedStart(STATUS_DEVICE_NOT_READY);"
    ]:
        fail("StartDevice must close and unwind when initial submission publication cannot open")
    if len(variable_write_offsets(start, "m_pHWDevice")) != 1 or re.search(r"\bdelete\s+m_pHWDevice\s*;", start):
        fail("StartDevice must only allocate its adapter and delegate every deletion to the checked unwind")

    unwind_body = function_body("VioGpuDod::UnwindFailedStart", VIOGPU_CODE)
    unwind = canonical_code(unwind_body)
    gate = unwind.find("InterlockedExchange(&m_HardwareResetState,VioGpuHardwareResetRequested);")
    wait = unwind.find("ExWaitForRundownProtectionRelease(&m_HardwareOperations);", gate)
    complete = unwind.find("ExRundownCompleted(&m_HardwareOperations);", wait)
    mark_completed = unwind.find("m_HardwareRundownCompleted=TRUE;", complete)
    close_adapter = unwind.find("NTSTATUScloseStatus=m_pHWDevice->HWClose();", mark_completed)
    delete_adapter = unwind.find("deletem_pHWDevice;", close_adapter)
    clear_adapter = unwind.find("m_pHWDevice=NULL;", delete_adapter)
    reopen = unwind.find("ExReInitializeRundownProtection(&m_HardwareOperations);", clear_adapter)
    mark_open = unwind.find("m_HardwareRundownCompleted=FALSE;", reopen)
    return_failure = unwind.find("returnNT_SUCCESS(closeStatus)?failureStatus:closeStatus;", mark_open)
    unwind_stages = (
        gate,
        wait,
        complete,
        mark_completed,
        close_adapter,
        delete_adapter,
        clear_adapter,
        reopen,
        mark_open,
        return_failure,
    )
    if min(unwind_stages) < 0 or tuple(sorted(unwind_stages)) != unwind_stages:
        fail("failed StartDevice unwind must gate, close rundown, close and delete the adapter, then reopen")
    close_rundown_blocks = [
        canonical_code(body)
        for condition, body, _, _ in if_blocks(unwind_body)
        if canonical_code(condition) == "!m_HardwareRundownCompleted"
    ]
    expected_close_rundown = (
        "ExWaitForRundownProtectionRelease(&m_HardwareOperations);"
        "ExRundownCompleted(&m_HardwareOperations);"
        "m_HardwareRundownCompleted=TRUE;"
    )
    success_blocks = [
        canonical_code(body)
        for condition, body, _, _ in if_blocks(unwind_body)
        if is_success_condition(condition, "closeStatus")
    ]
    expected_success = (
        "deletem_pHWDevice;m_pHWDevice=NULL;"
        "ExReInitializeRundownProtection(&m_HardwareOperations);"
        "m_HardwareRundownCompleted=FALSE;"
    )
    if close_rundown_blocks != [expected_close_rundown] or success_blocks != [expected_success]:
        fail("failed StartDevice unwind must retain a completed rundown and adapter after HWClose failure")
    if (
        unwind.count("ExWaitForRundownProtectionRelease(&m_HardwareOperations);") != 1
        or unwind.count("ExRundownCompleted(&m_HardwareOperations);") != 1
        or unwind.count("ExReInitializeRundownProtection(&m_HardwareOperations);") != 1
        or len(variable_write_offsets(unwind_body, "m_HardwareRundownCompleted")) != 2
        or len(variable_write_offsets(unwind_body, "m_pHWDevice")) != 1
    ):
        fail("failed StartDevice unwind must have one balanced ownership epoch")

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
    adapter_assert = destructor.find("NT_ASSERT(m_pHWDevice==NULL);", destructor.find(hardware_close))
    if (
        destructor_close_blocks != [hardware_close]
        or destructor.count("ExWaitForRundownProtectionRelease(&m_HardwareOperations)") != 1
        or destructor.count("ExRundownCompleted(&m_HardwareOperations)") != 1
        or adapter_assert < destructor.find(hardware_close)
        or re.search(r"\bdelete\s+m_pHWDevice\s*;", destructor_body)
        or len(variable_write_offsets(destructor_body, "m_pHWDevice")) != 0
    ):
        fail("DOD destructor must close rundown, require a cleared adapter, and never bypass HWClose")

    adapter_destructor_body = function_body("VioGpuAdapter::~VioGpuAdapter", VIOGPU_CODE)
    adapter_destructor = canonical_code(adapter_destructor_body)
    native_wait = adapter_destructor.find("ExWaitForRundownProtectionRelease(&m_NativeContextReferences);")
    native_complete = adapter_destructor.find("ExRundownCompleted(&m_NativeContextReferences);", native_wait)
    native_cleanup = adapter_destructor.find("CloseResolutionEvent();", native_complete)
    if (
        min(native_wait, native_complete, native_cleanup) < 0
        or not native_wait < native_complete < native_cleanup
        or adapter_destructor.count("ExWaitForRundownProtectionRelease(&m_NativeContextReferences);") != 1
        or adapter_destructor.count("ExRundownCompleted(&m_NativeContextReferences);") != 1
    ):
        fail("adapter destructor must complete native-context rundown before releasing remaining state")

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
            "VioGpuDod::QueryNativeContextReadiness",
            "returnFALSE;",
            "VioGpuAdapter*adapter=m_pHWDevice;"
            "BOOLEANready=!IsHardwareResetRequested()&&adapter!=NULL&&"
            "adapter->QueryNativeContextReadiness(capset,capsetVersion,capsetSize,resetGeneration);"
            "ExReleaseRundownProtection(&m_HardwareOperations);returnready;",
        ),
        (
            "VioGpuDod::CreateNativeContext",
            "returnSTATUS_DEVICE_NOT_READY;",
            "VioGpuAdapter*adapter=m_pHWDevice;"
            "NTSTATUSstatus=!IsHardwareResetRequested()&&adapter!=NULL?"
            "adapter->CreateNativeContext(context,expectedResetGeneration):STATUS_DEVICE_NOT_READY;"
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
    hw_close = canonical_code(hw_close_body)
    transport_stop = hw_close.find("NTSTATUSstatus=StopNativeContextTransport();")
    final_gate = hw_close.find("InterlockedExchange(&m_InterruptDispatchEnabled,FALSE);", transport_stop)
    final_barrier = hw_close.find("status=SynchronizeInterruptMessages();", final_gate)
    final_drain = hw_close.find("KeFlushQueuedDpcs();", final_barrier)
    pci_close = hw_close.find("status=m_PciResources.Close();", final_drain)
    close_return = hw_close.find("returnstatus;", pci_close)
    if min(transport_stop, final_gate, final_barrier, final_drain, pci_close, close_return) < 0 or not (
        transport_stop < final_gate < final_barrier < final_drain < pci_close < close_return
    ):
        fail("HWClose must stop transport, gate ISR work, synchronize all messages, drain DPCs, then close PCI")
    success_blocks = [
        canonical_code(body)
        for condition, body, _, _ in if_blocks(hw_close_body)
        if is_success_condition(condition, "status")
    ]
    if success_blocks != [
        "InterlockedExchange(&m_InterruptDispatchEnabled,FALSE);status=SynchronizeInterruptMessages();",
        "KeFlushQueuedDpcs();status=m_PciResources.Close();",
    ]:
        fail("HWClose final interrupt barrier and PCI close must be success-gated without transport-state shortcuts")
    if any(fragment in hw_close for fragment in ("m_NativeContextState", "m_bVirtioInitialized", "m_bQueuesInitialized")):
        fail("HWClose final interrupt barrier must also cover Offline and partial-init transport state")
    if len(variable_write_offsets(hw_close_body, "status")) != 3:
        fail("HWClose must preserve the first teardown, barrier, or PCI-close failure")

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
    reset_claim = (
        "LONGresetState=InterlockedCompareExchange(&m_HardwareResetState,"
        "VioGpuHardwareRecovering,VioGpuHardwareResetRequested);"
    )
    reset_consume = "if(resetState==VioGpuHardwareResetRequested)"
    reset_flag = "resetRecovery=TRUE;"
    reset_drain = "RequestWddmSubmissionDrainAtAnyIrql();"
    reset_wait = "if(!WaitForWddmSubmissionDrain())"
    reset_adapter = "m_pHWDevice->ResetDevice();"
    reset_reject = "elseif(resetState!=VioGpuHardwareActive){returnSTATUS_DEVICE_NOT_READY;}"
    reset_failure = (
        "if(!NT_SUCCESS(Status)&&resetRecovery){"
        "InterlockedCompareExchange(&m_HardwareResetState,VioGpuHardwareResetRequested,"
        "VioGpuHardwareRecovering);}"
    )
    d_state_failure = (
        "if(!NT_SUCCESS(Status)&&(DevicePowerState==PowerDeviceD1||"
        "DevicePowerState==PowerDeviceD2||DevicePowerState==PowerDeviceD3)){"
        "RequestHardwareResetAtAnyIrql();}"
    )
    reset_publish = (
        "if(InterlockedCompareExchange(&m_HardwareResetState,VioGpuHardwareActive,"
        "VioGpuHardwareRecovering)!=VioGpuHardwareRecovering){returnSTATUS_DEVICE_NOT_READY;}"
    )
    active_recheck = (
        "elseif(InterlockedCompareExchange(&m_HardwareResetState,VioGpuHardwareActive,"
        "VioGpuHardwareActive)!=VioGpuHardwareActive){returnSTATUS_DEVICE_NOT_READY;}"
    )
    reset_claim_offset = power_compact.find(reset_claim)
    reset_consume_offset = power_compact.find(reset_consume, reset_claim_offset)
    reset_flag_offset = power_compact.find(reset_flag, reset_consume_offset)
    reset_drain_offset = power_compact.find(reset_drain, reset_flag_offset)
    reset_wait_offset = power_compact.find(reset_wait, reset_drain_offset)
    reset_adapter_offset = power_compact.find(reset_adapter, reset_wait_offset)
    reset_reject_offset = power_compact.find(reset_reject, reset_adapter_offset)
    transition_offset = power_compact.find(transition, reset_reject_offset)
    reset_failure_offset = power_compact.find(reset_failure, transition_offset)
    d_state_failure_offset = power_compact.find(d_state_failure, reset_failure_offset)
    reset_caller_clear_offset = power_compact.find(reset_caller_clear, d_state_failure_offset)
    reset_publish_offset = power_compact.find(reset_publish, reset_caller_clear_offset)
    active_recheck_offset = power_compact.find(active_recheck, reset_publish_offset)
    if min(
        reset_claim_offset,
        reset_consume_offset,
        reset_flag_offset,
        reset_drain_offset,
        reset_wait_offset,
        reset_adapter_offset,
        reset_reject_offset,
        transition_offset,
        reset_failure_offset,
        d_state_failure_offset,
        reset_caller_clear_offset,
        reset_publish_offset,
        active_recheck_offset,
    ) < 0 or not (
        reset_claim_offset
        < reset_consume_offset
        < reset_flag_offset
        < reset_drain_offset
        < reset_wait_offset
        < reset_adapter_offset
        < reset_reject_offset
        < transition_offset
        < reset_failure_offset
        < d_state_failure_offset
        < reset_caller_clear_offset
        < reset_publish_offset
        < active_recheck_offset
    ):
        fail("power transitions must fail closed before D0 rebuild publishes the Active reset epoch")
    if (
        power_compact.count("BOOLEANresetRecovery=FALSE;") != 1
        or power_compact.count(reset_caller_clear) != 1
        or power_compact.count("InterlockedCompareExchange(&m_HardwareResetState") != 6
        or power_compact.count("m_pHWDevice->ResetDevice();") != 1
        or "InterlockedExchange(&m_HardwareResetState" in power_compact
    ):
        fail("D0 reset recovery must use only the six checked three-state CAS operations")

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
        "VIOGPU_NATIVE_CONTEXT=1",
        "VIOGPU_EXTERNAL_DRIVER_ENTRY=1",
    ):
        if definitions.count(required) != 1:
            fail(f"project must define {required} exactly once")

    test_definition_groups = [
        element
        for element in root.findall(".//msbuild:ItemDefinitionGroup", NAMESPACE)
        if element.attrib.get("Condition") == "'$(VIOGPU_WDDM_TEST_IMPLEMENTATIONS)'=='1'"
    ]
    if len(test_definition_groups) != 1:
        fail("project must contain exactly one opt-in WDDM test implementation property group")
    test_definitions = [
        (element.text or "")
        for element in test_definition_groups[0].findall(
            "msbuild:ClCompile/msbuild:PreprocessorDefinitions", NAMESPACE
        )
    ]
    if len(test_definitions) != 1 or "VIOGPU_WDDM_TEST_IMPLEMENTATIONS=1" not in test_definitions[0].split(";"):
        fail("opt-in WDDM test implementation property group must define its macro exactly once")

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
        fail("driver_entry.cpp must assert the Win8 declaration surface exactly once")

    sign_modes = [
        (element.text or "").strip() for element in root.findall(".//msbuild:SignMode", NAMESPACE)
    ]
    if not sign_modes or any(sign_mode != "Off" for sign_mode in sign_modes):
        fail(f"driver build must remain unsigned before package signing; found SignMode: {sign_modes or ['none']}")

    adjust_inf = [
        (element.text or "").strip()
        for element in root.findall(".//msbuild:Feature_AdjustInf", NAMESPACE)
    ]
    if adjust_inf != ["true"]:
        fail("full-miniport project must enable WDK INF token substitution")

    inf_arch = [
        (element.text or "").strip()
        for element in root.findall(".//msbuild:InfArch", NAMESPACE)
    ]
    if inf_arch != ["$(TargetArch).10.0...22621"]:
        fail("full-miniport project must keep the Win11 22621 InfArch decoration")

    optimize_references = [
        (element.text or "").strip()
        for element in root.findall(".//msbuild:Link/msbuild:OptimizeReferences", NAMESPACE)
    ]
    if optimize_references != ["true"]:
        fail("registered full-miniport project must enable reference optimization")

    forced_symbols = [
        (element.text or "").strip()
        for element in root.findall(".//msbuild:Link/msbuild:ForceSymbolReferences", NAMESPACE)
    ]
    if forced_symbols:
        fail("registered full-miniport project must not force-link an unreachable registration helper")

    generate_map_files = [
        (element.text or "").strip()
        for element in root.findall(".//msbuild:Link/msbuild:GenerateMapFile", NAMESPACE)
    ]
    if generate_map_files != ["true"]:
        fail("full-miniport project must generate one linker map for provenance evidence")

    map_file_names = [
        (element.text or "").strip()
        for element in root.findall(".//msbuild:Link/msbuild:MapFileName", NAMESPACE)
    ]
    if map_file_names != [r"$(OutDir)$(TargetName).map"]:
        fail("full-miniport project must emit its linker map beside the driver")

    target_names = [
        (element.text or "").strip()
        for element in root.findall(".//msbuild:TargetName", NAMESPACE)
    ]
    if target_names != ["viogpuwddm"]:
        fail("the Native Context composition target must retain its non-DOD viogpuwddm name")

    driver_items = [
        element
        for element in root.findall(".//msbuild:ClCompile[@Include]", NAMESPACE)
        if element.attrib["Include"].replace("\\", "/").endswith("/viogpudo/driver.cpp")
    ]
    if len(driver_items) != 1:
        fail("full-miniport project must contain exactly one inherited viogpudo driver.cpp input")

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
        fail("full-miniport project must track the WPP non-owner template exactly once")

    inf_inputs = [
        element.attrib.get("Include", "").replace("\\", "/")
        for element in root.findall(".//msbuild:Inf[@Include]", NAMESPACE)
    ]
    if inf_inputs != ["viogpuwddm.inx"]:
        fail("full-miniport project must contain exactly the ARM64 viogpuwddm INX input")

    package_inputs = [
        element.attrib.get("Include", "")
        for element in root.findall(".//msbuild:FilesToPackage[@Include]", NAMESPACE)
    ]
    if package_inputs != ["$(TargetPath)", "$(OutDir)viogpud3d.dll"]:
        fail("full-miniport project must package its linked SYS and ARM64 D3D UMD DLL")

    output_dirs = [
        (element.text or "").strip()
        for element in root.findall(".//msbuild:OutDir", NAMESPACE)
    ]
    if output_dirs != ["objfre_win11_arm64\\arm64\\"]:
        fail(f"full-miniport project must use the product ARM64 output directory: {output_dirs or ['none']}")


def check_installation_contract() -> None:
    if not INF_TEMPLATE.is_file():
        fail("full-miniport ARM64 INX is missing")
    source = INF_TEMPLATE.read_text(encoding="utf-8")
    compact = canonical_code(source)
    required = (
        'Signature="$WindowsNT$"',
        "Class=Display",
        "ClassGuid={4d36e968-e325-11ce-bfc1-08002be10318}",
        "DriverVer=08/22/2026,0.1.0.0",
        "CatalogFile=viogpuwddm.cat",
        "viogpud3d.dll=1,,",
        "%DroidVM%=VioGpuWddm,NT$ARCH$",
        "%VioGpuWddm.DeviceDesc%=VioGpuWddm_Install,PCI\\VEN_1AF4&DEV_1050",
        "Include=msdv.inf",
        "FeatureScore=F8",
        "viogpud3d.dll,,,2",
        "DelReg=VioGpuWddm_RetiredDeviceSettings",
        "[VioGpuWddm_RetiredDeviceSettings]HKR,,RequireRestrictedDma",
        "AddService=VioGpuWddm,%SPSVCINST_ASSOCSERVICE%,VioGpuWddm_Service,VioGpuWddm_EventLog",
        "ServiceBinary=%INX_PLATFORM_DRIVERS_DIR%\\viogpuwddm.sys",
        "MSISupported,%REG_DWORD%,1",
        "MessageNumberLimit,%REG_DWORD%,4",
        'UserModeDriverName,%REG_MULTI_SZ%,"%13%\\viogpud3d.dll",'
        '"%13%\\viogpud3d.dll","%13%\\viogpud3d.dll"',
        "InstalledDisplayDrivers,%REG_MULTI_SZ%,viogpud3d,viogpud3d,viogpud3d",
        "REG_MULTI_SZ=0x00010000",
    )
    for fragment in required:
        if compact.count(fragment) != 1:
            fail(f"full-miniport INX must contain exactly one installation contract fragment: {fragment}")
    if re.search(r"NT\$ARCH\$\.\d+\.\d+\.\.\.\d+", source):
        fail("full-miniport INX must leave TargetOSVersion decoration to InfArch")
    if re.search(r"(?i)nt(?:amd64|x86)|viogpudo\.sys", source):
        fail("full-miniport INX must remain ARM64-tokenized and independent of the display-only binary")
    if re.search(r"(?i)HKR\s*,\s*,\s*RequireRestrictedDma\s*,", source):
        fail("full-miniport INX must delete, never configure, the retired restricted-DMA value")
    if re.search(r"(?i)UserModeDriverNameWow|OpenAdapter12", source):
        fail("ARM64-only legacy package must not claim WoW64 or D3D12 UMD support")


def main() -> None:
    root = ET.parse(PROJECT).getroot()
    sources = project_compile_sources(root)
    check_driver_entry_gate()
    check_viogpudo_code_segment_contract()
    check_arm64_workflow_contract()
    check_d3d_umd_shim_contract()
    check_retired_pool_absence()
    check_native_start_diagnostics()
    check_native_query_adapter_info_diagnostics()
    check_native_present_diagnostics()
    check_native_win7_driver_caps_contract()
    check_registration_helper(sources)
    check_callback_table()
    check_vidpn_mode_contract()
    check_legacy_runtime_callback_contract()
    check_wddm_handle_ownership()
    check_virtio_reset_contract()
    check_virtio_queue_allocation_cleanup()
    check_dod_reset_entrypoints()
    check_adapter_line_interrupt_bitmap()
    check_native_context_readiness()
    check_no_retired_variant_contract(sources)
    check_queue_failure_semantics()
    check_control_queue_dma_and_response_contract()
    check_synchronous_2d_control_transactions()
    check_wddm_2d_resource_ownership()
    check_wddm_standard_paging()
    check_wddm_standard_primary_scanout()
    check_wddm_present_contract()
    check_native_context_ownership()
    check_native_map_diagnostics()
    check_native_parameter_diagnostics()
    check_wddm_private_abi(root)
    check_wddm_paging_transaction_gate()
    check_native_guest_allocation_extent_math()
    check_wddm_guest_allocation_lifecycle()
    check_allocation_lifecycle_wait_status_contract()
    check_wddm_context_lifetime()
    check_wddm_submission_lifetime()
    check_deferred_software_fence_model()
    check_dpc_completion_semantics()
    check_segment_failure_semantics()
    check_pci_resource_lifetime()
    check_adapter_lifecycle()
    check_worker_thread_lifetime()
    check_project_safety(root)
    check_installation_contract()
    print("viogpuwddm Native Context full-miniport contract: PASS")


if __name__ == "__main__":
    main()
