#!/usr/bin/env python3
import hashlib
from pathlib import Path


ROOT = Path(__file__).resolve().parent
REPO = ROOT.parent
INSTALLER = ROOT / "install-viogpu-58194-2141fc68.ps1"
RUNNER = ROOT / "run-zink-d3d11-offscreen-58194.ps1"
KMD_PACKAGE = REPO / ".artifacts/run-33499952618-signed-2141fc68/drivers/viogpu"
CERTIFICATE = REPO / ".artifacts/run-33499952618-signed-2141fc68/DroidVM_Test.cer"
ZINK_BUNDLE = REPO.parent / "reference/codes/virtio-win-mesa/.artifacts/33531015749-viogpud3d-zink-offscreen-arm64"


def require(text: str, needle: str, path: Path) -> None:
    if needle not in text:
        raise AssertionError(f"{path.name}: missing {needle!r}")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def require_hashes(root: Path, expected: dict[str, str]) -> None:
    for name, expected_hash in expected.items():
        path = root / name
        if not path.is_file():
            raise AssertionError(f"missing artifact file: {path}")
        actual_hash = sha256(path)
        if actual_hash != expected_hash:
            raise AssertionError(
                f"artifact hash mismatch for {path}: {actual_hash} != {expected_hash}"
            )


installer = INSTALLER.read_text(encoding="utf-8")
runner = RUNNER.read_text(encoding="utf-8")

kmd_hashes = {
    "viogpud3d.dll": "3860bfeb8c9834788535241a2adaf4933fea30f71de2081d2896c24884a77736",
    "viogpuwddm.cat": "fc1c382b07d05450a35df713e9642cfb62a1c545e83d84e70f878513288fa9ba",
    "viogpuwddm.inf": "5e2eacb94ed82ceeddd8f3cb122c33d38ea016ecd19752b3e68083f0755e7a8b",
    "viogpuwddm.sys": "45ef273edf3cd4766a541ecb3952a47165735c074037eb851dc59ec753e270d4",
}
zink_hashes = {
    "viogpud3d-zink.dll": "02fe46cc550676b7eda752dc70905b91dc404eded084cb9fb56565c0d87e8991",
    "zink_d3d11_offscreen.exe": "c5998734901ed7f92d5e502fc7308b3455bfa212e0b7e11b73ecabbe83c93b99",
}
require_hashes(KMD_PACKAGE, kmd_hashes)
require_hashes(ZINK_BUNDLE, zink_hashes)
if sha256(CERTIFICATE) != "da88f450dbd881c91511c5b801295cd9cc0d3ca84ea9d3dc08eff9481eed64b3":
    raise AssertionError("signed artifact certificate hash mismatch")

inf_text = (KMD_PACKAGE / "viogpuwddm.inf").read_text(encoding="utf-8-sig")
require(inf_text, "100.6.101.58194", KMD_PACKAGE / "viogpuwddm.inf")
require(inf_text, "HKR,Parameters,RenderOnly,%REG_DWORD%,1", KMD_PACKAGE / "viogpuwddm.inf")
require(inf_text, "HKR,,RenderOnly,%REG_DWORD%,1", KMD_PACKAGE / "viogpuwddm.inf")

for needle in (
    "100.6.101.58194",
    "100.6.101.58193",
    "oem28.inf",
    "Get-OptionalPnpData",
    "Assert-ExactDriverState",
    "pnputil.exe /export-driver",
    "pnputil.exe /add-driver",
    "viogpu-rollback-pre-58194-2141fc68",
    "3860bfeb8c9834788535241a2adaf4933fea30f71de2081d2896c24884a77736",
    "fc1c382b07d05450a35df713e9642cfb62a1c545e83d84e70f878513288fa9ba",
    "5e2eacb94ed82ceeddd8f3cb122c33d38ea016ecd19752b3e68083f0755e7a8b",
    "45ef273edf3cd4766a541ecb3952a47165735c074037eb851dc59ec753e270d4",
    "cc69318eec054240e73e4fc7c4a243c4fd33349915a361ec9a4d51f9e308e108",
    "9279c11fb3114f440cd0d033f644a54c6de8106eb8870a046d3534c1ef3d6de1",
    "b08152a7ad2b8f58137779661e85bdf706ea6fff48dab1b03bf6317a5e38a42c",
    "78e61f69787185fda68754842b0a46e4de8cf265e77139401b7a9b66537eaf37",
    "The staged INF does not default device RenderOnly to 1.",
    "The staged INF does not default service Parameters\\RenderOnly to 1.",
    "Get-AuthenticodeSignature",
):
    require(installer, needle, INSTALLER)

if installer.index("pnputil.exe /export-driver") > installer.index("pnputil.exe /add-driver"):
    raise AssertionError("installer must export rollback before installation")
if "drivers/NetKVM" in installer or "drivers\\NetKVM" in installer:
    raise AssertionError("installer must remain VioGPU-only")

for needle in (
    "100.6.101.58194",
    "Assert-ExactBundle",
    "Assert-ExactDriverState",
    "Get-NewOrChangedDumps",
    "WaitForExit($TimeoutSeconds * 1000)",
    "$process.Kill()",
    "checksum=2088960",
    "viogpud3d-zink.dll",
    "zink_d3d11_offscreen.exe",
    "02fe46cc550676b7eda752dc70905b91dc404eded084cb9fb56565c0d87e8991",
    "c5998734901ed7f92d5e502fc7308b3455bfa212e0b7e11b73ecabbe83c93b99",
    "45ef273edf3cd4766a541ecb3952a47165735c074037eb851dc59ec753e270d4",
    "3860bfeb8c9834788535241a2adaf4933fea30f71de2081d2896c24884a77736",
    "Probe stderr is not empty.",
    "New or changed dumps:",
    "Post-probe driver state failed:",
    "DriverAfterError = $driverAfterError",
):
    require(runner, needle, RUNNER)

for forbidden in ("pnputil", "reg.exe", "Set-ItemProperty", "Import-Certificate"):
    if forbidden in runner:
        raise AssertionError(f"runner must not mutate installation state: {forbidden}")

print("VioGPU 58194 and Zink staging contract: PASS")
