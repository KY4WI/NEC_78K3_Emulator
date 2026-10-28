/* Regression test for vbcc78k3 compiler bug #1 (see
 * KY4WI/vbcc machines/78k3/BUGS.md): an unsigned relational comparison
 * (<, <=, >, >=) on a u8/u16 value that can cross the top-bit boundary
 * (0x80 for a byte, 0x8000 for a word) must be compiled to the UNSIGNED
 * branch instruction. The bug made the compiler pick the branch based on
 * an unrelated internal label number instead of the real operand type,
 * so it would (reliably, for a small file like this one) emit the SIGNED
 * branch instead -- giving the wrong answer for exactly the inputs where
 * signed and unsigned interpretations disagree.
 *
 * Every input below is chosen so that the signed and unsigned readings
 * of the comparison give DIFFERENT answers -- found and fixed for real on
 * BETA_32's panel_test_b3_subcode() (0x8E failed to read as ">13"), then
 * generalized here as a permanent regression test so it can't come back
 * silently in a future compiler change.
 *
 * EXPECT: 01 00 00 01 01 00 00 01
 */

static volatile unsigned char result[8];

static unsigned char u8_gt13(unsigned char sub)
{
    /* the exact BETA_32 pattern: PANEL_B3_SUBCODE_MAX was 13. */
    if (sub > 13) return 1;
    return 0;
}

static unsigned char u8_lt128(unsigned char v)
{
    if (v < 128) return 1;
    return 0;
}

static unsigned char u16_gt1000(unsigned int w)
{
    if (w > 1000) return 1;
    return 0;
}

static unsigned char u16_lt30000(unsigned int w)
{
    if (w < 30000) return 1;
    return 0;
}

int main(void)
{
    /* 0x8E = 142 unsigned (> 13 true), -114 signed (> 13 false) */
    result[0] = u8_gt13(0x8E);
    /* 5 stays under the boundary either way -- sanity check */
    result[1] = u8_gt13(5);

    /* 0xFE = 254 unsigned (< 128 false), -2 signed (< 128 true) */
    result[2] = u8_lt128(0xFE);
    /* 10 stays under the boundary either way -- sanity check */
    result[3] = u8_lt128(10);

    /* 0x8010 = 32784 unsigned (> 1000 true), -32752 signed (> 1000 false) */
    result[4] = u16_gt1000(0x8010);
    /* 500 stays under the boundary either way -- sanity check */
    result[5] = u16_gt1000(500);

    /* 0xFFFF = 65535 unsigned (< 30000 false), -1 signed (< 30000 true) */
    result[6] = u16_lt30000(0xFFFF);
    /* 100 stays under the boundary either way -- sanity check */
    result[7] = u16_lt30000(100);

    return 0;
}
