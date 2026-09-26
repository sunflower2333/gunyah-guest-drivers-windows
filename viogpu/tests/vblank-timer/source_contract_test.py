#!/usr/bin/env python3
"""Reject pageable cadence locks and torn/partial snapshot publication."""
import importlib.util
import contextlib
import io
from pathlib import Path

root = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('cadence_contract', root / 'viogpu/viogpuwddm/check-contract.py')
contract = importlib.util.module_from_spec(spec)
spec.loader.exec_module(contract)
contract.check_legacy_runtime_callback_contract()
print('PASS cadence source/nonpaged/publication baseline')


def reject(global_name, old, new, expected):
    original = getattr(contract, global_name)
    assert original.count(old) == 1
    setattr(contract, global_name, original.replace(old, new))
    diagnostic = io.StringIO()
    try:
        with contextlib.redirect_stderr(diagnostic):
            contract.check_legacy_runtime_callback_contract()
    except SystemExit as error:
        if error.code != 1 or expected not in diagnostic.getvalue():
            raise AssertionError(diagnostic.getvalue()) from error
        print('PASS source negative: ' + expected)
    else:
        raise AssertionError('source negative missed: ' + expected)
    finally:
        setattr(contract, global_name, original)


for method in ('DisarmCrtcVsyncTimer', 'RecordCrtcVblankDelivery', 'ReadCrtcVblankCadence'):
    old = '__declspec(code_seg(".text")) VOID ' + method + '('
    reject('VIOGPU_HEADER_SOURCE', old, old.replace('".text"', '"PAGE"'),
           'vblank cadence spinlock routine must stay noinline and nonpaged: ' + method)
old = '    snapshot.Counters = m_CrtcVblankCadence;\n    KeReleaseSpinLock(&m_CrtcTimingLock, oldIrql);'
reject('VIOGPU_CODE', old,
       '    KeReleaseSpinLock(&m_CrtcTimingLock, oldIrql);\n    snapshot.Counters = m_CrtcVblankCadence;',
       'vblank cadence timestamp, period and counters must use one timing-lock snapshot')
reject('VIOGPU_CODE', 'REG_BINARY, &snapshot, sizeof(snapshot))', 'REG_BINARY, &snapshot, sizeof(snapshot) - 8)',
       'vblank cadence publication must atomically write one full binary snapshot and return its status')
