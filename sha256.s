.set vpm_write, function f0(p) { return (p.ADDR << 0 | p.SIZE << 8 | p.LANED << 10 | p.HORIZ << 11 | p.STRIDE << 12) ; } f0
.set vpm_read, function f1(p) { return (p.ADDR << 0 | p.SIZE << 8 | p.LANED << 10 | p.HORIZ << 11 | p.STRIDE << 12 | p.NUM << 20) ; } f1
.set vpm_store, function f2(p) { return (p.MODEW << 0 | p.VPMBASE << 3 | p.HORIZ << 14 | p.DEPTH << 16 | p.UNITS << 23 | 2 << 30) ; } f2
.set vpm_store_stride, function f3(p) { return (p.STRIDE << 0 | p.BLOCKMODE << 16 | 3 << 30) ; } f3
.set vpm_load, function f4(p) { return (p.ADDRXY << 0 | p.VERT << 11 | p.VPITCH << 12 | p.NROWS << 16 | p.ROWLEN << 20 | p.MPITCH << 24 | p.MODEW << 28 | 1 << 31) ; } f4
.set vpm_load_stride, function f5(p) { return (p.MPITCHB << 0 | 9 << 28) ; } f5

.global entry
.global exit

entry:
	# ra0 <- VPM setup start address
	# [5:4] <- Y[5:4] (qpu / 4)
	# [3:0] <- X[3:0] (4 * (qpu % 4))
	mov r1, qpu_num
	shr r0, r1, 2
	shl r0, r0, 4
	and r1, r1, 3
	shl r1, r1, 2
	or ra0, r0, r1

	# rb0 <- DMA setup start address
	# [9:4] <- Y[5:0] (16 * (qpu / 4))
	# [3:0] <- X[3:0] (4 * (qpu % 4))
	shl r0, r0, 4
	or rb0, r0, r1

	# r0, rb2 <- address of this QPU's message chunks (M or W)
	mov r0, unif
	mov rb2, r0
	# ra1 <- QPU message chunk address increment
	ldi ra1, 16
	# rb1 <- QPU schedule end address
	ldi r1, 256
	add rb1, r0, r1

	# DMA load M[0:3]
	ldi r1, vpm_load({VERT: 0, VPITCH: 1, NROWS: 0, ROWLEN: 4, MPITCH: 5})
	or r1, r1, rb0
	mov vr_setup, r1
	mov vr_addr, r0
	mov.never -, vr_wait

	# VPM read M[0:3]
	ldi r2, vpm_read({HORIZ: 0, STRIDE: 1, NUM: 4, SIZE: 2})
	or r2, r2, ra0
	mov vr_setup, r2
	mov.never -, vr_wait
	mov ra24, vpm
	mov rb24, vpm
	mov ra25, vpm
	mov rb25, vpm

	# DMA load M[4:7]
	add r0, r0, ra1		# r0 += 16
	mov vr_setup, r1
	mov vr_addr, r0
	mov.never -, vr_wait

	# VPM read M[4:7]
	mov vr_setup, r2
	mov.never -, vr_wait
	mov ra26, vpm
	mov rb26, vpm
	mov ra27, vpm
	mov rb27, vpm

	# DMA load M[8:11]
	add r0, r0, ra1		# r0 += 16
	mov vr_setup, r1
	mov vr_addr, r0
	mov.never -, vr_wait

	# VPM read M[8:11]
	mov vr_setup, r2
	mov.never -, vr_wait
	mov ra28, vpm
	mov rb28, vpm
	mov ra29, vpm
	mov rb29, vpm

	# DMA load M[12:15]
	add r0, r0, ra1		# r0 += 16
	mov vr_setup, r1
	mov vr_addr, r0
	mov.never -, vr_wait

	# VPM read M[12:15]
	mov vr_setup, r2
	mov.never -, vr_wait
	mov ra30, vpm
	mov rb30, vpm
	mov ra31, vpm
	mov rb31, vpm

	add r0, r0, ra1		# r0 += 16

	# Setup VPM write for W[16:19]
	ldi r1, vpm_write({HORIZ: 0, STRIDE: 1, SIZE: 2})
	or r1, r1, ra0
	mov vw_setup, r1

	# Place some big immediates in registers now, to save cycles later
	ldi rb17, 17
	ldi rb18, 18
	ldi rb19, 19

	# Setup DMA store stride
	ldi vw_setup, vpm_store_stride({STRIDE: 240})

