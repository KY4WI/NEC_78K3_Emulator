/* Union regression test -- previously-unexercised compiler surface
 * (aliasing a word with two bytes, and a union inside a struct).
 *
 * EXPECT: 34 12 2a
 */

typedef union {
    unsigned int w;
    struct { unsigned char lo, hi; } b;
} word_bytes;

struct holder {
    word_bytes u;
    unsigned char tag;
};

static volatile unsigned char result[3];

int main(void)
{
    word_bytes wb;
    struct holder h;

    wb.w = 0x1234;
    result[0] = wb.b.lo;   /* expect 0x34 */
    result[1] = wb.b.hi;   /* expect 0x12 */

    h.u.b.lo = 42;
    h.u.b.hi = 0;
    h.tag = (unsigned char)h.u.w; /* write through one union member,
                                    * read back through the other --
                                    * expect 42 (0x2a) */
    result[2] = h.tag;

    return 0;
}
