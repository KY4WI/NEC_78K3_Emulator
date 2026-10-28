/* Function-pointer regression test: dispatch through an array of
 * function pointers, and an indirect call through a variable.
 *
 * EXPECT: 07 01
 */

static volatile unsigned char result[2];

static unsigned char add3(unsigned char a, unsigned char b, unsigned char c)
{
    return (unsigned char)(a + b + c);
}

static unsigned char always_one(unsigned char a, unsigned char b, unsigned char c)
{
    (void)a; (void)b; (void)c;
    return 1;
}

typedef unsigned char (*op_fn)(unsigned char, unsigned char, unsigned char);

static op_fn ops[2] = { add3, always_one };

int main(void)
{
    op_fn f;

    result[0] = ops[0](2, 2, 3); /* expect 7 */

    f = ops[1];
    result[1] = f(9, 9, 9);      /* expect 1 */

    return 0;
}
