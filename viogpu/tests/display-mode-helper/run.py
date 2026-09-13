#!/usr/bin/env python3
"""Execute the actual embedded mode helper with controlled display API peers."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--negative-control-success-gate', action='store_true')
parser.add_argument('--negative-control-mask-failure', action='store_true')
args = parser.parse_args()
if args.negative_control_success_gate and args.negative_control_mask_failure:
    parser.error('Select one semantic negative control')
here = Path(__file__).resolve().parent
root = here.parents[2]
script = (root / '.install_scripts/viogpu-test-display-mode-v2.ps1').read_text()
start = "Add-Type -TypeDefinition @'\n"
end = "\n'@\n"
assert script.count(start) == 1 and script.count(end) == 1
production = script.split(start, 1)[1].split(end, 1)[0]
if args.negative_control_success_gate:
    original = 'if (applyAttempted) {'
    assert production.count(original) == 1
    production = production.replace(original,
        'if (applyAttempted && outcome.Apply != null && outcome.Apply.Result == 0) {')
elif args.negative_control_mask_failure:
    original = 'outcome.RestoreFailure = outcome.Restore.Failure;'
    assert production.count(original) == 1
    production = production.replace(original, original + '\n                    outcome.Failure = outcome.RestoreFailure;')
fixture = (here / 'display_mode_helper_test.cs').read_text()
# C# using clauses precede all declarations, including the embedded production.
fixture = fixture.replace('using System.IO;\n', '').replace('using ModeHelper = VioGpuDisplayModeTestV2;\n', '')
source = 'using System.IO;\nusing ModeHelper = VioGpuDisplayModeTestV2;\n' + production + '\n' + fixture
with tempfile.TemporaryDirectory(prefix='viogpu-display-mode-helper-') as temporary:
    output = Path(temporary)
    unit = output / 'test.cs'
    unit.write_text(source)
    binary = output / 'test.exe'
    if os.name == 'nt':
        compiler = Path(os.environ['WINDIR']) / 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
        subprocess.run([str(compiler), '/nologo', '/warn:4', '/warnaserror+', '/platform:x64',
                        '/out:' + str(binary), str(unit)], check=True, cwd=output)
        command = [str(binary)]
    else:
        if not shutil.which('mcs') or not shutil.which('mono'):
            raise SystemExit('Mono mcs and mono are required for local embedded C# testing')
        subprocess.run(['mcs', '-warn:4', '-warnaserror+', '-out:' + str(binary), str(unit)],
                       check=True, cwd=output)
        command = ['mono', str(binary)]
    result = subprocess.run(command, capture_output=True, text=True, cwd=output)
    print(result.stdout, end='')
    print(result.stderr, end='')
    if args.negative_control_success_gate:
        expected = 'FAIL failed apply with changed mode restores exactly once'
    elif args.negative_control_mask_failure:
        expected = 'FAIL restore failure preserves original apply failure and both outcomes'
    else:
        raise SystemExit(result.returncode)
    if result.returncode != 1 or expected not in result.stdout:
        raise SystemExit('Semantic negative did not detect the intended restoration regression')
    print('PASS semantic negative: ' + expected[5:])
