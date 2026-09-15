#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Check the real source checker's wide-identifier normalization, without loading the driver."""
import ast
from pathlib import Path
import re


# Extract only the production helpers; do not duplicate their implementation.
def main() -> None:
    root = Path(__file__).resolve().parents[3]
    path = root / "viogpu/viogpuwddm/check-contract.py"
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    helpers = [n for n in tree.body if isinstance(n, ast.FunctionDef) and
               n.name in ("compact_code", "canonical_code")]
    if len(helpers) != 2:
        raise AssertionError("expected two production canonicalization helpers")
    scope = {"re": re}
    exec(compile(ast.Module(body=helpers, type_ignores=[]), str(path), "exec"), scope)
    canonical = scope["canonical_code"]
    whole = 'L"NativeDisplayBlitReadbackUsec"'
    split = 'L"NativeDispla"\n   L"yBlitReadbac"\n L"kUsec"'
    assert canonical(whole) == canonical(split)
    assert canonical('L"" L"NativeDisplayBlitReadbackUsec"') == canonical(whole)
    assert canonical(split.replace('kUsec', 'kOther')) != canonical(whole)
    assert canonical('L"NativeDisplay" + L"BlitReadbackUsec"') != canonical(whole)
    assert canonical('"NativeDisplayBlitReadbackUsec"') != canonical(whole)
    assert canonical('L"NativeDisplay", L"BlitReadbackUsec"') != canonical(whole)
    assert canonical('L"\\x4e" L"ativeDisplayBlitReadbackUsec"') != canonical(whole)
    assert canonical('RecordDisplayValue(46,value)') != canonical('RecordDisplayValue(45,value)')
    print("PASS: production normalization preserves diagnostic names, encoding, operators and counter ownership")


if __name__ == "__main__":
    main()
