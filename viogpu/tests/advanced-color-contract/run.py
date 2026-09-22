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
    "mmio_flip.h": root / "viogpu/common/mmio_flip.h",
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


def ungate(text, marker, scope=None):
    """Delete the WDDM2.3 #if/#endif pair that encloses the first marker."""
    matches = anchor_matches(text, marker)
    if scope is not None:
        start = text.index(scope)
        matches = [m for m in matches if m.start() >= start]
    if len(matches) != 1:
        raise SystemExit(f"gate marker not unique: {marker!r}")
    at = matches[0].start()
    start = text.rindex(GATE, 0, at)
    end = text.index("#endif\n", at)
    return text[:start] + text[start + len(GATE):end] + text[end + len("#endif\n"):]


def anchor_matches(text, anchor):
    """Match the same non-whitespace source characters across formatter wrapping."""
    pattern = r"\s*".join(re.escape(ch) for ch in anchor if not ch.isspace())
    if not pattern:
        raise ValueError("empty mutation anchor")
    return list(re.finditer(pattern, text))


def replace_once(text, old, new):
    """Replace one unique production anchor; a changed name/operator still fails."""
    matches = anchor_matches(text, old)
    if len(matches) != 1:
        raise SystemExit(f"mutation anchor not unique ({len(matches)}): {old[:70]!r}")
    match = matches[0]
    return text[:match.start()] + new + text[match.end():]


