#!/usr/bin/env python3
import argparse
import json
import re
import sys
from collections import deque
from pathlib import Path


CLAIM_RE = re.compile(r"\bVA slice (?P<slice>\d+): \[")
TABLE_ADD_RE = re.compile(
    r"virgl context table add: ctx_id=(?P<ctx_id>\d+) result=(?P<result>ok|error)"
)
MARKERS = (
    (
        "rutabaga_destroy_begin",
        re.compile(r"rutabaga context destroy begin: ctx_id=(?P<ctx_id>\d+)"),
    ),
    (
        "rust_drop_begin",
        re.compile(r"virglrenderer context drop begin: ctx_id=(?P<ctx_id>\d+)"),
    ),
    (
        "virgl_table_remove_begin",
        re.compile(
            r"virgl context table remove begin: ctx_id=(?P<ctx_id>\d+) found=(?P<found>[01])"
        ),
    ),
    (
        "virgl_backend_destroy_begin",
        re.compile(r"virgl context backend destroy begin: ctx_id=(?P<ctx_id>\d+)"),
    ),
    (
        "kgsl_backend_destroy_begin",
        re.compile(
            r"ctx_id=(?P<ctx_id>\d+) backend destroy begin VA slice=(?P<slice>-?\d+)"
        ),
    ),
    (
        "va_slice_release",
        re.compile(
            r"ctx_id=(?P<ctx_id>\d+) released VA slice (?P<slice>\d+) "
            r"used=0x(?P<before>[0-9a-fA-F]+)->0x(?P<after>[0-9a-fA-F]+)"
        ),
    ),
    (
        "kgsl_backend_destroy_complete",
        re.compile(r"ctx_id=(?P<ctx_id>\d+) backend destroy complete"),
    ),
    (
        "virgl_backend_destroy_complete",
        re.compile(r"virgl context backend destroy complete: ctx_id=(?P<ctx_id>\d+)"),
    ),
    (
        "virgl_table_remove_complete",
        re.compile(
            r"virgl context table remove complete: ctx_id=(?P<ctx_id>\d+) found=(?P<found>[01])"
        ),
    ),
    (
        "rust_drop_complete",
        re.compile(r"virglrenderer context drop complete: ctx_id=(?P<ctx_id>\d+)"),
    ),
    (
        "rutabaga_destroy_complete",
        re.compile(r"rutabaga context destroy complete: ctx_id=(?P<ctx_id>\d+)"),
    ),
)


def _new_trace(ctx_id):
    return {
        "ctx_id": ctx_id,
        "created_in_delta": False,
        "slice": None,
        "claim_line": None,
        "add_line": None,
        "events": [],
    }


def _record_event(trace, name, line_number, match):
    details = {
        key: value
        for key, value in match.groupdict().items()
        if key != "ctx_id" and value is not None
    }
    trace["events"].append(
        {"name": name, "line": line_number, "details": details}
    )


def _next_event(trace, name, after, predicate=None):
    candidates = []
    for event in trace["events"]:
        if event["name"] != name or event["line"] <= after:
            continue
        if predicate is not None and not predicate(event["details"]):
            continue
        candidates.append(event)
    return min(candidates, key=lambda event: event["line"]) if candidates else None


def _assess_trace(trace):
    result = {
        "ctx_id": trace["ctx_id"],
        "slice": trace["slice"],
        "claim_line": trace["claim_line"],
        "add_line": trace["add_line"],
        "complete": False,
        "released": False,
        "first_missing": None,
        "events": trace["events"],
    }
    if trace["slice"] is None:
        result["first_missing"] = "va_slice_claim_correlation"
        return result

    after = trace["add_line"]
    steps_before_remove = (
        ("rutabaga_destroy_begin", "rutabaga_destroy_begin", None),
        ("rust_drop_begin", "virglrenderer_drop_begin", None),
    )
    steps_after_remove = (
        ("virgl_backend_destroy_begin", "virgl_backend_destroy_callback", None),
        ("kgsl_backend_destroy_begin", "drm2kgsl_backend_destroy", None),
        ("va_slice_release", "kgsl_va_slice_release", None),
        ("kgsl_backend_destroy_complete", "drm2kgsl_backend_destroy_complete", None),
        ("virgl_backend_destroy_complete", "virgl_backend_destroy_complete", None),
        ("virgl_table_remove_complete", "virgl_context_table_remove_complete", None),
        ("rust_drop_complete", "virglrenderer_drop_complete", None),
        ("rutabaga_destroy_complete", "rutabaga_destroy_complete", None),
    )

    release_event = None
    for event_name, missing_name, predicate in steps_before_remove:
        event = _next_event(trace, event_name, after, predicate)
        if event is None:
            result["first_missing"] = missing_name
            return result
        after = event["line"]

    remove_event = _next_event(trace, "virgl_table_remove_begin", after)
    if remove_event is None:
        result["first_missing"] = "virgl_context_table_remove"
        return result
    after = remove_event["line"]
    if remove_event["details"].get("found") != "1":
        result["first_missing"] = "virgl_context_table_lookup"
        return result

    for event_name, missing_name, predicate in steps_after_remove:
        event = _next_event(trace, event_name, after, predicate)
        if event is None:
            result["first_missing"] = missing_name
            return result
        after = event["line"]
        if event_name == "kgsl_backend_destroy_begin":
            if int(event["details"]["slice"]) != trace["slice"]:
                result["first_missing"] = "drm2kgsl_va_slice_identity"
                return result
        if event_name == "va_slice_release":
            release_event = event
        if event_name == "virgl_table_remove_complete":
            if event["details"].get("found") != "1":
                result["first_missing"] = (
                    "virgl_context_table_remove_complete_lookup"
                )
                return result

    release_slice = int(release_event["details"]["slice"])
    before = int(release_event["details"]["before"], 16)
    after_mask = int(release_event["details"]["after"], 16)
    expected_bit = 1 << trace["slice"]
    result["release"] = {
        "slice": release_slice,
        "used_before": f"0x{before:016x}",
        "used_after": f"0x{after_mask:016x}",
    }
    if release_slice != trace["slice"]:
        result["first_missing"] = "va_slice_release_identity"
        return result
    if not before & expected_bit or after_mask & expected_bit:
        result["first_missing"] = "va_slice_bit_clear"
        return result

    result["released"] = True
    result["complete"] = True
    return result


