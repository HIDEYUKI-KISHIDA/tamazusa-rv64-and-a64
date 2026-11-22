/* psci.c — PSCI（Arm の電源の決まり）への問い合わせ
 *
 *   起動したときに、ファームウェアが PSCI を持っているか、どの機能があるかを調べて表示する。
 *   ノートの「スリープ」（全体の停止）を標準の方法（SYSTEM_SUSPEND）で頼めるかを、実機で確かめるため。
 *
 *   呼び方（conduit）:
 *     EL2 以上で入ってきたとき（EL3 か EL2 にファームウェアがいる）… SMC
 *       Allwinner A733（デバイスツリーの psci は method = "smc"）、QEMU virt の virtualization=on
 *     EL1 で入ってきたとき … HVC（QEMU virt の既定。QEMU 自身が答える）
 *
 *   PSCI を持たない環境（Pi 4・Pi 5 の既定など）で呼ぶと例外になるので、呼ぶのはボードが許すときだけ。
 */

#include <stdint.h>

extern void uart_puts(const char *s);
extern void uart_putc(char c);
extern uint32_t boot_entry_el;          /* reset.S: 入ってきたときの EL（1〜3） */

#define PSCI_VERSION            0x84000000u
#define PSCI_FEATURES           0x8400000Au
#define PSCI_CPU_SUSPEND64      0xC4000001u
#define PSCI_SYSTEM_OFF         0x84000008u
#define PSCI_SYSTEM_RESET       0x84000009u
#define PSCI_SYSTEM_SUSPEND64   0xC400000Eu

#define PSCI_NOT_SUPPORTED      (-1)

struct psci_info {
    int      checked;       /* 調べたか */
    int      smc;           /* 1: SMC、0: HVC */
    uint32_t version;       /* 上位 16 ビットが major、下位が minor。0 は「なし」 */
    int      cpu_suspend;   /* PSCI_FEATURES の答え（0 以上ならある） */
    int      system_suspend;
    int      system_off;
    int      system_reset;
};

struct psci_info psci_info;

static int64_t psci_call(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
    register uint64_t x0 asm("x0") = fn;
    register uint64_t x1 asm("x1") = a1;
    register uint64_t x2 asm("x2") = a2;
    register uint64_t x3 asm("x3") = a3;

    if (psci_info.smc) {
        asm volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :: "memory");
    } else {
        asm volatile("hvc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :: "memory");
    }
    return (int64_t)x0;
}

static int features(uint32_t fn)
{
    return (int)(int32_t)psci_call(PSCI_FEATURES, fn, 0, 0);
}

static void put_dec(unsigned v)
{
    char b[12];
    int i = 0;
    do { b[i++] = (char)('0' + v % 10); v /= 10; } while (v && i < 11);
    while (i) { uart_putc(b[--i]); }
}

static void put_yes(const char *name, int r)
{
    uart_puts(name);
    uart_puts(r >= 0 ? " yes" : " no");
}

void psci_probe(void)
{
    uint32_t v;

    psci_info.smc = boot_entry_el >= 2;
    uart_puts("[psci] entered at EL");
    put_dec(boot_entry_el);
    uart_puts(psci_info.smc ? ", calling the firmware with SMC\r\n" : ", calling the firmware with HVC\r\n");

    v = (uint32_t)psci_call(PSCI_VERSION, 0, 0, 0);
    if ((int32_t)v == PSCI_NOT_SUPPORTED || v == 0) {
        psci_info.checked = 1;
        psci_info.version = 0;
        uart_puts("[psci] no PSCI\r\n");
        return;
    }
    psci_info.version = v;
    uart_puts("[psci] version ");
    put_dec(v >> 16);
    uart_putc('.');
    put_dec(v & 0xFFFFu);
    uart_puts("\r\n");

    /* PSCI_FEATURES は 1.0 から */
    if ((v >> 16) >= 1) {
        psci_info.cpu_suspend    = features(PSCI_CPU_SUSPEND64);
        psci_info.system_suspend = features(PSCI_SYSTEM_SUSPEND64);
        psci_info.system_off     = features(PSCI_SYSTEM_OFF);
        psci_info.system_reset   = features(PSCI_SYSTEM_RESET);
        uart_puts("[psci] ");
        put_yes("CPU_SUSPEND", psci_info.cpu_suspend);
        put_yes(", SYSTEM_SUSPEND", psci_info.system_suspend);
        put_yes(", SYSTEM_OFF", psci_info.system_off);
        put_yes(", SYSTEM_RESET", psci_info.system_reset);
        uart_puts("\r\n");
    } else {
        psci_info.cpu_suspend = psci_info.system_suspend = PSCI_NOT_SUPPORTED;
        psci_info.system_off = psci_info.system_reset = PSCI_NOT_SUPPORTED;
        uart_puts("[psci] version 0.x: PSCI_FEATURES is not available\r\n");
    }
    psci_info.checked = 1;
}
