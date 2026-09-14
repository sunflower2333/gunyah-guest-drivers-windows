#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Read drained VIOGPU cumulative records; compare observations, not GPU time/FPS."""
import argparse
import json
from pathlib import Path
import re
import sys
from typing import Dict, Iterable, List, Optional

FIELDS = (
    ('window', 'accepted', 'dispatched', 'retired', 'cancelled_queued'),
    ('render', 'bytes', 'refs', 'refs_max', 'pending_peak', 'host_pending_peak'),
    ('render_retired', 'retire_100ns_total', 'retire_100ns_max', 'handoffs'),
)
COUNTERS = ('accepted', 'dispatched', 'retired', 'cancelled_queued', 'render',
            'bytes', 'refs', 'render_retired', 'retire_100ns_total', 'handoffs')
PEAKS = ('refs_max', 'pending_peak', 'host_pending_peak', 'retire_100ns_max')
LIMIT = (1 << 64) - 1


# Parse exactly one schema row while allowing a debugger prefix before its anchor.
def parse_row(line: str, fields: tuple, line_number: int) -> Dict[str, int]:
    anchor = re.search(r'(?<![A-Za-z0-9_])' + fields[0] + r'=', line)
    if anchor is None:
        raise ValueError(f'line {line_number}: missing {fields[0]} row')
    values = {}
    for token in line[anchor.start():].split():
        match = re.fullmatch(r'([a-z0-9_]+)=([0-9]+)', token)
        if match is None:
            raise ValueError(f'line {line_number}: malformed token {token!r}')
        name, number = match.groups()
        if name not in fields or name in values:
            raise ValueError(f'line {line_number}: unknown or duplicate field {name}')
        if len(number) > 20 or int(number) > LIMIT:
            raise ValueError(f'line {line_number}: {name} exceeds uint64')
        values[name] = int(number)
    if set(values) != set(fields):
        raise ValueError(f'line {line_number}: incomplete record row')
    return values


# Enforce the production reporter's fully drained ownership accounting.
def validate_record(record: Dict[str, int]) -> None:
    if not 1 <= record['window'] <= 128:
        raise ValueError('pipeline window must be within 1..128')
    if record['accepted'] != record['retired'] + record['cancelled_queued']:
        raise ValueError('drained record has unbalanced accepted/retired/cancelled work')
    if record['dispatched'] != record['retired']:
        raise ValueError('drained record has unretired dispatched work')
    if record['render'] != record['render_retired'] or record['render'] > record['dispatched']:
        raise ValueError('drained record has inconsistent Render retirement')
    if record['handoffs'] > record['render']:
        raise ValueError('handoffs exceed Render dispatches')
    if record['host_pending_peak'] > record['window']:
        raise ValueError('host-owned/backlog peak exceeds configured window')
    if record['refs_max'] > record['refs'] or record['retire_100ns_max'] > record['retire_100ns_total']:
        raise ValueError('lifetime maximum exceeds cumulative sum')
    if record['render'] == 0 and any(record[k] for k in (
            'bytes', 'refs', 'refs_max', 'retire_100ns_total', 'retire_100ns_max')):
        raise ValueError('Render measurements exist without a Render dispatch')


# Ignore unrelated logs, but fail on partial/interleaved/truncated metric records.
def parse_records(lines: Iterable[str]) -> List[Dict[str, int]]:
    records = []
    pending = None
    row = 0
    for number, line in enumerate(lines, 1):
        if 'viogpu perf:' in line:
            if pending is not None:
                raise ValueError(f'line {number}: new header interrupts incomplete record')
            payload = line.split('viogpu perf:', 1)[1]
            pending = parse_row(payload, FIELDS[0], number)
            row = 1
            continue
        metric_row = next((i for i in (1, 2) if re.search(
            r'(?<![A-Za-z0-9_])' + FIELDS[i][0] + r'=', line)), None)
        if metric_row is None:
            continue
        if pending is None or metric_row != row:
            raise ValueError(f'line {number}: orphan or out-of-order metric row')
        pending.update(parse_row(line, FIELDS[row], number))
        row += 1
        if row == len(FIELDS):
            validate_record(pending)
            records.append(pending)
            pending = None
    if pending is not None:
        raise ValueError('truncated VIOGPU performance record at end of input')
    if not records:
        raise ValueError('no complete VIOGPU performance records found')
    return records


# Preserve zero-denominator observations as JSON null, never infinity or a fake zero.
def ratio(numerator: int, denominator: int) -> Optional[float]:
    return numerator / denominator if denominator else None


