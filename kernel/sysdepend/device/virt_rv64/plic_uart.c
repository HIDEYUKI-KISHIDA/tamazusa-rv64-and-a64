/*
 * plic_uart.c — 割込みコントローラ（PLIC）と UART の受信割込み（S-mode + MMU のみ）
 *
 *   QEMU virt の PLIC: 0x0C00_0000。hart 0 の S-mode は「コンテキスト 1」。
 *     優先度    : base + 4 * 番号
 *     許可      : base + 0x2000 + 0x80 * コンテキスト（番号ごとに 1 ビット）
 *     しきい値  : base + 0x20_0000 + 0x1000 * コンテキスト
 *     受け取り・完了: しきい値 + 4（読むと番号、同じ番号を書くと完了）
 *   UART0（16550）の割込みは 10 番。IER のビット 0 で受信割込みを許可する。
 *   virtio-mmio の窓 0〜7 は 1〜8 番（virtio_blk.c が rv_plic_enable で許可する）。
 *
 *   割込みが来たら、届いた文字をすべて受信用の入れ物（256 文字）に移し、
 *   入力を待っているタスクを起こす。rv_console_getc はそこから取り出し、
 *   空なら眠る（以前の「10ms おきに見る」をやめた）。
 */
#include <stdint.h>
#include "rv_priv.h"

extern int  tk_wup_tsk(int tskid);
extern int  tk_slp_tsk(int tmout);
extern int  tk_get_tid(void);

#define PLIC_BASE       RV_IO(0x0C000000UL)
#define PLIC_CTX        1               /* hart 0 の S-mode */
#define PLIC_PRIO(n)    (PLIC_BASE + 4UL * (n))
#define PLIC_ENABLE     (PLIC_BASE + 0x2000UL + 0x80UL * PLIC_CTX)
#define PLIC_THRESHOLD  (PLIC_BASE + 0x200000UL + 0x1000UL * PLIC_CTX)
#define PLIC_CLAIM      (PLIC_THRESHOLD + 4)

#define IRQ_UART0       10

#define UART0_BASE      RV_IO(0x10000000UL)
#define UART_RBR        0
#define UART_IER        1
#define UART_LSR        5
#define UART_MCR        4
#define MCR_LOOP        0x10            /* 折り返し（送った文字を自分で受け取る） */

#define SIE_SEIE        (1UL << 9)      /* S-mode の外部割込みを許可 */
#define TMO_FEVR        (-1)

#define RX_SIZE         256

static volatile uint8_t  rx_buf[RX_SIZE];
static volatile unsigned rx_head, rx_tail;  /* head = 次に書く、tail = 次に読む */
static volatile int      rx_waiter;         /* 入力を待っているタスク（0 = なし） */
volatile unsigned long   rv_uart_irq_count; /* テスト用: UART の割込みの回数 */
volatile unsigned long   rv_uart_rx_dropped;

static inline volatile uint32_t *reg32(uintptr_t a) { return (volatile uint32_t *)a; }
static inline volatile uint8_t  *uart(unsigned off) { return (volatile uint8_t *)(UART0_BASE + off); }

void rv_plic_init(void)
{
    *reg32(PLIC_PRIO(IRQ_UART0)) = 1;
    *reg32(PLIC_ENABLE) |= (1U << IRQ_UART0);
    *reg32(PLIC_THRESHOLD) = 0;
    *uart(UART_IER) = 0x01;             /* 受信データありで割込み */
    asm volatile("csrs sie, %0" :: "r"(SIE_SEIE));
}

/* ほかの装置の割込みを許可する（virtio-blk など） */
void rv_plic_enable(unsigned irq)
{
    *reg32(PLIC_PRIO(irq)) = 1;
    *reg32(PLIC_ENABLE) |= (1U << irq);
}

extern int virtio_blk_irq(unsigned irq);
__attribute__((weak)) int virtio_net_irq(unsigned irq) { (void)irq; return 0; }
/* 端末（linux_fs.c）: Ctrl-C などをシグナルにしたら 1。割込み処理の中で呼ぶ */
__attribute__((weak)) int rv_tty_rx_hook(int c) { (void)c; return 0; }

static void uart_rx_irq(void)
{
    int woke = 0;

    while (*uart(UART_LSR) & 1) {
        uint8_t c = *uart(UART_RBR);
        unsigned next = (rx_head + 1) % RX_SIZE;
        if (rv_tty_rx_hook(c)) {
            continue;                   /* Ctrl-C など: 端末がシグナルにした（入れ物には入れない） */
        }
        if (next == rx_tail) {
            rv_uart_rx_dropped++;       /* 入れ物がいっぱい */
            continue;
        }
        rx_buf[rx_head] = c;
        rx_head = next;
        woke = 1;
    }
    if (woke && rx_waiter > 0) {
        int t = rx_waiter;
        rx_waiter = 0;
        tk_wup_tsk(t);
    }
}

