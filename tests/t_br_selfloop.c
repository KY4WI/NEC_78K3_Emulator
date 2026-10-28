/* Regression test for the vasm local_label_disp() displacement bug
 * fixed 2026-09-30 (cpu.c): a same-file relative branch to a local
 * label -- including the trivial "for(;;);" self-loop every C function
 * like this one compiles to -- used to encode a displacement based on
 * DOUBLE-COUNTING the target label's own address (see cpu.c's fix
 * comment for the full derivation). Any loop at all exercises this, so
 * this test is deliberately nothing but a loop and a counter. */

/* EXPECT: 0a */
static volatile unsigned char result[1];

int main(void)
{
    unsigned char i;
    unsigned char sum = 0;
    for (i = 0; i < 10; i++)
        sum = (unsigned char)(sum + 1);
    result[0] = sum; /* expect 10 */
    for (;;) {
        /* deliberate infinite loop -- exercises the exact "br <self>"
         * shape that motivated this test, on top of the for-loop's own
         * backward conditional branch above. */
    }
}
