/* Version 2 canonical DRM/MSM native-context wire manifest. */

ABI_VALUE(protocol.capset_drm, ABI_CAPSET_ID, 6);
ABI_VALUE(protocol.context_msm, VIRTGPU_DRM_CONTEXT_MSM, 1);
ABI_VALUE(protocol.wire_format_version, VIRTGPU_DRM_WIRE_FORMAT_VERSION, 2);
ABI_VALUE(protocol.context_init_capset_mask, VIRTIO_GPU_CONTEXT_INIT_CAPSET_ID_MASK, 255);

ABI_VALUE(virtio.cmd.get_capset_info, VIRTIO_GPU_CMD_GET_CAPSET_INFO, 264);
ABI_VALUE(virtio.cmd.get_capset, VIRTIO_GPU_CMD_GET_CAPSET, 265);
ABI_VALUE(virtio.cmd.resource_create_blob, VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB, 268);
ABI_VALUE(virtio.cmd.ctx_create, VIRTIO_GPU_CMD_CTX_CREATE, 512);
ABI_VALUE(virtio.cmd.ctx_destroy, VIRTIO_GPU_CMD_CTX_DESTROY, 513);
ABI_VALUE(virtio.cmd.ctx_attach_resource, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, 514);
ABI_VALUE(virtio.cmd.ctx_detach_resource, VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE, 515);
ABI_VALUE(virtio.cmd.submit_3d, VIRTIO_GPU_CMD_SUBMIT_3D, 519);
ABI_VALUE(virtio.resp.ok_nodata, VIRTIO_GPU_RESP_OK_NODATA, 4352);
ABI_VALUE(virtio.resp.ok_capset_info, VIRTIO_GPU_RESP_OK_CAPSET_INFO, 4354);
ABI_VALUE(virtio.resp.ok_capset, VIRTIO_GPU_RESP_OK_CAPSET, 4355);
ABI_VALUE(virtio.resp.invalid_context_id, VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID, 4612);
ABI_VALUE(virtio.feature.resource_blob, VIRTIO_GPU_F_RESOURCE_BLOB, 3);
ABI_VALUE(virtio.feature.context_init, VIRTIO_GPU_F_CONTEXT_INIT, 4);
ABI_VALUE(virtio.flag.fence, VIRTIO_GPU_FLAG_FENCE, 1);
ABI_VALUE(virtio.flag.info_ring_idx, VIRTIO_GPU_FLAG_INFO_RING_IDX, 2);
ABI_VALUE(virtio.blob_mem.guest, VIRTIO_GPU_BLOB_MEM_GUEST, 1);
ABI_VALUE(virtio.blob_flag.mappable, VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE, 1);

ABI_VALUE(msm.flag.guest_alloc, MSM_BO_GUEST_ALLOC, 2147483648ULL);
ABI_VALUE(msm.ccmd.nop, MSM_CCMD_NOP, 1);
ABI_VALUE(msm.ccmd.ioctl_simple, MSM_CCMD_IOCTL_SIMPLE, 2);
ABI_VALUE(msm.ccmd.gem_new, MSM_CCMD_GEM_NEW, 3);
ABI_VALUE(msm.ccmd.gem_set_iova, MSM_CCMD_GEM_SET_IOVA, 4);
ABI_VALUE(msm.ccmd.gem_cpu_prep, MSM_CCMD_GEM_CPU_PREP, 5);
ABI_VALUE(msm.ccmd.gem_set_name, MSM_CCMD_GEM_SET_NAME, 6);
ABI_VALUE(msm.ccmd.gem_submit, MSM_CCMD_GEM_SUBMIT, 7);
ABI_VALUE(msm.ccmd.gem_upload, MSM_CCMD_GEM_UPLOAD, 8);
ABI_VALUE(msm.ccmd.submitqueue_query, MSM_CCMD_SUBMITQUEUE_QUERY, 9);
ABI_VALUE(msm.ccmd.wait_fence, MSM_CCMD_WAIT_FENCE, 10);
ABI_VALUE(msm.ccmd.set_debuginfo, MSM_CCMD_SET_DEBUGINFO, 11);
ABI_VALUE(msm.ccmd.last, MSM_CCMD_LAST, 12);