/* USB のキーボード（kernel/usb/xhci.c）から: 1 文字をシリアルと同じ入力の入れ物へ入れる（タスクから呼ぶ） */
void rv_uart_inject(int c)
{
    unsigned long st;
    int wake = 0;

    asm volatile("csrrc %0, " RV_STR(CSR_XSTATUS) ", %1" : "=r"(st) : "r"((unsigned long)XSTATUS_IE) : "memory");
    if (!rv_tty_rx_hook(c)) {
        unsigned next = (rx_head + 1) % RX_SIZE;
        if (next == rx_tail) {
            rv_uart_rx_dropped++;
        } else {
            rx_buf[rx_head] = (uint8_t)c;
            rx_head = next;
            wake = rx_waiter;
            rx_waiter = 0;
        }
    }
    if (st & XSTATUS_IE) {
        asm volatile("csrs " RV_STR(CSR_XSTATUS) ", %0" :: "r"((unsigned long)XSTATUS_IE) : "memory");
    }
    if (wake > 0) {
        tk_wup_tsk(wake);
    }
}

/* exception.S から（S-mode の外部割込み、割込み禁止のまま）呼ばれる */
void rv_ext_irq_handler(void)
{
    uint32_t irq;

    while ((irq = *reg32(PLIC_CLAIM)) != 0) {
        if (irq == IRQ_UART0) {
            rv_uart_irq_count++;
            uart_rx_irq();
        } else if (!virtio_blk_irq(irq)) {
            (void)virtio_net_irq(irq);
        }
        *reg32(PLIC_CLAIM) = irq;       /* 完了 */
    }
}

/* 1 文字取り出す。なければ -1（待たない） */
int rv_uart_try_getc(void)
{
    unsigned long st;
    int c = -1;

    asm volatile("csrrc %0, sstatus, %1" : "=r"(st) : "r"((unsigned long)XSTATUS_IE));
    if (rx_tail != rx_head) {
        c = rx_buf[rx_tail];
        rx_tail = (rx_tail + 1) % RX_SIZE;
    }
    if (st & XSTATUS_IE) {
        asm volatile("csrs sstatus, %0" :: "r"((unsigned long)XSTATUS_IE));
    }
    return c;
}

/* 受信した文字が残っていれば 1 */
int rv_uart_rx_available(void)
{
    return rx_tail != rx_head;
}

/* 1 文字取り出す。なければ届くまで眠る */
int rv_uart_getc_wait(void)
{
    for (;;) {
        unsigned long st;
        int c = -1;

        asm volatile("csrrc %0, sstatus, %1" : "=r"(st) : "r"((unsigned long)XSTATUS_IE));
        if (rx_tail != rx_head) {
            c = rx_buf[rx_tail];
            rx_tail = (rx_tail + 1) % RX_SIZE;
        } else {
            rx_waiter = tk_get_tid();
        }
        if (st & XSTATUS_IE) {
            asm volatile("csrs sstatus, %0" :: "r"((unsigned long)XSTATUS_IE));
        }
        if (c >= 0) {
            return c;
        }
        /* 割込みが先に来ていても、起床要求が数えられているのですぐ戻る */
        tk_slp_tsk(TMO_FEVR);
    }
}

/* 1 文字取り出す。なければ届くまで眠るが、intr() が 1 を返したら -1（シグナルで中断） */
int rv_uart_getc_wait_intr(int (*intr)(void))
{
    for (;;) {
        unsigned long st;
        int c = -1;

        if (intr != 0 && intr()) {
            return -1;
        }
        asm volatile("csrrc %0, sstatus, %1" : "=r"(st) : "r"((unsigned long)XSTATUS_IE));
        if (rx_tail != rx_head) {
            c = rx_buf[rx_tail];
            rx_tail = (rx_tail + 1) % RX_SIZE;
        } else {
            rx_waiter = tk_get_tid();
        }
        if (st & XSTATUS_IE) {
            asm volatile("csrs sstatus, %0" :: "r"((unsigned long)XSTATUS_IE));
        }
        if (c >= 0) {
            return c;
        }
        tk_slp_tsk(TMO_FEVR);           /* 文字が届くか、シグナルを送られると起こされる */
    }
}

/* テスト用: 折り返し（ループバック）の切り替え。折り返し中は画面に出ない */
void rv_uart_loopback(int on)
{
    uint8_t m = *uart(UART_MCR);
    *uart(UART_MCR) = on ? (uint8_t)(m | MCR_LOOP) : (uint8_t)(m & ~MCR_LOOP);
}
