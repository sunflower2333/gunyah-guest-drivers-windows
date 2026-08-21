#!/usr/bin/env python3

import os
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
WPP_NON_OWNER_TEMPLATE = PROJECT_DIR / "wpp-non-owner.tpl"
NAMESPACE = {"msbuild": "http://schemas.microsoft.com/developer/msbuild/2003"}
REGISTRATION_HELPER = "VioGpuWddmInitializeMiniportCompileOnly"
WORKFLOW_PATH = (PROJECT_DIR.parent.parent / ".github" / "workflows" / "viogpuwddm-arm64-ci.yml").resolve()
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
        "VioGpuSetNamedPoolNotificationDriverObject(driverObject); "
        "WPP_INIT_TRACING(driverObject, registryPath); "
        "NTSTATUS status = DxgkInitialize(driverObject, registryPath, &initialData); "
        "if (!NT_SUCCESS(status)) { WPP_CLEANUP(NULL); VioGpuClearNamedPoolNotificationDriverObject(); } "
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
    unload_cleanup = unload_body.find("WPP_CLEANUP(NULL);")
    unload_driver_object = unload_body.find("VioGpuClearNamedPoolNotificationDriverObject();")
    if unload_cleanup < 0 or unload_driver_object < unload_cleanup:
        fail("registered unload must clear named-pool notification DriverObject after WPP cleanup")


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
        "DxgkDdiCancelCommand": "VioGpuWddmCancelCommand",
        "DxgkDdiPreemptCommand": "VioGpuWddmPreemptCommand",
        "DxgkDdiQueryCurrentFence": "VioGpuWddmQueryCurrentFence",
        "DxgkDdiResetFromTimeout": "VioGpuWddmResetFromTimeout",
        "DxgkDdiRestartFromTimeout": "VioGpuWddmRestartFromTimeout",
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
    failure = function_body("VioGpuAdapter::FailNativeContextInitialization", viogpu_code)
    hw_close = function_body("VioGpuAdapter::HWClose", viogpu_code)
    stop = function_body("VioGpuAdapter::StopNativeContextTransportLocked", viogpu_code)
    destructor = function_body("VioGpuAdapter::~VioGpuAdapter", viogpu_code)
    buffer_close = function_body("VioGpuBuf::Close", QUEUE_CODE)
    segment_close = function_body("VioGpuMemSegment::Close", QUEUE_CODE)

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
    require_call_count(transport_start, "ConnectDrmHostPool", 1, "transport start")
    require_call_count(transport_start, "ConnectGpuGuestPool", 1, "transport start")
    transport_start_compact = compact_code(transport_start)
    host_connect_offset = transport_start_compact.find("status=ConnectDrmHostPool();")
    host_failure_offset = transport_start_compact.find(
        "if(!NT_SUCCESS(status)){returnstatus;}", host_connect_offset
    )
    guest_connect_offset = transport_start_compact.find("status=ConnectGpuGuestPool();")
    guest_failure_offset = transport_start_compact.find(
        "if(!NT_SUCCESS(status)){returnstatus;}", guest_connect_offset
    )
    virtio_init_offset = transport_start_compact.find("status=VioGpuAdapterInit(pDispInfo);")
    if min(
        host_connect_offset,
        host_failure_offset,
        guest_connect_offset,
        guest_failure_offset,
        virtio_init_offset,
    ) < 0 or not (
        host_connect_offset
        < host_failure_offset
        < guest_connect_offset
        < guest_failure_offset
        < virtio_init_offset
    ):
        fail("Native Context transport must connect drm2kgsl_host and gpu_guest before initializing VirtIO")
    hw_init_compact = compact_code(hw_init)
    probe_offset = compact_code(transport_start).find("status=ProbeNativeContextReadiness();")
    buffer_offset = compact_code(transport_start).find("m_GpuBuf.Init(allocation)")
    idr_offset = compact_code(transport_start).find("m_Idr.Init(1,VIOGPU_NATIVE_RESOURCE_ID_START)")
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
        ("status=m_GpuGuestPool.Disconnect()", "gpu_guest connection release"),
        ("status=m_DrmHostPool.Disconnect()", "drm2kgsl_host connection release"),
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
        ("status=m_GpuGuestPool.Disconnect()", "gpu_guest connection release"),
        ("status=m_DrmHostPool.Disconnect()", "drm2kgsl_host connection release"),
        ("InterlockedExchange(&m_NativeContextState,VioGpuNativeContextOffline)", "offline publication"),
    )
    teardown_offsets = [(stop_compact.find(fragment), description) for fragment, description in teardown_order]
    for (offset, description), (next_offset, next_description) in zip(teardown_offsets, teardown_offsets[1:]):
        if offset > next_offset:
            fail(f"transport teardown must perform {description} before {next_description}")
    guest_disconnect = stop_compact.find("status=m_GpuGuestPool.Disconnect()")
    guest_failure = stop_compact.find(
        "if(!NT_SUCCESS(status)){FailNativeContextAtAnyIrql();returnstatus;}", guest_disconnect
    )
    host_disconnect = stop_compact.find("status=m_DrmHostPool.Disconnect()")
    host_failure = stop_compact.find(
        "if(!NT_SUCCESS(status)){FailNativeContextAtAnyIrql();returnstatus;}", host_disconnect
    )
    offline_publish = stop_compact.find("InterlockedExchange(&m_NativeContextState,VioGpuNativeContextOffline)")
    if min(guest_disconnect, guest_failure, host_disconnect, host_failure, offline_publish) < 0 or not (
        guest_disconnect < guest_failure < host_disconnect < host_failure < offline_publish
    ):
        fail("transport teardown must retain the adapter when named-pool notification teardown fails")
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
    if len(lock_offsets) != 1 or len(unlock_offsets) != 1 or len(free_offsets) != 3:
        fail("control-buffer teardown must use one detach lock and free response, data, and descriptor storage")
    if not lock_offsets[0] < unlock_offsets[0] < free_offsets[0] <= free_offsets[-1]:
        fail("control-buffer teardown must detach every buffer before freeing allocations outside the spin lock")
    for list_name in ("m_InUseBufs", "m_FreeBufs"):
        drain = rf"\bwhile\s*\(\s*!\s*IsListEmpty\s*\(\s*&{list_name}\s*\)\s*\)"
        if len(re.findall(drain, buffer_close)) != 1:
            fail(f"control-buffer teardown must detach {list_name} exactly once")
    for member in ("m_uCount", "m_uCountMin"):
        writes = variable_write_offsets(buffer_close, member)
        if len(writes) != 1 or not lock_offsets[0] < writes[0] < unlock_offsets[0]:
            fail(f"control-buffer teardown must clear {member} exactly once while holding the detach lock")
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
        attach.find("entry_count>PAGE_SIZE/sizeof(*entries)"),
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
        create_host.find("AcquireGpuGuestPoolMapping(&mapping)"),
        create_host.find("entry.addr=(ULONGLONG)baseAddress.QuadPart+placementOffset;"),
        create_host.find("m_CtrlQueue.CreateResource2DSynchronous(resourceId,format,width,height)"),
        create_host.find("*resourceState=VioGpu2DResourceCreated;"),
        create_host.find("m_CtrlQueue.AttachBackingSynchronous(resourceId,&entry,1)"),
        create_host.find("*resourceState=VioGpu2DResourceBackingAttached;"),
    )
    if min(create_sequence) < 0 or list(create_sequence) != sorted(create_sequence):
        fail("2D primary creation must validate guest-pool backing before ordered create and attach ownership")
    for fragment in (
        "mapping.GetGeneration()==poolGeneration",
        "(baseAddress.QuadPart&(PAGE_SIZE-1))==0",
        "entry.length=(ULONG)backingSize;",
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
        "elseif(result==VioGpuHostContextUnknown||result==VioGpuHostContextRejected)",
        "*resourceState=VioGpu2DResourceUnknown;",
    ):
        if destroy_host.count(fragment) != 1:
            fail(f"2D primary teardown must retain confirmed-only UNREF ownership: {fragment}")
    if destroy_host.count("*released=TRUE;") != 2:
        fail("2D primary teardown may release only an already-empty or confirmed-UNREF owner")
    if destroy_host.count("FailNativeContextAtAnyIrql();") != 2:
        fail("2D primary teardown must quarantine both preexisting and response-derived unknown ownership")
    if "result==VioGpuHostContextConfirmed||result==VioGpuHostContextRejected" in destroy_host:
        fail("2D primary teardown must not interpret INVALID_RESOURCE_ID as released ownership")

    allocation_header = canonical_code(WDDM_DDI_HEADER_CODE)
    if allocation_header.count("VIOGPU_2D_RESOURCE_STATEResource2DState;") != 1:
        fail("each WDDM allocation must retain its exact 2D Host ownership state")
    create_allocation = canonical_code(function_body("VioGpuWddmCreateAllocation", WDDM_DDI_CODE))
    for fragment in (
        "elseif((privateData.Flags&VIOGPU_WDDM_ALLOCATION_PRIMARY)!=0)",
        "standardResourceId=adapter->Allocate2DResourceId();",
        "allocation->ResourceId=nativeContext!=NULL?nativeResourceId:standardResourceId;",
        "allocation->BlobId=nativeResourceId;",
        "allocation->Resource2DState=VioGpu2DResourceNone;",
    ):
        if create_allocation.count(fragment) != 1:
            fail(f"standard primary allocation must publish one disjoint 2D identity: {fragment}")

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


