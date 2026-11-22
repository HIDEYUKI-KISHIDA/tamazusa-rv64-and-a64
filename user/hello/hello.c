/* hello.c — ELF として読み込むテスト用ユーザープログラム（M12）
 *
 *   arg = 0: .data の初期値と .bss の 0 を確かめて挨拶 → 90（違えば 1 / 2）
 *   arg = 1: 自分のコード（読み取り専用）に書く → ストアページフォールトで強制終了
 *   arg = 2 | (id << 8): グローバル変数に id を書き、2 回眠ってから読み戻す → id（違えば 0）
 *   arg = 3: スタックの下（未割り当てのページ）に書く → ストアページフォールトで強制終了
 */
#include "usys.h"

static long counter = 1234;     /* .data */
static char work[512];          /* .bss */

int main(long arg);

static int bss_is_zero(void)
{
    volatile char *p = work;
    unsigned i;

    for (i = 0; i < sizeof(work); i++) {
        if (p[i] != 0) {
            return 0;
        }
    }
    return 1;
}

int main(long arg)
{
    switch (arg & 0xff) {
    case 0:
        if (*(volatile long *)&counter != 1234) {
            return 1;
        }
        if (!bss_is_zero()) {
            return 2;
        }
        work[0] = 'x';
        u_puts("[elf] hello from an ELF user program (.data/.bss OK)\r\n");
        return 90;
    case 1:
        *(volatile unsigned *)(void *)main = 0;     /* ここで強制終了されるはず */
        return 0;
    case 2:
        {
            long id = arg >> 8;
            counter = id;
            u_puts("[elf] instance stored its id\r\n");
            u_sleep(30);
            u_sleep(30);
            return (*(volatile long *)&counter == id) ? (int)id : 0;
        }
    case 3:
        {
            volatile char *below = (volatile char *)__builtin_frame_address(0) - 0x3000;
            *below = 1;                             /* ここで強制終了されるはず */
            return 0;
        }
    default:
        return 99;
    }
}
