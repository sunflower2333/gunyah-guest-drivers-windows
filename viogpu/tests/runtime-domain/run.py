#!/usr/bin/env python3
"""Execute the actual retained-domain implementation with kernel primitive shims."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument('--negative-control', choices=['unretained', 'wrong-generation', 'early-close'])
args = parser.parse_args()
here = Path(__file__).resolve().parent
root = here.parents[2]
production = (root / 'viogpu/viogpuwddm/wddm_runtime_domain.inc').read_text()
changes = {
    'unretained': ('++owner->DomainChildren;', '/* deliberately fail to retain */'),
    'wrong-generation': ('owner->DomainResetGeneration != generation ||', ''),
    'early-close': ('context->DomainChildren == 0 ? STATUS_SUCCESS : STATUS_DEVICE_BUSY', 'STATUS_SUCCESS'),
}
if args.negative_control:
    old, new = changes[args.negative_control]
    assert production.count(old) == 1
    production = production.replace(old, new)
fixture = (here / 'runtime_domain_test.cpp').read_text().replace('// INSERT_PRODUCTION', production)
with tempfile.TemporaryDirectory(prefix='viogpu-runtime-domain-') as output:
    directory = Path(output)
    unit = directory / 'test.cpp'
    unit.write_text(fixture)
    if shutil.which('cl'):
        exe = directory / 'test.exe'
        command = ['cl', '/nologo', '/EHsc', '/W4', '/WX', '/std:c++17', f'/I{root}', str(unit), f'/Fe{exe}']
    else:
        exe = directory / 'test'
        command = ['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                   '-fsanitize=address,undefined', '-fno-omit-frame-pointer', f'-I{root}', str(unit), '-o', str(exe)]
    subprocess.run(command, cwd=directory, check=True)
    result = subprocess.run([str(exe)], cwd=directory).returncode
    # Windows image scanners can keep a just-exited PE mapped briefly. Remove
    # this exact disposable executable with a bound before directory cleanup.
    for attempt in range(100):
        try:
            exe.unlink()
            break
        except PermissionError:
            if attempt == 99:
                raise
            time.sleep(0.1)
    if args.negative_control:
        if result == 0:
            raise SystemExit('negative control incorrectly passed')
        print('PASS rejected production mutation:', args.negative_control)
    else:
        raise SystemExit(result)
