# emu78k3 -- 78K/III compiler regression emulator

An instruction-level interpreter for the NEC 78K/III core (uPD78312A).
This is a companion project to **NEC-78K3_Toolchain**, the 78K/III
vbcc/vasm/vlink backends, built for regression-testing that C compiler,
assembler and linker (and other 78K/III code generally) without real
hardware. This is a CPU-level test emulator, **not a hardware
simulator**: timers, peripheral behavior, interrupts, EEPROM, the radio
panel, and the PLL are not modeled.

The emulator includes local copies of instruction-definition headers
derived from the same work as the companion
[disassembler](https://github.com/KY4WI/NEC_78KIII_Disassembler) and
NEC-78K3_Toolchain's vasm backend (`vasm/cpus/78k3/shared/` there).
These copies must be kept consistent, but are **not** automatically
synchronized across repositories -- see "Keeping the instruction tables
in sync" below for why that matters and how to check it, not just a
disclaimer.

This project is itself a standalone repo, pulled out of the main
[Kenwood_TK-x4x_Custom_Firmware](https://github.com/KY4WI/Kenwood_TK-x4x_Custom_Firmware)
project once it had proven useful enough to deserve its own home.

## Build

Build with GCC or Clang on a Unix-like system:

```sh
make
```

Builds as portable ISO C99 (`-std=c99 -pedantic`, no GNU extensions).

## Run

    ./emu78k3 <binary> [--base ADDR] [--entry ADDR] [--sp ADDR] \
        [--max-insns N] [--trace] [--dump-mem ADDR:LEN]

Runs until BRK, an illegal/unimplemented instruction, a branch to its
own address (the standard "for(;;);"/steady-state-end signal every test
here relies on), or `--max-insns` is hit, then prints final CPU state to
stderr. Exit code is 0 only for the two clean halts (self-loop or BRK).

## Run the regression tests

The test suite requires `vbcc78k3`, `vasm78k3_std`, and `vlink` from
NEC-78K3_Toolchain (the companion project named above). Install these
on your `PATH`, then run:

```sh
./tests/run_tests.sh
```

Alternatively, set `VBCC`, `VASM`, and `VLINK` to the respective
executable paths, for example:

```sh
VBCC=/path/to/vbcc78k3 VASM=/path/to/vasm78k3_std \
VLINK=/path/to/vlink ./tests/run_tests.sh
```

## Writing a test

See `tests/t_basic_arith.c` for the pattern: a small C program, linked
against `tests/test_crt0.s` + `tests/test.ld` (a bare-bones, non-hardware
crt0/linker script -- just enough to set SP and call `_main`). Two rules
`tests/run_tests.sh` depends on:

1. Exactly one file-scope object, `static volatile unsigned char
   result[N];`, and nothing else at file scope that lands in `.bss`.
   `vlink -M` doesn't list local/static `.bss` symbols at all for this
   target (checked empirically, not assumed), so there's no address to
   look up -- instead this convention guarantees, given `test.ld`'s
   layout, that `result[0]` sits at the fixed bss-region base address
   `0x1000`.
2. A single-line comment `/* EXPECT: xx xx xx ... */` somewhere in the
   file giving the expected bytes of `result[]` after a clean halt, in
   order, lowercase hex, space-separated.

Run `tests/run_tests.sh` to build, run, and automatically check every
`tests/t_*.c` against its `EXPECT` line -- it fails a test on a build
error, a non-clean halt (crash/runaway/unimplemented opcode), or a value
mismatch (and prints expected-vs-actual on mismatch). To derive an
`EXPECT` line for a new test, run it once with `--dump-mem` and read off
the real values (don't guess-and-hardcode without checking, and don't
trust hand-computed expected values without independently confirming
them -- see the bugs below, both found this way).

Hand-written assembly tests are `tests/a_*.s`: assembled with vasm alone
at address 0 (no crt0, no linker), with a `; EXPECT: xx xx ...` line for
the bytes left at `0x1000`. End in a self-loop; use `brk` for a check
inside the test that failed. `a_incdec_muluw_btclr.s` and `a_mov1.s` are
examples. These cover instructions vbcc doesn't emit but firmware code
does, and `a_saddr_order.s` additionally guards the operand-byte-order
fix described under "Keeping the instruction tables in sync" below.

Current coverage: byte/word arithmetic, loops (incl. the `for(;;);`
self-loop every `main()` degenerates to), struct field access, constant
array indexing (`t_min_arr`, the minimal repro for bug 1 below), unions
(`t_union`), bitfields (`t_bitfield`), function pointers/indirect calls
(`t_funcptr`), and recursion (`t_recursion`, factorial + Fibonacci --
exercises repeated CALL/RET and stack-frame isolation), the address
of a static object -- string literals, pointer tables in initialized data,
a static array indexed by a variable, `&tbl[i]` (`t_textloop`, `t_varadr`;
both fail with vbcc before its VARADR fix, which read the object's
contents where its address was meant), `MOV1` bit transfers (`a_mov1`),
and the two-saddr operand order (`a_saddr_order`, see below). Not yet
covered: floating point, `__interrupt`/CALLT, switch/goto, and execution
(as opposed to decode length) of `AND1`/`ANDI1`/`OR1`/`ORI1`/`XOR1` and
the `CMPM*`/`CMPBK*` family.

## Real bugs found by this emulator (2026-09-30, first test run)

Both fixed same-day; see the fix comments at the cited locations for the
full derivation/evidence.

1. **vbcc78k3 codegen** (NEC-78K3_Toolchain's `vbcc/machines/78k3/machine.c`,
   `emit_var_mem()`): a constant-indexed element of a static/global array,
   or constant-offset member of a static/global struct, silently dropped
   its offset and always addressed element/member 0. Affected both reads
   and writes; variable (non-constant) indices were unaffected.
2. **vasm78k3 relaxation** (NEC-78K3_Toolchain's `vasm/cpus/78k3/cpu.c`,
   `local_label_disp()`): a same-file relative branch (BR, or any
   conditional branch/DBNZ) to a local label double-counted the target's
   own address in its displacement math, landing on the wrong target --
   including the trivial "loop forever" self-branch almost every function
   compiles to. Also corrupted the range check `br_fits_relative()` uses
   to decide BR short-vs-absolute and conditional-branch invert-and-jump
   relaxation, so this could silently miscompile an in-range branch too,
   not just obviously-wrong ones.

This regression suite does not validate previously generated firmware
images. Binaries produced with an affected toolchain should be reviewed
and, where appropriate, rebuilt and tested separately.

## Keeping the instruction tables in sync (2026-10-28)

`Opcodes.h` and `Addressing.h` are local copies of tables that live
canonically in the companion NEC-78K3_Toolchain project's
`vasm/cpus/78k3/shared/` -- see the note at the top of this README.
Nothing automated checks the two copies against each other, and this
emulator's own regression suite doesn't either: its tests exercise
*behavior* (does this C program compute the right answer), which only
catches a table bug if some test happens to touch the exact instruction
form it's wrong about.

That gap was real, not hypothetical: a routine check of this emulator's
copies against the toolchain's canonical ones (done while preparing both
projects for publication) turned up three bugs that had been sitting
in `Opcodes.h`/`Addressing.h` undetected, because nothing here exercised
them:

3. **Two-saddr operand order swapped** (`Addressing.h`'s
   `addr_from_saddr_fe_byte()`, and every `OP_SADDR_FE_3`/`OP_SADDRP_FE_3`
   entry in `Opcodes.h`): the second operand of all 15 "two-saddr" forms
   (`MOV`/`XCH`/`MOVW`/`XCHW`/`ADD`/`ADDC`/`SUB`/`SUBC`/`AND`/`OR`/`XOR`/
   `CMP`/`ADDW`/`SUBW`/`CMPW` `saddr,saddr'`) read its byte from the wrong
   instruction position, and a since-superseded bit-5/4 swap formula in
   the address math compounded it. The toolchain's canonical copy fixed
   both, confirmed against real mask ROM code (see `Addressing.h`'s
   comment). Added `tests/a_saddr_order.s` so this can't silently
   regress again -- confirmed it fails against the old tables and
   passes against the fixed ones.
4. **`AND1`/`ANDI1`/`OR1`/`ORI1` with an `X`/`A`/`PSWH`/`PSWL` operand
   encoded as 3 bytes instead of 2** in `Opcodes.h` (the `SADDR`/`SFR`
   forms of the same four mnemonics were already correct at 3/4 bytes).
   Execution of this instruction family isn't implemented in `emu78k3.c`
   yet (same "not yet covered" gap as floating point and `CALLT`), so
   there's no behavioral test for the fix itself yet, but the wrong
   length alone would have misdecoded whatever instruction follows.
5. **One `CMPMNE` table row's subop byte** (the `OP_DE_S_P, OP_A` row)
   was `0x15` -- a copy-paste of the `OP_DE_A_P, OP_A` row right above
   it -- instead of `0x05`; same "not yet executed" caveat as #4.

Fixed by copying the toolchain's canonical `Opcodes.h`/`Addressing.h`
over these local copies verbatim (keeping this file's own license
header), not by hand-editing -- that's also the recommended way to pick
up any future canonical fix: diff before copying if you've made local
changes of your own.

## Copyright and license

Copyright (c) 2026 Dave, KY4WI.

The original emulator code and regression tests are licensed under the
**GNU General Public License, version 3 or (at your option) any later
version** (`GPL-3.0-or-later`). See [LICENSE](LICENSE).

The instruction-definition headers contain code derived from the
MIT-licensed [nec78k3_disassembler](https://github.com/wonst719/nec78k3_disassembler)
by wonst719, copyright (c) 2021 wonst719. Its copyright notice and MIT
license are preserved in [LICENSE.MIT](LICENSE.MIT). Those portions retain
their original MIT license terms, which are compatible with GPLv3.

This is an independent project, not affiliated with NEC or Renesas.
