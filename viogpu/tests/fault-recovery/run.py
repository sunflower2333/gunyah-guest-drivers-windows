#!/usr/bin/env python3
"""Run production fault/endpoint/preemption paths against a scheduler observer."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--revision', help='Older production source for the negative control')
args = parser.parse_args()
here = Path(__file__).resolve().parent
root = here.parents[2]

def source(path):
    return (subprocess.check_output(['git', 'show', f'{args.revision}:{path}'], cwd=root, text=True)
            if args.revision else (root / path).read_text())

def function(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

dod = source('viogpu/viogpudo/viogpudo.cpp')
ddi = source('viogpu/viogpuwddm/wddmddi.cpp')
production = '\n'.join([
    function(dod, '__declspec(noinline) void VioGpuDod::NotifyNativeSubmissionFault('),
    function(dod, 'void VioGpuDod::InvalidateNativeFenceTracker('),
    function(dod, 'void VioGpuDod::CompleteNativeFenceReset('),
    function(ddi, '_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmPreemptCommand('),
    function(ddi, '_Use_decl_annotations_ NTSTATUS APIENTRY VioGpuWddmQueryCurrentFence('),
])
fixture = (here / 'fault_recovery_test.cpp').read_text().replace('// INSERT_PRODUCTION', production)
with tempfile.TemporaryDirectory(prefix='viogpu-fault-recovery-') as output:
    directory = Path(output)
    unit = directory / 'test.cpp'
    unit.write_text(fixture)
    if shutil.which('cl'):
        exe = directory / 'test.exe'
        command = ['cl', '/nologo', '/EHsc', '/W4', '/WX', '/std:c++17', str(unit), f'/Fe{exe}']
    else:
        exe = directory / 'test'
        command = ['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(unit), '-o', str(exe)]
    subprocess.run(command, cwd=directory, check=True)
    raise SystemExit(subprocess.run([str(exe)], cwd=directory).returncode)
