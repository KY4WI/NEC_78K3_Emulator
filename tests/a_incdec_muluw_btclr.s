; INC/DEC, INCW/DECW, MULUW and BTCLR/BFSET, written by hand the way
; Kenwood's TK-x4x firmware uses them. Results go to 1000H-100BH.
; EXPECT: 12 00 00 34 00 00 01 00 ff ff 01 01 11 00
	.org	0x0000
start:
; MULUW: AX(high), rp1(low) <- AX x rp1 (the low word goes to rp1, not DE)
	movw	ax, #0x1234
	movw	bc, #0x0100
	movw	de, #0x5555
	muluw	bc			; 1234H x 0100H = 00123400H
	movw	!0x1000, ax		; 12 00
	movw	ax, bc
	movw	!0x1002, ax		; 00 34
; INC A from FFH: result 00H, Z set, CY left alone (set before)
	set1	cy
	mov	a, #0xFF
	inc	a
	bne	fail
	bnl	fail			; CY must still be 1
	mov	!0x1004, a		; 00
; DEC saddr from 01H: 00H and Z
	mov	0xFE20, #0x01
	dec	0xFE20
	bne	fail
	mov	a, 0xFE20
	mov	!0x1005, a		; 00
; INCW/DECW: no flags change (Z stays set from the DEC above)
	movw	hl, #0x0000
	incw	hl
	bne	fail
	movw	ax, hl
	movw	!0x1006, ax		; 01 00
	movw	0xFE22, #0x0000
	decw	0xFE22
	bne	fail
	movw	ax, 0xFE22
	movw	!0x1008, ax		; ff ff
; BTCLR: taken and the bit cleared, then not taken
	mov	0xFE24, #0x08
	btclr	0xFE24.3, bt1
	br	fail
bt1:	btclr	0xFE24.3, fail
	mov	a, #0x01
	mov	!0x100A, a		; 01
; BFSET: taken and the bit set, then not taken
	mov	0xFE25, #0x10
	bfset	0xFE25.0, bf1
	br	fail
bf1:	bfset	0xFE25.0, fail
	mov	a, #0x01
	mov	!0x100B, a		; 01
	mov	a, 0xFE25
	mov	!0x100C, a		; 11
	mov	a, 0xFE24
	mov	!0x100D, a		; 00
done:	br	done
fail:	brk
