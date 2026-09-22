Run `python3 viogpu/tests/native-surface/run.py --negative-controls`.

The fixture compiles the production escape, registry, import/release and
allocation-reference functions against a deterministic host shim. It checks
creator authority, exact free identity, generation checks, same-resource
allocation adoption, deferred FREE, failed detach retention, uncertain import
retention, busy UNREF retention, and confirmed device/context retirement.
Four mutations prove foreign FREE, premature UNREF, stale import, and forged
pitch are caught. The shared ABI manifest separately pins the 128-byte layout.

This is a local ownership checkpoint. Host-surface scanout and CPU Present
explicitly return DEVICE_NOT_READY until per-buffer producer completion and
Android release acquisition are integrated. These tests do not prove GPU or
SurfaceControl fence completion, Windows driver compilation, or runtime display.

One WDDM allocation wrapper may own a surface; Windows resource opens share
that wrapper. The registry caps host surfaces at 64 and 512 MiB per adapter
(256 MiB per surface). Missing creator FREE retains a bounded allocation until
confirmed adapter stop; process-exit cleanup is not yet a separate callback.
