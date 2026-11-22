/*
 * timer_sbi.c — 構成 B（S-mode）用タイマ
 *
 * S-mode からは CLINT の mtimecmp に書けないため、
 * 現在時刻は rdtime（time CSR）で読み、次回割込みは
 * SBI TIME 拡張（sbi_set_timer）で OpenSBI に設定してもらう。
 * sbi_set_timer を呼ぶと保留中の STIP もクリアされる。
 *
 * 公開関数は timer_clint.c と同じ名前・同じ意味。
 */
#include <stdint.h>
#include "rv_priv.h"

#define TIMEBASE_FREQ   10000000UL      /* QEMU virt: 10 MHz */

#define SBI_EXT_TIME    0x54494D45UL    /* "TIME" */
#define SBI_TIME_SET    0

static uint64_t g_timer_interval;
static uint64_t g_next_deadline;
volatile uint64_t rv_tick_deadline;     /* いま処理しているタイマの割込みが本来鳴る時刻（遅れの測定用） */
volatile uint64_t rv_tick_entered;      /* その割込みの処理に入った時刻 */

static inline uint64_t rv_rdtime(void)
{
    uint64_t t;
    asm volatile("rdtime %0" : "=r"(t));
    return t;
}

static long sbi_set_timer(uint64_t stime)
{
    register unsigned long a0 asm("a0") = (unsigned long)stime;
    register unsigned long a1 asm("a1") = 0;
    register unsigned long a6 asm("a6") = SBI_TIME_SET;
    register unsigned long a7 asm("a7") = SBI_EXT_TIME;

    asm volatile("ecall"
                 : "+r"(a0), "+r"(a1)
                 : "r"(a6), "r"(a7)
                 : "memory");
    return (long)a0;    /* SBI エラーコード（0 = 成功） */
}

void sys_timer_initialize(int hartid)
{
    (void)hartid;
    g_next_deadline = rv_rdtime() + g_timer_interval;
    (void)sbi_set_timer(g_next_deadline);
    asm volatile("csrs " RV_STR(CSR_XIE) ", %0" :: "r"((unsigned long)XIE_TIMER));
}

void sys_timer_interrupt_handler(int hartid)
{
    uint64_t now = rv_rdtime();
    uint64_t next = g_next_deadline + g_timer_interval;

    (void)hartid;
    rv_tick_deadline = g_next_deadline;
    rv_tick_entered = now;
    if ((int64_t)(next - now) <= 0) {   /* 取りこぼし時は現在時刻から再設定 */
        next = now + g_timer_interval;
    }
    g_next_deadline = next;
    (void)sbi_set_timer(next);
}

void timer_init(unsigned int hz)
{
    if (hz == 0) {
        hz = 100;
    }
    g_timer_interval = TIMEBASE_FREQ / (uint64_t)hz;
    sys_timer_initialize(0);
}

void timer_on_irq(void)
{
    sys_timer_interrupt_handler(0);
}
