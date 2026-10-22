/*
 * Probe-only translation of J3DScaleNrmMtx33's paired-single body
 * (J3DTransform.cpp:336-357). The unique symbol is intentionally unreferenced;
 * the game continues to use the portable C++ implementation.
 */
	.text
	.align 2
	.globl smg_wiiu_ps_probe_scale_nrm_mtx33
	.type smg_wiiu_ps_probe_scale_nrm_mtx33, @function
smg_wiiu_ps_probe_scale_nrm_mtx33:
	psq_l 0, 0(3), 0, 0
	psq_l 6, 0(4), 0, 0
	lfs 1, 8(3)
	lfs 7, 8(4)
	ps_mul 0, 0, 6
	psq_l 2, 12(3), 0, 0
	fmuls 1, 1, 7
	lfs 3, 0x14(3)
	ps_mul 2, 2, 6
	psq_l 4, 24(3), 0, 0
	fmuls 3, 3, 7
	lfs 5, 0x20(3)
	ps_mul 4, 4, 6
	psq_st 0, 0(3), 0, 0
	fmuls 5, 5, 7
	stfs 1, 8(3)
	psq_st 2, 12(3), 0, 0
	stfs 3, 0x14(3)
	psq_st 4, 24(3), 0, 0
	stfs 5, 0x20(3)
	blr
	.size smg_wiiu_ps_probe_scale_nrm_mtx33, .-smg_wiiu_ps_probe_scale_nrm_mtx33
