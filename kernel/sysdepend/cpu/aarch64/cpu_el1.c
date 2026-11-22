/* cpu_el1.c — EL1 の初期化（reset.S から、board_init の前に呼ぶ）
 *   浮動小数点・SIMD を EL1 で使えるようにし（CPACR_EL1.FPEN）、MMU とキャッシュを止め、
 *   例外ベクタ（exception.S の a64_vectors）を VBAR_EL1 に入れる */
#include <stdint.h>

#define SCTLR_M     (1ULL << 0)
#define SCTLR_C     (1ULL << 2)
#define SCTLR_I     (1ULL << 12)

extern char a64_vectors[];

void cpu_el1_init(void)
{
    uint64_t v;

    asm volatile("mrs %0, cpacr_el1" : "=r"(v));
    v |= (3ULL << 20);                      /* FPEN = 11: EL0・EL1 とも浮動小数点を使える */
    asm volatile("msr cpacr_el1, %0" :: "r"(v));
    asm volatile("isb");

    asm volatile("mrs %0, sctlr_el1" : "=r"(v));
    v &= ~(SCTLR_M | SCTLR_C | SCTLR_I);
    asm volatile("msr sctlr_el1, %0" :: "r"(v));
    asm volatile("isb");

    asm volatile("msr vbar_el1, %0" :: "r"((uint64_t)(uintptr_t)a64_vectors));
    asm volatile("isb");
}
