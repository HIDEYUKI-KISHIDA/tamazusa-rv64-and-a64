/* kernel_exception.c — 例外の処理（AArch64 版。RISC-V 版 cpu/rv64/kernel_exception.c と同じ働き）
 *
 *   EL1 の同期例外: 試験が予告した例外（rv_fault_expect）なら 1 回だけ受け流す。それ以外は表示して止まる。
 *   EL0 の svc: rv_user_syscall（ネイティブの試験のプログラムのシステムコール）
 *   EL0 のそれ以外の例外: rv_user_trap がプログラムを終わらせる
 *   原因は ESR の EC（[31:26]）、アドレスは FAR
 */
#include <stdint.h>
#include <stddef.h>
#include "rv_priv.h"
#include "cpu_attr.h"
#include "m5b_trace.h"

extern void uart_puts(const char *s);
extern void uart_putc(char c);

volatile int       rv_fault_armed;
volatile uint64_t  rv_fault_want_cause;
volatile uint64_t  rv_fault_want_tval;
volatile int       rv_fault_hit;
volatile uint64_t  rv_fault_got_cause;
volatile uint64_t  rv_fault_got_tval;

static void put_hex(uint64_t v)
{
    static const char hex[] = "0123456789abcdef";
    int i;
    uart_puts("0x");
    for (i = 60; i >= 0; i -= 4) {
        uart_putc(hex[(v >> i) & 0xF]);
    }
}

/* テストから: この原因（EC）・アドレスの例外を 1 回だけ受け流す */
void rv_fault_expect(uint64_t cause, uint64_t tval)
{
    rv_fault_hit = 0;
    rv_fault_want_cause = cause;
    rv_fault_want_tval = tval;
    rv_fault_armed = 1;
}

static const char *ec_name(uint32_t ec)
{
    switch (ec) {
    case 0x00: return "Unknown reason (undefined instruction)";
    case 0x07: return "FP/SIMD access trapped";
    case 0x15: return "SVC (AArch64)";
    case 0x18: return "MSR/MRS trapped";
    case 0x20: return "Instruction Abort (lower EL)";
    case 0x21: return "Instruction Abort (same EL)";
    case 0x22: return "PC alignment fault";
    case 0x24: return "Data Abort (lower EL)";
    case 0x25: return "Data Abort (same EL)";
    case 0x26: return "SP alignment fault";
    case 0x2F: return "SError";
    case 0x3C: return "BRK";
    default:   return "other";
    }
}

static void dump(const char *title, const uint64_t *f)
{
    uint64_t esr = f[TF_CAUSE];
    uart_puts("\r\n!!! KERNEL PANIC: ");
    uart_puts(title);
    uart_puts(" !!!\r\nESR ");
    put_hex(esr);
    uart_puts(" (");
    uart_puts(ec_name((uint32_t)((esr >> 26) & 0x3F)));
    uart_puts(")\r\nELR ");
    put_hex(f[TF_PC]);
    uart_puts("  FAR ");
    put_hex(f[TF_TVAL]);
    uart_puts("\r\nSPSR ");
    put_hex(f[TF_STATUS]);
    uart_puts("  SP ");
    put_hex(f[TF_SP]);
    uart_puts("  X30 ");
    put_hex(f[30]);
    uart_puts("\r\n");
    m5b_trace_dump();
}

int kernel_sync_exception_handler(uint64_t *f)
{
    uint64_t ec = (f[TF_CAUSE] >> 26) & 0x3F;

    if (rv_fault_armed && ec == rv_fault_want_cause && f[TF_TVAL] == rv_fault_want_tval) {
        rv_fault_armed = 0;
        rv_fault_hit = 1;
        rv_fault_got_cause = ec;
        rv_fault_got_tval = f[TF_TVAL];
        f[TF_PC] += 4;                  /* 例外を起こした命令を飛ばす */
        return 1;
    }
    dump("EL1 synchronous exception", f);
#if TK2_TEST_EXIT
    {
        extern void rv_poweroff(int pass);
        uart_puts("[TEST] FAILED\r\n");
        rv_poweroff(0);
    }
#endif
    return 0;
}

void kernel_bad_exception_handler(uint64_t *f)
{
    dump("unexpected exception vector", f);
}

#if TK2_MMU
/*
 * EL0 のプログラムのシステムコール（試験用のネイティブの番号）
 *   番号は x8、引数は x0〜x2、戻り値は x0
 *   戻り値: 0 = ユーザーへ戻る / 1 = プログラムの終了（終了コードは x0）
 */
#define SYS_PUTS        1
#define SYS_EXIT        2
#define SYS_SLEEP       3
#define SYS_GETTID      4

