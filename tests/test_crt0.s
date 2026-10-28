; Minimal crt0 for standalone compiler-regression tests run under
; emu78k3 -- NOT hardware firmware. No vector table, no CALLT table, no
; resident-ROM trampolines, no watchdog: emu78k3 never generates an
; interrupt and never times out a watchdog, so none of that machinery
; has anything to do here. Just enough to get from a cold PC to _main()
; with a valid stack, the same way any C runtime's _start does.
;
; SP is set via the project's own established idiom (sfr[0xfc].w, see
; firmware/*/crt0.s) rather than the "movw sp,#..." mnemonic, since
; that's the form already confirmed to assemble/decode correctly.
;
; After _main() returns (test programs are plain "int main(void)" or
; "void main(void)" that fall off the end, they don't loop forever
; themselves), fall into an unconditional self-branch -- emu78k3
; recognizes "br ." as the clean "program has reached steady state, stop
; and check final state" signal (see emu78k3.c's self-loop detection).

	.global	_main

	.section .text
_start:
	movw	sfr[0xfc].w,#0xfe00
	call	_main
halt:
	br	halt
