"""Assemble a registered ICD distribution, rejecting stale or mixed inputs."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct

SOURCE = '3c713e78de22e8aa18e555c997ee03a5e0ea9637'
COMPILER = '79e236af8febd67fd02adfd93f81295c87e868e9fd861f71d03d1057e6be1f9d'
MACHINES = {'arm64': 0xaa64, 'x64': 0x8664, 'x86': 0x14c, 'arm64x': 0xaa64}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def machine(path, arch):
    data = path.read_bytes()
    if data[:2] != b'MZ':
        raise ValueError(f'Not PE: {path}')
    offset, = struct.unpack_from('<I', data, 0x3c)
    if data[offset:offset+4] != b'PE\0\0':
        raise ValueError(f'Invalid PE: {path}')
    actual, = struct.unpack_from('<H', data, offset + 4)
    if actual != MACHINES[arch]:
        raise ValueError(f'Wrong machine: {path}: {actual:x} expected {arch}')


def verify_sums(root):
    sums = {}
    for line in (root / 'SHA256SUMS').read_text(encoding='utf-8-sig').splitlines():
        digest, name = line.split('  ', 1)
        if name in sums or Path(name).name != name:
            raise ValueError(f'Unsafe or duplicate hash entry: {name}')
        if sha(root / name) != digest.lower():
            raise ValueError(f'Input hash mismatch: {root / name}')
        sums[name] = digest
    return sums


def main():
    parser = argparse.ArgumentParser()
    for key in ('runtime', 'loaders', 'output'):
        parser.add_argument('--' + key, required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(exist_ok=False)
    for arch in ('arm64', 'x64', 'x86'):
        matches = list((args.runtime / f'viogpu-opencl-{arch}').rglob('SHA256SUMS'))
        if len(matches) != 1:
            raise ValueError(f'Expected exactly one {arch} runtime')
        source = matches[0].parent
        sums = verify_sums(source)
        if SOURCE not in (source / 'source.txt').read_text(encoding='utf-8-sig'):
            raise ValueError(f'Stale runtime: {arch}')
        destination = args.output / arch
        destination.mkdir()
        for file in source.glob('*.dll'):
            if file.name not in sums:
                raise ValueError(f'Unhashed dependency: {file}')
            machine(file, arch)
            target = 'viogpucl.dll' if file.name == 'OpenCL.dll' else file.name
            shutil.copy2(file, destination / target)
        if not (destination / 'viogpucl.dll').is_file():
            raise ValueError('Missing vendor ICD')
        probe = source / 'viogpu-opencl-check.exe'
        if probe.name not in sums:
            raise ValueError('Unhashed probe')
        machine(probe, arch)
        probes = args.output / 'probes' / arch
        probes.mkdir(parents=True)
        shutil.copy2(probe, probes / probe.name)
        # CRT only. No OpenCL or Vulkan DLL may accompany ordinary-app probes.
        for file in destination.glob('*.dll'):
            if file.name.lower().startswith(('msvcp', 'vcruntime', 'concrt')):
                shutil.copy2(file, probes / file.name)
    compiler = args.runtime / 'viogpu-opencl-compiler-x64'
    verify_sums(compiler)
    if sha(compiler / 'clspv.exe') != COMPILER:
        raise ValueError('Wrong external compiler')
    (args.output / 'compiler').mkdir()
    for file in compiler.iterdir():
        if file.suffix.lower() in ('.dll', '.exe'):
            machine(file, 'x64')
            shutil.copy2(file, args.output / 'compiler' / file.name)
    for arch in MACHINES:
        dll = args.loaders / arch / 'OpenCL.dll'
        machine(dll, arch)
        target = args.output / 'loaders' / arch
        target.mkdir(parents=True)
        shutil.copy2(dll, target / dll.name)
        check = args.loaders / ('arm64' if arch == 'arm64x' else arch) / 'loader-check.exe'
        shutil.copy2(check, target / check.name)
    (args.output / 'sources.json').write_text(json.dumps({
        'clvk': SOURCE, 'clvk_runtime_ci': 34612600265,
        'compiler_original_sha256': COMPILER,
        'khronos_loader': 'f27c925e782499eebc4df20e121144358ccd5ac6',
        'headers': '386ca390b2f52efeb3e1a55a500690eb8013f60e',
        'runtime_gpu_validation': 'pending after installation; ancestor 0506ac3 passed'
    }, indent=2) + '\n')
    print('PASS input hashes, source identities and architecture PE checks')


if __name__ == '__main__':
    main()