ABI_EXPR(size.capset_drm_msm, sizeof(((ABI_CAPSET *)0)->ABI_CAPSET_MSM), 88);
ABI_OFFSET(offset.capset_drm.wire_format_version, ABI_CAPSET, wire_format_version, 0);
ABI_OFFSET(offset.capset_drm.version_major, ABI_CAPSET, version_major, 4);
ABI_OFFSET(offset.capset_drm.version_minor, ABI_CAPSET, version_minor, 8);
ABI_OFFSET(offset.capset_drm.version_patchlevel, ABI_CAPSET, version_patchlevel, 12);
ABI_OFFSET(offset.capset_drm.context_type, ABI_CAPSET, context_type, 16);
ABI_OFFSET(offset.capset_drm.padding, ABI_CAPSET, ABI_CAPSET_PADDING, 20);
ABI_OFFSET(offset.capset_drm.msm, ABI_CAPSET, ABI_CAPSET_MSM, 24);
ABI_OFFSET(offset.capset_drm.msm.has_cached_coherent,
           ABI_CAPSET,
           ABI_CAPSET_MSM.has_cached_coherent,
           24);
ABI_OFFSET(offset.capset_drm.msm.priorities, ABI_CAPSET, ABI_CAPSET_MSM.priorities, 28);
ABI_OFFSET(offset.capset_drm.msm.va_start, ABI_CAPSET, ABI_CAPSET_MSM.va_start, 32);
ABI_OFFSET(offset.capset_drm.msm.va_size, ABI_CAPSET, ABI_CAPSET_MSM.va_size, 40);
ABI_OFFSET(offset.capset_drm.msm.gpu_id, ABI_CAPSET, ABI_CAPSET_MSM.gpu_id, 48);
ABI_OFFSET(offset.capset_drm.msm.gmem_size, ABI_CAPSET, ABI_CAPSET_MSM.gmem_size, 52);
ABI_OFFSET(offset.capset_drm.msm.gmem_base, ABI_CAPSET, ABI_CAPSET_MSM.gmem_base, 56);
ABI_OFFSET(offset.capset_drm.msm.chip_id, ABI_CAPSET, ABI_CAPSET_MSM.chip_id, 64);
ABI_OFFSET(offset.capset_drm.msm.max_freq, ABI_CAPSET, ABI_CAPSET_MSM.max_freq, 72);
ABI_OFFSET(offset.capset_drm.msm.highest_bank_bit,
           ABI_CAPSET,
           ABI_CAPSET_MSM.highest_bank_bit,
           76);
ABI_OFFSET(offset.capset_drm.msm.ubwc_swizzle, ABI_CAPSET, ABI_CAPSET_MSM.ubwc_swizzle, 80);
ABI_OFFSET(offset.capset_drm.msm.macrotile_mode,
           ABI_CAPSET,
           ABI_CAPSET_MSM.macrotile_mode,
           88);
ABI_OFFSET(offset.capset_drm.msm.has_raytracing,
           ABI_CAPSET,
           ABI_CAPSET_MSM.has_raytracing,
           96);
ABI_OFFSET(offset.capset_drm.msm.has_preemption,
           ABI_CAPSET,
           ABI_CAPSET_MSM.has_preemption,
           100);
ABI_OFFSET(offset.capset_drm.msm.uche_trap_base,
           ABI_CAPSET,
           ABI_CAPSET_MSM.uche_trap_base,
           104);
ABI_EXPR(extent.capset_drm_msm,
         offsetof(ABI_CAPSET, ABI_CAPSET_MSM) + sizeof(((ABI_CAPSET *)0)->ABI_CAPSET_MSM),
         112);

ABI_SIZE(size.vdrm_shmem, struct vdrm_shmem, 8);
ABI_OFFSET(offset.vdrm_shmem.seqno, struct vdrm_shmem, seqno, 0);
ABI_OFFSET(offset.vdrm_shmem.rsp_mem_offset, struct vdrm_shmem, rsp_mem_offset, 4);
ABI_SIZE(size.msm_shmem, struct msm_shmem, 16);
ABI_OFFSET(offset.msm_shmem.base, struct msm_shmem, base, 0);
ABI_OFFSET(offset.msm_shmem.async_error, struct msm_shmem, async_error, 8);
ABI_OFFSET(offset.msm_shmem.global_faults, struct msm_shmem, global_faults, 12);

