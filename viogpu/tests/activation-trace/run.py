#!/usr/bin/env python3
"""Execute production recorder with faulting registry and retired hardware peers."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--negative-control', choices=['lost-late-failure', 'reuse-epoch'])
args = parser.parse_args()
here = Path(__file__).resolve().parent
root = here.parents[2]
source = (root / 'viogpu/viogpudo/viogpudo.cpp').read_text()

def function(signature):
    start = source.index(signature)
    brace = source.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

production = '\n'.join(function(signature) for signature in (
    'VOID VioGpuDod::InitializeNativeActivationTrace(',
    'VOID VioGpuDod::PersistNativeActivationTrace(',
    'VOID VioGpuDod::RecordNativeActivationPhase(',
    'VOID VioGpuDod::RecordNativeActivationQuery(',
))
header = (root / 'viogpu/common/activation_trace.h').read_text()
if args.negative_control == 'lost-late-failure':
    header = header.replace('if (trace->Version != 2)\n        return false;',
                            'if (trace->Version != 2 || trace->Count == 64)\n        return false;')
elif args.negative_control == 'reuse-epoch':
    header = header.replace('*epoch = (previous & ~1U) + 2;', '*epoch = 2;')
unit = (here / 'activation_trace_test.cpp').read_text().replace('// INSERT_PRODUCTION', production)
with tempfile.TemporaryDirectory(prefix='viogpu-activation-') as temporary:
    output = Path(temporary)
    (output / 'activation_trace.h').write_text(header)
    file = output / 'test.cpp'
    file.write_text(unit)
    if shutil.which('cl'):
        executable = output / 'test.exe'
        command = ['cl', '/nologo', '/EHsc', '/W4', '/WX', '/std:c++17', str(file), f'/Fe{executable}']
    else:
        executable = output / 'test'
        command = ['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                   '-fsanitize=address,undefined', '-fno-omit-frame-pointer', str(file), '-o', str(executable)]
    subprocess.run(command, cwd=output, check=True)
    result = subprocess.run([str(executable)], cwd=output, capture_output=True, text=True)
    if args.negative_control:
        if result.returncode == 0 or 'FAIL:' not in result.stderr:
            raise SystemExit('negative control did not trigger a semantic test failure')
        print(f'PASS negative control {args.negative_control}: {result.stderr.strip()}')
    else:
        print(result.stdout, end='')
        print(result.stderr, end='')
        result.check_returncode()
