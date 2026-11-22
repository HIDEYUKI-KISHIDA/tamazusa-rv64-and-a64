/* timer_generic.c — Arm の汎用タイマ（EL1 の物理タイマ、CNTP_*）で一定の周期の割込みを作る
 *
 *   次に鳴る時刻を絶対値（CNTP_CVAL_EL0）で入れる。割込みの処理が遅れても周期がずれない
 *   （RISC-V 版の timer_sbi.c と同じ考え方。取りこぼしたときは今の時刻から入れ直す）。
 *   rv_tick_deadline・rv_tick_entered は M27（遅れの試験）が読む。単位は 10MHz（RV_RDTIME と同じ）。
 */
#include <stdint.h>

#define CNTFRQ_EL0      "cntfrq_el0"
#define CNTP_CTL_EL0    "cntp_ctl_el0"
#define CNTP_CVAL_EL0   "cntp_cval_el0"
#define CNTKCTL_EL1     "cntkctl_el1"

#define CTL_ENABLE      (1ULL << 0)

static uint64_t g_timer_freq;
static uint64_t g_interval;             /* 1 周期（カウンタの刻み） */
static uint64_t g_next;                 /* 次に鳴る時刻（カウンタの値） */

volatile uint64_t rv_tick_deadline;     /* いま処理しているタイマの割込みが本来鳴る時刻（10MHz 単位） */
volatile uint64_t rv_tick_entered;      /* その割込みの処理に入った時刻（10MHz 単位） */

static inline uint64_t cntpct(void)
{
    uint64_t v;
    asm volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

static inline uint64_t to_10mhz(uint64_t c)
{
    return (c / g_timer_freq) * 10000000u + ((c % g_timer_freq) * 10000000u) / g_timer_freq;
}

void timer_init(unsigned int hz)
{
    asm volatile("mrs %0, " CNTFRQ_EL0 : "=r"(g_timer_freq));

    if (hz == 0) { hz = 1; }
    g_interval = g_timer_freq / (uint64_t)hz;

    {
        uint64_t v;
        asm volatile("mrs %0, " CNTKCTL_EL1 : "=r"(v));
        v |= (1ULL << 0) | (1ULL << 1);
        asm volatile("msr " CNTKCTL_EL1 ", %0" :: "r"(v));
    }

    asm volatile("msr " CNTP_CTL_EL0 ", %0" :: "r"(0ULL));
    asm volatile("isb");
    g_next = cntpct() + g_interval;
    asm volatile("msr " CNTP_CVAL_EL0 ", %0" :: "r"(g_next));
    asm volatile("isb");
    asm volatile("msr " CNTP_CTL_EL0 ", %0" :: "r"(CTL_ENABLE));
    asm volatile("isb");
}

/* 割込みごと: 次の時刻を入れ直す（しないと割込みが立ったままになる） */
void timer_on_irq(void)
{
    uint64_t now = cntpct();
    uint64_t next = g_next + g_interval;

    rv_tick_deadline = to_10mhz(g_next);
    rv_tick_entered = to_10mhz(now);
    if ((int64_t)(next - now) <= 0) {   /* 取りこぼし: 今の時刻から */
        next = now + g_interval;
    }
    g_next = next;
    asm volatile("msr " CNTP_CVAL_EL0 ", %0" :: "r"(next));
    asm volatile("isb");
}