ABI_SIZE(size.vdrm_ccmd_req, struct vdrm_ccmd_req, 16);
ABI_OFFSET(offset.vdrm_ccmd_req.cmd, struct vdrm_ccmd_req, cmd, 0);
ABI_OFFSET(offset.vdrm_ccmd_req.len, struct vdrm_ccmd_req, len, 4);
ABI_OFFSET(offset.vdrm_ccmd_req.seqno, struct vdrm_ccmd_req, seqno, 8);
ABI_OFFSET(offset.vdrm_ccmd_req.rsp_off, struct vdrm_ccmd_req, rsp_off, 12);
ABI_SIZE(size.vdrm_ccmd_rsp, struct vdrm_ccmd_rsp, 4);
ABI_OFFSET(offset.vdrm_ccmd_rsp.len, struct vdrm_ccmd_rsp, len, 0);

ABI_SIZE(size.msm_ccmd_nop_req, struct msm_ccmd_nop_req, 16);
ABI_OFFSET(offset.msm_ccmd_nop_req.hdr, struct msm_ccmd_nop_req, hdr, 0);

ABI_SIZE(size.msm_ccmd_ioctl_simple_req, struct msm_ccmd_ioctl_simple_req, 20);
ABI_OFFSET(offset.msm_ccmd_ioctl_simple_req.hdr, struct msm_ccmd_ioctl_simple_req, hdr, 0);
ABI_OFFSET(offset.msm_ccmd_ioctl_simple_req.cmd, struct msm_ccmd_ioctl_simple_req, cmd, 16);
ABI_OFFSET(offset.msm_ccmd_ioctl_simple_req.payload,
           struct msm_ccmd_ioctl_simple_req,
           payload,
           20);
ABI_SIZE(size.msm_ccmd_ioctl_simple_rsp, struct msm_ccmd_ioctl_simple_rsp, 8);
ABI_OFFSET(offset.msm_ccmd_ioctl_simple_rsp.hdr, struct msm_ccmd_ioctl_simple_rsp, hdr, 0);
ABI_OFFSET(offset.msm_ccmd_ioctl_simple_rsp.ret, struct msm_ccmd_ioctl_simple_rsp, ret, 4);
ABI_OFFSET(offset.msm_ccmd_ioctl_simple_rsp.payload,
           struct msm_ccmd_ioctl_simple_rsp,
           payload,
           8);

ABI_SIZE(size.msm_ccmd_gem_new_req, struct msm_ccmd_gem_new_req, 40);
ABI_OFFSET(offset.msm_ccmd_gem_new_req.hdr, struct msm_ccmd_gem_new_req, hdr, 0);
ABI_OFFSET(offset.msm_ccmd_gem_new_req.iova, struct msm_ccmd_gem_new_req, iova, 16);
ABI_OFFSET(offset.msm_ccmd_gem_new_req.size, struct msm_ccmd_gem_new_req, size, 24);
ABI_OFFSET(offset.msm_ccmd_gem_new_req.flags, struct msm_ccmd_gem_new_req, flags, 32);
ABI_OFFSET(offset.msm_ccmd_gem_new_req.blob_id, struct msm_ccmd_gem_new_req, blob_id, 36);
ABI_SIZE(size.msm_gem_new_run, struct msm_gem_new_run, 16);
ABI_OFFSET(offset.msm_gem_new_run.arena_off, struct msm_gem_new_run, arena_off, 0);
ABI_OFFSET(offset.msm_gem_new_run.len, struct msm_gem_new_run, len, 8);
ABI_SIZE(size.msm_ccmd_gem_new_run_list, struct msm_ccmd_gem_new_run_list, 8);
ABI_OFFSET(offset.msm_ccmd_gem_new_run_list.nr_runs,
           struct msm_ccmd_gem_new_run_list,
           nr_runs,
           0);
ABI_OFFSET(offset.msm_ccmd_gem_new_run_list.pad,
           struct msm_ccmd_gem_new_run_list,
           pad,
           4);
