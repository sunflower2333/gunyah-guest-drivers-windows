#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Apply the reviewed viogpu KMD hot-path transforms before ClCompile.

The GitHub contents API replaces whole files, while wddmddi.cpp is very large.
Keep the performance edits reviewable and deterministic: every transform is an
exact one-shot replacement and the build fails if the expected source shape is
missing or ambiguous. Re-running after a successful transform is idempotent.
"""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]


def replace_once(path: Path, old: str, new: str, label: str) -> None:
    text = path.read_text(encoding="utf-8")
    old_count = text.count(old)
    new_count = text.count(new)
    if old_count == 1 and new_count == 0:
        path.write_text(text.replace(old, new, 1), encoding="utf-8")
        print(f"perf-patch: {label}: applied")
        return
    if old_count == 0 and new_count == 1:
        print(f"perf-patch: {label}: already applied")
        return
    raise RuntimeError(
        f"{label}: expected one old or one new block, got old={old_count} new={new_count}"
    )


def patch_queue() -> None:
    header = ROOT / "viogpu/common/viogpu_queue.h"
    source = ROOT / "viogpu/common/viogpu_queue.cpp"

    replace_once(
        header,
        "    PVOID AllocateMemory(SIZE_T size, SIZE_T alignment = PAGE_SIZE);\n"
        "    void FreeMemory(PVOID address);",
        "    PVOID AllocateMemoryUninitialized(SIZE_T size);\n"
        "    PVOID AllocateMemory(SIZE_T size, SIZE_T alignment = PAGE_SIZE);\n"
        "    void FreeMemory(PVOID address);",
        "queue allocator declaration",
    )

    replace_once(
        source,
        "    PVOID payload = m_pBuf->AllocateMemory(command_size, sizeof(ULONGLONG));\n"
        "    if (payload == NULL)",
        "    /* The validated command stream overwrites the complete payload. */\n"
        "    PVOID payload = m_pBuf->AllocateMemoryUninitialized(command_size);\n"
        "    if (payload == NULL)",
        "native submit payload allocation",
    )

    replace_once(
        source,
        "PVOID VioGpuBuf::AllocateMemory(SIZE_T size, SIZE_T alignment)\n"
        "{\n"
        "    UNREFERENCED_PARAMETER(alignment);\n"
        "    PVOID address = ExAllocatePoolUninitialized(NonPagedPoolNx, size, VIOGPUTAG);\n"
        "    if (address != NULL)\n"
        "    {\n"
        "        RtlZeroMemory(address, size);\n"
        "    }\n"
        "    return address;\n"
        "}",
        "PVOID VioGpuBuf::AllocateMemoryUninitialized(SIZE_T size)\n"
        "{\n"
        "    if (size == 0)\n"
        "    {\n"
        "        return NULL;\n"
        "    }\n"
        "    return ExAllocatePoolUninitialized(NonPagedPoolNx, size, VIOGPUTAG);\n"
        "}\n"
        "\n"
        "PVOID VioGpuBuf::AllocateMemory(SIZE_T size, SIZE_T alignment)\n"
        "{\n"
        "    UNREFERENCED_PARAMETER(alignment);\n"
        "    PVOID address = AllocateMemoryUninitialized(size);\n"
        "    if (address != NULL)\n"
        "    {\n"
        "        RtlZeroMemory(address, size);\n"
        "    }\n"
        "    return address;\n"
        "}",
        "uninitialized pool helper",
    )


def patch_wddm_bindings() -> None:
    header = ROOT / "viogpu/viogpuwddm/wddmddi.h"
    source = ROOT / "viogpu/viogpuwddm/wddmddi.cpp"

    replace_once(
        header,
        "    ULONGLONG Length;\n"
        "    UINT PatchOffset;\n"
        "    UINT Reserved;\n"
        "};\n"
        "\n"
        "enum VIOGPU_WDDM_SUBMISSION_STATE : LONG",
        "    ULONGLONG Length;\n"
        "    UINT PatchOffset;\n"
        "    UINT Reserved;\n"
        "    /* Worker-time binding snapshot. These fields are not wire ABI. */\n"
        "    UINT PatchedResourceId;\n"
        "    UINT PatchedReserved;\n"
        "    ULONGLONG PatchedIova;\n"
        "};\n"
        "\n"
        "enum VIOGPU_WDDM_SUBMISSION_STATE : LONG",
        "submission binding snapshot fields",
    )

    replace_once(
        source,
        "    UINT *patchedResourceIds = NULL;\n"
        "    ULONGLONG *patchedIovas = NULL;\n"
        "    if (NT_SUCCESS(status))\n"
        "    {\n"
        "        patchedResourceIds = new (NonPagedPoolNx) UINT[submission->AllocationCount];\n"
        "        patchedIovas = new (NonPagedPoolNx) ULONGLONG[submission->AllocationCount];\n"
        "        if (patchedResourceIds == NULL || patchedIovas == NULL)\n"
        "        {\n"
        "            status = STATUS_NO_MEMORY;\n"
        "        }\n"
        "        else\n"
        "        {\n"
        "            RtlZeroMemory(patchedResourceIds, (SIZE_T)submission->AllocationCount * sizeof(*patchedResourceIds));\n"
        "            RtlZeroMemory(patchedIovas, (SIZE_T)submission->AllocationCount * sizeof(*patchedIovas));\n"
        "        }\n"
        "    }\n"
        "\n"
        "    BOOLEAN patchClaimed = FALSE;",
        "    BOOLEAN patchClaimed = FALSE;",
        "remove per-patch temporary binding arrays",
    )

    replace_once(
        source,
        "            const VIOGPU_WDDM_SUBMISSION_REFERENCE *reference = &submission->References[index];\n"
        "            const D3DDDI_PATCHLOCATIONLIST *patch =",
        "            VIOGPU_WDDM_SUBMISSION_REFERENCE *reference = &submission->References[index];\n"
        "            const D3DDDI_PATCHLOCATIONLIST *patch =",
        "mutable submission binding reference",
    )

    replace_once(
        source,
        "            if (valid)\n"
        "            {\n"
        "                patchedResourceIds[index] = allocation->ResourceId;\n"
        "                patchedIovas[index] = allocation->PrivateData.RequestedIova + reference->AllocationOffset;\n"
        "            }",
        "            if (valid)\n"
        "            {\n"
        "                reference->PatchedResourceId = allocation->ResourceId;\n"
        "                reference->PatchedReserved = 0;\n"
        "                reference->PatchedIova =\n"
        "                    allocation->PrivateData.RequestedIova +\n"
        "                    reference->AllocationOffset;\n"
        "            }",
        "retain validated binding snapshot",
    )

    replace_once(
        source,
        "            RtlCopyMemory(&submitBo->Handle, &patchedResourceIds[index], sizeof(patchedResourceIds[index]));\n"
        "            RtlCopyMemory(patchAddress, &patchedIovas[index], sizeof(patchedIovas[index]));",
        "            RtlCopyMemory(&submitBo->Handle,\n"
        "                          &reference->PatchedResourceId,\n"
        "                          sizeof(reference->PatchedResourceId));\n"
        "            RtlCopyMemory(patchAddress,\n"
        "                          &reference->PatchedIova,\n"
        "                          sizeof(reference->PatchedIova));",
        "consume retained binding snapshot",
    )

    replace_once(
        source,
        "    delete[] patchedIovas;\n"
        "    delete[] patchedResourceIds;\n"
        "    adapter->ReleaseNativeSubmissionOperation();",
        "    adapter->ReleaseNativeSubmissionOperation();",
        "remove binding-array frees",
    )


def validate() -> None:
    queue = (ROOT / "viogpu/common/viogpu_queue.cpp").read_text(encoding="utf-8")
    wddm = (ROOT / "viogpu/viogpuwddm/wddmddi.cpp").read_text(encoding="utf-8")
    wddm_h = (ROOT / "viogpu/viogpuwddm/wddmddi.h").read_text(encoding="utf-8")

    checks = {
        "native payload still zero-allocates": "AllocateMemory(command_size, sizeof(ULONGLONG))" not in queue,
        "uninitialized allocator missing": "AllocateMemoryUninitialized(command_size)" in queue,
        "binding temp resource array remains": "patchedResourceIds" not in wddm,
        "binding temp iova array remains": "patchedIovas" not in wddm,
        "binding snapshot fields missing": "PatchedResourceId" in wddm_h and "PatchedIova" in wddm_h,
        "binding snapshot not consumed": "reference->PatchedResourceId" in wddm and "reference->PatchedIova" in wddm,
    }
    failed = [message for message, ok in checks.items() if not ok]
    if failed:
        raise RuntimeError("; ".join(failed))


if __name__ == "__main__":
    try:
        patch_wddm_bindings()
        patch_queue()
        validate()
    except Exception as exc:
        print(f"perf-patch: ERROR: {exc}", file=sys.stderr)
        raise
    print("perf-patch: PASS KMD hot-path transforms")
