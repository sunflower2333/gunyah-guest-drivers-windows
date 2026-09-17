# Zero-copy shared surfaces and scanout (design, 2026-09-17)

Goal: at a 165 Hz guest mode the Android native display shows every frame
DWM composes, and DWM composes every frame TestUFO (Edge) renders, so the
two frame rates match.

## Why the copies must go

With the DWM flip model enabled (58507+), and with the vsync-republish latch
(58509) and wait-free refresh upload (58510) in place, the measured ratio of
Android fps to guest fps is:

| guest mode | ratio |
|---|---|
| 60 Hz | ~0.95 |
| 165 Hz | ~0.5 |

At 165 Hz DWM needs 10-13 ms per frame (PresentMon `msUntilRenderStart`
-8..-14 ms, Present 3 ms), which is two to three 6.06 ms vblanks. Most of
that is CPU copying between D3D allocations and zink's GPU textures:

| cost per frame | where |
|---|---|
| 2 x refresh of Edge's shared surfaces | DWM: KMD scheduled copy allocation->staging, LockCb wait, memcpy, upload |
| publish of DWM's primary | DWM: GPU wait, READ map, memcpy, staging->primary scheduled copy |
| publish of Edge's swapchain buffer | Edge: GPU wait, READ map, memcpy |

The only per-frame work that should remain is DWM's GPU composition.

## Key facts (verified in source)

- Turnip BOs on WDDM are KMD allocations flagged `VIOGPU_WDDM_ALLOCATION_NATIVE`,
  bound to one native context (`ContextId`) at a guest-chosen IOVA.
  - At aperture map the KMD sends `MSM_CCMD_GEM_NEW(MSM_BO_GUEST_ALLOC, blob_id, iova)`
    followed by `RESOURCE_CREATE_BLOB` over the allocation's guest pages.
  - drm2kgsl imports crosvm's udmabuf of those pages (`dmabuf_blob` exports).
- drm2kgsl `attach_resource` imports such a resource into another context via
  `virgl_resource_export_fd` -> `kgsl_import_dmabuf`. The importer then binds it
  with `GEM_SET_IOVA`. **No host change is needed for cross-context sharing.**
- crosvm scans out a guest-alloc blob with no CPU copy: `pool_scanout_dmabuf`
  plus `SET_SCANOUT_BLOB` geometry, the "drm2kgsl 1-gpu-copy" path.
  **No host change is needed for native scanout.**
- The KMD has no sender for `VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE` yet.
  `OpenAllocation` only references the allocation.
- Turnip WDDM rejects `TU_BO_ALLOC_SHAREABLE`, and `bo_init_dmabuf` returns
  `FEATURE_NOT_PRESENT`.
- zink already imports `VK_KHR_external_memory_win32`, but it
  `DuplicateHandle`s the value, which only works for NT handles.
- The D3D UMD's Vulkan driver is **not** built from the submodule. It is
  `viogpu_gl_vk_<arch>.dll` from the pinned OpenGL payload (Mesa run
  34757244563). A Turnip change needs that payload rebuilt and repinned.

## Design

### Shared pixel memory belongs to the D3D allocation

The allocation the D3D runtime shares across processes must *be* the pixel
memory. A Turnip BO owned by the producer would die with the producer's
texture while DWM still samples it.

- The UMD creates shareable textures, and DWM's flip-chain primaries, as
  runtime-device allocations flagged `NATIVE | SHARED`, with:
  - a UMD-generated random 64-bit `ShareKey`;
  - a linear surface layout (`Pitch` = the Adreno linear row pitch).
- These allocations have no `ContextId`. The KMD owns them in one internal
  *sharing context* per adapter, created at native transport start. It picks
  their IOVA from that context's heap, so the host object outlives any process.

### Import primitive (KMD escape, context scoped)

`VIOGPU_WDDM_ESCAPE_IMPORT_SHARED { ShareKey, Iova, Size }`, issued on a
Turnip native context:

1. Look up the allocation by `ShareKey`.
2. Require that the calling process created or opened it. `OpenAllocation`
   and `CreateAllocation` record the process.
3. Send `CTX_ATTACH_RESOURCE(caller ctx, res_id)`, then
   `GEM_SET_IOVA(caller ctx, res_id, Iova)`.
4. Record the import on the caller's context and take a reference on the
   allocation.

`VIOGPU_WDDM_ESCAPE_RELEASE_SHARED` undoes this. So do context destroy and
allocation destroy: the allocation force-detaches every remaining import
before its host resource is released.

Imported BOs are not in Turnip's DMA allocation list. The host reaches them
through the importing context's VBO. The owning allocation stays resident
because its creator and openers hold it resident.

### Turnip

- Expose `VK_KHR_external_memory_win32` with import-only
  `VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT`. The handle value is the
  `ShareKey`.
- `bo_init_import` allocates a VMA range and issues `IMPORT_SHARED`. The BO has
  no WDDM allocation: it is not mappable, is excluded from submit lists, and
  frees with `RELEASE_SHARED`.

### zink

- Import `OPAQUE_WIN32_KMT` handles as values: no `DuplicateHandle`, no
  `CloseHandle`.

### d3d10umd

- Shared textures and flip-chain primaries use the native shared allocation,
  and a zink texture imported from its `ShareKey`. The texture is linear, with
  the same create parameters on both sides.
- Opened resources import the `ShareKey` taken from the open private data.
- These resources skip publish and refresh. Before a Present that hands such a
  buffer to another process or to scanout, the UMD waits for its GPU work.
- If import fails (old KMD or payload), the existing copy path is used.

### Scanout

- `BindStandardPrimaryScanout` accepts a native shared primary. It sends
  `SET_SCANOUT_BLOB(res_id, width, height, stride, offset 0)` plus flush, and
  crosvm imports the pool udmabuf.

## Phases and acceptance

1. **KMD** sharing context, native shared allocations, `IMPORT_SHARED`/`RELEASE_SHARED`,
   open-process records, cleanup.
   - Acceptance: a two-`VkDevice` probe (two native contexts, one process)
     imports one allocation and reads the other device's writes.
2. **Turnip** import and **zink** KMT import; OpenGL payload rebuilt and repinned.
3. **d3d10umd** shared textures zero-copy (Edge -> DWM).
   - Acceptance: DWM `umd_timing.log` shows no refresh operations for Edge
     surfaces; TestUFO correct.
4. **Native primary scanout** (DWM publish removed).
   - Acceptance: no publish operations; Android shows frames.
5. **Measurement at 165 Hz.**
   - Acceptance: Android fps ~= TestUFO fps.

## Risks

- VidMm eviction of a shared allocation while another context imports it: the
  host import would keep reading pages VidMm reused. Openers keep it resident;
  eviction under pressure must be handled before this leaves opt-in.
- Cross-process GPU ordering: there is no implicit sync across KGSL contexts,
  so producers wait for their own fence before Present.
- The linear layout and row pitch must agree on both sides and with
  `SET_SCANOUT_BLOB`.
