#!/usr/bin/env python3
"""Execute production scanout bounds, packets and response ownership."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--negative-control", choices=("timeout-release", "malformed-success", "scanout-opcode"))
args = parser.parse_args()
here = Path(__file__).resolve().parent
root = here.parents[2]


def definition(text, signature):
    start = text.index(signature)
    brace = text.index("{", start)
    if ";" in text[start:brace]:
        raise ValueError(f"Expected definition: {signature}")
    end, depth = brace + 1, 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


queue = (root / "viogpu/common/viogpu_queue.cpp").read_text()
header = (root / "viogpu/common/viogpu.h").read_text()
wire = (root / "viogpu/common/viogpu_3d_wire.h").read_text()
queue_header = (root / "viogpu/common/viogpu_queue.h").read_text()
declarations = [definition(wire, "enum virtio_gpu_ctrl_type") + ";",
                definition(header, "enum virtio_gpu_formats") + ";"]
for tag in ("virtio_gpu_rect", "virtio_gpu_ctrl_hdr", "virtio_gpu_resource_create_blob",
            "virtio_gpu_set_scanout_blob", "virtio_gpu_mem_entry"):
    declarations.append(re.search(r"#pragma pack\(1\)\s*typedef struct " + tag +
                                  r"\s*\{.*?\}[^;]+;\s*#pragma pack\(\)", header, re.S).group(0))
declarations.append(re.search(r"typedef struct\s*\{[^{}]+\} VIOGPU_PRIMARY_SCANOUT_LAYOUT;", header).group(0))
declarations.append(definition(queue_header, "enum VIOGPU_HOST_CONTEXT_RESULT") + ";")
for name, contents in (("VIOGPU_NATIVE_RESOURCE_ID_START", header),
                       ("VIRTIO_GPU_MAX_SCANOUTS", header),
                       ("VIRTIO_GPU_BLOB_MEM_GUEST", wire),
                       ("VIOGPU_MAX_BACKING_ENTRIES", queue_header)):
    declarations.append(re.search(r"^#define " + name + r"\s+[^\n]+", contents, re.M).group(0))
methods = "\n".join(definition(queue, signature) for signature in (
    "static BOOLEAN IsPlainControlResponse(", "static BOOLEAN IsPlainControlErrorResponse(",
    "static BOOLEAN IsStandard2DResourceId(",
    "VIOGPU_HOST_CONTEXT_RESULT CtrlQueue::SubmitSynchronousNoDataLocked(",
    "VIOGPU_HOST_CONTEXT_RESULT CtrlQueue::CreateGuestBlobSynchronous(",
    "VIOGPU_HOST_CONTEXT_RESULT CtrlQueue::SetScanoutBlobSynchronous("))
mutations = {
    "timeout-release": ("if (releaseBuffer)", "if (TRUE)"),
    "malformed-success": ("IsPlainControlResponse(response, VIRTIO_GPU_RESP_OK_NODATA)",
                          "(IsPlainControlResponse(response, VIRTIO_GPU_RESP_OK_NODATA) || "
                          "(response != NULL && response->type == VIRTIO_GPU_RESP_OK_NODATA))"),
    "scanout-opcode": ("command->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT_BLOB;",
                       "command->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;"),
}
if args.negative_control:
    before, after = mutations[args.negative_control]
    if methods.count(before) != 1:
        raise ValueError("Negative control no longer matches exactly one production site")
    methods = methods.replace(before, after)

with tempfile.TemporaryDirectory(prefix="viogpu-guest-scanout-") as temporary:
    directory = Path(temporary)
    packet_source = directory / "packets.cpp"
    fixture = (here / "guest_scanout_packets_test.cpp").read_text()
    fixture = fixture.replace("// INSERT_DECLARATIONS", "\n".join(declarations))
    fixture = fixture.replace("// INSERT_PRODUCTION", methods)
    packet_source.write_text(fixture)
    sources = [packet_source] if args.negative_control else [here / "guest_scanout_test.cpp", packet_source]
    for source in sources:
        if shutil.which("cl"):
            binary = directory / "test.exe"
            command = ["cl", "/nologo", "/EHsc", "/W4", "/WX", "/std:c++17",
                       f"/I{root / 'viogpu/common'}", str(source), f"/Fe{binary}"]
        else:
            binary = directory / "test"
            command = ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                       "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                       "-I", str(root / "viogpu/common"), str(source), "-o", str(binary)]
        subprocess.run(command, cwd=directory, check=True)
        if args.negative_control:
            result = subprocess.run([str(binary)], cwd=directory, capture_output=True, text=True)
            if result.returncode == 0 or "CHECK FAILED:" not in result.stderr:
                raise RuntimeError(f"Negative control did not reach the expected assertion: {result}")
            print(f"Negative control {args.negative_control}: caught by {result.stderr.strip().splitlines()[0]}")
        else:
            subprocess.run([str(binary)], cwd=directory, check=True)
        # Windows runners can retain the exited image briefly (AV/image cleanup).
        for attempt in range(21):
            try:
                binary.unlink()
                break
            except PermissionError:
                if attempt == 20:
                    raise
                time.sleep(0.25)
