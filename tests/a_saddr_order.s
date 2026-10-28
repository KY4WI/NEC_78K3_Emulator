; Two-saddr forms (MOV saddr,saddr' and friends): the FIRST operand after
; the mnemonic is the destination and sits in the instruction's THIRD
; byte; the SECOND operand is the source and sits in the SECOND byte
; (see Opcodes.h's comment above the MOV/XCH/MOVW/... OP_SADDR_3,
; OP_SADDR_FE_2 entries, and Addressing.h's addr_from_saddr_fe_byte).
; This is a real-world regression, not a hypothetical: an earlier
; version of this project's shared opcode/addressing tables had the
; destination and source byte positions swapped for every one of these
; 15 instruction forms (MOV/XCH/MOVW/XCHW/ADD/ADDC/SUB/SUBC/AND/OR/XOR/
; CMP/ADDW/SUBW/CMPW), which went undetected until the tables were
; checked against the companion toolchain repository's canonical copy.
; vasm (the real encoder) and emu78k3 (this emulator, the independent
; decoder) share no code for this -- they only share this table -- so if
; either one's copy disagrees with the other about which byte is which,
; this test fails.
;
; FE24H starts at 0, FE28H starts at 0x42. "mov 0xfe24, 0xfe28" must
; copy source (FE28H) into destination (FE24H): FE24H -> 0x42,
; FE28H stays 0x42. Swapped, it would instead copy FE24H's 0 into
; FE28H, leaving FE24H at 0 and FE28H at 0.
; EXPECT: 42 42
	.org	0x0000
start:
	mov	0xFE24, #0x00
	mov	0xFE28, #0x42
	mov	0xFE24, 0xFE28		; dest=FE24H, src=FE28H
	mov	a, 0xFE24
	mov	!0x1000, a		; expect 42 (copied)
	mov	a, 0xFE28
	mov	!0x1001, a		; expect 42 (source unchanged)
done:	br	done
