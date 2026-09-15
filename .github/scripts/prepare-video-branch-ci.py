#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""One-time exact migration of the independent video branch's CI contracts."""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
BRANCH = "work/vpu-video-umd-perf-20260915"
EXPECTED = {
    ".github/workflows/viogpuwddm-arm64-ci.yml": "60ce73243c81cd1ab3a6edd8e45a3796ea6546df",
    ".github/workflows/build-arm64-drivers.yml": "3837dc3ab42456681577af60d37cc8de56627bb9",
    ".github/workflows/viogpu-perf-regression.yml": "d9171bcd27a7dc154e4c433826c5739ca0b74e75",
    ".github/workflows/viogpu-video-ci.yml": "91204dca13e61f49f1f465fddf1df5bd16c51311",
    "viogpu/viogpuwddm/check-contract.py": "a27b84dc09d03dd5a9936449c42fcb73ed41f624",
    "viogpu/video/README.md": "a1c9b10c0c12493efbe138370e3187e47b93f2c1",
}
VIDEO_NAMES = (
    "VioGpuVideoOpen", "VioGpuVideoControl", "VioGpuVideoAllocate",
    "VioGpuVideoQueue", "VioGpuVideoDequeue", "VioGpuVideoCopy",
    "VioGpuVideoStream", "VioGpuVideoClose",
)
EXTENSION = "$expectedExports += @(" + ", ".join(repr(n) for n in VIDEO_NAMES) + ")"
LEGACY = "$expectedExports = @('OpenAdapter', 'OpenAdapter10', 'OpenAdapter10_2')"


def replace_once(source: str, old: str, new: str) -> str:
    """Refuse a changed input instead of editing a neighboring check by accident."""
    if source.count(old) != 1:
        raise RuntimeError("Expected exactly one migration target: " + old[:120])
    return source.replace(old, new)


def main() -> None:
    """Verify every source blob first, then update only the intended contracts."""
    sources = {}
    for name, expected in EXPECTED.items():
        raw = (ROOT / name).read_bytes()
        actual = hashlib.sha1(b"blob " + str(len(raw)).encode() + b"\0" + raw).hexdigest()
        if actual != expected:
            raise RuntimeError(f"Concurrent source change: {name}: {actual}")
        sources[name] = raw.decode("utf-8")

    for name in (".github/workflows/viogpuwddm-arm64-ci.yml", ".github/workflows/build-arm64-drivers.yml"):
        sources[name] = replace_once(sources[name], LEGACY, LEGACY + "\n          " + EXTENSION)
        comparison = "$exportDelta = Compare-Object -ReferenceObject $expectedExports -DifferenceObject $exportNames"
        sources[name] = replace_once(sources[name], comparison,
            'if ($exportNames.Count -eq 0) { throw "UMD export table mismatch: no parsed exports" }' +
            "\n          " + comparison)

    name = "viogpu/viogpuwddm/check-contract.py"
    target = '''            fail(f"{label} workflow must verify the exact legacy D3D UMD exports")'''
    added = target + "\n        if source.count(" + repr(EXTENSION) + ''') != 1:
            fail(f"{label} workflow must verify exactly eight explicit video bridge exports")'''
    sources[name] = replace_once(sources[name], target, added)

    name = ".github/workflows/viogpuwddm-arm64-ci.yml"
    sources[name] = replace_once(sources[name], "branches: [ master, viogpu-wddm3.1 ]",
                                "branches: [ master, viogpu-wddm3.1, " + BRANCH + " ]")
    # Preserve every existing path; the paired tests and media bridge must also trigger WDDM CI.
    path = '      - "viogpu/viogpud3d/**"'
    if sources[name].count(path) != 2:
        raise RuntimeError("Unexpected full-WDDM trigger lists")
    sources[name] = sources[name].replace(path, path + '\n      - "viogpu/video/**"\n      - "viogpu/tests/video/**"')
    target = "      - name: Validate ARM64 full-miniport INF and PE"
    step = '''      - name: Prove both production export gates reject missing and unknown functions
        shell: pwsh
        run: ./viogpu/tests/video/binary-export-contract.ps1 -Umd viogpu/viogpuwddm/objfre_win11_arm64/arm64/viogpud3d.dll

'''
    sources[name] = replace_once(sources[name], target, step + target)
    sources[name] = replace_once(sources[name], "\njobs:\n", "\npermissions:\n  contents: read\n\njobs:\n")

    name = ".github/workflows/viogpu-perf-regression.yml"
    sources[name] = replace_once(sources[name], "branches: [perf/droidvm-gpu-20260914]",
                                "branches: [perf/droidvm-gpu-20260914, " + BRANCH + "]")

    name = ".github/workflows/viogpu-video-ci.yml"
    target = "      - name: Upload unsigned review build and test client"
    sources[name] = replace_once(sources[name], target, step + target)
    old = "'viogpu/viogpuwddm/check-contract.py', '.github/workflows/viogpu-video-ci.yml'"
    new = "'viogpu/viogpuwddm/check-contract.py', '.github/workflows/viogpuwddm-arm64-ci.yml', '.github/workflows/build-arm64-drivers.yml', '.github/workflows/viogpu-video-ci.yml'"
    if sources[name].count(old) != 2:
        raise RuntimeError("Unexpected video trigger lists")
    sources[name] = sources[name].replace(old, new)

    name = "viogpu/video/README.md"
    sources[name] = replace_once(sources[name],
        "Development branch: `work/vpu-video-umd-perf-20260915` (draft PR #3).",
        "Development branch: `work/vpu-video-umd-perf-20260915`.\n"
        "PR #3 was closed without merging at the owner's request. Development and\n"
        "verification continue directly on this independent branch; no open PR is\n"
        "required. Branch pushes run video, full-WDDM, performance and repository\n"
        "format checks. CI success is not a hardware acceptance result.")
    for name, source in sources.items():
        (ROOT / name).write_text(source, encoding="utf-8")
    print("PASS: exact export contract and independent branch triggers migrated")


