#!/usr/bin/env python3
"""Check the lost-interrupt recovery contract in VioGpuAdapter::ControlInterrupt.

This is intentionally a source-level test: the Windows driver cannot be built
on the Linux host, but the virtqueue callback contract is unambiguous.  A
false EnableInterrupt result means pending used buffers and must publish a
DPC reason after the dispatch gate is opened.
"""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "viogpu/viogpudo/viogpudo.cpp"
QUEUE_HEADER = ROOT / "viogpu/common/viogpu_queue.h"


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


def main() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    queue_header = QUEUE_HEADER.read_text(encoding="utf-8")
    body = function_body(source, "NTSTATUS VioGpuAdapter::ControlInterrupt")

    assert "BOOLEAN IsInitialized(void) const" in queue_header
    assert "if (!m_CtrlQueue.IsInitialized() || !m_CursorQueue.IsInitialized())" in body
    assert "const BOOLEAN controlQueueIdle = m_CtrlQueue.EnableInterrupt();" in body
    assert "const BOOLEAN cursorQueueIdle = m_CursorQueue.EnableInterrupt();" in body
    assert "InterlockedExchange(&m_InterruptDispatchEnabled, TRUE);" in body
    assert "pendingReasons |= ISR_REASON_DISPLAY;" in body
    assert "pendingReasons |= ISR_REASON_CURSOR;" in body
    assert "InterlockedOr((PLONG)&m_PendingWorks, pendingReasons);" in body
    assert "dxgkInterface->DxgkCbQueueDpc(dxgkInterface->DeviceHandle);" in body

    gate = body.index("InterlockedExchange(&m_InterruptDispatchEnabled, TRUE);")
    publish = body.index("InterlockedOr((PLONG)&m_PendingWorks, pendingReasons);")
    queue = body.index("dxgkInterface->DxgkCbQueueDpc(dxgkInterface->DeviceHandle);")
    assert gate < publish < queue

    # The old all-or-nothing test would strand a completion. Keep this exact
    # anti-regression check local to the enable path.
    assert not re.search(r"if \(!m_CtrlQueue\.EnableInterrupt\(\) \|\| !m_CursorQueue\.EnableInterrupt\(\)\)", body)
    print("interrupt-control source contract: PASS")


if __name__ == "__main__":
    main()
