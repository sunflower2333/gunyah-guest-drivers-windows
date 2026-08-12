#!/usr/bin/env python3

import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Optional


PROJECT_DIR = Path(__file__).resolve().parent
DRIVER_SOURCE_PATH = (PROJECT_DIR / "driver_entry.cpp").resolve()
DRIVER_SOURCE = DRIVER_SOURCE_PATH.read_text(encoding="utf-8")
PROJECT = PROJECT_DIR / "viogpuwddm.vcxproj"
WPP_NON_OWNER_TEMPLATE = PROJECT_DIR / "wpp-non-owner.tpl"
NAMESPACE = {"msbuild": "http://schemas.microsoft.com/developer/msbuild/2003"}
REGISTRATION_HELPER = "VioGpuWddmInitializeMiniportCompileOnly"


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
            blank(offset, end)
            offset = end
            continue

        offset += 1

    return "".join(result)


DRIVER_CODE = strip_cpp_comments_and_literals(DRIVER_SOURCE)


def fail(message: str) -> None:
    print(f"viogpuwddm contract failure: {message}", file=sys.stderr)
    raise SystemExit(1)


def function_body_span(name: str, source: Optional[str] = None) -> tuple[str, int, int]:
    if source is None:
        source = DRIVER_CODE

    matches = list(re.finditer(rf"\b{re.escape(name)}\s*\([^;{{}}]*?\)\s*\{{", source, re.DOTALL))
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


def require_fragment(body: str, fragment: str, owner: str) -> None:
    if fragment not in body:
        fail(f"{owner} is missing {fragment}")


def project_compile_sources(root: ET.Element) -> dict[Path, str]:
    sources: dict[Path, str] = {}
    for element in root.findall(".//msbuild:ClCompile[@Include]", NAMESPACE):
        include = element.attrib["Include"].replace("\\", "/")
        path = (PROJECT_DIR / include).resolve()
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
    check_project_safety(root)
    print("viogpuwddm compile-only safety contract: PASS")


if __name__ == "__main__":
    main()