ABI_OFFSET(offset.msm_ccmd_gem_new_run_list.runs,
           struct msm_ccmd_gem_new_run_list,
           runs,
           8);

ABI_SIZE(size.msm_ccmd_gem_set_iova_req, struct msm_ccmd_gem_set_iova_req, 32);
ABI_OFFSET(offset.msm_ccmd_gem_set_iova_req.hdr, struct msm_ccmd_gem_set_iova_req, hdr, 0);
ABI_OFFSET(offset.msm_ccmd_gem_set_iova_req.iova, struct msm_ccmd_gem_set_iova_req, iova, 16);
ABI_OFFSET(offset.msm_ccmd_gem_set_iova_req.res_id,
           struct msm_ccmd_gem_set_iova_req,
           res_id,
           24);

ABI_SIZE(size.msm_ccmd_gem_cpu_prep_req, struct msm_ccmd_gem_cpu_prep_req, 24);
ABI_OFFSET(offset.msm_ccmd_gem_cpu_prep_req.hdr, struct msm_ccmd_gem_cpu_prep_req, hdr, 0);
ABI_OFFSET(offset.msm_ccmd_gem_cpu_prep_req.res_id,
           struct msm_ccmd_gem_cpu_prep_req,
           res_id,
           16);
ABI_OFFSET(offset.msm_ccmd_gem_cpu_prep_req.op, struct msm_ccmd_gem_cpu_prep_req, op, 20);
ABI_SIZE(size.msm_ccmd_gem_cpu_prep_rsp, struct msm_ccmd_gem_cpu_prep_rsp, 8);
ABI_OFFSET(offset.msm_ccmd_gem_cpu_prep_rsp.hdr, struct msm_ccmd_gem_cpu_prep_rsp, hdr, 0);
ABI_OFFSET(offset.msm_ccmd_gem_cpu_prep_rsp.ret, struct msm_ccmd_gem_cpu_prep_rsp, ret, 4);

ABI_SIZE(size.msm_ccmd_gem_set_name_req, struct msm_ccmd_gem_set_name_req, 24);
ABI_OFFSET(offset.msm_ccmd_gem_set_name_req.hdr, struct msm_ccmd_gem_set_name_req, hdr, 0);
ABI_OFFSET(offset.msm_ccmd_gem_set_name_req.res_id,
           struct msm_ccmd_gem_set_name_req,
           res_id,
           16);
ABI_OFFSET(offset.msm_ccmd_gem_set_name_req.len, struct msm_ccmd_gem_set_name_req, len, 20);
ABI_OFFSET(offset.msm_ccmd_gem_set_name_req.payload,
           struct msm_ccmd_gem_set_name_req,
           payload,
           24);

ABI_SIZE(size.msm_ccmd_gem_submit_req, struct msm_ccmd_gem_submit_req, 36);
ABI_OFFSET(offset.msm_ccmd_gem_submit_req.hdr, struct msm_ccmd_gem_submit_req, hdr, 0);
ABI_OFFSET(offset.msm_ccmd_gem_submit_req.flags, struct msm_ccmd_gem_submit_req, flags, 16);
ABI_OFFSET(offset.msm_ccmd_gem_submit_req.queue_id,
           struct msm_ccmd_gem_submit_req,
           queue_id,
           20);
ABI_OFFSET(offset.msm_ccmd_gem_submit_req.nr_bos, struct msm_ccmd_gem_submit_req, nr_bos, 24);
ABI_OFFSET(offset.msm_ccmd_gem_submit_req.nr_cmds,
           struct msm_ccmd_gem_submit_req,
           nr_cmds,
           28);
ABI_OFFSET(offset.msm_ccmd_gem_submit_req.fence, struct msm_ccmd_gem_submit_req, fence, 32);
ABI_OFFSET(offset.msm_ccmd_gem_submit_req.payload,
           struct msm_ccmd_gem_submit_req,
           payload,
           36);

