"""Exercise actual PE acceptance and reject frozen private-linked regressions."""
import argparse
import json
from pathlib import Path
import runpy
from package import sha, imports

validate = runpy.run_path(str(Path(__file__).with_name('package-flat.py')))['validate_system_probe']

def rejected(path, arch, entry, expected):
    try:
        validate(path, arch, entry)
    except ValueError as error:
        if expected not in str(error):
            raise AssertionError(f'Wrong rejection: {error}') from error
        print(f'PASS reject {arch}: {expected}')
    else:
        raise AssertionError(f'Accepted invalid ordinary probe: {path}')

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--probes', type=Path, required=True)
    args = parser.parse_args()
    receipt = json.loads((args.probes/'opencl-system-probes-receipt.json').read_text())
    for arch in ('arm64','x64','x86'):
        name = f'viogpu-opencl-check-{arch}.exe'
        new = args.probes/name
        entry = receipt['files'][name]
        validate(new, arch, entry)
        print(f'PASS accept real {arch} public-loader PE')
        old = list((args.runtime/f'viogpu-opencl-{arch}').rglob(name))
        if len(old) != 1:
            raise ValueError(f'Expected one frozen private-linked probe: {arch}')
        # Give the old binary its *correct* hash and import receipt, so this
        # negative can pass only if the actual public-loader contract is lost.
        old_entry = {**entry,'sha256':sha(old[0]),'imports':imports(old[0])}
        rejected(old[0],arch,old_entry,'must import system OpenCL.dll')
        rejected(new,arch,{**entry,'sha256':'0'*64},'hash mismatch')
        rejected(new,arch,{**entry,'machine':'wrong'},'metadata')
        rejected(new,arch,{**entry,'imports':[]},'import receipt mismatch')
    print('PASS 15 real-PE ordinary OpenCL probe acceptance/rejection checks')

if __name__ == '__main__':
    main()
