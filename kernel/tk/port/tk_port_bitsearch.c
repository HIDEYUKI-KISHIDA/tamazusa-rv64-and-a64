/*
 * tk_port_bitsearch.c — LP64 で正しく動くビット検索（上流 libsys の置き換え）
 *
 * 上流 T-Kernel 2.0 は typedef unsigned long UW;（コメントは「32 bit」）。
 * RV64（LP64）では UW が 64 ビットになるが、上流の BitSearch0_w / BitSearch1_w は
 * 「UW = 32 ビット」を前提に cp += offset / 32 と進むため、検索幅を超えて
 * 配列の外を読み、誤った位置を返す。
 *   例: 実行可能キューが空になったとき top_priority が 140 ではなく 131 になり、
 *       schedtsk / ctxtsk が ready_queue 内部を指して壊れる。
 *
 * ここでは BitSet / BitClr / BitTest（上流、バイト単位・リトルエンディアン）と
 * 同じビット番号付けで、バイト単位に検索する。検索幅の外は読まない。
 * 上流の bitsearch0.c / bitsearch1.c はリンクしない（Makefile）。
 */
#include <basic.h>
#include <sys/bitop.h>

static W bit_search(const void *base, W offset, W width, int want)
{
    const UB *p = (const UB *)base;
    W i;

    for (i = 0; i < width; i++) {
        W b = offset + i;
        int bit = (p[b >> 3] >> (b & 7)) & 1;
        if (bit == want) {
            return i;       /* 検索開始位置からの相対位置 */
        }
    }
    return -1;
}

EXPORT W BitSearch0(const void *base, W offset, W width)
{
    return bit_search(base, offset, width, 0);
}

EXPORT W BitSearch1(const void *base, W offset, W width)
{
    return bit_search(base, offset, width, 1);
}

EXPORT W BitSearch0_w(const UW *base, W offset, W width)
{
    return bit_search(base, offset, width, 0);
}

EXPORT W BitSearch1_w(const UW *base, W offset, W width)
{
    return bit_search(base, offset, width, 1);
}
