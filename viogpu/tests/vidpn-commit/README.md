# VidPN whole-source commit regression

`python run.py` compiles the actual production `VioGpuDod::CommitVidPn` body
against strict VidPN peers. The peers reject `D3DDDI_ID_ALL` in per-source
callbacks, as the Windows API requires. Both whole-VidPN and source-zero commits
must forward the selected 1,160,680,000 Hz target, release every acquired mode,
mode set and path, and report the true final status. Thirteen injected callback
failures must preserve the previous selection and release borrowed references.
Invalid source IDs, powered-off/empty/unpinned source cases and a missing target
complete the 41-check fixture.

`python run.py --negative-control-source-all` removes only source normalization
from the extracted body. The executable must exit 1 and identify the missing
whole-source contract; the runner verifies that expected failure.

Linux runs use ASan/UBSan. Windows CI compiles the same fixture with MSVC and
builds the complete ARM64 driver against WDK. These controlled peers prove the
callback contract, not successful target mode switching or hardware rendering.