ABI_SIZE(size.msm_ccmd_gem_upload_req, struct msm_ccmd_gem_upload_req, 32);
ABI_OFFSET(offset.msm_ccmd_gem_upload_req.hdr, struct msm_ccmd_gem_upload_req, hdr, 0);
ABI_OFFSET(offset.msm_ccmd_gem_upload_req.res_id, struct msm_ccmd_gem_upload_req, res_id, 16);
ABI_OFFSET(offset.msm_ccmd_gem_upload_req.pad, struct msm_ccmd_gem_upload_req, pad, 20);
ABI_OFFSET(offset.msm_ccmd_gem_upload_req.off, struct msm_ccmd_gem_upload_req, off, 24);
ABI_OFFSET(offset.msm_ccmd_gem_upload_req.len, struct msm_ccmd_gem_upload_req, len, 28);
ABI_OFFSET(offset.msm_ccmd_gem_upload_req.payload,
           struct msm_ccmd_gem_upload_req,
           payload,
           32);

ABI_SIZE(size.msm_ccmd_submitqueue_query_req, struct msm_ccmd_submitqueue_query_req, 28);
ABI_OFFSET(offset.msm_ccmd_submitqueue_query_req.hdr,
           struct msm_ccmd_submitqueue_query_req,
           hdr,
           0);
ABI_OFFSET(offset.msm_ccmd_submitqueue_query_req.queue_id,
           struct msm_ccmd_submitqueue_query_req,
           queue_id,
           16);
ABI_OFFSET(offset.msm_ccmd_submitqueue_query_req.param,
           struct msm_ccmd_submitqueue_query_req,
           param,
           20);
ABI_OFFSET(offset.msm_ccmd_submitqueue_query_req.len,
           struct msm_ccmd_submitqueue_query_req,
           len,
           24);
ABI_SIZE(size.msm_ccmd_submitqueue_query_rsp, struct msm_ccmd_submitqueue_query_rsp, 12);
ABI_OFFSET(offset.msm_ccmd_submitqueue_query_rsp.hdr,
           struct msm_ccmd_submitqueue_query_rsp,
           hdr,
           0);
ABI_OFFSET(offset.msm_ccmd_submitqueue_query_rsp.ret,
           struct msm_ccmd_submitqueue_query_rsp,
           ret,
           4);
ABI_OFFSET(offset.msm_ccmd_submitqueue_query_rsp.out_len,
           struct msm_ccmd_submitqueue_query_rsp,
           out_len,
           8);
ABI_OFFSET(offset.msm_ccmd_submitqueue_query_rsp.payload,
           struct msm_ccmd_submitqueue_query_rsp,
           payload,
           12);

ABI_SIZE(size.msm_ccmd_wait_fence_req, struct msm_ccmd_wait_fence_req, 24);
ABI_OFFSET(offset.msm_ccmd_wait_fence_req.hdr, struct msm_ccmd_wait_fence_req, hdr, 0);
ABI_OFFSET(offset.msm_ccmd_wait_fence_req.queue_id,
           struct msm_ccmd_wait_fence_req,
           queue_id,
           16);
ABI_OFFSET(offset.msm_ccmd_wait_fence_req.fence,
           struct msm_ccmd_wait_fence_req,
           fence,
           20);
ABI_SIZE(size.msm_ccmd_wait_fence_rsp, struct msm_ccmd_wait_fence_rsp, 8);
ABI_OFFSET(offset.msm_ccmd_wait_fence_rsp.hdr, struct msm_ccmd_wait_fence_rsp, hdr, 0);
ABI_OFFSET(offset.msm_ccmd_wait_fence_rsp.ret, struct msm_ccmd_wait_fence_rsp, ret, 4);

ABI_SIZE(size.msm_ccmd_set_debuginfo_req, struct msm_ccmd_set_debuginfo_req, 24);
ABI_OFFSET(offset.msm_ccmd_set_debuginfo_req.hdr,
           struct msm_ccmd_set_debuginfo_req,
           hdr,
           0);
ABI_OFFSET(offset.msm_ccmd_set_debuginfo_req.comm_len,
           struct msm_ccmd_set_debuginfo_req,
           comm_len,
           16);
ABI_OFFSET(offset.msm_ccmd_set_debuginfo_req.cmdline_len,
           struct msm_ccmd_set_debuginfo_req,
           cmdline_len,
           20);
ABI_OFFSET(offset.msm_ccmd_set_debuginfo_req.payload,
           struct msm_ccmd_set_debuginfo_req,
           payload,
           24);
