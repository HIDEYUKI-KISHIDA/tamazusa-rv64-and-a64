/*
 * pci_virt.c — QEMU virt の PCIe（ECAM）（S-mode + MMU のみ）
 *
 *   QEMU virt の PCIe のホスト（gpex）は、設定の窓（ECAM）が 0x3000_0000 から 256MiB、
 *   装置のメモリの窓が 0x4000_0000 から 1GiB。バス 0 の 32 台を見て、見つけた装置の BAR に
 *   メモリの窓から順に番地を割り当て、メモリの読み書きと DMA（バスマスター）を許す。
 *   割込み（INTx・MSI）は使わない（ドライバは状態を見に行く）。
 *   AArch64 の QEMU virt（highmem-ecam=off）は、ECAM が 0x3F00_0000 から 16MiB、メモリの窓が 0x1000_0000 から。
 *   実機では、SoM ごとの PCIe のドライバがこの役をする（ECAM の形は同じことが多い）。
 */
#include <stdint.h>
#include <stddef.h>
#include <rv_priv.h>

extern void uart_puts(const char *s);

#if defined(__aarch64__)
/* QEMU virt（AArch64、highmem-ecam=off）: ECAM 0x3F00_0000 から 16MiB、メモリの窓 0x1000_0000 から */
#define ECAM_BASE   0x3F000000UL
#define MMIO_BASE   0x10000000UL
#define MMIO_SIZE   0x2EFF0000UL
#else
#define ECAM_BASE   0x30000000UL
#define MMIO_BASE   0x40000000UL
#define MMIO_SIZE   0x40000000UL
#endif

static uintptr_t next_mmio = MMIO_BASE;

static volatile uint32_t *cfg(unsigned bus, unsigned dev, unsigned fn, unsigned reg)
{
    return (volatile uint32_t *)RV_IO(ECAM_BASE + ((uintptr_t)bus << 20) + ((uintptr_t)dev << 15)
                                      + ((uintptr_t)fn << 12) + reg);
}

uint32_t rv_pci_read32(unsigned bus, unsigned dev, unsigned fn, unsigned reg)
{
    return *cfg(bus, dev, fn, reg & ~3u);
}

void rv_pci_write32(unsigned bus, unsigned dev, unsigned fn, unsigned reg, uint32_t v)
{
    *cfg(bus, dev, fn, reg & ~3u) = v;
}

static void put_hex(uint32_t v, int digits)
{
    char b[12];
    int i;
    for (i = digits - 1; i >= 0; i--) {
        b[i] = "0123456789abcdef"[v & 15];
        v >>= 4;
    }
    b[digits] = 0;
    uart_puts(b);
}

/* 装置のクラス（上位 24 ビット: クラス・サブクラス・プログラミング IF）で探し、BAR0 を割り当てる。
 * 見つかれば 1 で、*bar0 に BAR0 の仮想アドレス（RV_IO を通したもの）、*dev_out に番号 */
int rv_pci_find_class(uint32_t class24, uintptr_t *bar0, unsigned *dev_out)
{
    unsigned dev;

    for (dev = 0; dev < 32; dev++) {
        uint32_t id = rv_pci_read32(0, dev, 0, 0x00);
        uint32_t cls, b0, size, cmd;
        int is64;
        uint64_t addr;

        if ((id & 0xffff) == 0xffff) {
            continue;
        }
        cls = rv_pci_read32(0, dev, 0, 0x08) >> 8;
        if (cls != class24) {
            continue;
        }
        /* BAR0 の大きさを調べる（全部 1 を書いて読み戻す） */
        b0 = rv_pci_read32(0, dev, 0, 0x10);
        if (b0 & 1) {
            continue;                       /* I/O の BAR は使わない */
        }
        is64 = ((b0 >> 1) & 3) == 2;
        rv_pci_write32(0, dev, 0, 0x10, 0xffffffffU);
        size = ~(rv_pci_read32(0, dev, 0, 0x10) & ~0xfU) + 1;
        if (size == 0 || size > MMIO_SIZE) {
            continue;
        }
        next_mmio = (next_mmio + size - 1) & ~((uintptr_t)size - 1);
        addr = next_mmio;
        next_mmio += size;
        rv_pci_write32(0, dev, 0, 0x10, (uint32_t)addr | (b0 & 0xf));
        if (is64) {
            rv_pci_write32(0, dev, 0, 0x14, (uint32_t)(addr >> 32));
        }
        cmd = rv_pci_read32(0, dev, 0, 0x04);
        rv_pci_write32(0, dev, 0, 0x04, (cmd & 0xffff) | 0x0006);    /* Memory Space + Bus Master */
        uart_puts("[pci] 00:");
        put_hex(dev, 2);
        uart_puts(".0 ");
        put_hex(id & 0xffff, 4);
        uart_puts(":");
        put_hex(id >> 16, 4);
        uart_puts(" class ");
        put_hex(cls, 6);
        uart_puts(" BAR0=0x");
        put_hex((uint32_t)addr, 8);
        uart_puts("\r\n");
        *bar0 = RV_IO(addr);
        *dev_out = dev;
        return 1;
    }
    return 0;
}
