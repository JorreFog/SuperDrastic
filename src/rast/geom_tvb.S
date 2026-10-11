/* geom_tvb.S: geometry_transform_vertex_block_asm (DraStic 0x9dda0) for the in-order Cortex-A55, the same bytes.
 *
 * geom_tvb(geom): the batch of geometry_add_vertex (n = +0x300 vertices: x at +0, y at +0x100, z at +0x200, s32)
 * through the clip matrix (4x4 s32 at +0x97e4, rows m0..m3: out[k] = (x*m0[k] + y*m1[k] + z*m2[k] + (m3[k] << 12))
 * >> 12, a wrapping 64-bit sum whose bits 12..43 are kept) into the clip-space vertices from index i = +0x330
 * (+0x650 + 16i: x, y, z, w) and their clip codes (+0x1290 + i, one byte: bit 0 x > w, 1 x < -w, 2 y > w, 3 y < -w,
 * 4 z > w, 5 z < -w, signed). Four vertices a step, as the original: it steps while n - 4k stays >= 0, so it
 * reads and writes one group past a multiple of 4 (n = 4: two groups), and so does this.
 * What changes is the order. The original computes each output's three products as one chain (smlal after smlal
 * on the same accumulator, 4-cycle latency each in llvm-mca's A55 model), first the low pair of lanes, then the high
 * pair, and starts every accumulator from a copy of the translation << 12 (eight 128-bit movs a step): 130 modeled
 * cycles a step. Here the eight accumulators (four outputs x two lane pairs) take their products in turn, the first
 * by smull, and the translation is added after the narrowing (shrn #12, then a 32-bit add of m3[k]: the same bits,
 * since m3[k] << 12 changes only bits 12 and up of the sum and bits 12..43 then gain m3[k] mod 2^32); the loads use
 * one offset register instead of three writebacks: 66 cycles a step (llvm-mca -mcpu=cortex-a55, 100 iterations).
 * Clobbers x1-x8, v0-v7, v16-v31 (the original: x1-x7, v0-v7, v16-v27); its caller is C (AAPCS). */
	.text
	.p2align 4
	.globl geom_tvb
	.type geom_tvb, %function
geom_tvb:
	add	x1, x0, #0x9, lsl #12
	ldr	w3, [x0, #0x330]
	add	x1, x1, #0x7e4
	ldr	w2, [x0, #0x300]
	add	x4, x0, #0x100
	add	x5, x0, #0x200
	cbz	w2, 2f
	ldp	q0, q1, [x1]
	ldp	q2, q3, [x1, #32]
	add	x6, x0, #0x650
	add	x7, x6, #0xc40
	add	x6, x6, w3, uxtw #4
	add	x7, x7, w3, uxtw
	dup	v4.4s, v3.s[0]
	dup	v5.4s, v3.s[1]
	dup	v6.4s, v3.s[2]
	dup	v7.4s, v3.s[3]
	mov	x8, #0
1:
	ldr	q16, [x0, x8]
	ldr	q17, [x4, x8]
	ldr	q18, [x5, x8]
	add	x8, x8, #16
	smull	v19.2d, v16.2s, v0.s[0]
	smull	v20.2d, v16.2s, v0.s[1]
	smull	v21.2d, v16.2s, v0.s[2]
	smull	v22.2d, v16.2s, v0.s[3]
	smull2	v28.2d, v16.4s, v0.s[0]
	smull2	v29.2d, v16.4s, v0.s[1]
	smull2	v30.2d, v16.4s, v0.s[2]
	smull2	v31.2d, v16.4s, v0.s[3]
	smlal	v19.2d, v17.2s, v1.s[0]
	smlal	v20.2d, v17.2s, v1.s[1]
	smlal	v21.2d, v17.2s, v1.s[2]
	smlal	v22.2d, v17.2s, v1.s[3]
	smlal2	v28.2d, v17.4s, v1.s[0]
	smlal2	v29.2d, v17.4s, v1.s[1]
	smlal2	v30.2d, v17.4s, v1.s[2]
	smlal2	v31.2d, v17.4s, v1.s[3]
	smlal	v19.2d, v18.2s, v2.s[0]
	smlal	v20.2d, v18.2s, v2.s[1]
	smlal	v21.2d, v18.2s, v2.s[2]
	smlal	v22.2d, v18.2s, v2.s[3]
	smlal2	v28.2d, v18.4s, v2.s[0]
	smlal2	v29.2d, v18.4s, v2.s[1]
	smlal2	v30.2d, v18.4s, v2.s[2]
	smlal2	v31.2d, v18.4s, v2.s[3]
	shrn	v23.2s, v19.2d, #12
	shrn	v24.2s, v20.2d, #12
	shrn	v25.2s, v21.2d, #12
	shrn	v26.2s, v22.2d, #12
	shrn2	v23.4s, v28.2d, #12
	shrn2	v24.4s, v29.2d, #12
	shrn2	v25.4s, v30.2d, #12
	shrn2	v26.4s, v31.2d, #12
	add	v26.4s, v26.4s, v7.4s
	add	v23.4s, v23.4s, v4.4s
	add	v24.4s, v24.4s, v5.4s
	add	v25.4s, v25.4s, v6.4s
	neg	v27.4s, v26.4s
	cmgt	v16.4s, v23.4s, v26.4s
	cmgt	v18.4s, v24.4s, v26.4s
	cmgt	v20.4s, v25.4s, v26.4s
	cmgt	v17.4s, v27.4s, v23.4s
	cmgt	v19.4s, v27.4s, v24.4s
	cmgt	v21.4s, v27.4s, v25.4s
	st4	{ v23.4s, v24.4s, v25.4s, v26.4s }, [x6], #64
	bic	v16.8h, #0xfe
	bic	v18.8h, #0xfb
	bic	v20.8h, #0xef
	bic	v17.8h, #0xfd
	bic	v19.8h, #0xf7
	bic	v21.8h, #0xdf
	orr	v16.16b, v16.16b, v17.16b
	orr	v18.16b, v18.16b, v19.16b
	orr	v20.16b, v20.16b, v21.16b
	orr	v16.16b, v16.16b, v18.16b
	orr	v16.16b, v16.16b, v20.16b
	xtn	v16.4h, v16.4s
	xtn	v16.8b, v16.8h
	st1	{ v16.s }[0], [x7], #4
	subs	w2, w2, #0x4
	b.pl	1b
2:
	ret
	.size geom_tvb, . - geom_tvb
