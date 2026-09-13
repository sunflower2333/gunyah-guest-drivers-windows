#!/usr/bin/env python3
"""Exercise the Advanced Color default-build and HDR admission source contract.

The real sources must satisfy check-contract.py's advanced_color_violations().
Each mutation reintroduces one concrete defect -- an ungated ten-bit path in the
default WDDM 2.0 build, an HDR exposure that ignores usable PQ, a monitor policy
that drops the SDR display -- and must be reported with the expected message.
"""
from pathlib import Path
import importlib.util
import re
import sys

here = Path(__file__).resolve().parent
root = here.parents[2]
spec = importlib.util.spec_from_file_location("check_contract", root / "viogpu/viogpuwddm/check-contract.py")
contract = importlib.util.module_from_spec(spec)
spec.loader.exec_module(contract)

paths = {
    "viogpudo.cpp": root / "viogpu/viogpudo/viogpudo.cpp",
    "viogpudo.h": root / "viogpu/viogpudo/viogpudo.h",
    "wddmddi.cpp": root / "viogpu/viogpuwddm/wddmddi.cpp",
    "driver_entry.cpp": root / "viogpu/viogpuwddm/driver_entry.cpp",
    "advanced_color_ddi.inc": root / "viogpu/viogpuwddm/advanced_color_ddi.inc",
    "viogpu_display_color.h": root / "viogpu/shared/viogpu_display_color.h",
    "viogpuwddm.vcxproj": root / "viogpu/viogpuwddm/viogpuwddm.vcxproj",
}
sources = {name: path.read_text(encoding="utf-8") for name, path in paths.items()}

failures = 0
baseline = contract.advanced_color_violations(sources)
if baseline:
    print("FAIL production sources:", "; ".join(baseline))
    failures += 1
else:
    print("PASS production sources satisfy the Advanced Color contract")

GATE = "#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_3)\n"


def ungate(text, marker):
    """Delete the WDDM2.3 #if/#endif pair that encloses the first marker."""
    at = text.index(marker)
    start = text.rindex(GATE, 0, at)
    end = text.index("#endif\n", at)
    return text[:start] + text[start + len(GATE):end] + text[end + len("#endif\n"):]


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise SystemExit(f"mutation anchor not unique ({text.count(old)}): {old[:70]!r}")
    return text.replace(old, new)


