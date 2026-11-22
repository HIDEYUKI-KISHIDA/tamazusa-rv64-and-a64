/* kernel/io/fdt.c — デバイスツリー（FDT）を読む
 *
 *   OpenSBI・U-Boot は、起動のときに機械の中身（どこに何の装置があるか）を書いたデバイスツリーを渡す
 *   （rv_boot_dtb に物理アドレスが入っている）。SoM ごとの違いは、ここに書かれている。
 *   ここでは読むだけ: すべての節を順にたどり、節ごとに、親の #address-cells・#size-cells と一緒に知らせる。
 *   形は devicetree の仕様（Flattened Devicetree）のとおり。数はすべてビッグエンディアン。
 */
#include <stdint.h>
#include <stddef.h>
#include "fdt.h"

#define FDT_MAGIC       0xd00dfeedU
#define FDT_BEGIN_NODE  1
#define FDT_END_NODE    2
#define FDT_PROP        3
#define FDT_NOP         4
#define FDT_END         9

uint32_t fdt_be32(const void *p)
{
    const uint8_t *b = (const uint8_t *)p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static size_t str_len(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

int fdt_valid(const void *fdt)
{
    return fdt && fdt_be32(fdt) == FDT_MAGIC && fdt_be32((const uint8_t *)fdt + 20) >= 16
           && fdt_be32((const uint8_t *)fdt + 4) <= 0x200000;   /* 全体の大きさ（QEMU の AArch64 virt はちょうど 1MiB） */
}

static const uint8_t *structs(const void *fdt)
{
    return (const uint8_t *)fdt + fdt_be32((const uint8_t *)fdt + 8);
}

static const char *strings(const void *fdt)
{
    return (const char *)fdt + fdt_be32((const uint8_t *)fdt + 12);
}

/* 節 off（構造の中の位置）の属性 name を探す。あれば値を返し、*len に長さ */
const void *fdt_getprop(const void *fdt, int off, const char *name, int *len)
{
    const uint8_t *s = structs(fdt);
    uint32_t pos = (uint32_t)off;
    if (fdt_be32(s + pos) != FDT_BEGIN_NODE) {
        return NULL;
    }
    pos += 4;
    pos += (uint32_t)((str_len((const char *)s + pos) + 4) & ~3u);
    for (;;) {
        uint32_t tag = fdt_be32(s + pos);
        if (tag == FDT_NOP) {
            pos += 4;
        } else if (tag == FDT_PROP) {
            uint32_t l = fdt_be32(s + pos + 4), no = fdt_be32(s + pos + 8);
            if (str_eq(strings(fdt) + no, name)) {
                if (len) {
                    *len = (int)l;
                }
                return s + pos + 12;
            }
            pos += 12 + ((l + 3) & ~3u);
        } else {
            return NULL;                /* 子の節か、節の終わり: 属性は前にしか書かない */
        }
    }
}

uint32_t fdt_getu32(const void *fdt, int off, const char *name, uint32_t dflt)
{
    int len;
    const void *p = fdt_getprop(fdt, off, name, &len);
    return (p && len >= 4) ? fdt_be32(p) : dflt;
}

/* compatible の中に c があるか（compatible は 0 で区切った文字列の並び） */
int fdt_compatible(const void *fdt, int off, const char *c)
{
    int len, i = 0;
    const char *p = (const char *)fdt_getprop(fdt, off, "compatible", &len);
    while (p && i < len) {
        if (str_eq(p + i, c)) {
            return 1;
        }
        i += (int)str_len(p + i) + 1;
    }
    return 0;
}

/* status が "okay" か、書いていない */
int fdt_enabled(const void *fdt, int off)
{
    int len;
    const char *p = (const char *)fdt_getprop(fdt, off, "status", &len);
    return !p || str_eq(p, "okay") || str_eq(p, "ok");
}

/* reg の i 番目の住所（cells の数は親の #address-cells・#size-cells） */
int fdt_reg(const void *fdt, const struct fdt_node *n, int i, uint64_t *addr, uint64_t *size)
{
    int len, k;
    const uint8_t *p = (const uint8_t *)fdt_getprop(fdt, n->off, "reg", &len);
    int step = 4 * (n->ac + n->sc);
    uint64_t a = 0, s = 0;
    if (!p || step == 0 || (i + 1) * step > len) {
        return 0;
    }
    p += i * step;
    for (k = 0; k < n->ac; k++) {
        a = (a << 32) | fdt_be32(p + 4 * k);
    }
    for (k = 0; k < n->sc; k++) {
        s = (s << 32) | fdt_be32(p + 4 * (n->ac + k));
    }
    *addr = a;
    if (size) {
        *size = s;
    }
    return 1;
}

/* すべての節をたどる。cb が 0 以外を返したら止めて、その値を返す */
int fdt_walk(const void *fdt, int (*cb)(const void *fdt, const struct fdt_node *n, void *arg), void *arg)
{
    const uint8_t *s = structs(fdt);
    uint32_t pos = 0, end = fdt_be32((const uint8_t *)fdt + 36);
    int depth = -1;
    int ac[17], sc[17], offs[17];               /* 深さごとの、子に効く cells と、節の位置 */

    while (pos < end) {
        uint32_t tag = fdt_be32(s + pos);
        if (tag == FDT_BEGIN_NODE) {
            struct fdt_node n;
            int r;
            uint32_t start = pos;
            depth++;
            if (depth >= 16) {
                return -1;
            }
            pos += 4;
            n.name = (const char *)s + pos;
            pos += (uint32_t)((str_len(n.name) + 4) & ~3u);
            n.off = (int)start;
            n.depth = depth;
            n.parent = depth > 0 ? offs[depth - 1] : -1;
            n.ac = depth > 0 ? ac[depth - 1] : 2;
            n.sc = depth > 0 ? sc[depth - 1] : 1;
            offs[depth] = (int)start;
            ac[depth] = (int)fdt_getu32(fdt, (int)start, "#address-cells", 2);
            sc[depth] = (int)fdt_getu32(fdt, (int)start, "#size-cells", 1);
            r = cb(fdt, &n, arg);
            if (r) {
                return r;
            }
        } else if (tag == FDT_END_NODE) {
            depth--;
            pos += 4;
        } else if (tag == FDT_PROP) {
            pos += 12 + ((fdt_be32(s + pos + 4) + 3) & ~3u);
        } else if (tag == FDT_NOP) {
            pos += 4;
        } else {
            break;                              /* FDT_END */
        }
    }
    return 0;
}

struct ph_find { uint32_t ph; int off; };

static int ph_cb(const void *fdt, const struct fdt_node *n, void *arg)
{
    struct ph_find *f = (struct ph_find *)arg;
    if (fdt_getu32(fdt, n->off, "phandle", 0) == f->ph || fdt_getu32(fdt, n->off, "linux,phandle", 0) == f->ph) {
        f->off = n->off;
        return 1;
    }
    return 0;
}

/* phandle（ほかの節を指す番号）から節の位置 */
int fdt_by_phandle(const void *fdt, uint32_t ph)
{
    struct ph_find f;
    f.ph = ph;
    f.off = -1;
    if (ph == 0) {
        return -1;
    }
    fdt_walk(fdt, ph_cb, &f);
    return f.off;
}
