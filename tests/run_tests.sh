#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Dave, KY4WI
# Compiler/assembler regression runner: builds every tests/t_*.c against
# test_crt0.s + test.ld, runs it under emu78k3, and checks the result.
#
# Convention every t_*.c must follow:
#   - exactly one file-scope object, `static volatile unsigned char
#     result[N];`, and nothing else at file scope that lands in .bss --
#     this guarantees (given test.ld's layout, verified empirically: vlink
#     -M doesn't list local/static .bss symbols at all for this target, so
#     there's no address to look up) that result[0] sits at the fixed
#     bss-region base address 0x1000.
#   - a single-line comment `/* EXPECT: xx xx xx ... */` (anywhere in the
#     file) giving the N expected bytes of result[], in order, lowercase
#     hex, space-separated. N is taken from the count of hex bytes given.
#
# A test fails if the build fails, if it doesn't reach a clean halt
# (self-loop or BRK) within --max-insns, or if the dumped bytes at 0x1000
# don't match EXPECT.
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
EMUDIR="$(dirname "$DIR")"
# Use executables on PATH, or override any of these with an explicit path.
VBCC="${VBCC:-vbcc78k3}"
VASM="${VASM:-vasm78k3_std}"
VLINK="${VLINK:-vlink}"
EMU="${EMU:-$EMUDIR/emu78k3}"

for tool in "$VBCC" "$VASM" "$VLINK"; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "Missing toolchain executable: $tool" >&2
        echo "Install it on PATH or set VBCC, VASM, and VLINK." >&2
        exit 2
    fi
done

if [ ! -x "$EMU" ]; then
	echo "building emu78k3..."
	make -C "$EMUDIR"
fi

"$VASM" -Fvobj -o "$DIR/test_crt0.o" "$DIR/test_crt0.s" >/dev/null

pass=0
fail=0
for c in "$DIR"/t_*.c; do
	name="$(basename "$c" .c)"
	s="$DIR/$name.s"
	o="$DIR/$name.o"
	bin="$DIR/$name.bin"

	expect="$(grep -o 'EXPECT:[0-9a-f ]*' "$c" | head -1 | sed 's/EXPECT: *//' | sed 's/ *$//')"
	if [ -z "$expect" ]; then
		echo "FAIL $name (no '/* EXPECT: xx xx ... */' line found)"
		fail=$((fail+1))
		continue
	fi
	nbytes=$(( $(echo "$expect" | wc -w) ))

	"$VBCC" -c99 -o="$s" "$c" >/dev/null 2>"$DIR/$name.vbcc.log" || {
		echo "FAIL $name (vbcc78k3 error, see $name.vbcc.log)"
		fail=$((fail+1))
		continue
	}
	"$VASM" -Fvobj -o "$o" "$s" >/dev/null 2>"$DIR/$name.vasm.log" || {
		echo "FAIL $name (vasm78k3 error, see $name.vasm.log)"
		fail=$((fail+1))
		continue
	}
	"$VLINK" -T "$DIR/test.ld" -b rawbin1 -o "$bin" "$DIR/test_crt0.o" "$o" \
		>/dev/null 2>"$DIR/$name.vlink.log" || {
		echo "FAIL $name (vlink error, see $name.vlink.log)"
		fail=$((fail+1))
		continue
	}

	set +e
	dump="$("$EMU" "$bin" --base 0 --entry 0 --max-insns 2000000 \
		--dump-mem 0x1000:"$nbytes" 2>"$DIR/$name.emu.log")"
	status=$?
	set -e
	if [ "$status" -ne 0 ]; then
		echo "FAIL $name (did not reach a clean halt, see $name.emu.log)"
		fail=$((fail+1))
		rm -f "$s" "$o" "$bin"
		continue
	fi
	got="$(echo "$dump" | sed -n 's/^mem [0-9a-f]*: //p')"
	if [ "$got" != "$expect" ]; then
		echo "FAIL $name (value mismatch: expected [$expect], got [$got])"
		fail=$((fail+1))
	else
		echo "PASS $name"
		pass=$((pass+1))
		rm -f "$DIR/$name.vbcc.log" "$DIR/$name.vasm.log" "$DIR/$name.vlink.log" "$DIR/$name.emu.log"
	fi
	rm -f "$s" "$o" "$bin"
done

# Hand-written assembly tests: tests/a_*.s, assembled with vasm alone at
# address 0 (no crt0 or linker). Each has a '; EXPECT: xx xx ...' line for
# the bytes it leaves at 0x1000, and must end in a self-loop; BRK means a
# check inside the test failed.
for a in "$DIR"/a_*.s; do
	[ -e "$a" ] || continue
	name="$(basename "$a" .s)"
	bin="$DIR/$name.bin"
	expect="$(grep -o 'EXPECT:[0-9a-f ]*' "$a" | head -1 | sed 's/EXPECT: *//' | sed 's/ *$//')"
	if [ -z "$expect" ]; then
		echo "FAIL $name (no '; EXPECT: xx xx ...' line found)"
		fail=$((fail+1))
		continue
	fi
	nbytes=$(( $(echo "$expect" | wc -w) ))
	"$VASM" -Fbin -quiet -o "$bin" "$a" >/dev/null 2>"$DIR/$name.vasm.log" || {
		echo "FAIL $name (vasm78k3 error, see $name.vasm.log)"
		fail=$((fail+1))
		continue
	}
	set +e
	dump="$("$EMU" "$bin" --base 0 --entry 0 --max-insns 2000000 \
		--dump-mem 0x1000:"$nbytes" 2>"$DIR/$name.emu.log")"
	status=$?
	set -e
	got="$(echo "$dump" | sed -n 's/^mem [0-9a-f]*: //p')"
	if grep -q "reason=1\b\|BRK" "$DIR/$name.emu.log"; then
		echo "FAIL $name (a check in the test failed: BRK, see $name.emu.log)"
		fail=$((fail+1))
	elif [ "$status" -ne 0 ]; then
		echo "FAIL $name (did not reach a clean halt, see $name.emu.log)"
		fail=$((fail+1))
	elif [ "$got" != "$expect" ]; then
		echo "FAIL $name (value mismatch: expected [$expect], got [$got])"
		fail=$((fail+1))
	else
		echo "PASS $name"
		pass=$((pass+1))
		rm -f "$DIR/$name.vasm.log" "$DIR/$name.emu.log"
	fi
	rm -f "$bin"
done

echo "----"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
