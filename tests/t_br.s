	.global _start
	.text
_start:
	nop
target:
	nop
	br	target
	nop
	br	forward
forward:
	nop
loop:
	br	loop