w_init_loop:
	# s0 := (w[i-15] rightrotate 7) xor (w[i-15] rightrotate 18) xor (w[i-15] rightshift 3)
	mov r3, rb24
	ror r1, r3, 7
	ror r2, r3, rb18
	xor r1, r1, r2
	shr r2, r3, 3
	xor ra2, r1, r2		# ra2 <- s0

	# s1 := (w[i-2] rightrotate 17) xor (w[i-2] rightrotate 19) xor (w[i-2] rightshift 10)
	ror r1, ra31, rb17
	ror r2, ra31, rb19
	xor r1, r1, r2
	shr r2, ra31, 10
	xor rb3, r1, r2		# rb3 <- s1

	# w[i] := w[i-16] + s0 + w[i-7] + s1
	add r1, ra24, rb28	# r1 <- w[i-16] + w[i-7]
	add r1, r1, ra2		# r1 += s0
	add r1, r1, rb3		# r1 += s1

	# shift to next iteration
	mov ra24, rb24		; mov rb24, ra25
	mov ra25, rb25		; mov rb25, ra26
	mov ra26, rb26		; mov rb26, ra27
	mov ra27, rb27		; mov rb27, ra28
	mov ra28, rb28 		; mov rb28, ra29
	mov ra29, rb29		; mov rb29, ra30
	mov ra30, rb30		; mov rb30, ra31
	mov ra31, rb31		; mov rb31, r1

	# store W[i]
	mov vpm, r1

	# Advance schedule pointer
	add r0, r0, 4		# r0 += 4

	# Check DMA store condition
	and.setf -, r0, 15
	brr.allnz -, w_init_loop_cond
	# Not adding nops because it's safe to run next 3 instructions in both cases

	# DMA store W[?:?+3]
	ldi r1, vpm_store({HORIZ: 1, DEPTH: 4, UNITS: 16})
	mov r2, rb0
	shl r2, r2, 3
	or vw_setup, r1, r2
	sub r1, r0, ra1
	nop			; mov vw_addr, r1
	nop 			; mov.never -, vw_wait

	# Start writing from the beginning of our VPM section again
	ldi r1, vpm_write({HORIZ: 0, STRIDE: 1, SIZE: 2})
	or r1, r1, ra0
	mov vw_setup, r1

w_init_loop_cond:
	# Check loop condition
	sub.setf -, rb1, r0	# r0 == W + 256
	brr.allnz -, w_init_loop
	nop
	nop
	nop

	# end of W init loop

	# r0, rb3 <- address of this QPU's working vars (V)
	mov r0, unif
	mov rb3, r0

	# DMA load V[0:3]
	ldi r1, vpm_load({VERT: 0, VPITCH: 1, NROWS: 0, ROWLEN: 4, MPITCH: 2})
	or r1, r1, rb0
	mov vr_setup, r1
	mov vr_addr, r0
	mov.never -, vr_wait

	# VPM read V[0:3]
	ldi r2, vpm_read({HORIZ: 0, STRIDE: 1, NUM: 4, SIZE: 2})
	or r2, r2, ra0
	mov vr_setup, r2
	mov.never -, vr_wait
	mov ra24, vpm
	mov ra25, vpm
	mov ra26, vpm
	mov ra27, vpm

	# DMA load V[4:7]
	add r0, r0, ra1		# r0 += 16
	mov vr_setup, r1
	mov vr_addr, r0
	mov.never -, vr_wait

	# VPM read V[4:7]
	mov vr_setup, r2
	mov.never -, vr_wait
	mov ra28, vpm
	mov ra29, vpm
	mov ra30, vpm
	mov ra31, vpm

	# r0 <- address of this QPU's message schedules (W)
	mov r0, rb2

	# Place some big immediates in registers now, to save cycles later
	ldi rb22, 22
	ldi rb25, 25

main_loop_read:
	# DMA load W[?:?+3]
	ldi r1, vpm_load({VERT: 0, VPITCH: 1, NROWS: 0, ROWLEN: 4, MPITCH: 5})
	or r1, r1, rb0
	mov vr_setup, r1
	mov vr_addr, r0
	mov.never -, vr_wait

	# Setup VPM read W[?:?+3]
	ldi r2, vpm_read({HORIZ: 0, STRIDE: 1, NUM: 4, SIZE: 2})
	or r2, r2, ra0
	mov vr_setup, r2
	mov.never -, vr_wait

