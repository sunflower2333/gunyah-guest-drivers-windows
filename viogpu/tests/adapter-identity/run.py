#!/usr/bin/env python3
"""Exercise actual adapter reply, identity publication and readiness code."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument('--prepatch-revision', help='Use an old private reply as the negative control')
args = parser.parse_args()
here = Path(__file__).resolve().parent
root = here.parents[2]
dod = (root / 'viogpu/viogpudo/viogpudo.cpp').read_text()
path = 'viogpu/viogpuwddm/wddmddi.cpp'
wddm = (root / path).read_text()
reply = (subprocess.check_output(['git', 'show', f'{args.prepatch_revision}:{path}'],
         cwd=root, text=True) if args.prepatch_revision else wddm)

def function(text, signature):
    start = text.rindex(signature)
    brace = text.index('{', start)
    if ';' in text[start:brace]:
        raise ValueError(f'Not a definition: {signature}')
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

# Check real PnP lifecycle call ordering around the extracted publication
# routine. Runtime below also injects stop between hardware readiness and
# the atomic identity read, and distinguishes reset from a new adapter start.
start = function(dod, 'NTSTATUS VioGpuDod::StartDevice(')
stop = function(dod, 'NTSTATUS VioGpuDod::StopDevice(')
unwind = function(dod, 'NTSTATUS VioGpuDod::UnwindFailedStart(')
assert start.index('SetNativeAdapterLuid(NULL)') < start.index('ResetNativeFenceTracker()')
assert start.index('OpenWddmPresentTransactions()') < start.index('SetNativeAdapterLuid(&pDxgkStartInfo->AdapterLuid)')
assert start.index('SetNativeAdapterLuid(&pDxgkStartInfo->AdapterLuid)') < start.index('m_Flags.DriverStarted = TRUE')
assert stop.index('SetNativeAdapterLuid(NULL)') < stop.index('ExWaitForRundownProtectionRelease')
assert unwind.index('SetNativeAdapterLuid(NULL)') < unwind.index('ExWaitForRundownProtectionRelease')
assert 'SetNativeAdapterLuid' not in function(dod, 'NTSTATUS VioGpuDod::ResetFromTimeout(')
production = '\n'.join([
    function(dod, 'VOID VioGpuDod::SetNativeAdapterLuid('),
    function(dod, 'BOOLEAN VioGpuDod::QueryNativeContextReadiness('),
    function(wddm, 'VOID InitializeAbiHeader('),
    function(reply, 'NTSTATUS QueryUmdPrivateInfo('),
])
fixture = (here / 'adapter_identity_test.cpp').read_text().replace('// INSERT_PRODUCTION', production)
with tempfile.TemporaryDirectory(prefix='viogpu-adapter-identity-') as temporary:
    output = Path(temporary)
    unit = output / 'test.cpp'
    unit.write_text(fixture)
    includes = [root / 'viogpu/shared', root / 'viogpu/common']
    if shutil.which('cl'):
        exe = output / 'test.exe'
        command = ['cl', '/nologo', '/EHsc', '/W4', '/WX', '/std:c++17',
                   *[f'/I{p}' for p in includes], str(unit), f'/Fe{exe}']
    else:
        exe = output / 'test'
        command = ['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                   '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                   *[f'-I{p}' for p in includes], str(unit), '-o', str(exe)]
    subprocess.run(command, cwd=output, check=True)
    result = subprocess.run([str(exe)], cwd=output).returncode
    for attempt in range(21):
        try:
            exe.unlink()
            break
        except PermissionError:
            if attempt == 20:
                raise
            time.sleep(0.25)
    raise SystemExit(result)
