# Actual host DVSA discovery consumer regression

This executes the production KMD byte parser used by
`CtrlQueue::QueryDisplayAllocationDiscovery`. That query logs observations from
`GetDisplayInfo`; it changes no WDDM/display/scanout capability and issues no
allocation, GPU submission or presentation request. Queue timeouts retain the
existing synchronous descriptor quarantine; parsing requires the actual used
length and an independently observed PCI shared-memory region size.

`fixtures/*.bin` are immutable128byte responses emitted by the actual crosvm
`SharedAllocationDiscovery::from_host` factory and production `encode` path,
including the real ordinary virtio control-header type. They were generated
before mapping-only admission was added, so all four have feature_bits0 and
max_allocations0. They are not reconstructed by this test. Source provenance
and exact parameters are recorded below; SHA256 identity is checked in CI by
`verify-fixtures.ps1`.

| Fixture | Surface generation | Dimensions | BAR/prefix/alignment bytes | Reasons | Triple lower bound |
|---|---:|---|---|---|---:|
| absent.bin | 0 | 0x0 | no mapper | 0x1f9 | 0 |
| bar8m_reserved8m_align16k.bin | 17 | 3040x1904 | 8388608/8388608/16384 | 0x1f6 | 69500928 |
| bar128m_reserved8m_align4k.bin | 17 | 3040x1904 | 134217728/8388608/4096 | 0x1f0 | 69464064 |
| bar128m_reserved8m_align16k.bin | 17 | 3040x1904 | 134217728/8388608/16384 | 0x1f0 | 69500928 |

The canonical `shared/dvsa_protocol.h` is byte-identical to the host freeze at
crosvm5f24ae0; SHA25641e1b2cc133cf23622d4d78f410eb8497496e41badb49a4f6005512ae0ba3525.
LF and binary Git attributes preserve these identities on Windows runners.
Production uses the freestanding discovery subset in `shared/viogpu_dvsa_wire.h`
because the canonical host header's user-mode `stdint.h` conflicts with the
WDK kernel CRT. This test checks every guest field offset/width, struct size/
alignment, and protocol constant against the unchanged canonical header.
Query bytes are compared with a canonical host structure, and response bytes
are checked against the four immutable host fixtures. Real WDK compilation
remains a separate CI gate; user-mode parser compilation cannot substitute it.

Producer source hashes at fixture generation, crosvm8c97833 descendant:
```
7489bd90260f5e2912fabe9d3f4588b754349a006347244ea7af1c795521b8e8 devices/src/virtio/gpu/control_header.rs
a4d6ba8edea6b27bfee12dc9c8de838fd29bc398491f0d1b18d3ff8d8e2a4465 devices/src/virtio/gpu/shared_allocation_protocol.rs
404d4ec3b4919966ad4da223ec1cd3a5fdf38111eb0dd983f95346503d5499a3 devices/src/virtio/gpu/protocol.rs
5656533191d07cdb1b5e787bb384e1178adf67f3c11428073df3c4ac369185b7 vm_control/src/shared_allocation.rs
```

The test also derives explicitly labelled semantic controls for allocation-only
bit1, unknown/full-SDR features, quota bounds, missing prerequisites, truncated
payloads, unaligned buffers, contradictory reasons and arithmetic bounds.
Derived controls are not additional host runtime fixtures. Every malformed
response must leave the complete guarded output untouched. Bit0/full SDR is
rejected by this consumer; observing valid bit1 grants no allocation or lease.

Run `run-msvc.cmd` from an MSVC environment. The real ARM64 WDK workflow and
joint signed-driver workflow both register it. Local Clang validation uses:
```
clang++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-omit-frame-pointer display_allocation_test.cpp -o /path/to/test
/path/to/test fixtures
```
Local PASS5866 checks. CI verifies actual driver compilation independently.

The descendant allocation client now lives in `common/display_allocation_client.h`.
The actual adapter calls it after initialization and display configuration events
under a nonblocking lifecycle mutex; shutdown attempts cleanup before quiescing
the transport. It creates a dedicated native DRM context and three host-owned
AHBs, reserves disjoint ranges from authoritative `allocation_size`, then executes
MAP/ACK. It exposes no guest CPU address, GPU write or presentation lease and
never changes scanout/capabilities. Current fully reserved8MiB BAR admits no
allocation. Legacy control slots exclude any retained dynamic suffix.

`allocation_client_test.cpp` includes this production state machine directly.
Canonical host structures independently interpret every emitted mutation;
synthetic transport responses use the actual unfenced frontend envelope
(response context0, original request context retained locally). These controls
are not additional host runtime fixtures. Tests execute normal triple allocation,
cached/WC mappings, padded sizes, old-Surface cleanup, every creation/submission/
cleanup fault point, lost replies, retained errors, malformed metadata, truncated
and unaligned input, repeated calls and transport retirement.

Only confirmed UNMAP then DESTROY authorizes mapped-owner release. A submitted
ALLOCATE error may retain an import while returning no token, so guest identity
and context remain quarantined. Reset alone proves no external-memory release:
this adapter preserves unresolved records/ranges and refuses reuse in a later
transport epoch. An adapter destruction ends its local journal; the frozen host
independently retains unproven native owners/ranges until process termination.
Reliable recovery of tokenless owners needs a future host query/cleanup contract.
Full SDR and producer/consumer synchronization remain separate work.