def check_wddm_standard_primary_paging() -> None:
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
        fail("paging packet validation must allow context-zero standard primary transfers")
    if "VioGpuWddmPagingFlagAllocationIdle)==0&&hasContext" in validator:
        fail("paging packet validation must allow context-zero standard primary fills")

    build = canonical_code(function_body("VioGpuWddmBuildPagingBuffer", WDDM_DDI_CODE))
    branch = (
        "if(IsStandardPrimaryAllocation(allocation))"
        "{returnBuildStandardPrimaryPagingBuffer(adapter,pagingBuffer,allocation,segmentAddress,transferMdl,"
        "mdlOffset,transferOffset,transferSize,fillPattern,packetFlags);}"
    )
    if build.count(branch) != 1:
        fail("BuildPagingBuffer must route only standard primaries into their separate paging transaction")
    if build.count("if(!IsNativeAllocation(allocation)||") != 1:
        fail("BuildPagingBuffer must retain the native-only gate after the standard primary branch")

    standard_build = canonical_code(function_body("BuildStandardPrimaryPagingBuffer", WDDM_DDI_CODE))
    for fragment in (
        "!IsStandardPrimaryAllocation(allocation)",
        "allocation->ResourceId>=VIOGPU_NATIVE_RESOURCE_ID_START",
        "allocation->BlobId!=0",
        "allocation->NativeContext!=NULL",
        "allocation->HostState!=VioGpuWddmAllocationHostNone",
        "QueryStandardPlacementPoolGeneration(adapter,",
        "AcquireAllocationSubmissionReference(allocation,adapter)",
        "ResolveTransferMdlAddress(transferMdl,mdlOffset,transferSize,&systemAddress);",
        "CopyStandardPlacement(allocation,",
        "packet->ContextId=0;",
        "packet->ContextGeneration=0;",
        "packet->ResetGeneration=0;",
        "transaction->ContextId=0;",
        "transaction->ContextGeneration=0;",
        "transaction->ResetGeneration=0;",
        "transaction->TransferDataComplete=transferDataComplete;",
        "InterlockedExchange(&transaction->ReferenceHeld,1);",
        "pagingPrivate->Work.Routine=NativePagingBatchWorker;",
    ):
        if fragment not in standard_build:
            fail(f"standard primary paging build must retain exact deferred ownership: {fragment}")
    if "Create2DResourceBacking(" in standard_build or "Destroy2DResource(" in standard_build:
        fail("BuildPagingBuffer must not mutate standard primary Host ownership before SubmitCommand")

    dispatch = canonical_code(function_body("ExecutePagingTransaction", WDDM_DDI_CODE))
    dispatch_sequence = (
        dispatch.find("VIOGPU_WDDM_ALLOCATION*allocation=transaction->Allocation;"),
        dispatch.find("if(transaction->ContextId==0)"),
        dispatch.find("returnExecuteStandardPrimaryPagingTransaction(transaction);"),
        dispatch.find("AcquireAllocationNativeContextSnapshot(allocation,&snapshot)"),
    )
    if min(dispatch_sequence) < 0 or list(dispatch_sequence) != sorted(dispatch_sequence):
        fail("paging execution must route context-zero packets before acquiring native context ownership")

    execute = canonical_code(function_body("ExecuteStandardPrimaryPagingTransaction", WDDM_DDI_CODE))
    for fragment in (
        "transaction->ContextId!=0",
        "allocation->Resource2DState==VioGpu2DResourceUnknown",
        "AddPagingRange(allocation,transaction->TransferOffset,transaction->TransferSize,&pagingRange);",
        "allocation->PagingCoveredBytes!=allocation->BackingSize",
        "ResolveStandard2DFormat(allocation->Format,&virtioFormat)",
        "transaction->Adapter->Create2DResourceBacking(",
        "allocation->Resource2DState==VioGpu2DResourceBackingAttached",
        "PublishStandardPlacement(allocation,transaction->PlacementOffset,transaction->PoolGeneration);",
        "transaction->Adapter->Destroy2DResource(allocation->ResourceId,",
        "if(released){ClearNativePlacement(allocation);}",
        "ClearNativePagingState(allocation);",
    ):
        if fragment not in execute:
            fail(f"standard primary paging execution must retain transactional placement ownership: {fragment}")
    if execute.count("result==VioGpuHostContextConfirmed&&released") != 2:
        fail("standard primary page-out and discard may release placement only after confirmed UNREF")

    query_pool = canonical_code(function_body("QueryStandardPlacementPoolGeneration", WDDM_DDI_CODE))
    copy_pool = canonical_code(function_body("CopyStandardPlacement", WDDM_DDI_CODE))
    fill_pool = canonical_code(function_body("FillStandardPlacement", WDDM_DDI_CODE))
    for owner, body in (("query", query_pool), ("copy", copy_pool), ("fill", fill_pool)):
        for fragment in (
            "AcquireGpuGuestPoolMapping(&mapping)",
            "mapping.GetBaseAddress()!=NULL",
            "mapping.Release();",
        ):
            if fragment not in body:
                fail(f"standard primary {owner} must retain a bounded guest-pool mapping lease: {fragment}")
    if "mapping.GetGeneration()==expectedPoolGeneration" not in copy_pool or \
       "mapping.GetGeneration()==expectedPoolGeneration" not in fill_pool:
        fail("standard primary data movement must stay in one exact guest-pool generation")


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
        "UINTm_2DScanoutResourceId;",
        "BOOLEANm_2DScanoutUnknown;",
    ):
        if adapter_header.count(fragment) != 1:
            fail(f"adapter must retain one serialized 2D scanout owner: {fragment}")

    set_host = canonical_code(function_body("VioGpuAdapter::Set2DScanout", VIOGPU_CODE))
    for fragment in (
        "KeGetCurrentIrql()!=PASSIVE_LEVEL",
        "KeWaitForSingleObject(&m_2DScanoutMutex,Executive,KernelMode,FALSE,&timeout)",
        "*previousResourceId=m_2DScanoutResourceId;",
        "if(m_2DScanoutUnknown)",
        "m_CtrlQueue.SetScanoutSynchronous(scanoutId,resourceId,width,height,0,0)",
        "if(result==VioGpuHostContextConfirmed){m_2DScanoutResourceId=resourceId;}",
        "elseif(result==VioGpuHostContextUnknown)",
        "m_2DScanoutUnknown=TRUE;",
        "FailNativeContextAtAnyIrql();",
        "KeReleaseMutex(&m_2DScanoutMutex,FALSE);",
    ):
        if fragment not in set_host:
            fail(f"2D scanout switch must retain confirmed-only serialized ownership: {fragment}")
    if "m_2DScanoutResourceId=resourceId;" in set_host.split("if(result==VioGpuHostContextConfirmed)", 1)[0]:
        fail("2D scanout ownership must not publish before Host confirmation")

    query_host = canonical_code(function_body("VioGpuAdapter::Query2DScanoutResource", VIOGPU_CODE))
    for fragment in (
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
        "allocation->Resource2DState!=VioGpu2DResourceBackingAttached",
        "!allocation->PlacementValid",
        "allocation->PagingState!=VioGpuWddmAllocationPagingIdle",
        "setVidPnSourceAddress->PrimaryAddress.QuadPart)!=allocation->PlacementOffset",
        "adapter->Set2DScanout(0,allocation->ResourceId,allocation->Width,allocation->Height,&previousResourceId)",
        "result==VioGpuHostContextConfirmed?STATUS_SUCCESS:STATUS_DEVICE_NOT_READY",
    ):
        if fragment not in set_ddi:
            fail(f"SetVidPnSourceAddress must retain the exact mode-change primary contract: {fragment}")
    if "STATUS_NOT_SUPPORTED" in set_ddi:
        fail("SetVidPnSourceAddress must no longer reject the completed standard primary mode-change path")

    destroy_allocation = canonical_code(function_body("VioGpuWddmDestroyAllocation", WDDM_DDI_CODE))
    standard_execute = canonical_code(function_body("ExecuteStandardPrimaryPagingTransaction", WDDM_DDI_CODE))
    standard_build = canonical_code(function_body("BuildStandardPrimaryPagingBuffer", WDDM_DDI_CODE))
    if destroy_allocation.count("Query2DScanoutResource(allocation->ResourceId,&active)") != 1:
        fail("DestroyAllocation must not unref a scanout-owned primary")
    if standard_execute.count("Query2DScanoutResource(allocation->ResourceId,&active)") != 1 or \
       standard_build.count("Query2DScanoutResource(allocation->ResourceId,&active)") != 1:
        fail("standard primary page-out and discard must reject scanout-owned backing before copy and execution")

    query_caps = canonical_code(function_body("VioGpuDod::QueryAdapterInfo", VIOGPU_CODE))
    wddm_query_caps = canonical_code(function_body("VioGpuWddmQueryAdapterInfo", WDDM_DDI_CODE))
    if "RtlZeroMemory(pDriverCaps,pQueryAdapterInfo->OutputDataSize);" not in query_caps or \
       "FlipOnVSyncMmIo" in query_caps or "FlipOnVSyncMmIo" in wddm_query_caps:
        fail("the synchronous PASSIVE_LEVEL scanout path must not advertise MMIO flip capability")


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

    wire_requirements = (
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
        "response->map_info==(VIRTIO_GPU_MAP_INFO_POOL|VIRTIO_GPU_MAP_CACHE_CACHED)",
        "response->pool_offset!=0",
        "(response->pool_offset&(PAGE_SIZE-1))==0",
    ):
        if map_blob.count(fragment) != 1:
            fail(f"control blob map must accept only the exact cached named-pool response: {fragment}")

    native_header = canonical_code(VIOGPU_HEADER_CODE)
    for field in (
        "UINTControlResourceId;",
        "ULONGControlPoolOffset;",
        "ULONGControlBlobSize;",
        "ULONGLastControlSeqno;",
        "ULONGLONGControlPoolGeneration;",
        "BOOLEANControlResourceCreated;",
        "BOOLEANControlMapped;",
        "BOOLEANSubmitQueueCreated;",
    ):
        if native_header.count(field) != 1:
            fail(f"native owner must retain one control-resource field: {field}")
    if native_header.count("UINTm_NextNativeResourceId;") != 1:
        fail("native control resources must use one adapter-owned high-range allocator")

    resource_header = canonical_code(RESOURCE_HEADER_CODE)
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

    seed = canonical_code(
        function_body_with_parameters(
            "VioGpuSeedNativeControlResponse",
            "_In_ VioGpuAdapter *adapter, _Inout_ VIOGPU_NATIVE_CONTEXT_OWNER *owner, _In_ ULONG sequence, "
            "_In_ ULONG responseSize",
            VIOGPU_CODE,
        )
    )
    copy = canonical_code(
        function_body_with_parameters(
            "VioGpuCopyNativeControlResponse",
            "_In_ VioGpuAdapter *adapter, _In_ const VIOGPU_NATIVE_CONTEXT_OWNER *owner, _In_ ULONG sequence, "
            "_Out_ PVOID response, _In_ ULONG responseSize",
            VIOGPU_CODE,
        )
    )
    consume = canonical_code(
        function_body_with_parameters(
            "VioGpuConsumeNativeControlResponse",
            "_In_ VioGpuAdapter *adapter, _In_ const VIOGPU_NATIVE_CONTEXT_OWNER *owner, _In_ ULONG sequence, "
            "_In_ ULONG parameter, _Out_ PULONGLONG value",
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
        if body.count("adapter->AcquireDrmHostPoolMapping(&mapping)") != 1 or body.count("mapping.Release();") != 1:
            fail(f"native response {owner} must own exactly one short named-pool lease")
        if "KeWaitForSingleObject" in body or "SubmitNativeControl" in body or "PAGED_CODE" in body:
            fail(f"native response {owner} must remain non-waiting and nonpageable while leased")
        enter = body.find("KeEnterGuardedRegion();")
        acquire = body.find("adapter->AcquireDrmHostPoolMapping(&mapping)")
        release = body.find("mapping.Release();")
        leave = body.find("KeLeaveGuardedRegion();")
        if min(enter, acquire, release, leave) < 0 or not enter < acquire < release < leave:
            fail(f"native response {owner} must keep APCs disabled for the complete short lease")
    seed_order = (
        seed.find("RtlZeroMemory(response,responseSize);"),
        seed.find(
            "InterlockedExchange(reinterpret_cast<volatileLONG*>(&responseHeader->ret),MAXLONG);"
        ),
        seed.find(
            "InterlockedExchange(reinterpret_cast<volatileLONG*>(&responseHeader->hdr.len),"
            "static_cast<LONG>(responseSize));"
        ),
    )
    if min(seed_order) < 0 or list(seed_order) != sorted(seed_order):
        fail("native response seed must install an invalid return sentinel before publishing the exact length")
    seed_generation = "owner->ControlPoolGeneration=mapping.GetGeneration();"
    if seed.count(seed_generation) != 1:
        fail("native response seed must capture one named-pool generation")
    copy_sequence = (
        copy.find("owner->ControlPoolGeneration==mapping.GetGeneration()"),
        copy.find("VioGpuReadSharedU32(&shmem->base.seqno)==sequence"),
        copy.find("KeMemoryBarrier();"),
        copy.find("RtlCopyMemory(response,sharedResponse,responseSize);"),
        copy.find("responseHeader->len==responseSize"),
    )
    if min(copy_sequence) < 0 or list(copy_sequence) != sorted(copy_sequence):
        fail("native response copy must acquire seqno before copying and validating the response")
    for fragment in (
        "owner->ControlPoolGeneration==mapping.GetGeneration()",
        "VioGpuReadSharedU32(&shmem->async_error)==0",
        "VioGpuReadSharedU32(&shmem->global_faults)==0",
    ):
        if faults_clear.count(fragment) != 1:
            fail(f"native fault snapshot must validate its exact short-lease state: {fragment}")

    query_param = canonical_code(function_body("VioGpuAdapter::QueryNativeContextParameterLocked", VIOGPU_CODE))
    query_sequence = (
        query_param.find(
            "VioGpuSeedNativeControlResponse(this,owner,sequence,sizeof(MSM_CCMD_IOCTL_SIMPLE_GET_PARAM_RSP))"
        ),
        query_param.find("m_CtrlQueue.SubmitNativeControl(owner->ContextId,&request,sizeof(request))"),
        query_param.find("VioGpuConsumeNativeControlResponse(this,owner,sequence,parameter,value)"),
    )
    if min(query_sequence) < 0 or list(query_sequence) != sorted(query_sequence):
        fail("GET_PARAM must release its seed lease before submit and reacquire only after VirtIO completion")
    if "AcquireDrmHostPoolMapping" in query_param or query_param.count("m_CtrlQueue.PoisonSynchronousRequests();") != 2:
        fail("GET_PARAM must not hold a pool lease across submit and must poison malformed shared-memory state")
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
        create.find("m_CtrlQueue.MapNativeControlBlob(resourceId,&poolOffset)"),
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
        cleanup.find("m_CtrlQueue.UnrefNativeResource(owner->ControlResourceId)"),
        cleanup.find("m_CtrlQueue.DestroyNativeContext(owner->ContextId)"),
    )
    if min(cleanup_sequence) < 0 or list(cleanup_sequence) != sorted(cleanup_sequence):
        fail("normal native teardown must close submitqueue, unmap, unref, then destroy the Host context")
    if cleanup.count("returnVioGpuHostContextUnknown;") != 6 or cleanup.count(
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
    if query.count("InitializeAbiHeader(&adapterInfo->Header,sizeof(*adapterInfo));") != 1:
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
    if any(token in query for token in ("va_start", "va_size", "ContextId", "ResourceId", "PhysicalAddress")):
        fail("UMDRIVERPRIVATE must not expose VA ranges or KMD/transport identities")
    query_dispatch = canonical_code(function_body("VioGpuWddmQueryAdapterInfo", WDDM_DDI_CODE))
    if query_dispatch.count("if(pQueryAdapterInfo->Type==DXGKQAITYPE_UMDRIVERPRIVATE)") != 1 or query_dispatch.count(
        "returnQueryUmdPrivateInfo(adapter,pQueryAdapterInfo);"
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
        "createContext->Flags.Value!=0",
        "createContext->pPrivateDriverData==NULL",
        "createContext->PrivateDriverDataSize!=sizeof(VIOGPU_WDDM_CONTEXT_CREATE)",
        "RtlCopyMemory(&privateData,createContext->pPrivateDriverData,sizeof(privateData));",
        "!IsCurrentAbiHeader(&privateData.Header,sizeof(privateData))",
        "privateData.ExpectedResetGeneration==0",
        "privateData.Flags!=VIOGPU_WDDM_CONTEXT_FLAGS_NONE",
        "privateData.Reserved!=0",
        "context->RuntimeContext=NULL;",
        "device->Adapter->CreateNativeContext(&context->NativeContext,privateData.ExpectedResetGeneration)",
    )
    for fragment in create_requirements:
        if create.count(fragment) != 1:
            fail(f"CreateContext must enforce its exact input-only private ABI contract: {fragment}")
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

    destroy_allocation = canonical_code(function_body("VioGpuWddmDestroyAllocation", WDDM_DDI_CODE))
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
        "PoolGeneration",
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
    build = canonical_code(function_body("VioGpuWddmBuildPagingBuffer", WDDM_DDI_CODE))
    if "AcquireAllocationSubmissionReference(allocation,adapter)" not in build:
        fail("BuildPagingBuffer must retain an allocation submission reference")
    if "InterlockedExchange(&transaction->State,VioGpuWddmPagingTransactionBuilt);" not in build:
        fail("BuildPagingBuffer must publish the Built transaction state only after initialization")
    build_state_sequence = (
        build.find("InterlockedExchange(&transaction->ReferenceHeld,1);"),
        build.find("InterlockedExchange(&transaction->ExecutionStarted,0);"),
        build.find("InterlockedExchange(&transaction->CancelRequested,0);"),
        build.find("InterlockedExchange(&transaction->State,VioGpuWddmPagingTransactionBuilt);"),
    )
    if min(build_state_sequence) < 0 or list(build_state_sequence) != sorted(build_state_sequence):
        fail("BuildPagingBuffer must initialize ownership and cancellation before publishing Built")
    if "NativePagingBatchWorker" not in build:
        fail("BuildPagingBuffer must initialize the passive paging worker owner")
    build_copy = build.find("CopyNativePlacement(allocation,")
    build_publish = build.find("InterlockedExchange(&transaction->State,VioGpuWddmPagingTransactionBuilt);")
    if min(build_copy, build_publish) < 0 or build_copy > build_publish:
        fail("BuildPagingBuffer must consume the transfer MDL before publishing Built")
    if "ResolveTransferMdlAddress(" in canonical_code(function_body("ExecutePagingTransaction", WDDM_DDI_CODE)):
        fail("passive paging execution must not dereference a BuildPagingBuffer-owned MDL")
    cancel = canonical_code(function_body("VioGpuWddmCancelCommand", WDDM_DDI_CODE))
    for fragment in (
        "CancelPagingTransaction(&pagingPrivate->Transaction)",
        "adapter->CancelNativePassiveWork(&firstPrivate->Work)",
        "ownership!=VioGpuNativePassiveOwnershipWorkerOwned",
        "ReleasePreparedSubmission(submission)",
        "returnSTATUS_SUCCESS;",
    ):
        if fragment not in cancel:
            fail(f"CancelCommand must retain safe cleanup and success semantics: {fragment}")
    submit = canonical_code(function_body("VioGpuWddmSubmitCommand", WDDM_DDI_CODE))
    for fragment in (
        "ResolvePagingBatch(",
        "InterlockedCompareExchange(&pagingPrivate->Transaction.State,VioGpuWddmPagingTransactionQueued,VioGpuWddmPagingTransactionBuilt)",
        "adapter->QueueNativePassiveWork(&firstPrivate->Work)",
        "CancelPagingTransaction(&pagingPrivate->Transaction)",
    ):
        if fragment not in submit:
            fail(f"SubmitCommand must transfer paging ownership to a passive worker: {fragment}")
    if "CancelCommandAware=0" in canonical_code(function_body("VioGpuWddmQueryAdapterInfo", WDDM_DDI_CODE)):
        fail("driver caps must advertise CancelCommandAware once paging owners are cancellable")

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
        fail("an executed paging record must retain its allocation reference until batch rollback is impossible")
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

    batch_worker = canonical_code(function_body("NativePagingBatchWorker", WDDM_DDI_CODE))
    for fragment in (
        "VioGpuWddmPagingTransactionExecuting,VioGpuWddmPagingTransactionQueued",
        "InterlockedExchange(&pagingPrivate->Transaction.ExecutionStarted,1);",
        "VioGpuWddmPagingTransactionExecuting,VioGpuWddmPagingTransactionCancelled",
        "VioGpuWddmPagingTransactionExecuting,VioGpuWddmPagingTransactionFinished",
        "RollbackPagingBatch(",
        "ReleasePagingTransactionReference(&pagingPrivate->Transaction);",
    ):
        if fragment not in batch_worker:
            fail(f"paging worker must retain claim, rollback, and terminal ownership: {fragment}")
    worker_claim = batch_worker.find(
        "VioGpuWddmPagingTransactionExecuting,VioGpuWddmPagingTransactionQueued"
    )
    worker_execute = batch_worker.find("status=ExecutePagingTransaction(&pagingPrivate->Transaction);")
    worker_rollback = batch_worker.find("RollbackPagingBatch(")
    worker_release = batch_worker.find("ReleasePagingTransactionReference(&pagingPrivate->Transaction);")
    if min(worker_claim, worker_execute, worker_rollback, worker_release) < 0 or not (
        worker_claim < worker_execute < worker_rollback < worker_release
    ):
        fail("paging worker must claim before execution and retain references through failure rollback")

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
    if "expectedState==VioGpuWddmPagingTransactionAny" not in resolve:
        fail("CancelCommand must be able to resolve a batch across ownership-state races")
    if "dmaBuffer==NULL?static_cast<VIOGPU_WDDM_PAGING_DMA_PACKET*>(header->Packet)" not in resolve:
        fail("SubmitCommand paging resolution must use private packet ownership without a CPU DMA pointer")
    if "ResolvePagingBatch(NULL,submitCommand->DmaBufferSize" not in submit:
        fail("SubmitCommand must not read a nonexistent pDmaBuffer member")
    if "VioGpuWddmPagingTransactionAny" not in cancel:
        fail("CancelCommand must resolve both Built and Queued paging records")
    if cancel.count("ownership!=VioGpuNativePassiveOwnershipWorkerOwned") != 2:
        fail("CancelCommand must transfer worker ownership in both exact and fallback paging resolution")

    passive_header = canonical_code(VIOGPU_HEADER_CODE)
    for fragment in (
        "volatileLONGState;",
        "volatileLONGRetired;",
        "VIOGPU_NATIVE_PASSIVE_WORK_OWNERSHIPCancelNativePassiveWork",
    ):
        if fragment not in passive_header:
            fail(f"passive work ownership ABI must expose {fragment}")
    passive_queue = canonical_code(function_body("QueueNativePassiveWork", VIOGPU_CODE))
    passive_cancel = canonical_code(function_body("CancelNativePassiveWork", VIOGPU_CODE))
    passive_worker = canonical_code(function_body("RunNativePassiveWorker", VIOGPU_CODE))
    for fragment in (
        "InterlockedExchange(&work->State,VioGpuNativePassiveWorkQueued);",
        "InterlockedCompareExchange(&work->Retired,0,0)==0",
    ):
        if fragment not in passive_queue:
            fail(f"passive queueing must retain retirement ownership: {fragment}")
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
        "VIOGPU_WDDM_ALLOCATION_PAGING_STATEPagingState;",
        "ULONGLONGPagingPlacementOffset;",
        "ULONGLONGPagingPoolGeneration;",
        "LIST_ENTRYPagingRanges;",
        "SIZE_TPagingCoveredBytes;",
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
    if released_context.count("context->AllocationReferences==0") != 1:
        fail("context release proof must wait for every KMD allocation reference")
    range_register = canonical_code(function_body("RegisterNativeAllocationRange", WDDM_DDI_CODE))
    if range_register.count("InsertTailList(&registration->AllocationRanges,&range->Link)") != 1 or range_register.count(
        "range->Iova<=existingEnd&&existing->Iova<=rangeEnd"
    ) != 1:
        fail("native allocation range registration must reject overlapping context IOVA intervals")
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
        create_host.find("entry.addr=(ULONGLONG)baseAddress.QuadPart+segmentOffset;"),
        create_host.find("entry.length=(ULONG)backingSize;"),
        create_host.find("request.flags=msmFlags|MSM_BO_GUEST_ALLOC;"),
        create_host.find("TryReferenceNativeAllocationCount(snapshot->Owner)"),
        create_host.find("m_CtrlQueue.SubmitNativeControl(snapshot->ContextId,&request,sizeof(request))"),
        create_host.find("m_CtrlQueue.CreateNativeGuestBlob("),
    )
    if min(host_sequence) < 0 or list(host_sequence) != sorted(host_sequence):
        fail("guest-backed BO creation must bind GEM_NEW ownership before creating its guest-memory blob")
    for fragment in (
        "resourceId!=blobId",
        "backingSize>MAXULONG",
        "backingSize<PAGE_SIZE",
        "logicalSize<=(ULONGLONG)backingSize-PAGE_SIZE",
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
    if "baseAddress.QuadPart&(PAGE_SIZE-1)" not in create_host:
        fail("guest-backed SG entry must prove that the named-pool physical base is page aligned")

    destroy_host = canonical_code(function_body("VioGpuAdapter::DestroyNativeGuestAllocation", VIOGPU_CODE))
    if "result==VioGpuHostContextConfirmed||result==VioGpuHostContextRejected" in destroy_host:
        fail("guest allocation teardown cannot treat INVALID_RESOURCE_ID as released ownership")
    for fragment in (
        "if(result==VioGpuHostContextConfirmed)",
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

    create_blob = canonical_code(function_body("CtrlQueue::CreateNativeGuestBlob", QUEUE_CODE))
    blob_sequence = (
        create_blob.find("PGPU_MEM_ENTRYownedEntry="),
        create_blob.find("*ownedEntry=*entry;"),
        create_blob.find("command->blob_mem=VIRTIO_GPU_BLOB_MEM_HOST3D_GUEST;"),
        create_blob.find("command->blob_flags=blob_flags;"),
        create_blob.find("command->nr_entries=1;"),
        create_blob.find("vbuf->data_buf=ownedEntry;"),
        create_blob.find("SubmitSynchronousLocked(vbuf,&releaseBuffer,&submitted)"),
    )
    if min(blob_sequence) < 0 or list(blob_sequence) != sorted(blob_sequence):
        fail("RESOURCE_CREATE_BLOB must submit one queue-owned HOST3D_GUEST SG entry")

    paging = canonical_code(function_body("VioGpuWddmBuildPagingBuffer", WDDM_DDI_CODE))
    if "pagingBuffer->Transfer.TransferOffset!=0" in paging or "TransferSize!=allocation->BackingSize" in paging:
        fail("BuildPagingBuffer must accept VidMm sub-transfers instead of requiring a whole allocation")
    for fragment in (
        "transferOffset=pagingBuffer->Transfer.TransferOffset;",
        "transferMdl=pagingBuffer->Transfer.Source.pMdl;",
        "transferSize=pagingBuffer->Transfer.TransferSize;",
        "transferOffset>allocation->BackingSize",
        "AcquireAllocationSubmissionReference(allocation,adapter)",
        "ResolveTransferMdlAddress(transferMdl,mdlOffset,transferSize,&systemAddress);",
        "CopyNativePlacement(allocation,",
        "transaction->TransferDataComplete=transferDataComplete;",
        "transaction->PoolGeneration=poolGeneration;",
    ):
        if fragment not in paging:
            fail(f"BuildPagingBuffer must retain exact deferred transfer state: {fragment}")
    if "AddPagingRange(" in paging or "CreateNativeGuestAllocation(" in paging:
        fail("BuildPagingBuffer must not publish paging progress or host ownership before SubmitCommand")
    worker = canonical_code(function_body("ExecutePagingTransaction", WDDM_DDI_CODE))
    for fragment in (
        "AddPagingRange(allocation,",
        "QueryNativePlacementPoolGeneration(&snapshot,",
        "!transaction->TransferDataComplete",
        "FillNativePlacement(allocation,",
        "snapshot.Adapter->CreateNativeGuestAllocation(",
        "ReleaseAllocationHostOwnership(allocation,&snapshot,TRUE)",
        "ClearNativePagingState(allocation)",
    ):
        if fragment not in worker:
            fail(f"passive paging execution must retain the operation: {fragment}")
    for fragment in (
        "BOOLEANcontinuingPageIn=",
        "if(continuingPageIn)",
        "elseif(!continuingPageIn)",
        "BOOLEANcontinuingPageOut=",
        "if(continuingPageOut)",
        "elseif(!continuingPageOut)",
    ):
        if fragment not in worker:
            fail(f"paging execution must accept repeated TransferStart only for the active transfer: {fragment}")

    pool_mapping = canonical_code(function_body("AcquireNativeGuestPoolMapping", WDDM_DDI_CODE))
    if "snapshot->Adapter->AcquireGpuGuestPoolMapping(mapping)" not in pool_mapping:
        fail("paging pool access must use the snapshot-owned adapter lease directly")
    for helper_name in (
        "CopyNativePlacement",
        "FillNativePlacement",
        "QueryNativePlacementPoolGeneration",
    ):
        helper = canonical_code(function_body(helper_name, WDDM_DDI_CODE))
        if "AcquireNativeGuestPoolMapping(snapshot,&mapping)" not in helper:
            fail(f"{helper_name} must preserve native-context then pool-lease lock ordering")
        if "allocation->Adapter->AcquireGpuGuestPoolMapping" in helper or "adapter->AcquireGpuGuestPoolMapping" in helper:
            fail(f"{helper_name} must not re-enter VioGpuDod hardware rundown under a context snapshot")
    if "pagingBuffer->MultipassOffset!=0" in paging or "pagingBuffer->MultipassOffset=" in paging:
        fail("atomic paging markers must leave MultipassOffset unchanged")
    capacity = paging.find(
        "if(dmaSize<sizeof(VIOGPU_WDDM_PAGING_DMA_PACKET)||dmaPrivateSize<sizeof(VIOGPU_WDDM_PAGING_PRIVATE))"
    )
    if capacity < 0:
        fail("BuildPagingBuffer must snapshot and check both DMA capacities")
    publication = paging.find("RtlZeroMemory(packet,sizeof(*packet));")
    private_publication = paging.find("RtlZeroMemory(pagingPrivate,sizeof(*pagingPrivate));", publication)
    cursor_publication = paging.find("pagingBuffer->pDmaBuffer=static_cast<BYTE*>(dmaBuffer)+sizeof(*packet);", private_publication)
    private_cursor = paging.find(
        "pagingBuffer->pDmaBufferPrivateData=static_cast<BYTE*>(dmaPrivateBuffer)+sizeof(*pagingPrivate);",
        cursor_publication,
    )
    if min(publication, private_publication, cursor_publication, private_cursor) < 0:
        fail("BuildPagingBuffer must publish one packet/private record and advance both cursors")
    if not (capacity < publication < private_publication < cursor_publication < private_cursor):
        fail("BuildPagingBuffer must publish metadata only after ownership acquisition")

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
        "ULONGLONGPoolGeneration;",
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
        "packet->ResourceId<VIOGPU_NATIVE_RESOURCE_ID_START",
        "packet->ResourceId==MAXUINT",
        "packet->PoolGeneration==0",
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
        "if(dmaSize<sizeof(VIOGPU_WDDM_PAGING_DMA_PACKET)||"
        "dmaPrivateSize<sizeof(VIOGPU_WDDM_PAGING_PRIVATE))"
    )
    if capacity < 0:
        fail("BuildPagingBuffer must snapshot and check both DMA capacities")
    publication = paging.find("RtlZeroMemory(packet,sizeof(*packet));")
    private_publication = paging.find("RtlZeroMemory(pagingPrivate,sizeof(*pagingPrivate));", publication)
    cursor_publication = paging.find("pagingBuffer->pDmaBuffer=static_cast<BYTE*>(dmaBuffer)+sizeof(*packet);", private_publication)
    private_cursor = paging.find(
        "pagingBuffer->pDmaBufferPrivateData=static_cast<BYTE*>(dmaPrivateBuffer)+sizeof(*pagingPrivate);",
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
        "pagingBuffer->DmaBufferPrivateDataSize=dmaPrivateSize-sizeof(*pagingPrivate);",
    ):
        if paging.count(fragment) != 1:
            fail(f"BuildPagingBuffer packet publication must retain exact metadata/cursors: {fragment}")
    if not (capacity < publication < private_publication < cursor_publication < private_cursor):
        fail("BuildPagingBuffer must publish metadata only after operation success and in cursor order")

    render = canonical_code(function_body("VioGpuWddmRender", WDDM_DDI_CODE))
    for fragment in (
        "privateData->Version=VioGpuWddmDmaPrivateVersion;",
        "privateData->Kind=VioGpuWddmDmaKindRender;",
        "privateData->Packet=dmaBuffer;",
        "privateData->PacketLength=render->CommandLength;",
        "privateData->Reserved=0;",
    ):
        if render.count(fragment) != 1:
            fail(f"Render private data must identify its exact DMA kind and payload: {fragment}")


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
    host_create = create.find(
        "device->Adapter->CreateNativeContext(&context->NativeContext,privateData.ExpectedResetGeneration)"
    )
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
    def require_order(code: str, fragments: tuple[str, ...], message: str) -> None:
        offsets = [code.find(fragment) for fragment in fragments]
        if min(offsets) < 0 or offsets != sorted(offsets):
            fail(message)

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
        "LIST_ENTRYContextLink;",
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
        "volatileLONGState;",
        "VIOGPU_WDDM_ALLOCATION*Allocations[VioGpuWddmSubmissionAllocationLimit];",
        "VIOGPU_WDDM_SUBMISSION_REFERENCEReferences[VioGpuWddmSubmissionAllocationLimit];",
    ):
        if submission_header.count(field) != 1:
            fail(f"submission record must retain exactly one nonpaged ownership field: {field}")
    header = canonical_code(WDDM_DDI_HEADER_CODE)
    if header.count("VioGpuWddmSubmissionAllocationLimit=128,") != 1:
        fail("submission record must use the bounded allocation-reference limit")
    source = canonical_code(WDDM_DDI_SOURCE)
    if source.count(
        "constUINTVIOGPU_WDDM_ALLOCATION_LIST_SIZE=VioGpuWddmSubmissionAllocationLimit;"
    ) != 1:
        fail("the VidSch allocation list must match the bounded submission-reference limit")
    if WDDM_DDI_SOURCE.count("const ULONG VIOGPU_WDDM_SUBMISSION_SIGNATURE = 'sWGV';") != 1:
        fail("submission record must have one private signature")

    queue_header = canonical_code(QUEUE_HEADER_CODE)
    for field in (
        "void(*cancel_cb)(void*ctx);",
        "void*cancel_ctx;",
        "void(*queue_error_cb)(void*ctx);",
        "void*queue_error_ctx;",
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
    if "context->SubmissionClosing=TRUE;" not in begin_context or "context->SubmissionReferences!=0" not in begin_context:
        fail("context destruction must close publication before rejecting live submissions")
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
    acquire_allocation = canonical_code(function_body("AcquireAllocationSubmissionReference", WDDM_DDI_CODE))
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
            "ClearPagingRanges(allocation);",
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
            "!IsListEmpty(&context->PendingSubmissions)",
            "if(!context->OperationsRundownCompleted)",
        ),
        "DestroyContext must close submissions before draining callback operations",
    )

    publish = canonical_code(function_body("PublishPreparedSubmission", WDDM_DDI_CODE))
    require_order(
        publish,
        (
            "RtlZeroMemory(submission,sizeof(*submission));",
            "submission->Signature=VIOGPU_WDDM_SUBMISSION_SIGNATURE;",
            "submission->State=VioGpuWddmSubmissionPrepared;",
            "for(UINTindex=0;index<allocationCount;++index)",
            "KeAcquireSpinLock(&context->SubmissionLock,&oldIrql);",
            "privateData->Submission=submission;",
            "virtioBuffer->complete_cb=NativeSubmissionComplete;",
            "virtioBuffer->cancel_cb=NativeSubmissionCancelled;",
            "virtioBuffer->queue_error_cb=NativeSubmissionQueueFailed;",
            "virtioBuffer->auto_release=FALSE;",
            "InsertTailList(&context->PendingSubmissions,&submission->ContextLink);",
        ),
        "prepared submission must publish private-data identity and all callbacks under the context lock",
    )
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
            "VIOGPU_WDDM_SUBMISSION *submission, LONG expectedState, BOOLEAN releaseBuffer",
            WDDM_DDI_CODE,
        )
    )
    require_order(
        quarantine,
        (
            "InterlockedCompareExchange(&submission->State,expectedState,expectedState)",
            "RemoveEntryList(&submission->ContextLink);",
            "InterlockedExchange(&submission->State,VioGpuWddmSubmissionQuarantined);",
            "virtioBuffer->complete_cb=NULL;",
            "virtioBuffer->cancel_cb=NULL;",
            "virtioBuffer->queue_error_cb=NULL;",
            "ReleaseAllocationSubmissionReference(submission->Allocations[index]);",
            "ReleaseContextSubmissionReference(context);",
            "privateData->Submission=NULL;",
            "submission->Signature=0;",
            "deletesubmission;",
        ),
        "submission quarantine must detach callbacks before releasing retained objects",
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

    patch = canonical_code(function_body("VioGpuWddmPatch", WDDM_DDI_CODE))
    for fragment in (
        "allocation->HostState==VioGpuWddmAllocationHostLive",
        "allocation->PlacementValid",
        "allocation->PoolGeneration!=0",
        "allocation->ResourceId>=VIOGPU_NATIVE_RESOURCE_ID_START",
        "allocation->BlobId!=0",
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
        "patchArguments->hContext==NULL",
        "patchArguments->pAllocationList==NULL",
        "patchArguments->AllocationListSize==0",
        "patchArguments->pPatchLocationList==NULL",
        "patchArguments->PatchLocationListSize==0",
        "patchArguments->PatchLocationListSubmissionStart==0",
        "patchArguments->PatchLocationListSubmissionLength==0",
        "ResolvePagingBatch(",
        "VioGpuWddmPagingTransactionBuilt",
        "returnexact?STATUS_SUCCESS:STATUS_INVALID_PARAMETER;",
    ):
        if fragment not in paging_patch:
            fail(f"paging Patch must validate one exact no-op submission range: {fragment}")
    if "AcquireNativeSubmissionOperation" in paging_patch or "RecordNativeSubmissionFence" in paging_patch:
        fail("paging Patch must remain a validation-only no-op")
    require_order(
        patch,
        (
            "ResolveSubmissionPrivateData(",
            "patch->PatchOffset!=submission->CommandStreamOffset+reference->PatchOffset",
            "allocation->PrivateData.RequestedIova<=MAXULONGLONG-reference->AllocationOffset",
            "patchedResourceIds[index]=allocation->ResourceId;",
            "patchedIovas[index]=allocation->PrivateData.RequestedIova+reference->AllocationOffset;",
            "RtlCopyMemory(&submitBo->Handle,&patchedResourceIds[index],sizeof(patchedResourceIds[index]));",
            "RtlCopyMemory(patchAddress,&patchedIovas[index],sizeof(patchedIovas[index]));",
            "adapter->RefreshNativeSubmit(submission->VirtioBuffer,submission->CommandStream,submission->CommandStreamSize)",
            "submission->FenceId=patchArguments->SubmissionFenceId;",
            "KeMemoryBarrier();",
            "submission->PatchApplied=TRUE;",
        ),
        "Patch must validate placement, write resource IDs and requested IOVAs, refresh payload and publish the WDDM fence",
    )
    if "ReleasePreparedSubmission(submission);" not in patch:
        fail("Patch failure must quarantine the prepared submission")

    submit_body = function_body("VioGpuWddmSubmitCommand", WDDM_DDI_CODE)
    submit = canonical_code(submit_body)
    if "KeGetCurrentIrql()!=DISPATCH_LEVEL" not in submit:
        fail("SubmitCommand must enforce its DISPATCH_LEVEL contract")
    if "submitCommand->SubmissionFenceId>MAXUINT" not in submit:
        fail("SubmitCommand must reject fence values that the bounded tracker cannot represent")
    for forbidden in ("KeWaitForSingleObject", "PAGED_CODE", "new(", "ExAllocatePool"):
        if forbidden in submit_body:
            fail(f"SubmitCommand must not wait or allocate pageable state at DISPATCH_LEVEL: {forbidden}")
    require_order(
        submit,
        (
            "status=ResolveSubmissionPrivateData(",
            "submission->PatchApplied",
            "InterlockedCompareExchange(&submission->State,VioGpuWddmSubmissionQueued,VioGpuWddmSubmissionPrepared)",
            "adapter->RecordNativeSubmissionFence(fenceId)",
            "RecordContextUmdFence(submission->Context,submission->UmdFenceId)",
            "adapter->QueueNativeSubmit(buffer,fenceId)",
            "InvalidateContextUmdFenceTracker(submission->Context)",
        ),
        "SubmitCommand must resolve exact private data before Prepared-to-Queued publication and enqueue",
    )
    paging_worker = canonical_code(function_body("NativePagingBatchWorker", WDDM_DDI_CODE))
    for fragment in (
        "privateData->Kind==VioGpuWddmDmaKindPaging",
        "ResolvePagingBatch(",
        "packet->ContextId==0||!adapter->IsNativeContextGenerationCurrent(packet->ContextGeneration,packet->ResetGeneration)",
        "adapter->RecordNativeSubmissionFence(submitCommand->SubmissionFenceId)",
        "adapter->NotifyNativeSoftwareCompletion(",
        "ReleaseQueuedSubmission(submission,TRUE);",
        "adapter->NotifyNativeSubmissionFault(",
    ):
        if fragment not in submit and fragment not in paging_worker:
            fail(f"SubmitCommand must retain paging/render completion and fault semantics: {fragment}")
    if not submit.endswith("returnSTATUS_SUCCESS;") or "returnSTATUS_NOT_SUPPORTED;" in submit:
        fail("SubmitCommand must convert post-validation scheduler failures into fault notification plus success")

    complete = canonical_code(function_body("NativeSubmissionComplete", WDDM_DDI_CODE))
    for fragment in (
        "response->type==VIRTIO_GPU_RESP_OK_NODATA",
        "response->flags==expectedFlags",
        "response->fence_id==submission->FenceId",
        "response->ctx_id==submission->ContextId",
        "response->ring_idx==1",
        "adapter->IsNativeContextGenerationCurrent(submission->Generation,submission->ResetGeneration)",
        "RetireContextUmdFence(context,submission->UmdFenceId)",
        "ReleaseQueuedSubmission(submission,TRUE)",
        "adapter->NotifyNativeSubmissionCompletion(fenceId,nodeOrdinal,engineOrdinal,FALSE);",
        "adapter->NotifyNativeSubmissionFault(",
    ):
        if fragment not in complete:
            fail(f"native completion must validate Host retirement before VidSch notification: {fragment}")
    cancelled = canonical_code(function_body("NativeSubmissionCancelled", WDDM_DDI_CODE))
    if "QuarantineSubmission(submission,VioGpuWddmSubmissionQueued,FALSE);" not in cancelled or (
        "NotifyNativeSubmissionCompletion" in cancelled
    ):
        fail("reset cancellation must release ownership without fabricating a completed fence")
    queue_failed = canonical_code(function_body("NativeSubmissionQueueFailed", WDDM_DDI_CODE))
    require_order(
        queue_failed,
        ("ReleaseQueuedSubmission(submission,FALSE)", "adapter->NotifyNativeSubmissionFault("),
        "permanent backlog enqueue failure must release ownership and fault the scheduler",
    )
    fault = canonical_code(function_body("VioGpuDod::NotifyNativeSubmissionFault", VIOGPU_CODE))
    if "InvalidateNativeFenceTracker();" not in fault or "ResetNativeFenceTracker();" in fault:
        fail("a native submission fault must clear pending fences while preserving the submitted reset endpoint")

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
    if driver_caps.count("driverCaps->SchedulingCaps.PreemptionAware=0;") != 1:
        fail("the reset-only Native Context path must not advertise Windows 8 hardware preemption")

    reset_timeout = canonical_code(function_body("VioGpuDod::ResetFromTimeout", VIOGPU_CODE))
    require_order(
        reset_timeout,
        (
            "InterlockedExchange(&m_HardwareResetState,VioGpuHardwareResetRequested);",
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
    d0_recovery_blocks = [
        canonical_code(body)
        for condition, body, _, _ in if_blocks(function_body("VioGpuDod::SetPowerState", VIOGPU_CODE))
        if canonical_code(condition) == "resetRecovery"
    ]
    if len(d0_recovery_blocks) != 1 or "CompleteNativeFenceReset();" not in d0_recovery_blocks[0]:
        fail("the existing D0 recovery path must publish the reset fence after claiming Active")
    d0_recovery = d0_recovery_blocks[0]
    if d0_recovery.find("InterlockedCompareExchange(&m_HardwareResetState,VioGpuHardwareActive,") > d0_recovery.find(
        "CompleteNativeFenceReset();"
    ):
        fail("D0 recovery must claim Active before publishing the reset fence endpoint")
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
        ("m_CtrlQueue.DetachNativeSubmitBacklog();", "m_CtrlQueue.Close();", "m_GpuBuf.Close();"),
        "teardown must detach software backlog links before buffer cancellation and backing-pool close",
    )


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
    ):
        if pci_header.count(member) != 1:
            fail(f"PCI resources must retain exactly one interrupt ownership field: {member}")
    constructor = (
        "CPciResources():m_pDxgkInterface(NULL),m_InterruptFlags(0),"
        "m_InterruptMessageCount(0),m_InterruptMessageCountKnown(FALSE){}"
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
    owner_offset = close.find(clear_owner, message_trust_offset)
    if min(
        loop_offset,
        unmap_offset,
        record_offset,
        failure_offset,
        reset_offset,
        flags_offset,
        messages_offset,
        message_trust_offset,
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
        < owner_offset
    ):
        fail("PCI close must retain interrupt ownership until every BAR unmap succeeds")
    if (
        close.count(unmap_bar) != 1
        or len(loops) != 2
        or close.count(clear_flags) != 1
        or close.count(clear_messages) != 1
        or close.count(clear_message_trust) != 1
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

    begin_recovery = (
        "LONGstartResetState=InterlockedCompareExchange(&m_HardwareResetState,"
        "VioGpuHardwareRecovering,VioGpuHardwareActive);"
        "if(startResetState==VioGpuHardwareResetRequested){"
        "startResetState=InterlockedCompareExchange(&m_HardwareResetState,"
        "VioGpuHardwareRecovering,VioGpuHardwareResetRequested);}"
        "if(startResetState!=VioGpuHardwareActive&&startResetState!=VioGpuHardwareResetRequested)"
        "{returnSTATUS_DEVICE_NOT_READY;}"
    )
    rollback_recovery = (
        "InterlockedCompareExchange(&m_HardwareResetState,startResetState,"
        "VioGpuHardwareRecovering);"
    )
    final_publish = (
        "InterlockedCompareExchange(&m_HardwareResetState,VioGpuHardwareActive,"
        "VioGpuHardwareRecovering)!=VioGpuHardwareRecovering"
    )
    final_publish_offset = start_compact.find(final_publish, allocation_offset)
    started_offset = start_compact.find("m_Flags.DriverStarted=TRUE;", final_publish_offset)
    if (
        start_compact.count(begin_recovery) != 1
        or start_compact.count(rollback_recovery) != 3
        or start_compact.count(final_publish) != 1
        or not retained_offset < start_compact.find(begin_recovery) < interface_copy
        or final_publish_offset < allocation_offset
        or started_offset < final_publish_offset
    ):
        fail("StartDevice must claim initial or stopped recovery, roll back to its source state, and publish only a complete adapter")

    allocation_failure_end = start.find("Status = GetRegisterInfo()")
    pre_adapter_failures = start[:allocation_failure_end]
    rollback_returns = re.findall(
        r"\bInterlockedCompareExchange\s*\(\s*&m_HardwareResetState\s*,\s*startResetState\s*,\s*"
        r"VioGpuHardwareRecovering\s*\)\s*;\s*return\s+(?:Status|STATUS_GRAPHICS_DRIVER_MISMATCH)\s*;",
        pre_adapter_failures,
        re.DOTALL,
    )
    if len(rollback_returns) != 3:
        fail("every pre-adapter StartDevice failure must roll back only its own Recovering claim")

    failed_start_cleanup = re.findall(
        r"\breturn\s+UnwindFailedStart\s*\(\s*"
        r"(?:Status|STATUS_UNSUCCESSFUL|STATUS_DEVICE_NOT_READY)\s*\)\s*;",
        start,
    )
    if len(failed_start_cleanup) != 4 or start_compact.count("UnwindFailedStart(") != 4:
        fail("every post-allocation StartDevice failure must use the shared ownership-safe unwind")
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
            "VioGpuDod::QueryVidMmSegment",
            "returnFALSE;",
            "VioGpuAdapter*adapter=m_pHWDevice;"
            "BOOLEANavailable=!IsHardwareResetRequested()&&adapter!=NULL&&"
            "adapter->QueryVidMmSegment(physicalAddress,size);"
            "ExReleaseRundownProtection(&m_HardwareOperations);returnavailable;",
        ),
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
    reset_consume = (
        "if(resetState==VioGpuHardwareResetRequested){"
        "resetRecovery=TRUE;m_pHWDevice->ResetDevice();}"
        "elseif(resetState!=VioGpuHardwareActive){returnSTATUS_DEVICE_NOT_READY;}"
    )
    reset_failure = (
        "if(!NT_SUCCESS(Status)&&resetRecovery){"
        "InterlockedCompareExchange(&m_HardwareResetState,VioGpuHardwareResetRequested,"
        "VioGpuHardwareRecovering);}"
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
    transition_offset = power_compact.find(transition, reset_consume_offset)
    reset_failure_offset = power_compact.find(reset_failure, transition_offset)
    reset_publish_offset = power_compact.find(reset_publish, reset_failure_offset)
    active_recheck_offset = power_compact.find(active_recheck, reset_publish_offset)
    if min(
        reset_claim_offset,
        reset_consume_offset,
        transition_offset,
        reset_failure_offset,
        reset_publish_offset,
        active_recheck_offset,
    ) < 0 or not (
        reset_claim_offset
        < reset_consume_offset
        < transition_offset
        < reset_failure_offset
        < reset_publish_offset
        < active_recheck_offset
    ):
        fail("D0 must own reset recovery, rebuild transport, and reject a concurrent reset before publishing Active")
    if (
        power_compact.count("BOOLEANresetRecovery=FALSE;") != 1
        or power_compact.count("InterlockedCompareExchange(&m_HardwareResetState") != 4
        or power_compact.count("m_pHWDevice->ResetDevice();") != 1
        or "InterlockedExchange(&m_HardwareResetState" in power_compact
    ):
        fail("D0 reset recovery must use only the four checked three-state CAS operations")

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
    check_adapter_line_interrupt_bitmap()
    check_native_context_readiness()
    check_no_retired_variant_contract(sources)
    check_queue_failure_semantics()
    check_synchronous_2d_control_transactions()
    check_wddm_2d_resource_ownership()
    check_wddm_standard_primary_paging()
    check_wddm_standard_primary_scanout()
    check_native_context_ownership()
    check_wddm_private_abi(root)
    check_wddm_paging_transaction_gate()
    check_wddm_guest_allocation_lifecycle()
    check_wddm_context_lifetime()
    check_wddm_submission_lifetime()
    check_dpc_completion_semantics()
    check_segment_failure_semantics()
    check_pci_resource_lifetime()
    check_adapter_lifecycle()
    check_worker_thread_lifetime()
    check_project_safety(root)
    print("viogpuwddm compile-only safety contract: PASS")


if __name__ == "__main__":
    main()
