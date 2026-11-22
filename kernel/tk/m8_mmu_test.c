/* kernel/tk/m8_mmu_test.c — M8: Sv39 MMU の確認（SMODE=1 かつ MMU=1 のときだけ）
 *
 *   1. satp のモードが Sv39（8）になっている
 *   2. 読み取り専用ページ（rv_mmu_probe_page）は読める
 *   3. そのページへの書き込みは「ストアページフォールト」（scause = 15）になり、
 *      stval にそのアドレスが入る。書き込みは反映されない
 *
 * ここまでの M5〜M7 がページング有効のまま通っていることも、
 * 恒等マップが正しいことの確認になる。
 *
 * 合格の目印: [M8] PASS
 */
#include <stdint.h>

extern void uart_puts(const char *s);
extern uint64_t rv_mmu_read_satp(void);
extern uint8_t rv_mmu_probe_page[];

extern void rv_fault_expect(uint64_t cause, uint64_t tval);
extern volatile int rv_fault_hit;
extern volatile int rv_fault_armed;
extern volatile uint64_t rv_fault_got_cause;
extern volatile uint64_t rv_fault_got_tval;

#if defined(__aarch64__)
#define CAUSE_STORE_PAGE_FAULT  0x25UL      /* EC: 同じ EL のデータアボート */
#else
#define CAUSE_STORE_PAGE_FAULT  15UL
#endif

int m8_run_mmu_test(void)
{
    volatile uint8_t *probe = (volatile uint8_t *)rv_mmu_probe_page;
    uint8_t before;

#if defined(__aarch64__)
    uint64_t sctlr;
    (void)rv_mmu_read_satp;
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    uart_puts("\r\n[M8] AArch64 MMU test\r\n");
    if ((sctlr & 1) == 0) {
        uart_puts("[M8] SCTLR_EL1.M is 0 (MMU off)\r\n[M8] FAIL\r\n");
        return 0;
    }
    uart_puts("[M8] SCTLR_EL1.M = 1 (MMU on, 4KiB pages, 39-bit VA) OK\r\n");
#else
    uint64_t satp = rv_mmu_read_satp();

    uart_puts("\r\n[M8] Sv39 MMU test\r\n");

    if ((satp >> 60) != 8) {
        uart_puts("[M8] satp mode is not Sv39\r\n[M8] FAIL\r\n");
        return 0;
    }
    uart_puts("[M8] satp mode = Sv39 OK\r\n");
#endif

    before = probe[0];                  /* 読み取りは許可されている */
    uart_puts("[M8] read of read-only page OK\r\n");

    rv_fault_expect(CAUSE_STORE_PAGE_FAULT, (uint64_t)(uintptr_t)rv_mmu_probe_page);
    probe[0] = (uint8_t)(before ^ 0x5a); /* ここでストアページフォールト */
    rv_fault_armed = 0;

    if (!rv_fault_hit) {
        uart_puts("[M8] write to read-only page did NOT fault\r\n[M8] FAIL\r\n");
        return 0;
    }
    if (probe[0] != before) {
        uart_puts("[M8] read-only page was modified\r\n[M8] FAIL\r\n");
        return 0;
    }
#if defined(__aarch64__)
    uart_puts("[M8] write to read-only page -> data abort (EC=0x25) OK\r\n");
#else
    uart_puts("[M8] write to read-only page -> store page fault (scause=15) OK\r\n");
#endif
    uart_puts("[M8] PASS\r\n");
    return 1;
}
