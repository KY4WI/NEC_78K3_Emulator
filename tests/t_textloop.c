/* String-literal address regression test (vbcc 2945d2e): the panel text
 * routine from the TK-x4x probes, with the panel output replaced by a
 * store into result[]. Before VARADR was handled, the string's address
 * was passed as the string's first two bytes ("PA" = 4150h), so the
 * routine read from the wrong place.
 *
 * EXPECT: 02 50 41 4e 45 4c 20 4f 4b 09
 */
typedef unsigned char u8;
static volatile unsigned char result[10];
void panel_send_byte(u8 c) { result[result[9]] = c; result[9]++; }
void panel_display_text(const char *text)
{
    u8 i;
    u8 done = 0;
    const char *p = text;
    for (i = 0; i < 9; i++) {
        u8 c;
        if (i == 0) {
            c = 2;
        } else if (!done) {
            c = (u8)*p;
            p++;
            if (c == 0) { done = 1; c = ' '; }
        } else {
            c = ' ';
        }
        panel_send_byte(c);
    }
}
int main(void)
{
    result[9] = 0;
    panel_display_text("PANEL OK");
    return 0;
}