def publish_objects() -> None:
    """Prepare an unattached commit; only the connected reviewer updates the branch ref."""
    repo = "sunflower2333/gunyah-guest-drivers-windows"
    parent = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    if os.environ.get("GITHUB_REPOSITORY") != repo or os.environ.get("GITHUB_REF") != "refs/heads/" + BRANCH:
        raise RuntimeError("Wrong repository or branch")

    def api(endpoint: str, payload=None):
        """Use the job token only for this repository's Git-object API."""
        args = ["gh", "api", "repos/" + repo + "/" + endpoint]
        if payload is not None:
            args += ["--method", "POST", "--input", "-"]
        result = subprocess.run(args, input=None if payload is None else json.dumps(payload),
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                check=True, timeout=120)
        return json.loads(result.stdout)

    head = api("git/ref/heads/" + BRANCH)["object"]["sha"]
    if head != parent:
        raise RuntimeError("Branch advanced during validation; refusing to prepare a stale commit")
    temporary = [".github/scripts/prepare-video-branch-ci.py",
                 ".github/workflows/viogpu-video-branch-ci-once.yml"]
    paths = subprocess.check_output(["git", "diff", "--name-only", "HEAD"], cwd=ROOT, text=True).splitlines()
    paths = sorted(set(paths + temporary))
    elements = []
    for name in paths:
        if not (name.startswith("viogpu/") or name in EXPECTED or name in temporary or
                name == ".install_scripts/viogpud3d-interpose.c"):
            raise RuntimeError("Out-of-scope change: " + name)
        path = ROOT / name
        mode = "100755" if path.exists() and path.stat().st_mode & 0o111 else "100644"
        element = {"path": name, "mode": mode, "type": "blob"}
        if name in temporary or not path.exists():
            element["sha"] = None
        else:
            element["content"] = path.read_text(encoding="utf-8")
        elements.append(element)
    base = subprocess.check_output(["git", "rev-parse", "HEAD^{tree}"], cwd=ROOT, text=True).strip()
    tree = api("git/trees", {"base_tree": base, "tree": elements})["sha"]
    message = ("viogpu/video: validate exact binary exports on the independent branch\n\n"
               "Preserve INF, AA64, unknown-DDI and strict count checks. Exercise both\n"
               "production PowerShell export gates with missing/unknown/duplicate fixtures\n"
               "and the built UMD. Enable full-WDDM and performance CI on branch pushes.\n"
               "Normalize existing C/C++ formatting with repository clang-format 16 and\n"
               "retain all regression gates. Remove the one-time preparation workflow.\n\n"
               "No runtime codec or hardware validation is inferred from CI.")
    commit = api("git/commits", {"message": message, "tree": tree, "parents": [parent]})["sha"]
    record = {"repository": repo, "branch": BRANCH, "parent": parent,
              "tree": tree, "commit": commit, "paths": paths,
              "branch_updated": False, "runtime_validated": False}
    (ROOT / "video-ci-candidate.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(record, indent=2))


if __name__ == "__main__":
    if sys.argv[1:] == ["--publish-objects"]:
        publish_objects()
    elif sys.argv[1:]:
        raise SystemExit("unexpected arguments")
    else:
        main()
