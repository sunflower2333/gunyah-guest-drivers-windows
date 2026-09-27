#!/usr/bin/env python3
"""Compile geometry policy and actual synchronous control-queue methods."""
from pathlib import Path
import os
import re
try:
    import resource
except ImportError:  # Windows has no POSIX resource limits.
    resource = None
import subprocess
import tempfile

if resource is not None:
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
here = Path(__file__).resolve().parent
root = here.parents[2]
assert (root / 'viogpu/shared/viogpu_scanout_geometry.h').read_bytes() == (
    root / 'external/mesa/src/freedreno/vulkan/viogpu_scanout_geometry.h').read_bytes(), 'KMD/Mesa geometry drift'
assert (root / 'viogpu/shared/viogpu_scanout_profile.h').read_bytes() == (
    root / 'external/mesa/src/freedreno/vulkan/viogpu_scanout_profile.h').read_bytes(), 'KMD/Mesa profile drift'
assert (root / 'viogpu/shared/viogpu_wddm_scanout.h').read_bytes() == (
    root / 'external/mesa/src/freedreno/vulkan/viogpu_wddm_scanout.h').read_bytes(), 'KMD/Mesa profile ABI drift'
assert (root / 'viogpu/shared/viogpu_wddm_scanout_diagnostic.h').read_bytes() == (
    root / 'external/mesa/src/freedreno/vulkan/viogpu_wddm_scanout_diagnostic.h').read_bytes(), 'KMD/Mesa diagnostic ABI drift'
source = (root / 'viogpu/common/viogpu_queue.cpp').read_text()

def definition(name):
    match = re.search(r'^[^;\n]*\bCtrlQueue::' + name + r'\([^;]*?\)\s*\{', source, re.M)
    assert match, name
    start = source.index('{', match.start())
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]

production = '\n'.join(definition(n) for n in ('QueryScanoutGeometry', 'ConfigureScanoutGeometry',
                                              'BindScanoutGeometry', 'QueryScanoutProfile',
                                              'ConfigureScanoutProfile'))
fixture = (here / 'geometry_transport_test.cpp').read_text()
variants = [('transport', production)]
for name, old, new in (
    ('missing-negotiation', '!negotiated ||', '(negotiated && false) ||'),
    ('invented-reset', 'command->HostResetGeneration = geometry->HostResetGeneration;',
     'command->HostResetGeneration = geometry->HostResetGeneration + 1;'),
    ('profile-reset-mismatch', 'profile->HostResetGeneration != geometry->HostResetGeneration ||', ''),
    ('profile-token-restamp', 'command->ProfileGeneration = profile->Profile.ProfileGeneration;',
     'command->ProfileGeneration = profile->Profile.ProfileGeneration + 1;'),
):
    assert old in production
    variants.append((name, production.replace(old, new)))
