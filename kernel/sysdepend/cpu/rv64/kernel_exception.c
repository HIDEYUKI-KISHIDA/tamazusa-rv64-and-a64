/*
 * kernel_exception.c — 同期例外（ページフォールト、不正命令など）の処理
 *
 * exception.S の .Lsync_trap から、288B トラップフレームのアドレスを受け取る。
 *   frame[31] = xepc, frame[32] = xstatus, frame[33] = xcause, frame[34] = xtval
 * 戻り値 1: frame の xepc から復帰する / 0: 停止する
 *
 * テスト用に「予告された例外」を 1 回だけ受け流せる（rv_fault_expect）。
 * 予告と一致すれば、例外を起こした命令を飛ばして復帰する。
 */
#include <stdint.h>
#include "rv_priv.h"

extern void uart_puts(const char *s);

/* テストから設定する: この原因・アドレスの例外を 1 回だけ受け流す */
volatile int       rv_fault_armed;
volatile uint64_t  rv_fault_want_cause;
volatile uint64_t  rv_fault_want_tval;
volatile int       rv_fault_hit;
volatile uint64_t  rv_fault_got_cause;
volatile uint64_t  rv_fault_got_tval;

static void put_hex(uint64_t v)
{
    static const char hex[] = "0123456789abcdef";
    char buf[19];
    int i;

    buf[0] = '0';
    buf[1] = 'x';
    for (i = 0; i < 16; i++) {
        buf[2 + i] = hex[(v >> (60 - 4 * i)) & 0xf];
    }
    buf[18] = '\0';
    uart_puts(buf);
}

void rv_fault_expect(uint64_t cause, uint64_t tval)
{
    rv_fault_hit = 0;
    rv_fault_want_cause = cause;
    rv_fault_want_tval = tval;
    rv_fault_armed = 1;
}

#if TK2_MMU
/*
 * U-mode からのシステムコール（ecall）と例外
 *   システムコール: exception.S が呼んだタスクの文脈で、割込みを許可して
 *                   rv_user_syscall を呼ぶ（SYS_SLEEP で眠れる）
 *   それ以外の例外: user_trap がプログラムを強制終了する
 *   終了コードはフレームの a0（f[9]）に入れて exception.S に返す
 */
#define SYS_PUTS        1
#define SYS_EXIT        2
#define SYS_SLEEP       3
#define SYS_GETTID      4
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
#include <stddef.h>
extern size_t rv_user_copy_str(char *dst, uintptr_t uva, size_t max);

/* T-Kernel の型: ER = ID = int、RELTIM = unsigned long（LP64） */
extern int tk_dly_tsk(unsigned long dlytim);
extern int tk_get_tid(void);

/* M10 テスト用: SYS_PUTS を呼んだタスク ID の記録 */
#define RV_PUTS_LOG_MAX 32
volatile int rv_user_puts_log[RV_PUTS_LOG_MAX];
volatile int rv_user_puts_count;

volatile uint64_t rv_user_fault_cause;
volatile uint64_t rv_user_fault_tval;

static int64_t sys_puts(uint64_t uaddr)
{
    char buf[128];
    size_t n;

    /* 実行中の空間のページ表をたどり、U と R のあるページだけ読む
     * （カーネルのメモリや未割り当てのアドレスを読ませない） */
    n = rv_user_copy_str(buf, (uintptr_t)uaddr, sizeof(buf));
    rv_console_puts(buf);
    return (int64_t)n;
}

/* 戻り値: 0 = ユーザーへ戻る / 1 = プログラム終了（終了コードは f[9]） */
struct rv_uproc;
extern struct rv_uproc *rv_linux_current(void);
extern int rv_linux_syscall(struct rv_uproc *p, uint64_t *f);