mutations = [
    ("default private ten-bit allocation", "wddmddi.cpp",
     lambda t: ungate(t, "            return VIOGPU_WDDM_FORMAT_R10G10B10A2_UNORM;"),
     "default WDDM2.0 wddmddi.cpp references a ten-bit format"),
    ("default framebuffer PQ tag", "viogpudo.cpp",
     lambda t: ungate(t, "A ten-bit fallback primary is real PQ storage too"),
     "default WDDM2.0 viogpudo.cpp issues a DVCL display color command"),
    ("candidate forces interruptible HPD", "viogpudo.cpp",
     lambda t: replace_once(t, "            static_cast<DXGK_CHILD_DEVICE_HPD_AWARENESS>(descriptor.HpdAwareness);",
                            "            HpdAwarenessInterruptible;"),
     "the Advanced Color child must keep the selected descriptor"),
    ("timing reports an internal panel", "advanced_color_ddi.inc",
     lambda t: replace_once(t, "static_cast<D3DKMDT_VIDEO_OUTPUT_TECHNOLOGY>(adapter->ChildDescriptor().InterfaceTechnology);",
                            "D3DKMDT_VOT_INTERNAL;"),
     "timing target state must report the selected connector technology"),
    ("always-connected descriptor pulsed by the driver", "viogpudo.cpp",
     lambda t: replace_once(t, "m_pVioGpuDod->ChildDescriptor().HpdAwareness == VioGpuHpdInterruptible &&", "true &&"),
     "only an interruptible child with HPD enabled may be pulsed"),
    ("HPD disable ignored by pulses", "viogpudo.cpp",
     lambda t: replace_once(t, " &&\n                               InterlockedCompareExchange(&m_pVioGpuDod->m_ColorHpdEnabled, 0, 0) != 0;", ";"),
     "only an interruptible child with HPD enabled may be pulsed"),
    ("link caps claim HighColorSpace", "advanced_color_ddi.inc",
     lambda t: replace_once(t, "VioGpuMonitorLinkCapabilities(nullptr, false);", "VIOGPU_LINK_CAP_WIDE_COLOR_SPACE | VIOGPU_LINK_CAP_HIGH_COLOR_SPACE;"),
     "without canonical FP16 scanout no monitor link color capability may be claimed"),
    ("link policy ignores FP16 scanout", "viogpu_display_color.h",
     lambda t: replace_once(t, "if (!VioGpuScRgbScanoutAdmitted(caps, canonicalFp16Scanout))",
                            "if (!VioGpuDisplayColorPqAdmitted(caps))"),
     "Wide/HighColorSpace require canonical FP16 scanout and admitted PQ"),
    ("link claim forces HighColorSpace", "viogpu_display_color.h",
     lambda t: replace_once(t, "(claimHighColor ? VIOGPU_LINK_CAP_HIGH_COLOR_SPACE : 0U)",
                            "VIOGPU_LINK_CAP_HIGH_COLOR_SPACE"),
     "the high-colour claim must be the only part the trial bit drops"),
    ("link claim drops the trial bit", "advanced_color_ddi.inc",
     lambda t: replace_once(t, ", adapter->HdrTrialHighColor());", ");"),
     "the monitor link claim must pass the high-colour trial bit"),
    ("connection change invents a queued change", "advanced_color_ddi.inc",
     lambda t: replace_once(t, "    return STATUS_ALREADY_COMPLETE;", "    return STATUS_SUCCESS;"),
     "no numbered connection change is ever queued"),
    ("default build registers link info", "driver_entry.cpp",
     lambda t: t.replace("#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_3)\n        initialData->DxgkDdiSetTargetAdjustedColorimetry",
                         "        initialData->DxgkDdiUpdateMonitorLinkInfo = VioGpuWddmUpdateMonitorLinkInfo;\n#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_3)\n        initialData->DxgkDdiSetTargetAdjustedColorimetry"),
     "default WDDM2.0 driver_entry.cpp exposes VioGpuWddmUpdateMonitorLinkInfo"),
    ("default build refreshes color connection", "viogpudo.cpp",
     lambda t: ungate(t, "m_pHWDevice->RequestColorConnectionRefresh();", "NTSTATUS VioGpuDod::QueryChildRelations("),
     "default WDDM2.0 viogpudo.cpp exposes RequestColorConnectionRefresh"),
    ("display event drops SDR connected report", "viogpudo.cpp",
     lambda t: replace_once(t, "        if (InterlockedCompareExchange(&m_pVioGpuDod->m_ColorMonitorConnected, 0, 0) != 0)\n        {\n            UpdateChildStatus(TRUE);\n        }\n", ""),
     "a display event must keep the SDR connected report before the HDR refresh"),
    ("MMIO flip scans out a ten-bit primary", "wddmddi.cpp",
     lambda t: replace_once(t, "        target.HighPrecision = target.StandardPrimary && IsHighPrecisionSurfaceFormat(allocation->Format);\n", ""),
     "an MMIO flip must refuse ten-bit primaries and end color presentation before publishing"),
    ("MMIO flip keeps a stale color PresentId", "wddmddi.cpp",
     lambda t: replace_once(t, "     * Interlocked writes only, so this stays lock-free at DIRQL. */\n    adapter->ClearColorPresentCompletion();\n",
                            "     * Interlocked writes only, so this stays lock-free at DIRQL. */\n"),
     "an MMIO flip must refuse ten-bit primaries and end color presentation before publishing"),
    ("mode change skips the color slot", "wddmddi.cpp",
     lambda t: replace_once(t, "    VioGpuDod::ColorStateOperation colorOperation(adapter);\n    if (!colorOperation.Acquired())\n    {\n        return STATUS_DEVICE_NOT_READY;\n    }\n    adapter->ClearColorPresentCompletion();\n#endif\n    /* A mode change",
                            "#endif\n    /* A mode change"),
     "a mode change must take the color slot before the flip-apply mutex"),
    ("flip present accepts an unadmitted ten-bit source", "wddmddi.cpp",
     lambda t: replace_once(t, "    return !IsHighPrecisionSurfaceFormat(allocation->Format) || "
                               "adapter->IsNativeHdrModeAvailable();", "    return TRUE;"),
     "the flip source color gate must consult Advanced Color availability"),
    ("shared bind scans out an unadmitted ten-bit primary", "wddmddi.cpp",
     lambda t: replace_once(t, "    if (highPrecision && !adapter->IsNativeHdrModeAvailable())\n    {\n"
                               "        return STATUS_INVALID_PARAMETER;\n    }\n", ""),
     "a ten-bit primary must be refused with a legal status unless Advanced Color is usable"),
    ("shared bind skips the high-precision color tag", "wddmddi.cpp",
     lambda t: replace_once(t, "        const BOOLEAN colorTagged = !highPrecision || "
                               "TagHighPrecisionPrimaryColor(adapter, allocation);",
                            "        const BOOLEAN colorTagged = TRUE;"),
     "a ten-bit primary must be tagged with its Host color before the scanout binds it"),
    ("color tag ignores the negotiated mode", "wddmddi.cpp",
     lambda t: replace_once(t, "    if (!adapter->MatchesNativeHdrMode(allocation->Width, allocation->Height))\n"
                               "    {\n        return FALSE;\n    }\n", ""),
     "the high-precision color tag must stay bound to the negotiated mode: "
     "adapter->MatchesNativeHdrMode(allocation->Width,allocation->Height)"),
    ("flip worker takes the flip mutex before the color slot", "wddmddi.cpp",
     lambda t: replace_once(t, "    VioGpuDod::ColorStateOperation colorOperation(adapter);\n#endif\n"
                               "    adapter->AcquireFlipApply();\n    VIOGPU_WDDM_ALLOCATION *allocation =",
                            "#endif\n    adapter->AcquireFlipApply();\n"
                            "    VioGpuDod::ColorStateOperation colorOperation(adapter);\n"
                            "    VIOGPU_WDDM_ALLOCATION *allocation ="),
     "the flip worker must take the color slot before the flip-apply mutex"),
    ("power-off leaves a pending flip", "viogpudo.cpp",
     lambda t: replace_once(t, "        AcquireFlipApply();\n        (VOID) TakePendingFlip();\n        const auto result = Set2DScanout(0, 0, 0, 0, &previousResource);\n        ReleaseFlipApply();",
                            "        const auto result = Set2DScanout(0, 0, 0, 0, &previousResource);"),
     "target power-off must supersede an unbound flip"),
    ("flip policy ignores ten-bit primaries", "mmio_flip.h",
     lambda t: replace_once(t, "    if (target.HighPrecision && !target.HighPrecisionAdmitted)\n"
                               "    {\n        return VioGpuFlipTargetHighPrecision;\n    }\n", ""),
     "the flip policy must refuse unadmitted ten-bit primaries after ownership and type"),
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
    ("trial mask can turn the source modes on", "viogpudo.cpp",
     lambda t: replace_once(t, "if (VioGpuScRgbScanoutAdmitted(&colorCaps, true) && HdrTrialSourceModes())",
                            "if (VioGpuScRgbScanoutAdmitted(&colorCaps, true))"),
     "the canonical FP16 source mode must require an admitted scRGB scanout"),
    ("trial mask ignored by the link claim", "advanced_color_ddi.inc",
     lambda t: replace_once(t, " &&\n                          adapter->HdrTrialLinkCaps();", ";"),
     "the monitor link claim must consult the HDR trial mask"),
    ("trial mask ignored by the monitor descriptor", "viogpudo.cpp",
     lambda t: replace_once(t, " ||\n        !m_pVioGpuDod->HdrTrialMonitorEdid()", ""),
     "the HDR monitor descriptor must consult the HDR trial mask"),
    ("monitor mode ignores configured dynamic range", "viogpudo.cpp",
     lambda t: replace_once(t, "    const UINT monitorColorRange = MonitorColorRange();",
                            "    const UINT monitorColorRange = 10;"),
     "the monitor source mode dynamic range must come from the registry selector"),
    ("monitor mode describes only the preferred mode", "viogpudo.cpp",
     lambda t: replace_once(
         t,
         "        pMonitorSourceMode->ColorCoeffDynamicRanges.FourthChannel = monitorColorRange;\n"
         "        if (Idx == m_pHWDevice->GetCurrentModeIndex())",
         "        pMonitorSourceMode->ColorCoeffDynamicRanges.FourthChannel = 8;\n"
         "        if (Idx == m_pHWDevice->GetCurrentModeIndex())"),
     "every monitor source mode must carry the same dynamic range"),
    ("HDR monitor descriptor without admission", "viogpudo.cpp",
     lambda t: replace_once(t, "!VioGpuScRgbScanoutAdmitted(&caps, true) ||\n"
                               "        !m_pVioGpuDod->HdrTrialMonitorEdid()",
                            "!m_pVioGpuDod->HdrTrialMonitorEdid()"),
     "the HDR monitor descriptor must require an admitted scRGB scanout"),
    ("FP16 source mode without admission", "viogpudo.cpp",
     lambda t: replace_once(t, "        if (VioGpuScRgbScanoutAdmitted(&colorCaps, true) && HdrTrialSourceModes())\n        {\n"
                               "            formatCount = 3;\n        }",
                            "        formatCount = 3;"),
     "the canonical FP16 source mode must require an admitted scRGB scanout"),
    ("colour tag hard-codes ten-bit storage", "wddmddi.cpp",
     lambda t: replace_once(t, "    color.format = wireFormat;\n    color.encoding = wireEncoding;",
                            "    color.format = VIOGPU_DISPLAY_FORMAT_AB30;\n"
                            "    color.encoding = VIOGPU_DISPLAY_COLOR_PQ;"),
     "the high-precision color tag must stay bound to the negotiated mode"),
    ("high-precision source validation without HDR", "viogpudo.cpp",
     lambda t: replace_once(t, "VioGpuIsHighPrecisionSourceFormat(pSourceMode->Format.Graphics.PixelFormat) &&\n"
                               "            IsNativeHdrModeAvailable())",
                            "VioGpuIsHighPrecisionSourceFormat(pSourceMode->Format.Graphics.PixelFormat))"),
     "high-precision source validation must require native HDR"),
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
     "PreserveInherited must be applied as a normal timing set, never consulted or refused"),
    ("failed discovery drops SDR monitor", "viogpu_display_color.h",
     lambda t: replace_once(t, "return wasAdmitted ? VioGpuColorConnectionReenumerate : VioGpuColorConnectionNone;",
                            "return VioGpuColorConnectionReenumerate;"),
     "lost discovery may only withdraw an admitted HDR monitor"),
    ("always-connected child pulsed", "viogpu_display_color.h",
     lambda t: replace_once(t, "    if (!interruptible)\n    {\n        return VioGpuColorConnectionNone;\n    }\n", ""),
     "an always-connected child must never be reported disconnected"),
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
