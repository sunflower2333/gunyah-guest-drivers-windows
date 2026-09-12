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

Subsequent allocation client is a separate change: after validated discovery,
create a dedicated native DRM diagnostic context, reserve a disjoint aligned
range using authoritative returned allocation_size, issue ALLOCATE/MAP/ACK,
then terminal UNMAP/DESTROY with exact tokens and cleanup receipts. Current
fully reserved8MiB BAR provides no eligible range. That client must never use
the active rendering context, infer a write lease from ACK, or alter display
output before the missing producer/consumer contract is implemented.
