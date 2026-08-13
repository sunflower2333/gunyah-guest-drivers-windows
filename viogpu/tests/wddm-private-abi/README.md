# DroidVM WDDM private ABI fixture

This fixture compiles the same WDK-independent private ABI snapshot as
independent KMD and UMD endpoints. Both endpoints must emit the exact
experimental pre-v1 `sizeof`/`offsetof`/value manifest.

The fixture is a regression lock for the current validation scaffold, not a
published version-1 contract. Version 1 remains blocked on real context VA
query, requested allocation IOVA, guest-backed allocation, and teardown
semantics.

The ABI contains no pointers, Windows handles, physical addresses, GPA values,
VirtIO identifiers, KGSL identifiers, or KMD context identifiers. Reserved
fields must be zero. The only accepted revision is the current pre-v1 revision.

Run the local GCC and Clang checks with:

```text
viogpu/tests/wddm-private-abi/run-local.sh
```

`run-msvc.cmd` is the matching Windows MSVC gate.