with tempfile.TemporaryDirectory(prefix='.geometry-', dir=here) as temp:
    output = Path(temp)
    flags = ['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
             '-fno-omit-frame-pointer', '-I' + str(root / 'viogpu/common')]
    binary = output / 'geometry'
    subprocess.run(flags + [str(here / 'scanout_geometry_test.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    for name, body in variants:
        unit, binary = output / (name + '.cpp'), output / name
        unit.write_text(fixture.replace('// INSERT_PRODUCTION', body))
        subprocess.run(flags + [str(unit), '-o', str(binary)], env={**os.environ, 'TMPDIR':temp}, check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if (result.returncode == 0) != (name == 'transport'):
            raise SystemExit(name + ': unexpected result\n' + result.stdout + result.stderr)
        print(result.stdout.strip() if name == 'transport' else 'PASS negative control ' + name + ' rejected')

    mode_source = (root / 'viogpu/viogpudo/viogpudo.cpp').read_text()
    assert mode_source.count('AckFeature(VIRTIO_GPU_F_NATIVE_SCANOUT_GEOMETRY)') == 1
    assert 'm_pVioGpuDod->NativeScanoutDiagnosticEnabled() && SupportsNativeAhbPaging() &&' in mode_source
    pivot_start = mode_source.index('        D3DKMDT_VIDPN_PRESENT_PATH LocalVidPnPresentPath = *pVidPnPresentPath;')
    pivot_end = mode_source.index('        if (SupportFieldsModified)', pivot_start)
    pivot_production = mode_source[pivot_start:pivot_end]
    pivot_fixture = (here / 'vidpn_pivot_test.cpp').read_text()
    for name, body, native in (
        ('pivot-native', pivot_production, True), ('pivot-legacy', pivot_production, False),
        ('pivot-diagnostic', pivot_production, True),
        ('pivot-missing-offset0', pivot_production.replace(
            'LocalVidPnPresentPath.ContentTransformation.RotationSupport.Offset0 = 1;', ''), True),
        ('pivot-invalid-offset90', pivot_production.replace(
            'LocalVidPnPresentPath.ContentTransformation.RotationSupport.Offset0 = 1;',
            'LocalVidPnPresentPath.ContentTransformation.RotationSupport.Offset90 = 1;'), True),
        ('pivot-inverted', pivot_production.replace('EnumPivotType == D3DKMDT_EPT_ROTATION',
            'EnumPivotType != D3DKMDT_EPT_ROTATION'), True),
        ('pivot-stale-reserved', re.sub(r'\s*RtlZeroMemory\(&LocalVidPnPresentPath.ContentTransformation.RotationSupport,\s*'
            r'sizeof\(LocalVidPnPresentPath.ContentTransformation.RotationSupport\)\);', '', pivot_production), True),
    ):
        unit, binary = output / (name + '.cpp'), output / name
        unit.write_text(pivot_fixture.replace('// INSERT_PRODUCTION', body))
        subprocess.run(flags + (['-DVIOGPU_NATIVE_CONTEXT'] if native else []) +
                       (['-DDIAGNOSTIC_ENABLED'] if name == 'pivot-diagnostic' else []) +
                       [str(unit), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if (result.returncode == 0) != (name in ('pivot-native', 'pivot-legacy', 'pivot-diagnostic')):
            raise SystemExit(name + ': unexpected result\n' + result.stdout + result.stderr)
        print(result.stdout.strip() if result.returncode == 0 else 'PASS negative control ' + name + ' rejected')
    binding_bodies = []
    for name in ('SupportsNativeScanoutGeometry', 'QueryNativeScanoutState',
                 'NativeScanoutBindingCurrent', 'BindNativeScanoutProfile'):
        match = re.search(r'^[^;\n]*\bVioGpuAdapter::' + name + r'\([^;]*?\)\s*(?:const\s*)?\{', mode_source, re.M)
        assert match, name
        start = mode_source.index('{', match.start())
        depth, end = 1, start + 1
        while depth:
            depth += (mode_source[end] == '{') - (mode_source[end] == '}')
            end += 1
        binding_bodies.append(mode_source[match.start():end])
    binding_production = '\n'.join(binding_bodies)
    transaction_bodies = binding_bodies[:2]
    for name in ('ReserveNativeScanoutMode', 'NativeScanoutModeEligible', 'CommitNativeScanoutMode',
                 'QueryReservedNativeScanoutMode', 'ObserveNativeScanoutProfile'):
        match = re.search(r'^[^;\n]*\bVioGpuAdapter::' + name + r'\([^;]*?\)\s*\{', mode_source, re.M)
        assert match, name
        start = mode_source.index('{', match.start())
        depth, end = 1, start + 1
        while depth:
            depth += (mode_source[end] == '{') - (mode_source[end] == '}')
            end += 1
        transaction_bodies.append(mode_source[match.start():end])
    transaction_production = '\n'.join(transaction_bodies)
    transaction_fixture = (here / 'mode_transaction_test.cpp').read_text()
    for name, body in (
        ('transaction', transaction_production),
        ('transaction-local-reset', transaction_production.replace('mode->LocalResetGeneration != localReset ||', '')),
        ('transaction-forged-reservation', transaction_production.replace(
            '!VioGpuNativeScanoutModeEqual(mode, &m_NativeScanoutCandidate) ||', '')),
        ('transaction-post-ack-query', transaction_production.replace('m_NativeScanoutCommitted = *mode;',
            'if (!QueryNativeScanoutState(&active, &preferred, &localReset)) result = VioGpuHostContextRejected;\n'
            '            m_NativeScanoutCommitted = *mode;')),
        ('transaction-unknown-reset', transaction_production.replace('m_pVioGpuDod->RequestHardwareResetAtAnyIrql();', '')),
        ('transaction-recycled-reservation', transaction_production.replace(
            'last = m_NativeScanoutReservedMode;', 'last = active.ModeGeneration;')),
    ):
        unit, binary = output / (name + '.cpp'), output / name
        unit.write_text(transaction_fixture.replace('// INSERT_PRODUCTION', body))
        subprocess.run(flags + [str(unit), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if (result.returncode == 0) != (name == 'transaction'):
            raise SystemExit(name + ': unexpected result\n' + result.stdout + result.stderr)
        print(result.stdout.strip() if name == 'transaction' else 'PASS negative control ' + name + ' rejected')
    binding_fixture = (here / 'profile_binding_test.cpp').read_text()
    for name, body in (
        ('binding', binding_production),
        ('binding-reset-domain', binding_production.replace('generation != localReset ||', '')),
        ('binding-token-restamp', binding_production.replace('candidate.ProfileGeneration = profile->ProfileGeneration;',
            'candidate.ProfileGeneration = profile->ProfileGeneration + 1;')),
        ('binding-revoked-publication', binding_production.replace(
            'if (!NativeScanoutBindingCurrent(&candidate)) return VioGpuHostContextUnknown;', '')),
    ):
        unit, binary = output / (name + '.cpp'), output / name
        unit.write_text(binding_fixture.replace('// INSERT_PRODUCTION', body))
        subprocess.run(flags + [str(unit), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if (result.returncode == 0) != (name == 'binding'):
            raise SystemExit(name + ': unexpected result\n' + result.stdout + result.stderr)
        print(result.stdout.strip() if name == 'binding' else 'PASS negative control ' + name + ' rejected')
    # Preserve the real declaration order: the framebuffer owner calls the
    # global helper before its late definition in this translation unit.
    color_declaration = re.search(r'^UINT ColorFormat\(UINT format\);$', mode_source, re.M)
    color_definition = re.search(r'^UINT ColorFormat\(UINT format\)\s*\{', mode_source, re.M)
    assert color_declaration and color_definition
    owner_bodies = [color_declaration.group()]
    for name in ('ReleaseFrameBufferOwner', 'RetireFrameBufferMode', 'CollectRetiredFrameBuffers',
                 'PrepareFrameBufferMode', 'CommitFrameBufferMode'):
        match = re.search(r'^[^;\n]*\bVioGpuAdapter::' + name + r'\([^;]*?\)\s*\{', mode_source, re.M)
        assert match, name
        assert color_declaration.start() < match.start() < color_definition.start(), name
        start = mode_source.index('{', match.start())
        depth, end = 1, start + 1
        while depth:
            depth += (mode_source[end] == '{') - (mode_source[end] == '}')
            end += 1
        owner_bodies.append(mode_source[match.start():end])
    start = mode_source.index('{', color_definition.start())
    depth, end = 1, start + 1
    while depth:
        depth += (mode_source[end] == '{') - (mode_source[end] == '}')
        end += 1
    owner_bodies.append(mode_source[color_definition.start():end])
    owner_production = '\n'.join(owner_bodies)
    owner_fixture = (here / 'framebuffer_owner_test.cpp').read_text()
    for name, body in (
        ('framebuffer', owner_production),
        ('retirement-unconfirmed', owner_production.replace('return FALSE;', 'return TRUE;', 1)),
        ('framebuffer-early-publish', owner_production.replace('UINT previous = 0;',
             'm_FrameBufferOwner = prepared; UINT previous = 0;')),
        ('framebuffer-helper-undeclared', owner_production.replace(color_declaration.group(), '')),
    ):
        unit, binary = output / (name + '.cpp'), output / name
        unit.write_text(owner_fixture.replace('// INSERT_PRODUCTION', body))
        compiled = subprocess.run(flags + [str(unit), '-o', str(binary)], capture_output=True, text=True)
        if name == 'framebuffer-helper-undeclared':
            assert compiled.returncode != 0 and 'ColorFormat' in compiled.stderr, compiled.stderr
            print('PASS negative control framebuffer-helper-undeclared rejected at compilation')
            continue
        if compiled.returncode != 0:
            raise SystemExit(name + ': compilation failed\n' + compiled.stdout + compiled.stderr)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if (result.returncode == 0) != (name == 'framebuffer'):
            raise SystemExit(name + ': unexpected result\n' + result.stdout + result.stderr)
        print(result.stdout.strip() if name == 'framebuffer' else 'PASS negative control ' + name + ' rejected')
    bodies = []
    for name in ('SetSourceModeAndPath', 'UpdateActiveVidPnPresentPath'):
        match = re.search(r'NTSTATUS\s+VioGpuDod::' + name + r'\([^;]*?\)\s*\{', mode_source)
        assert match, name
        start = mode_source.index('{', match.start())
        depth, end = 1, start + 1
        while depth:
            depth += (mode_source[end] == '{') - (mode_source[end] == '}')
            end += 1
        bodies.append(mode_source[match.start():end])
    mode_fixture = (here / 'native_mode_test.cpp').read_text().replace('// INSERT_PRODUCTION', '\n'.join(bodies))
    mode_policy = (root / 'viogpu/common/viogpu_native_mode_policy.h').read_text()
    for name, policy in (('mode', mode_policy), ('mode-ungated', mode_policy.replace('== 1U', '!= 0U')),
                         ('mode-early-publish', mode_policy)):
        (output / 'viogpu_native_mode_policy.h').write_text(policy)
        unit, binary = output / (name + '.cpp'), output / name
        unit.write_text(mode_fixture if name != 'mode-early-publish' else mode_fixture.replace(
            'pCurrentMode->Flags.FullscreenPresent = TRUE;',
            'pCurrentMode->Flags.FullscreenPresent = TRUE; m_CurrentMode = candidate;'))
        subprocess.run(flags + [str(unit), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if (result.returncode == 0) != (name == 'mode'):
            raise SystemExit(name + ': unexpected result\n' + result.stdout + result.stderr)
        print(result.stdout.strip() if name == 'mode' else 'PASS negative control ' + name + ' rejected')
