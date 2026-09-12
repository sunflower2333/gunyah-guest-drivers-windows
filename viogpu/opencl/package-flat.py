"""Assemble one real flat OpenCL driver payload; never rewrite PE bytes."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
from package import sha, machine, verify_sums, imports

SOURCE = 'c5aa9e64fd1fb20e05865e4d25c14d11e6ec57c7'
RUNTIME_CI = 34725343962
# Ordinary public-loader probes retain their independently validated source.
PROBE_SOURCE = '0f436fe7110813ddf01dfaebc44b9de7cd39b3b8'
COMPILER = '79e236af8febd67fd02adfd93f81295c87e868e9fd861f71d03d1057e6be1f9d'
SYSTEM = {'kernel32.dll','user32.dll','gdi32.dll','advapi32.dll','ole32.dll',
          'oleaut32.dll','shell32.dll','shlwapi.dll','cfgmgr32.dll','ntdll.dll',
          'bcrypt.dll','version.dll','ws2_32.dll','secur32.dll','rpcrt4.dll',
          'msvcrt.dll','ucrtbase.dll','psapi.dll','setupapi.dll','runtimeobject.dll'}

def validate_system_probe(path, arch, entry):
    """Reject old private-linked probes even when their supplied hashes match."""
    machine(path, arch)
    if entry.get('machine') != arch or entry.get('role') != 'ordinary-application-probe':
        raise ValueError(f'Wrong ordinary probe metadata: {path}')
    if sha(path) != entry.get('sha256'):
        raise ValueError(f'Ordinary probe hash mismatch: {path}')
    actual = sorted(imports(path))
    if actual != ['kernel32.dll','opencl.dll']:
        raise ValueError(f'Ordinary probe must import system OpenCL.dll: {path}: {actual}')
    if actual != sorted(entry.get('imports', [])):
        raise ValueError(f'Ordinary probe import receipt mismatch: {path}')

def main():
    parser = argparse.ArgumentParser()
    for option in ('runtime','loaders','proxy','probes','output'):
        parser.add_argument('--'+option, type=Path, required=True)
    args = parser.parse_args()
    probe_receipt = json.loads((args.probes/'opencl-system-probes-receipt.json').read_text())
    if (probe_receipt.get('schema') != 1 or probe_receipt.get('family') != 'opencl-system-probes'
            or probe_receipt.get('probe_build',{}).get('clvk_test_source') != PROBE_SOURCE):
        raise ValueError('Missing/wrong ordinary OpenCL probe source receipt')
    args.output.mkdir(exist_ok=False)
    files = {}
    ordinary_probes = set()
    def stage(source, name, arch, role):
        if name in files or Path(name).name != name: raise ValueError(f'Duplicate/unsafe flat name {name}')
        machine(source, arch)
        shutil.copy2(source, args.output/name)
        files[name] = {'sha256':sha(source),'machine':arch,'role':role}
    for arch in ('arm64','x64','x86'):
        found = list((args.runtime/f'viogpu-opencl-{arch}').rglob('SHA256SUMS'))
        if len(found) != 1: raise ValueError(f'Expected one exact {arch} runtime')
        source = found[0].parent
        sums = verify_sums(source)
        if (source/'source.txt').read_text(encoding='utf-8-sig').splitlines()[0].strip() != SOURCE:
            raise ValueError(f'Stale runtime source {arch}')
        names = [f'viogpucl_{arch}.dll',f'viogpucl_vk_{arch}.dll',
                 f'windows-flat-check-{arch}.exe',f'windows-exec-check-{arch}.exe']
        for name in names:
            if name not in sums: raise ValueError(f'Unhashed input {name}')
            stage(source/name,name,arch,'runtime' if name.endswith('.dll') else 'probe')
        name = f'viogpu-opencl-check-{arch}.exe'
        validate_system_probe(args.probes/name, arch, probe_receipt['files'].get(name,{}))
        stage(args.probes/name,name,arch,'probe')
        ordinary_probes.add(name)
        stage(args.proxy/f'opencl-proxy-check-{arch}.exe',f'opencl-proxy-check-{arch}.exe',arch,'probe')
        stage(args.proxy/f'opencl-fixture-{arch}.dll',f'opencl-fixture-{arch}.dll',arch,'probe')
        stage(args.loaders/arch/'loader-check.exe',f'opencl-loader-check-{arch}.exe',arch,'installer-helper')
    stage(args.proxy/'viogpucl.dll','viogpucl.dll','arm64x','icd')
    stage(args.loaders/'arm64x/OpenCL.dll','OpenCL.dll','arm64x','system-loader')
    stage(args.loaders/'x86/OpenCL.dll','OpenCL32.dll','x86','system-loader')
    compiler = args.runtime/'viogpu-opencl-compiler-x64'
    sums = verify_sums(compiler)
    if sha(compiler/'viogpu_clspv_x64.exe') != COMPILER: raise ValueError('Wrong compiler identity')
    for source in compiler.iterdir():
        if source.suffix.lower() in ('.dll','.exe'):
            if source.name not in sums: raise ValueError(f'Unhashed compiler dependency {source.name}')
            stage(source,source.name,'x64','compiler')
    for name, info in files.items():
        for dependency in imports(args.output/name):
            # Ordinary apps use the OS's architecture-specific public loader.
            # They do not import the flat package's private ICD or loader path.
            if name in ordinary_probes and dependency == 'opencl.dll': continue
            if dependency in SYSTEM or dependency.startswith(('api-ms-win-','ext-ms-win-')): continue
            matches = [entry for entry in files if entry.lower() == dependency]
            if len(matches) != 1: raise ValueError(f'Unresolved flat import {name} -> {dependency}')
            target = files[matches[0]]
            if target['machine'] not in (info['machine'],'arm64x'):
                raise ValueError(f'Wrong architecture dependency {name} -> {dependency}')
            if info['role'] in ('runtime','icd') and target['role'] == 'compiler':
                raise ValueError(f'Runtime must use static CRT: {name} -> {dependency}')
    parent = subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip()
    manifest = {'schema':1,'family':'opencl','sources':{
        'clvk':SOURCE,'clvk_stable_base':'ee0ea93dfdad4e243016702c5757def99975fec5',
        'clvk_runtime_ci':RUNTIME_CI,'parent':parent,
        'ordinary_probes':probe_receipt['probe_build'],
        'compiler':'c20f7c8ccf58f317972ac1ffeab68a96019cbbaa',
        'compiler_original_sha256':COMPILER,'compiler_upstream_ci':34487278874,
        'khronos_loader':'f27c925e782499eebc4df20e121144358ccd5ac6',
        'headers':'386ca390b2f52efeb3e1a55a500690eb8013f60e'},
        'loader_probes':{arch:f'opencl-loader-check-{arch}.exe' for arch in ('arm64','x64','x86')},'files':files}
    (args.output/'flat-runtime.json').write_text(json.dumps(manifest,indent=2)+'\n')
    if any(path.is_dir() for path in args.output.iterdir()): raise ValueError('Flat payload contains a directory')
    print(f'PASS flat source/hash/architecture/import closure: {len(files)} files; manifest self-excluded')

if __name__ == '__main__': main()
