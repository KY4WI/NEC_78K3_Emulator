/* Recursion regression test -- exercises repeated CALL/RET and stack
 * discipline (each call frame's locals/args must not clobber the
 * caller's), previously unexercised by the loop-only earlier tests.
 *
 * EXPECT: d0 e9
 */

static volatile unsigned char result[2];

static unsigned char fact(unsigned char n)
{
    if (n <= 1)
        return 1;
    return (unsigned char)(n * fact((unsigned char)(n - 1)));
}

static unsigned char fib(unsigned char n)
{
    if (n < 2)
        return n;
    return (unsigned char)(fib((unsigned char)(n - 1)) + fib((unsigned char)(n - 2)));
}

int main(void)
{
    result[0] = fact(6);  /* 720 & 0xff = 0xd0 */
    result[1] = fib(13);  /* fib(13) = 233 = 0xe9 */

    return 0;
}
