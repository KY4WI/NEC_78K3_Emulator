/* Address-of-static regression test (vbcc 2945d2e): a table of string
 * pointers in initialized data, a static array indexed by a variable,
 * &tbl[i] (directly and returned from a function), and a static struct
 * reached by pointer and by name. Each of these used the object's
 * contents where its address was meant before VARADR was handled.
 *
 * EXPECT: 41 44 42 07 33 05 11 22 4b 58
 */
static volatile unsigned char result[10];
static const char * const names[3] = { "AB", "CD", "EF" };
static unsigned char tbl[6] = { 1, 3, 5, 7, 9, 11 };
struct s { unsigned char a; unsigned char b; unsigned int w; };
static struct s st = { 0x11, 0x22, 0x4b58 };
static unsigned char *pick(unsigned char i) { return &tbl[i]; }
int main(void)
{
    const char *p;
    unsigned char *q;
    struct s *sp = &st;
    unsigned char i = 1;
    p = names[0];  result[0] = p[0];          /* 'A' via initialized pointer table */
    p = names[i];  result[1] = p[1];          /* 'D' */
    result[2] = names[0][1];                  /* 'B' */
    result[3] = tbl[i + 2];                   /* 7: static array, variable index */
    q = pick(4);   result[4] = (unsigned char)(*q * 5 + 6);  /* 9*5+6 = 51 */
    q = &tbl[2];   result[5] = *q;            /* 5 */
    result[6] = sp->a;                        /* 0x11 */
    result[7] = st.b;                         /* 0x22 */
    result[8] = (unsigned char)(sp->w >> 8);  /* 0x4b */
    result[9] = (unsigned char)st.w;          /* 0x58 */
    return 0;
}
