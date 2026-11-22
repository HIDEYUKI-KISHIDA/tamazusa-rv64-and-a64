/* PL011 UART — board bases from board_hw.h (virt / rpi4) */

#include <stdint.h>
#include <board_hw.h>   /* ボードごと（-I の順で選ぶ） */
#include "rv_priv.h"

#define UART_DR    (*(volatile uint32_t *)(RV_IO(BOARD_UART_BASE) + 0x00))
#define UART_FR    (*(volatile uint32_t *)(RV_IO(BOARD_UART_BASE) + 0x18))
#define UART_IBRD  (*(volatile uint32_t *)(RV_IO(BOARD_UART_BASE) + 0x24))
#define UART_FBRD  (*(volatile uint32_t *)(RV_IO(BOARD_UART_BASE) + 0x28))
#define UART_LCR_H (*(volatile uint32_t *)(RV_IO(BOARD_UART_BASE) + 0x2C))
#define UART_CR    (*(volatile uint32_t *)(RV_IO(BOARD_UART_BASE) + 0x30))
#define UART_IMSC  (*(volatile uint32_t *)(RV_IO(BOARD_UART_BASE) + 0x38))

#define FR_TXFF    (1u << 5)
#define FR_RXFE    (1u << 4)
#define CR_UARTEN  (1u << 0)
#define CR_TXE     (1u << 8)
#define CR_RXE     (1u << 9)
#define LCR_FEN    (1u << 4)
#define LCR_WLEN8  (3u << 5)

#if defined(BOARD_RPI4)
#define UART_CLK_HZ  48000000u
#define UART_BAUD    115200u
#endif

#if defined(BOARD_RPI5)
/* RP1 UART: firmware sets clock; 8N1 without divisors works for early bring-up */
#endif

void uart_pl011_init(void)
{
#if defined(BOARD_RPI4)
    uint32_t div, frac;

    UART_CR = 0;
    div = (UART_CLK_HZ + (8u * UART_BAUD) / 2u) / (8u * UART_BAUD);
    if (div == 0u) { div = 1u; }
    frac = ((UART_CLK_HZ * 64u) / (UART_BAUD * div) + 1u) / 2u;
    if (frac >= 64u) { frac = 0; }
    UART_IBRD = div;
    UART_FBRD = frac;
    UART_LCR_H = LCR_FEN | LCR_WLEN8;
    UART_IMSC = 0;
    UART_CR = CR_UARTEN | CR_TXE | CR_RXE;
#elif defined(BOARD_RPI5)
    UART_CR = 0;
    UART_LCR_H = LCR_FEN | LCR_WLEN8;
    UART_IMSC = 0;
    UART_CR = CR_UARTEN | CR_TXE | CR_RXE;
#else
    UART_CR = 0;
    UART_LCR_H = LCR_FEN | LCR_WLEN8;
    UART_IMSC = 0;
    UART_CR = CR_UARTEN | CR_TXE | CR_RXE;
#endif
}

/* デスクトップの端末の窓へも写す（kernel/gui/desktop.c。ないときは何もしない） */
extern void rv_con_tap(char c) __attribute__((weak));

void uart_putc(char c)
{
    while (UART_FR & FR_TXFF) { }
    UART_DR = (uint32_t)(unsigned char)c;
    if (rv_con_tap) {
        rv_con_tap(c);
    }
}

void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') { uart_putc('\r'); }
        uart_putc(*s++);
    }
}

int uart_getc(void)
{
    if (UART_FR & FR_RXFE) { return -1; }
    return (int)(UART_DR & 0xFF);
}

/* ---- 受信（割込み） ----
 *   PL011 の受信割込み（RXIM: FIFO に半分たまった、RTIM: 少し待っても次が来ない）で、届いた文字を
 *   受信用の入れ物（256 文字）に移し、入力を待っているタスクを起こす（RISC-V 版の plic_uart.c と同じ）。
 *   QEMU virt の UART0 の割込みは SPI 1（INTID 33）。
 */
