/* Basic 8/16-bit arithmetic + a simple loop -- first smoke test for
 * emu78k3. Exercises: byte add/sub, a for-loop (DBNZ or BNE-style
 * compare+branch, whichever machine.c chooses), word arithmetic,
 * struct field access (a previously-unexercised compiler surface).
 *
 * EXPECT: 0d 07 0a 2a 04 d2 00 00
 * (run_tests.sh reads this line: hex bytes it expects at &result[0]
 * after a clean halt, one byte per array element in declaration order.)
 */

struct point { unsigned char x, y; unsigned int sum; };

static volatile unsigned char result[8];

int main(void)
{
    unsigned char a = 10, b = 3;
    unsigned int w1 = 1000, w2 = 234;
    unsigned char i, acc;
    struct point p;

    result[0] = (unsigned char)(a + b);   /* expect 13 */
    result[1] = (unsigned char)(a - b);   /* expect 7 */

    acc = 0;
    for (i = 0; i < 5; i++)
        acc = (unsigned char)(acc + i);   /* 0+1+2+3+4 = 10 */
    result[2] = acc;

    p.x = 40;
    p.y = 2;
    p.sum = (unsigned int)(p.x + p.y);    /* 42 */
    result[3] = (unsigned char)p.sum;

    result[4] = (unsigned char)((w1 + w2) >> 8); /* (1234)>>8 = 4 */
    result[5] = (unsigned char)(w1 + w2);        /* 1234 & 0xff = 210 */

    return 0;
}
