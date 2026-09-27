#!/usr/bin/env python3
"""Execute production validation snapshot/writer, preserving admission semantics."""
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

def body(name, scope='VioGpuDod::'):
    match = re.search(r'^[^;\n]*\b' + scope + name + r'\([^;]*?\)\s*(?:const\s*)?\{', source, re.M)
    assert match, name
    start = source.index('{', match.start())
    end, depth = start + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]

production = body('VioGpuCaptureValidationInputs', '') + '\n' + '\n'.join(body(name) for name in (
    'PrepareNativeDiagnosticMode', 'NativeDiagnosticConstraintsMatch',
    'NativeDiagnosticModeCofunctional', 'RecordNativeValidation'))
fixture = (here / 'diagnostic_mode_test.cpp').read_text().split('// INSERT_PRODUCTION')[0]
kernel, rest = (here / 'validation_capture_test.cpp').read_text().split('// INSERT_MEMBERS')
members, main = rest.split('// INSERT_MAIN')
fixture = fixture.replace('struct VioGpuDod {', kernel + '\nstruct VioGpuDod {\n' + members)

# Every DDI exit publishes its final status. Hooks use acquired inputs only;
# no diagnostic host reservation/query is inserted into either DDI.
for name, returns in (('IsSupportedVidPn', 9), ('EnumVidPnCofuncModality', 4)):
    text = body(name)
    assert len(re.findall(r'return complete\(', text)) == returns, name
    assert len(re.findall(r'return status;', text)) == 1, name
    assert 'QueryReservedNativeScanoutMode' not in text and 'ReserveNativeScanoutMode' not in text
    assert 'VioGpuCaptureValidationInputs(&capture,' in text
    assert 'RecordNativeValidation(&capture, status);' in text
enum = body('EnumVidPnCofuncModality')
assert 'target, pVidPnPresentPath, &capture);' in enum
assert 'capture.SourceModeStatus = static_cast<UINT>(Status);' in enum
assert 'capture.TargetModeStatus = static_cast<UINT>(Status);' in enum
assert 'VioGpuCaptureValidationInputs(&capture, pVidPnPinnedSourceModeInfo, NULL, NULL);' in enum
assert 'VioGpuCaptureValidationInputs(&capture, NULL, &LocalVidPnPresentPath, NULL);' in enum

variants = [('production', production)]
for name, old, new in (
    ('ungated', 'if (!NativeScanoutDiagnosticEnabled()) return;', ''),
    ('status-lost', 'record->Data.Status = static_cast<UINT>(status);',
     'record->Data.Status = 0; (void)status;'),
    ('source-transposed', 'record.SourceWidth = source->Format.Graphics.PrimSurfSize.cx;',
     'record.SourceWidth = source->Format.Graphics.PrimSurfSize.cy;'),
    ('expected-timing-lost', 'capture->ExpectedHSyncDenominator = signal.HSyncFreq.Denominator;',
     'capture->ExpectedHSyncDenominator = 1;'),
    ('refusal-hidden', 'capture->Result = !prepared ? 1 : (matches ? 3 : 2);',
     'capture->Result = 3;'),
    ('boot-fills-capture', 'VioGpuAppendNativeValidation(&m_NativeValidationCapture, record);',
     'if (m_NativeValidationCapture.Count < 32) VioGpuAppendNativeValidation(&m_NativeValidationCapture, record);'),
):
    assert old in production, name
    variants.append((name, production.replace(old, new)))
with tempfile.TemporaryDirectory(prefix='.validation-', dir=here) as temporary:
    output = Path(temporary)
    for name, actual in variants:
        unit, binary = output / (name + '.cpp'), output / name
        unit.write_text(fixture + actual + main)
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-I' + str(root / 'viogpu/common'), str(unit), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if (result.returncode == 0) != (name == 'production'):
            raise SystemExit(name + ': unexpected result\n' + result.stdout + result.stderr)
        print(result.stdout.strip() if name == 'production' else 'PASS negative control ' + name + ' rejected')
