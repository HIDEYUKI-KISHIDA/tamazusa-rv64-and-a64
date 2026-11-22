/* ulib.h — ユーザープログラム用の小さな関数（文字列・数の表示） */
#ifndef ULIB_H
#define ULIB_H
#include "usys.h"

static inline long u_strlen(const char *s)
{
    long n = 0;
    while (s[n] != 0) {
        n++;
    }
    return n;
}

static inline int u_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static inline void u_strcpy_n(char *dst, const char *src, long max)
{
    long i;
    for (i = 0; i < max - 1 && src[i] != 0; i++) {
        dst[i] = src[i];
    }
    dst[i] = 0;
}

static inline void u_putc(char c)
{
    char b[2];
    b[0] = c;
    b[1] = 0;
    u_puts(b);
}

static inline void u_put_num(long v)
{
    char buf[24];
    int n = 0;
    unsigned long u = (v < 0) ? (unsigned long)(-v) : (unsigned long)v;

    if (v < 0) {
        u_putc('-');
    }
    do {
        buf[n++] = (char)('0' + (u % 10));
        u /= 10;
    } while (u != 0);
    {
        char out[25];
        int k = 0;
        while (n > 0) {
            out[k++] = buf[--n];
        }
        out[k] = 0;
        u_puts(out);
    }
}

#endif