# Use the first observed snapshot as baseline; never pretend capture began at zero.
def summarize(records: List[Dict[str, int]]) -> dict:
    if not records:
        raise ValueError('no snapshots to summarize')
    previous = records[0]
    distinct = [previous]
    for current in records[1:]:
        if current['window'] != previous['window']:
            raise ValueError('mixed windows: split captures by adapter/run before comparing')
        if any(current[key] < previous[key] for key in COUNTERS + PEAKS):
            raise ValueError('counter/peak decreased: reset, wrap, mixed adapters or reordered capture; split the log')
        if current != previous:
            distinct.append(current)
        previous = current
    first, last = distinct[0], distinct[-1]
    delta = {key: last[key] - first[key] for key in COUNTERS} if len(distinct) > 1 else None
    interval = None
    if delta is not None:
        if delta['accepted'] != delta['retired'] + delta['cancelled_queued']:
            raise ValueError('interval ownership accounting is inconsistent')
        if delta['handoffs'] > delta['render']:
            raise ValueError('interval handoffs exceed Render dispatches')
        if not delta['render'] and any(delta[k] for k in ('bytes', 'refs', 'retire_100ns_total')):
            raise ValueError('interval Render measurements changed without a Render dispatch')
        interval = {
            'counters': delta,
            'refs_per_render': ratio(delta['refs'], delta['render']),
            'bytes_per_render': ratio(delta['bytes'], delta['render']),
            'dispatch_to_terminal_us': ratio(delta['retire_100ns_total'], delta['render_retired'] * 10),
            'pipeline_handoff_fraction': ratio(delta['handoffs'], delta['render']),
        }
    return {
        'window': last['window'],
        'snapshots': len(records),
        'distinct_snapshots': len(distinct),
        'duplicate_snapshots': len(records) - len(distinct),
        'baseline': first,
        'final': last,
        'interval': interval,
        # Peaks have no inverse: subtracting two lifetime maxima is not an interval maximum.
        'lifetime_peaks': {key: last[key] for key in PEAKS},
    }


# Compare only well-defined interval averages; leave workload equivalence to the caller.
def compare(baseline: dict, candidate: dict) -> dict:
    left, right = baseline['interval'], candidate['interval']
    if left is None or right is None:
        raise ValueError('comparison needs at least two distinct snapshots in each capture')
    if not left['counters']['render'] or not right['counters']['render']:
        raise ValueError('comparison needs Render activity in each observed interval')
    changes = {}
    for key in ('refs_per_render', 'bytes_per_render', 'dispatch_to_terminal_us', 'pipeline_handoff_fraction'):
        before, after = left[key], right[key]
        changes[key] = {
            'baseline': before,
            'candidate': after,
            'relative_change_percent': ((after - before) / before * 100) if before else None,
        }
    return {
        'baseline_window': baseline['window'],
        'candidate_window': candidate['window'],
        'observations': changes,
        'performance_verdict': None,
    }


# Accept UTF-8 debugger exports strictly so damaged bytes cannot become valid metrics.
def read_capture(path: Path) -> dict:
    with path.open(encoding='utf-8-sig') as stream:
        return summarize(parse_records(stream))


# Emit machine-readable measurements without estimating FPS or physical GPU utilization.
def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path, help='UTF-8 log from one adapter lifetime')
    parser.add_argument('--compare', type=Path, help='candidate log from one adapter lifetime')
    parser.add_argument('--output', type=Path, help='write JSON here instead of stdout')
    args = parser.parse_args(argv)
    try:
        if args.output and args.output.resolve() in {
                args.capture.resolve(), args.compare.resolve() if args.compare else None}:
            raise ValueError('output must not overwrite an input capture')
        baseline = read_capture(args.capture)
        result = {
            'schema_version': 1,
            'capture': str(args.capture),
            'baseline': baseline,
            'limitations': [
                'One adapter lifetime per capture; indistinguishable interleaved adapters cannot be detected.',
                'First snapshot is the baseline; earlier work is excluded. One snapshot has no interval.',
                'Host-pending includes software backlog, not only physical-host inflight work.',
                'Dispatch-to-terminal includes validation and queuing, not GPU-only execution time.',
                'Lifetime peaks are not interval peaks. No wall time, FPS, tail percentile or utilization is inferred.',
                'A/B comparison requires the same workload, revisions and settings apart from the tested variable.',
            ],
        }
        if args.compare:
            candidate = read_capture(args.compare)
            result['candidate_capture'] = str(args.compare)
            result['candidate'] = candidate
            result['comparison'] = compare(baseline, candidate)
        encoded = json.dumps(result, indent=2, allow_nan=False) + '\n'
        if args.output:
            args.output.write_text(encoded, encoding='utf-8')
        else:
            sys.stdout.write(encoded)
    except (OSError, UnicodeError, ValueError) as error:
        print(f'viogpu-perf: ERROR: {error}', file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
