# Destroy request preflight

Run `python3 viogpu/tests/destroy-preflight/run.py`.

The fixture executes the actual `VioGpuWddmDestroyAllocation` prefix up to the
allocation lifecycle loop, with counted share revocation and flip cancellation.
Fifteen cases cover valid requests and rejected resource/allocation/IRQL/flag
requests. ASan/UBSan are enabled. A semantic negative restores the original
early side effects; six rejected resource cases must then fail.

This checks that invalid resource handles, wrong resource membership and an
incomplete resource allocation list do not revoke live sharing or cancel flips.
It does not simulate final host teardown, prove atomic multi-allocation destroy,
or resolve the native scanout lease/unmap lifetime problems. The fix must still
pass the actual ARM64 miniport build before integration into a signed candidate.
