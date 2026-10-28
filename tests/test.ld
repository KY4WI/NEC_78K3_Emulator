/* test.ld -- linker script for standalone emu78k3 compiler-regression
 * test binaries. Not a hardware image: no fixed mask-ROM addresses, no
 * vector table, no KENWOOD signature, no required output size. Just a
 * flat load-at-0 image emu78k3 loads with --base 0 (the default).
 *
 * .text/.data (code, rodata, initialized globals -- see the firmware
 * linker scripts' comment on why an initialized global is read-only on
 * this architecture) land at 0x0000 up. .bss (uninitialized globals)
 * gets its own RAM region well clear of both the code (test programs
 * are small) and the stack (SP starts at 0xFE00 and grows down, per
 * test_crt0.s) and the register-bank storage window (FE80h-FEFFh).
 */

MEMORY {
  text_region (rx) : ORIGIN = 0x0000, LENGTH = 0x1000
  bss_region  (rw) : ORIGIN = 0x1000, LENGTH = 0x1000
}

SECTIONS {
  text 0x0000 : { *(.text) *(.data) }   >text_region
  bss  0x1000 (NOLOAD) : { *(.bss) }    >bss_region
}
