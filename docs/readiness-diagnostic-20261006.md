# DXVK target readiness diagnostics

This diagnostic continuation starts at the exact installed 58623 source
`d10f9ba608d1d03378af658c188c4f86cad7c343`. The signed SYS hash matches the
active PCI device's DriverStore binary. Exact-LUID KMT open/device creation
succeeds, but the private adapter query and typed D3D9 OpenAdapter return
`DEVICE_NOT_READY`. No GPU backend construction or rendering is established.

The old first paging refusal is UnmapStandard, status `c00000a3`, detail 0
(`NotSubmitted`). Its first reset RVA resolves to CompletePagingBufferOperation;
the submitted native GET_PARAM timeout is a different record. Neither old
record has a timestamp. Host logs contain multiple boots and cannot establish
the guest records' order.

This change adds fixed-storage, immutable records. Capture performs no wait,
allocation, registry access or additional ownership operation. Existing mutex
budgets, poison/recovery, reset escalation, destruction ledger and callback
behavior stay intact. The normal passive allocation-statistics publisher writes
the new scalars. It does not add publishing to the destruction path.

`NativeAdapterAdmissionValid=1` identifies the first adapter-channel queue
refusal. Reason codes are 1 lock order, 2 IRQL, 3 initial epoch, 4 mutex wait,
5 epoch after waiting, 6 resource ID, 7 UNMAP/UNREF command allocation and
8 unsubmitted no-data command. CallerRva, WaitStatus, EpochState and
EpochGeneration accompany the record. The native channel remains separate.
Mutex capture occurs before poisoning. Submission reason 8 is not evidence
that the host received the descriptor.

`Native2DDestroyValid=1` identifies the first standard-resource destruction
refusal. Stages are 1 arguments, 2 Dod submission rundown, 3 missing adapter,
4 ledger/generation, 5 reset reconciliation, 6 UNMAP_BLOB and 7 RESOURCE_UNREF.
Result and ResourceId accompany the copied ledger state and full resource/
adapter reset generations. ResourceState `ffffffff` and zero generations
mean the arguments or adapter were not available for a ledger observation.
Record a transport failure before any existing escalation mutates its ledger.
An acknowledged UNMAP followed by refused UNREF remains distinguishable.

`NativeResetRequestValid=1` identifies the first reset request observed during
the Dod object's lifetime. It records its CallerRva and the HardwareState
observed before this call changes the reset latch. It is separate from the
existing per-reset first/latest requester scalars and survives recovery.

Each new record and `NativeSynchronousFirstTimeout` has TimeLow/TimeHigh
from `KeQueryInterruptTime`: `(uint64_t(TimeHigh) << 32) | TimeLow`, in 100 ns
ticks. They establish capture ordering on the same guest clock. They are
observations, not an atomic system snapshot or proof of causal ordering.
A competing request can change state between observations. An unrelated
earlier queue refusal is not automatically the cause of a later destroy
failure; compare stage, caller, generation and capture times.

Publication state 0 is absent, 1 is being written and 2 is complete. Getters
return only complete records and zero output on absent/partial reads. The
capture records are retained for their owning object lifetime, while registry
values follow the existing passive publication cadence. Bind the active PCI
driver, exact SYS and current boot before interpreting them; a partially
updated or historical registry snapshot is not current event evidence.

Validation: focused production capture/destruction fixture with four semantic
negative controls, production queue wait/poison/recovery fixture and the full
Native Context contract checker. Native ARM64 KMD compile evidence is recorded
separately. This source adds diagnostics, not a readiness repair.