mutations = [
    ("default private ten-bit allocation", "wddmddi.cpp",
     lambda t: ungate(t, "            return VIOGPU_WDDM_FORMAT_R10G10B10A2_UNORM;"),
     "default WDDM2.0 wddmddi.cpp references a ten-bit format"),
    ("default framebuffer PQ tag", "viogpudo.cpp",
     lambda t: ungate(t, "A ten-bit fallback primary is real PQ storage too"),
     "default WDDM2.0 viogpudo.cpp references a ten-bit format"),
    ("default interruptible monitor", "viogpudo.cpp",
     lambda t: replace_once(t, "#else\n        pChildRelations[ChildIndex].ChildCapabilities.HpdAwareness = HpdAwarenessAlwaysConnected;\n#endif",
                            "#endif"),
     "default WDDM2.0 monitor must stay always connected"),
    ("default Advanced Color registration", "driver_entry.cpp",
     lambda t: ungate(t, "initialData->DxgkDdiSetTargetGamma = VioGpuWddmSetTargetGamma;"),
     "default WDDM2.0 driver_entry.cpp exposes VioGpuWddmSetTargetGamma"),
    ("default mode set reformats storage", "viogpudo.cpp",
     lambda t: replace_once(t, "    const D3DDDIFORMAT storageFormat = pCurrentMode->DispInfo.ColorFormat;",
                            "    const D3DDDIFORMAT storageFormat = pSourceMode->Format.Graphics.PixelFormat;"),
     "default WDDM2.0 mode set must preserve framebuffer storage"),
    ("observed HDR treated as usable", "viogpudo.cpp",
     lambda t: replace_once(t, "(caps->usable_hdr_types & VIOGPU_DISPLAY_COLOR_PQ) ? 1 : 0",
                            "(caps->observed_hdr_types & VIOGPU_DISPLAY_COLOR_PQ) ? 1 : 0"),
     "HDR mode availability must require usable PQ"),
    ("ten-bit source mode without HDR", "viogpudo.cpp",
     lambda t: replace_once(t, "    if (QueryDisplayColor(&colorCaps) && IsNativeHdrModeAvailable())\n    {\n        formatCount = 2;",
                            "    if (QueryDisplayColor(&colorCaps))\n    {\n        formatCount = 2;"),
     "ten-bit source modes must require native HDR"),
    ("ten-bit source validation without HDR", "viogpudo.cpp",
     lambda t: replace_once(t, "PixelFormat == D3DDDIFMT_A2B10G10R10 && IsNativeHdrModeAvailable())",
                            "PixelFormat == D3DDDIFMT_A2B10G10R10)"),
     "ten-bit source validation must require native HDR"),
    ("colorimetry override without HDR", "viogpudo.cpp",
     lambda t: replace_once(t, "                if (QueryDisplayColor(&caps) && IsNativeHdrModeAvailable())\n                {\n                    // WDK requires",
                            "                if (QueryDisplayColor(&caps))\n                {\n                    // WDK requires"),
     "colorimetry overrides must require native HDR before ST2084"),
    ("CheckMPO3 without usable PQ", "advanced_color_ddi.inc",
     lambda t: replace_once(t, "adapter->QueryDisplayColor(&caps) && (caps.usable_hdr_types & VIOGPU_DISPLAY_COLOR_PQ) != 0 &&",
                            "adapter->QueryDisplayColor(&caps) && (caps.observed_hdr_types & VIOGPU_DISPLAY_COLOR_PQ) != 0 &&"),
     "CheckMPO3 must require usable PQ"),
    ("HDR timing without usable PQ", "advanced_color_ddi.inc",
     lambda t: replace_once(t, "if (!adapter->QueryDisplayColor(&caps) || !(caps.usable_hdr_types & VIOGPU_DISPLAY_COLOR_PQ))",
                            "if (!adapter->QueryDisplayColor(&caps))"),
     "an HDR timing must require usable PQ"),
    ("PreserveInherited refused", "advanced_color_ddi.inc",
     lambda t: replace_once(t, "    const auto wire = path.SelectedWireFormat;\n",
                            "    const auto wire = path.SelectedWireFormat;\n    if (path.Input.PreserveInherited)\n    {\n        return STATUS_NOT_SUPPORTED;\n    }\n"),
     "PreserveInherited must be applied and reported, not refused"),
    ("failed discovery drops SDR monitor", "viogpu_display_color.h",
     lambda t: replace_once(t, "return wasAdmitted ? VioGpuColorConnectionReenumerate : VioGpuColorConnectionNone;",
                            "return VioGpuColorConnectionReenumerate;"),
     "lost discovery may only withdraw an admitted HDR monitor"),
    ("monitor starts disconnected", "viogpudo.h",
     lambda t: replace_once(t, "volatile LONG m_ColorMonitorConnected = 1;", "volatile LONG m_ColorMonitorConnected = 0;"),
     "the SDR monitor must be connected before the first DVCL refresh"),
    ("model 2.3 reported by every candidate", "viogpudo.cpp",
     lambda t: replace_once(t, "#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_3) && defined(VIOGPU_REPORT_WDDM2_3)",
                            "#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_3)"),
     "driver model 2.3 may be reported only by the explicit VIOGPU_REPORT_WDDM2_3 experiment"),
    ("candidate SDR storage follows alpha format", "viogpudo.cpp",
     lambda t: replace_once(t, "                                           : D3DDDIFMT_X8R8G8B8;",
                            "                                           : D3DDDIFMT_A8R8G8B8;"),
     "candidate SDR modes must keep X8R8G8B8 framebuffer storage"),
    ("ten-bit Present reinterpretation", "wddmddi.cpp",
     lambda t: replace_once(t, "!IsPresentFormatPairSupported(source->Format, destination->Format) ||",
                            "!IsSupportedSurfaceFormat(source->Format) || !IsSupportedSurfaceFormat(destination->Format) ||"),
     "Present must validate the format pair"),
]

for name, target, mutate, expected in mutations:
    mutated = dict(sources)
    mutated[target] = mutate(sources[target])
    if mutated[target] == sources[target]:
        print(f"FAIL {name}: mutation changed nothing")
        failures += 1
        continue
    violations = contract.advanced_color_violations(mutated)
    if not any(expected in violation for violation in violations):
        print(f"FAIL {name}: expected {expected!r}, got {violations}")
        failures += 1
    else:
        print(f"PASS negative control: {name}")

# The preprocessor view itself: #else belongs to the default build, unknown
# conditionals keep both branches.
view = contract.interface_view(
    "a\n#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_3)\nhdr\n#else\nsdr\n#endif\n"
    "#if defined(OTHER)\nboth\n#endif\n", advanced=False)
if re.findall(r"\w+", view) != ["a", "sdr", "both"]:
    print(f"FAIL interface view: {view!r}")
    failures += 1
else:
    print("PASS interface view selects the default branch and keeps unrelated conditionals")

print(f"Advanced Color contract: {len(mutations)} negative controls, {failures} failures")
sys.exit(1 if failures else 0)
