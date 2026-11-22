/* hello_musl.c — RISC-V の Linux 用にコンパイルした musl のプログラム（M18）
 *   ふつうの Linux のプログラムと同じ書き方。Tamazusa-RV の Linux 互換層で動く。
 *   終了コード 7
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *home = getenv("HOME");
    char *p = malloc(100000);

    printf("Hello from musl on Tamazusa-RV: %d\n", 42);
    printf("argc=%d argv[0]=%s HOME=%s\n", argc, argv[0], home ? home : "(none)");
    if (p == NULL) {
        return 3;
    }
    memset(p, 0x5a, 100000);
    printf("malloc 100000 OK (p[99999]=%#x)\n", p[99999] & 0xff);
    free(p);
    printf("float: %.3f\n", 1.0 / 3.0);
    return 7;
}