int rv_user_syscall(uint64_t *f)
{
    /* Linux 互換のプログラムなら、RISC-V の Linux の番号で処理する */
    {
        struct rv_uproc *lp = rv_linux_current();
        if (lp != NULL) {
            return rv_linux_syscall(lp, f);
        }
    }

    uint64_t nr = f[16];                /* a7 */
    uint64_t a0 = f[9];                 /* a0 */
    uint64_t a1 = f[10];                /* a1 */
    uint64_t a2 = f[11];                /* a2 */

    switch (nr) {
    case SYS_OPEN:
        f[9] = (uint64_t)rv_sys_open((uintptr_t)a0);
        return 0;
    case SYS_READ:
        f[9] = (uint64_t)rv_sys_read((long)a0, (uintptr_t)a1, a2);
        return 0;
    case SYS_CLOSE:
        f[9] = (uint64_t)rv_sys_close((long)a0);
        return 0;
    case SYS_SPAWN:
        f[9] = (uint64_t)rv_sys_spawn((uintptr_t)a0, (long)a1, (uintptr_t)a2);
        return 0;
    case SYS_GETC:
        f[9] = (uint64_t)(int64_t)rv_console_getc();
        return 0;
    case SYS_DIRENT:
        f[9] = (uint64_t)rv_sys_dirent((long)a0, (uintptr_t)a1, a2);
        return 0;
    case SYS_PUTS:
        {
            int i = rv_user_puts_count;
            if (i < RV_PUTS_LOG_MAX) {
                rv_user_puts_log[i] = tk_get_tid();
                rv_user_puts_count = i + 1;
            }
        }
        f[9] = (uint64_t)sys_puts(a0);
        return 0;
    case SYS_EXIT:
        rv_user_fault_cause = 0;
        return 1;                       /* f[9] = 終了コードのまま */
    case SYS_SLEEP:
        f[9] = (uint64_t)(int64_t)tk_dly_tsk((unsigned long)a0);
        return 0;
    case SYS_GETTID:
        f[9] = (uint64_t)(int64_t)tk_get_tid();
        return 0;
    default:
        f[9] = (uint64_t)-1;            /* 未対応のシステムコール */
        return 0;
    }
}

static int user_trap(uint64_t *f)
{
    uint64_t cause = f[33];

    /* ユーザープログラムを強制終了する */
    uart_puts("[user] killed: cause=");
    put_hex(cause);
    uart_puts(" epc=");
    put_hex(f[31]);
    uart_puts(" tval=");
    put_hex(f[34]);
    uart_puts("\r\n");
    rv_user_fault_cause = cause;
    rv_user_fault_tval = f[34];
    f[9] = (uint64_t)-1;                /* 終了コード */
    return 2;
}
#endif

int kernel_sync_exception_handler(unsigned long trap_frame)
{
    uint64_t *f = (uint64_t *)trap_frame;
    uint64_t epc = f[31];
    uint64_t cause = f[33];
    uint64_t tval = f[34];

    if (rv_fault_armed && cause == rv_fault_want_cause && tval == rv_fault_want_tval) {
        uint16_t insn_lo = *(volatile uint16_t *)epc;
        rv_fault_armed = 0;
        rv_fault_hit = 1;
        rv_fault_got_cause = cause;
        rv_fault_got_tval = tval;
        f[31] = epc + (((insn_lo & 3) == 3) ? 4 : 2);  /* 例外を起こした命令を飛ばす */
        return 1;
    }

#if TK2_MMU
    if ((f[32] & XSTATUS_PP) == 0) {    /* U-mode から */
        return user_trap(f);
    }
#endif

    uart_puts("\r\n[RV64] sync exception — halt\r\n  cause=");
    put_hex(cause);
    uart_puts("\r\n  epc  =");
    put_hex(epc);
    uart_puts("\r\n  tval =");
    put_hex(tval);
    uart_puts("\r\n  ra   =");
    put_hex(f[0]);
    uart_puts("\r\n  sp   =");
    put_hex(f[1]);
    uart_puts("\r\n  s0   =");
    put_hex(f[7]);
    uart_puts("\r\n");
    {
        extern int rv_heap_check(void);
        extern void rv_heap_dump(void);
        if (rv_heap_check() != 0) {   /* カーネルのメモリが壊れていれば一覧も出す */
            rv_heap_dump();
        }
    }
#if TK2_TEST_EXIT
    {
        extern void rv_poweroff(int pass);
        rv_poweroff(0);     /* テスト実行中なら QEMU を失敗（終了コード 1）で終える */
    }
#endif
    return 0;
}
