#!/usr/bin/env python3
"""Execute the actual physical VidPN diagnostic helpers and semantic negatives."""
from pathlib import Path
import re
import subprocess
import tempfile
try:
    import resource
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
except ImportError:
    pass

here = Path(__file__).resolve().parent
root = here.parents[2]
source = (root / 'viogpu/viogpudo/viogpudo.cpp').read_text()
bodies = []
for name in ('PrepareNativeDiagnosticMode', 'SetNativeDiagnosticModeAndPath',
             'AddNativeDiagnosticSourceMode', 'AddNativeDiagnosticTargetMode', 'AddNativeDiagnosticMonitorMode'):
    match = re.search(r'^[^;\n]*\bVioGpuDod::' + name + r'\([^;]*?\)\s*\{', source, re.M)
    assert match, name
    start = source.index('{', match.start())
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    bodies.append(source[match.start():end])
production = '\n'.join(bodies)
fixture = (here / 'diagnostic_mode_test.cpp').read_text()
variants = [('production', production)]
for name, old, new in (
    ('primary-validation', '!VioGpuWddmValidateDiagnosticPrimary(this, primary, &mode)',
     '(!VioGpuWddmValidateDiagnosticPrimary(this, primary, &mode) && false)'),
    ('full-timing', 'signal->VSyncFreq.Numerator != expected.VSyncFreq.Numerator ||', ''),
    ('precommit-update', '!m_pHWDevice->NativeScanoutModeEligible(&mode, TRUE) ||', ''),
    ('publish-before-ack', 'const auto committed = m_pHWDevice->CommitNativeScanoutMode(&mode);',
     'm_CurrentMode = candidate; const auto committed = m_pHWDevice->CommitNativeScanoutMode(&mode);'),
    ('post-ack-timing', '(VOID)TakePendingFlip();', 'SetCrtcTiming(timing); (VOID)TakePendingFlip();'),
):
    assert old in production
    variants.append((name, production.replace(old, new)))
with tempfile.TemporaryDirectory(prefix='.diagnostic-', dir=here) as temporary:
    output = Path(temporary)
    for name, body in variants:
        unit, binary = output / (name + '.cpp'), output / name
        unit.write_text(fixture.replace('// INSERT_PRODUCTION', body))
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer', '-I' + str(root / 'viogpu/common'), str(unit), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if (result.returncode == 0) != (name == 'production'):
            raise SystemExit(name + ': unexpected result\n' + result.stdout + result.stderr)
        print(result.stdout.strip() if name == 'production' else 'PASS negative control ' + name + ' rejected')
    start = source.index('NTSTATUS VioGpuAdapter::NegotiateNativeContextFeatures(')
    end = source.index('NTSTATUS VioGpuAdapter::FailNativeContextInitialization(', start)
    negotiation = source[start:end]
    fixture = (here / 'diagnostic_negotiation_test.cpp').read_text()
    for name, body in (
        ('negotiation', negotiation),
        ('negotiation-ungated', negotiation.replace('m_pVioGpuDod->NativeScanoutDiagnosticEnabled() &&', '')),
        ('negotiation-prerequisites', negotiation.replace('SupportsNativeAhbPaging() &&', '')),
    ):
        unit, binary = output / (name + '.cpp'), output / name
        unit.write_text(fixture.replace('// INSERT_PRODUCTION', body))
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(unit), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if (result.returncode == 0) != (name == 'negotiation'):
            raise SystemExit(name + ': unexpected result\n' + result.stdout + result.stderr)
        print(result.stdout.strip() if name == 'negotiation' else 'PASS negative control ' + name + ' rejected')
