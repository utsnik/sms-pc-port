/* Minimal GCC/Gekko paired-single probe. This is intentionally link-unused;
 * Stage 1 keeps the portable fpu-01 implementation for the game. */
	.text
	.align 2
	.globl sms_wiiu_ps_probe
	.type sms_wiiu_ps_probe, @function
sms_wiiu_ps_probe:
	psq_l %f0, 0(%r3), 1, 0
	psq_l %f1, 4(%r3), 0, 0
	ps_add %f2, %f0, %f1
	psq_st %f2, 0(%r4), 0, 0
	blr
	.size sms_wiiu_ps_probe, .-sms_wiiu_ps_probe
