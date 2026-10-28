/* EXPECT: 0b 16 21 00 */
static volatile unsigned char result[4];
int main(void)
{
    result[0] = 11;
    result[1] = 22;
    result[2] = 33;
    return 0;
}
