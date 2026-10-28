/* Bitfield regression test -- previously-unexercised compiler surface.
 *
 * EXPECT: b5 05
 */

struct flags {
    unsigned char a : 3;
    unsigned char b : 5;
};

static volatile unsigned char result[2];

int main(void)
{
    struct flags f;

    f.a = 5;   /* 0b101 */
    f.b = 22;  /* 0b10110, but only 5 bits fit -> 22 fits (0..31) */

    result[0] = (unsigned char)((f.b << 3) | f.a); /* pack both fields
                                                      * into one byte by
                                                      * hand to check
                                                      * both read back
                                                      * correctly:
                                                      * (22<<3)|5 = 176|5
                                                      * = 181 = 0xb5 */
    result[1] = f.a; /* expect 5 */

    return 0;
}