#include <rv_priv.h>
#define UART_RIS   (*(volatile uint32_t *)(RV_IO(BOARD_UART_BASE) + 0x3C))
#define UART_MIS   (*(volatile uint32_t *)(RV_IO(BOARD_UART_BASE) + 0x40))
#define UART_ICR   (*(volatile uint32_t *)(RV_IO(BOARD_UART_BASE) + 0x44))
#define IM_RX      (1u << 4)
#define IM_RT      (1u << 6)
#define CR_LBE     (1u << 7)        /* 折り返し（送った文字を自分で受け取る） */
#define UART_IRQ   33
#define RX_SIZE    256
#define TMO_FEVR   (-1)

extern int  tk_wup_tsk(int tskid);
extern int  tk_slp_tsk(int tmout);
extern int  tk_get_tid(void);
extern void gic_spi_enable(unsigned irq);

static volatile uint8_t  rx_buf[RX_SIZE];
static volatile unsigned rx_head, rx_tail;  /* head = 次に書く、tail = 次に読む */
static volatile int      rx_waiter;         /* 入力を待っているタスク（0 = なし） */
volatile unsigned long   rv_uart_irq_count; /* テスト用: UART の割込みの回数 */
volatile unsigned long   rv_uart_rx_dropped;

/* 端末（linux_fs.c）: Ctrl-C などをシグナルにしたら 1。割込み処理の中で呼ぶ */
__attribute__((weak)) int rv_tty_rx_hook(int c) { (void)c; return 0; }

/* 受信割込みを有効にする（MMU と GIC の準備のあと） */
void rv_uart_irq_init(void)
{
    UART_ICR = 0x7FFu;
    UART_IMSC = IM_RX | IM_RT;
    gic_spi_enable(UART_IRQ);
}

/* 割込み処理（タスク独立部）から。UART の割込みなら 1 */
int rv_uart_irq(unsigned irq)
{
    int woke = 0;

    if (irq != UART_IRQ) {
        return 0;
    }
    rv_uart_irq_count++;
    while (!(UART_FR & FR_RXFE)) {
        uint8_t c = (uint8_t)(UART_DR & 0xFFu);
        unsigned next = (rx_head + 1) % RX_SIZE;
        if (rv_tty_rx_hook(c)) {
            continue;
        }
        if (next == rx_tail) {
            rv_uart_rx_dropped++;
            continue;
        }
        rx_buf[rx_head] = c;
        rx_head = next;
        woke = 1;
    }
    UART_ICR = IM_RX | IM_RT;
    if (woke && rx_waiter > 0) {
        int t = rx_waiter;
        rx_waiter = 0;
        tk_wup_tsk(t);
    }
    return 1;
}

/* USB のキーボードから: 1 文字をシリアルと同じ入力の入れ物へ入れる（タスクから呼ぶ） */
void rv_uart_inject(int c)
{
    unsigned long st = RV_IRQ_SAVE();
    int wake = 0;

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
    RV_IRQ_RESTORE(st);
    if (wake > 0) {
        tk_wup_tsk(wake);
    }
}

/* 1 文字取り出す。なければ -1（待たない） */
int rv_uart_try_getc(void)
{
    unsigned long st = RV_IRQ_SAVE();
    int c = -1;

    if (rx_tail != rx_head) {
        c = rx_buf[rx_tail];
        rx_tail = (rx_tail + 1) % RX_SIZE;
    }
    RV_IRQ_RESTORE(st);
    return c;
}

int rv_uart_rx_available(void)
{
    return rx_tail != rx_head;
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
        st = RV_IRQ_SAVE();
        if (rx_tail != rx_head) {
            c = rx_buf[rx_tail];
            rx_tail = (rx_tail + 1) % RX_SIZE;
        } else {
            rx_waiter = tk_get_tid();
        }
        RV_IRQ_RESTORE(st);
        if (c >= 0) {
            return c;
        }
        tk_slp_tsk(TMO_FEVR);           /* 割込みが先に来ていても、起床要求が数えられているのですぐ戻る */
    }
}

int rv_uart_getc_wait(void)
{
    return rv_uart_getc_wait_intr(0);
}

/* テスト用: 折り返し（ループバック）の切り替え。折り返し中は画面に出ない */
void rv_uart_loopback(int on)
{
    UART_CR = on ? (UART_CR | CR_LBE) : (UART_CR & ~CR_LBE);
}
