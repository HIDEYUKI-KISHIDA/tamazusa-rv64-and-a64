/* kernel/io/fdt.h — デバイスツリー（FDT）を読む */
#ifndef KERNEL_IO_FDT_H
#define KERNEL_IO_FDT_H

#include <stdint.h>

struct fdt_node {
    int off;            /* 構造の中の位置（属性を引くときに使う） */
    int depth;
    int parent;         /* 親の位置（根は −1） */
    int ac, sc;         /* reg を読むための、親の #address-cells・#size-cells */
    const char *name;
};

uint32_t fdt_be32(const void *p);
int fdt_valid(const void *fdt);
const void *fdt_getprop(const void *fdt, int off, const char *name, int *len);
uint32_t fdt_getu32(const void *fdt, int off, const char *name, uint32_t dflt);
int fdt_compatible(const void *fdt, int off, const char *c);
int fdt_enabled(const void *fdt, int off);
int fdt_reg(const void *fdt, const struct fdt_node *n, int i, uint64_t *addr, uint64_t *size);
int fdt_walk(const void *fdt, int (*cb)(const void *fdt, const struct fdt_node *n, void *arg), void *arg);
int fdt_by_phandle(const void *fdt, uint32_t ph);

#endif