extern size_t rv_user_copy_str(char *dst, uintptr_t uva, size_t max);
extern int tk_dly_tsk(unsigned long dlytim);
extern int tk_get_tid(void);

/* M10 の試験用: SYS_PUTS を呼んだタスク ID の記録 */
#define RV_PUTS_LOG_MAX 32
volatile int rv_user_puts_log[RV_PUTS_LOG_MAX];
volatile int rv_user_puts_count;

volatile uint64_t rv_user_fault_cause;
volatile uint64_t rv_user_fault_tval;

#define SYS_OPEN        5
#define SYS_READ        6
#define SYS_CLOSE       7
#define SYS_SPAWN       8
#define SYS_GETC        9
#define SYS_DIRENT      10
extern void rv_console_puts(const char *s);
extern int  rv_console_getc(void);
extern long rv_sys_dirent(long index, uintptr_t ubuf, uint64_t len);
extern long rv_sys_open(uintptr_t upath);
extern long rv_sys_read(long fd, uintptr_t ubuf, uint64_t len);
extern long rv_sys_close(long fd);
extern long rv_sys_spawn(uintptr_t upath, long arg, uintptr_t uargs);

#if TK2_LINUX
struct rv_uproc;
extern struct rv_uproc *rv_linux_current(void);
extern int rv_linux_syscall(struct rv_uproc *p, uint64_t *f);
#endif

int rv_user_syscall(uint64_t *f)
{
#if TK2_LINUX
    /* Linux 互換のプログラムなら、Linux の番号（asm-generic、RISC-V と同じ）で処理する */
    {
        struct rv_uproc *lp = rv_linux_current();
        if (lp != NULL) {
            return rv_linux_syscall(lp, f);
        }
    }
#endif
    uint64_t nr = f[TF_NR];
    uint64_t a0 = f[TF_A0];
    uint64_t a1 = f[TF_A0 + 1];
    uint64_t a2 = f[TF_A0 + 2];

    switch (nr) {
    case SYS_OPEN:
        f[TF_A0] = (uint64_t)rv_sys_open((uintptr_t)a0);
        return 0;
    case SYS_READ:
        f[TF_A0] = (uint64_t)rv_sys_read((long)a0, (uintptr_t)a1, a2);
        return 0;
    case SYS_CLOSE:
        f[TF_A0] = (uint64_t)rv_sys_close((long)a0);
        return 0;
    case SYS_SPAWN:
        f[TF_A0] = (uint64_t)rv_sys_spawn((uintptr_t)a0, (long)a1, (uintptr_t)a2);
        return 0;
    case SYS_GETC:
        f[TF_A0] = (uint64_t)(int64_t)rv_console_getc();
        return 0;
    case SYS_DIRENT:
        f[TF_A0] = (uint64_t)rv_sys_dirent((long)a0, (uintptr_t)a1, a2);
        return 0;
    case SYS_PUTS:
        {
            char buf[128];
            int i = rv_user_puts_count;
            if (i < RV_PUTS_LOG_MAX) {
                rv_user_puts_log[i] = tk_get_tid();
                rv_user_puts_count = i + 1;
            }
            f[TF_A0] = (uint64_t)rv_user_copy_str(buf, (uintptr_t)a0, sizeof(buf));
            rv_console_puts(buf);       /* 画面（とテストの記録）へ */
        }
        return 0;
    case SYS_EXIT:
        rv_user_fault_cause = 0;
        return 1;                       /* x0 = 終了コードのまま */
    case SYS_SLEEP:
        f[TF_A0] = (uint64_t)(int64_t)tk_dly_tsk((unsigned long)a0);
        return 0;
    case SYS_GETTID:
        f[TF_A0] = (uint64_t)(int64_t)tk_get_tid();
        return 0;
    default:
        f[TF_A0] = (uint64_t)-1;        /* 未対応のシステムコール */
        return 0;
    }
}

/* EL0 の例外: プログラムを終わらせる（原因は EC） */
int rv_user_trap(uint64_t *f)
{
    uint64_t ec = (f[TF_CAUSE] >> 26) & 0x3F;

    uart_puts("[user] killed: ");
    uart_puts(ec_name((uint32_t)ec));
    uart_puts(" elr=");
    put_hex(f[TF_PC]);
    uart_puts(" far=");
    put_hex(f[TF_TVAL]);
    uart_puts("\r\n");
    rv_user_fault_cause = ec;
    rv_user_fault_tval = f[TF_TVAL];
    f[TF_A0] = (uint64_t)-1;            /* 終了コード */
    return 2;
}
#endif