def classify_text(text, expected_created=2):
    traces = {}
    pending_claims = deque()
    anomalies = []
    failed_adds = []
    marker_line_count = 0

    for line_number, line in enumerate(text.splitlines(), 1):
        claim = CLAIM_RE.search(line)
        if claim:
            marker_line_count += 1
            pending_claims.append((int(claim.group("slice")), line_number))

        table_add = TABLE_ADD_RE.search(line)
        if table_add:
            marker_line_count += 1
            ctx_id = int(table_add.group("ctx_id"))
            result = table_add.group("result")
            trace = traces.setdefault(ctx_id, _new_trace(ctx_id))
            if result == "ok":
                if trace["created_in_delta"]:
                    anomalies.append(f"duplicate successful table add for ctx_id={ctx_id}")
                trace["created_in_delta"] = True
                trace["add_line"] = line_number
                if pending_claims:
                    trace["slice"], trace["claim_line"] = pending_claims.popleft()
                else:
                    anomalies.append(f"ctx_id={ctx_id} table add has no pending VA slice")
            else:
                failed_adds.append({"ctx_id": ctx_id, "line": line_number})
                if pending_claims:
                    pending_claims.popleft()

        for name, pattern in MARKERS:
            match = pattern.search(line)
            if match is None:
                continue
            marker_line_count += 1
            ctx_id = int(match.group("ctx_id"))
            trace = traces.setdefault(ctx_id, _new_trace(ctx_id))
            _record_event(trace, name, line_number, match)

    candidates = [
        trace for trace in traces.values() if trace["created_in_delta"]
    ]
    candidates.sort(key=lambda trace: trace["add_line"])
    assessments = [_assess_trace(trace) for trace in candidates]

    if pending_claims:
        anomalies.append(
            "unassigned VA slice claims: "
            + ", ".join(
                f"slice={slice_idx}@line={line}" for slice_idx, line in pending_claims
            )
        )
    if len(candidates) != expected_created:
        anomalies.append(
            f"created context count is {len(candidates)}, expected {expected_created}"
        )

    incomplete = [item for item in assessments if not item["complete"]]
    passed = not anomalies and not failed_adds and not incomplete
    if anomalies or failed_adds:
        classification = "structural_capture_error"
    elif incomplete:
        first = incomplete[0]
        classification = (
            f"ctx_id={first['ctx_id']} first_missing={first['first_missing']}"
        )
    else:
        classification = "all_created_contexts_destroyed_and_released"

    return {
        "passed": passed,
        "classification": classification,
        "expected_created_contexts": expected_created,
        "created_context_count": len(candidates),
        "marker_line_count": marker_line_count,
        "anomalies": anomalies,
        "failed_table_adds": failed_adds,
        "contexts": assessments,
        "external_context_ids": sorted(
            trace["ctx_id"]
            for trace in traces.values()
            if not trace["created_in_delta"]
        ),
    }


def main():
    parser = argparse.ArgumentParser(
        description="Classify one bounded crosvm context-diagnostic log delta."
    )
    parser.add_argument("input", help="Capture output path, or '-' for stdin")
    parser.add_argument("--expected-created", type=int, default=2)
    args = parser.parse_args()
    if args.expected_created < 1:
        parser.error("--expected-created must be positive")

    if args.input == "-":
        text = sys.stdin.read()
    else:
        text = Path(args.input).read_text(encoding="utf-8", errors="replace")
    result = classify_text(text, expected_created=args.expected_created)
    print(json.dumps(result, indent=2, sort_keys=True))
    if result["anomalies"] or result["failed_table_adds"]:
        return 2
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
