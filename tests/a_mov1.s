; MOV1 both ways (saddr.bit and A.bit), the way AB00's TOT Off patch uses
; it to copy CY into FE7DH bit 1. Results go to 1000H-1001H.
; EXPECT: 96 97
	.org	0x0000
start:
	mov	0xFE7D, #0x55
	set1	cy
	mov1	0xFE7D.1, cy		; 55H -> 57H
	clr1	cy
	mov1	0xFE7D.0, cy		; -> 56H
	mov1	cy, 0xFE7D.4		; CY = 1
	bnl	fail			; (BNL = branch if CY clear)
	mov1	0xFE7D.7, cy		; -> D6H
	mov1	cy, 0xFE7D.3		; CY = 0
	bl	fail			; (BL = branch if CY set)
	mov1	0xFE7D.6, cy		; -> 96H
	mov	a, 0xFE7D
	mov	!0x1000, a		; 96
	set1	cy
	mov1	a.0, cy			; 97H
	mov	!0x1001, a		; 97
done:	br	done
fail:	brk