main_loop_compute:
	# S1 := (e rightrotate 6) xor (e rightrotate 11) xor (e rightrotate 25)
	# ra23 <- S1
	ror r1, ra28, 6
	ror r2, ra28, 11
	xor r1, r1, r2
	ror r2, ra28, rb25
	xor ra23, r1, r2

	# ch := (e and f) xor ((not e) and g)
	# ra22 <- ch
	mov r1, ra28
	and r1, r1, ra29
	not r2, ra28
	and r2, r2, ra30
	xor ra22, r1, r2

	# temp1 := h + S1 + ch + k[i] + w[i]
	# ra21 <- k
	mov ra21, unif
	# ra20 <- w[i]
	mov ra20, vpm
	# ra19 <- temp1
	mov r1, ra31
	add r1, r1, ra23
	add r1, r1, ra22
	add r1, r1, ra21
	add ra19, r1, ra20

	# S0 := (a rightrotate 2) xor (a rightrotate 13) xor (a rightrotate 22)
	# ra18 <- S0
	ror r1, ra24, 2
	ror r2, ra24, 13
	xor r1, r1, r2
	ror r2, ra24, rb22
	xor ra18, r1, r2

	# maj := (a and b) xor (a and c) xor (b and c)
	# ra17 <- maj
	mov r2, ra24
	and r1, r2, ra25
	and r2, r2, ra26
	xor r1, r1, r2
	mov r2, ra25
	and r2, r2, ra26
	xor ra17, r1, r2

	# temp2 := S0 + maj
	# ra16 <- temp2
	mov r1, ra18
	add ra16, r1, ra17

	# h := g
	# g := f
	# f := e
	# e := d + temp1
	# d := c
	# c := b
	# b := a
	# a := temp1 + temp2
	mov ra31, ra30
	mov ra30, ra29
	mov ra29, ra28
	mov r1, ra19
	add ra28, ra27, r1
	mov ra27, ra26
	mov ra26, ra25
	mov ra25, ra24
	add ra24, r1, ra16

	# Advance schedule pointer
	add r0, r0, 4		# r0 += 4

	# Check loop termination condition
	sub.setf -, rb1, r0	# r0 == W + 256
	brr.allz -, main_loop_end
	nop
	nop
	nop

	# Check DMA load condition
	and.setf -, r0, 15
	brr.allz -, main_loop_read
	nop
	nop
	nop

	brr -, main_loop_compute
	nop
	nop
	nop

main_loop_end:
	# end of main loop

	# Setup DMA store stride
	ldi vw_setup, vpm_store_stride({STRIDE: 16})

	# DMA load V[0:3]
	ldi r0, vpm_load({VERT: 0, VPITCH: 1, NROWS: 0, ROWLEN: 4, MPITCH: 2})
	or r0, r0, rb0
	mov vr_setup, r0
	mov vr_addr, rb3
	mov.never -, vr_wait

	# VPM read V[0:3]
	ldi r1, vpm_read({HORIZ: 0, STRIDE: 1, NUM: 4, SIZE: 2})
	or r1, r1, ra0
	mov vr_setup, r1
	mov.never -, vr_wait

	# Add V[0:3] to a through d
	add ra24, ra24, vpm
	add ra25, ra25, vpm
	add ra26, ra26, vpm
	add ra27, ra27, vpm

	# VPM write V[0:3]
	ldi r2, vpm_write({HORIZ: 0, STRIDE: 1, SIZE: 2})
	or r2, r2, ra0
	mov vw_setup, r2
	mov vpm, ra24
	mov vpm, ra25
	mov vpm, ra26
	mov vpm, ra27

	# DMA store V[0:3]
	ldi ra2, vpm_store({HORIZ: 1, DEPTH: 4, UNITS: 16})
	mov r3, rb0
	shl r3, r3, 3
	or vw_setup, ra2, r3
	nop			; mov vw_addr, rb3
	nop 			; mov.never -, vw_wait

	# DMA load V[4:7]
	add rb3, ra1, rb3	# rb3 += 16
	mov vr_setup, r0
	mov vr_addr, rb3
	mov.never -, vr_wait

	# VPM read V[4:7]
	mov vr_setup, r1
	mov.never -, vr_wait

	# Add V[4:7] to e through h
	add ra28, ra28, vpm
	add ra29, ra29, vpm
	add ra30, ra30, vpm
	add ra31, ra31, vpm

	# VPM write V[4:7]
	mov vw_setup, r2
	mov vpm, ra28
	mov vpm, ra29
	mov vpm, ra30
	mov vpm, ra31

	# DMA store V[4:7]
	or vw_setup, ra2, r3
	nop			; mov vw_addr, rb3
	nop 			; mov.never -, vw_wait

	# This QPU is done. Release the semaphore.
	srel.never -, 0

	# If we're QPU 0, wait for everyone else to release the semaphore too.
	mov.setf -, unif
	brr.allz -, qpuzero
	nop
	nop
	nop

	# If we're not QPU 0 then exit.
	nop			; nop; thrend
	nop
	nop

qpuzero:
	# Number of QPUs
	mov r0, unif
sacq_loop:
	sacq.never -, 0
	sub.setf r0, r0, 1
	brr.allnz -, sacq_loop
	nop
	nop
	nop

exit:
	mov irq, 1
	nop			; nop; thrend
	nop
	nop
