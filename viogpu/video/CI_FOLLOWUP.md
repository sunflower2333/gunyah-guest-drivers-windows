# Independent video branch CI follow-up — 2026-09-15

Branch: `work/vpu-video-umd-perf-20260915`. PR #3 is closed without merging;
these fixes do not change the perf branch or reopen a pull request.

At `a521403d1d5a84ab648138ae8d5b57c47b039338`, the dedicated video build
(run 34927517113) and repository format check (34927517172) passed. Full
WDDM run 34927517145 stopped in the Present raw-copy negative-control
injector: its exact two-line anchor no longer matched the formatted call.
Performance run 34927517129 stopped in legacy migration idempotence checks
for the same kind of source-shape change. These runs were not all-green.

This follow-up keeps exact operands and unique matches while accepting line
wrapping in the Present and VidPN negative-control injectors. The legacy
migration utility accepts only two explicitly reviewed already-applied
layouts, not arbitrary whitespace-normalized source. Mixed, duplicate,
missing and semantically altered blocks still fail closed. New unit tests
cover accepted layouts and rejection cases. No production C++/codec code,
Mesa pin, required check or signing policy is changed by these fixes.

Local validation: Present positives (25/25 and 37/37), both Present negative
modes, VidPN positive (44/44) and all-source negative, timeout overwrite
negative, seven migration unit tests, power (28/28), Render (51/51), fault
(29/29), backing (56/56) and adapter identity (198 checks) passed. Local
source archives lack Git metadata, so the Mesa gitlink package test must be
validated in the real Actions checkout, not inferred from the archive.

Consult the exact follow-up commit's branch-push Actions for final results.
An earlier passing video artifact does not establish later full-suite
success. Standard DXVA/MFT integration and on-device MediaCodec validation
remain separate outstanding work; compilation is not hardware acceptance.
