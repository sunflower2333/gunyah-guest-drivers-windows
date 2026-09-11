#!/usr/bin/env python3
"""Run production Render translation/final-binding checks across VidMm eviction."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument('--prepatch-revision', help='Older Render translation as a negative control')
args = parser.parse_args()
here = Path(__file__).resolve().parent
root = here.parents[2]
path = 'viogpu/viogpuwddm/wddmddi.cpp'
current = (root / path).read_text()
previous = (subprocess.check_output(['git', 'show', f'{args.prepatch_revision}:{path}'],
            cwd=root, text=True) if args.prepatch_revision else current)

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

production = '\n'.join([
    function(previous, 'NTSTATUS ApplyRenderPrepatches('),
    function(current, 'NTSTATUS ValidateNativeRenderBindings('),
])
fixture = (here / 'render_prepatch_test.cpp').read_text().replace('// INSERT_PRODUCTION', production)
with tempfile.TemporaryDirectory(prefix='viogpu-render-prepatch-') as output:
    directory = Path(output)
    unit = directory / 'test.cpp'
    unit.write_text(fixture)
    if shutil.which('cl'):
        exe = directory / 'test.exe'
        command = ['cl', '/nologo', '/EHsc', '/W4', '/WX', '/std:c++17', str(unit), f'/Fe{exe}']
    else:
        exe = directory / 'test'
        command = ['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                   '-fsanitize=address,undefined', '-fno-omit-frame-pointer', str(unit), '-o', str(exe)]
    subprocess.run(command, cwd=directory, check=True)
    result = subprocess.run([str(exe)], cwd=directory).returncode
    # ARM64 Windows can retain an emulated compiler output briefly after exit.
    for attempt in range(21):
        try:
            exe.unlink()
            break
        except PermissionError:
            if attempt == 20:
                raise
            time.sleep(0.25)
    raise SystemExit(result)
