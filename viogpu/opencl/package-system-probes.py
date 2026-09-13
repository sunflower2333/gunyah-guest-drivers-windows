"""Supplemental probe receipt; never replace an installed runtime manifest."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess

CLVK = 'fd4454676a30ca5e04ed567cb3d51b5f008113db'
LOADER = 'f27c925e782499eebc4df20e121144358ccd5ac6'
HEADERS = '386ca390b2f52efeb3e1a55a500690eb8013f60e'
PROBE_SHA = '224033cb16045a784dd1f5c29a9204536598159ae219535dc6c234ba1aefe9a7'

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def revision(path):
    return subprocess.check_output(['git','-C',str(path),'rev-parse','HEAD'], text=True).strip()

def main():
    for path, expected in [('clvk',CLVK),('opencl-loader',LOADER),('opencl-headers',HEADERS)]:
        if revision(path) != expected:
            raise ValueError(f'Unexpected source revision: {path}')
    source = Path('clvk/tools/viogpu-opencl-check.cpp')
    if sha(source) != PROBE_SHA:
        raise ValueError('GPU probe source changed; frozen test source required')
    spec = importlib.util.spec_from_file_location('flat_pe', 'clvk/tools/flat-pe.py')
    pe = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(pe)
    root = Path('opencl-system-probes')
    files = {}
    for arch in ('arm64','x64','x86'):
        path = root/f'viogpu-opencl-check-{arch}.exe'
        machine, imports = pe.pe(path)  # Includes native and delay imports.
        if machine != pe.MACHINES[arch]:
            raise ValueError(f'Wrong PE architecture: {path}')
        if imports.count('opencl.dll') != 1:
            raise ValueError(f'Ordinary probe must import public OpenCL.dll: {path}: {imports}')
        # /MT keeps ordinary applications independent of VC redistributables.
        # This bounded source currently needs only Kernel32 plus OpenCL.
        if set(imports) != {'opencl.dll','kernel32.dll'}:
            raise ValueError(f'Unexpected private/runtime dependency: {path}: {imports}')
        files[path.name] = {'sha256':sha(path), 'machine':arch,
                            'role':'ordinary-application-probe', 'imports':imports}
        print(f'PASS {arch} native/delay imports: {imports}; sha256={sha(path)}')
    if {p.name for p in root.iterdir()} != set(files):
        raise ValueError('Probe archive must contain only the three applications before receipt')
    receipt = {
        'schema':1, 'family':'opencl-system-probes',
        'scope':'probe-only supplement; installed runtime manifest and hashes unchanged',
        'companion_runtime':{'clvk':CLVK, 'clvk_runtime_ci':34736596797,
                             'target_installation':'not verified by probe packaging'},
        'probe_build':{'parent':revision('.'), 'ci':os.environ.get('GITHUB_RUN_ID'),
                       'clvk_test_source':CLVK, 'test_source_path':str(source),
                       'test_source_sha256':PROBE_SHA, 'khronos_loader':LOADER,
                       'headers':HEADERS, 'crt':'static /MT', 'manifest':'asInvoker',
                       'link_import_library':'official Khronos OpenCL.lib'},
        'validation':{'all_architecture_public_imports':'passed',
                      'native_and_emulated_launch':'separate CI launch evidence',
                      'target_gpu_execution':'not performed by this build'},
        'files':files}
    (root/'opencl-system-probes-receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')

if __name__ == '__main__':
    main()
