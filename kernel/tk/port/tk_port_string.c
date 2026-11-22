/*
 * tk_port_string.c — 64 ビットで正しく動く memset
 *
 *   T-Kernel の libstr の tkl_memset は、埋める値を 8→16 ビットまでしか
 *   広げない（long が 32 ビットの前提）。RV64 では long が 64 ビットなので、
 *   0 以外の値で埋めると 8 バイトのうち下の 4 バイトしか埋まらない。
 *   include/libstr.h で memset をここの rv_memset に向ける（コンパイラが
 *   自分で出す memset の呼び出しも、強いシンボルの memset でここに来る）。
 *   （0 で埋める場合は元のままでも正しいので、MEMSET を使う所はそのまま）
 */
#include <stddef.h>
#include <stdint.h>

void *rv_memset(void *s, int c, size_t n);
void *memset(void *s, int c, size_t n) __attribute__((alias("rv_memset")));

void *rv_memset(void *s, int c, size_t n)
{
    unsigned char *p = (unsigned char *)s;
    uint64_t v = (unsigned char)c;

    v |= v << 8;
    v |= v << 16;
    v |= v << 32;
    while (n > 0 && ((uintptr_t)p & 7U) != 0) {
        *p++ = (unsigned char)c;
        n--;
    }
    while (n >= 8) {
        *(uint64_t *)p = v;
        p += 8;
        n -= 8;
    }
    while (n > 0) {
        *p++ = (unsigned char)c;
        n--;
    }
    return s;
}
