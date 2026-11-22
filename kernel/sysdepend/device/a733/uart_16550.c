/* uart_16550.c — Allwinner A733 の UART0（16550 互換、DesignWare。レジスタは 4 バイトおき）
 *
 *   通信の速さ（115200bps、8N1）は U-Boot が設定済みなので、そのまま使う。
 *   いまは送信と、見て回る形の受信だけ（受信割込みはこのあと）。
 */
#include <stdint.h>
#include <board_hw.h>
#include <rv_priv.h>

#define REG(off)   (*(volatile uint32_t *)(RV_IO(BOARD_UART_BASE) + (off)))
#define UART_THR   REG(0x00)    /* 送信（書き込み） */
#define UART_RBR   REG(0x00)    /* 受信（読み出し） */
#define UART_IER   REG(0x04)
#define UART_LSR   REG(0x14)
#define LSR_DR     (1u << 0)    /* 受信データあり */
#define LSR_THRE   (1u << 5)    /* 送信の入れ物が空 */

extern int tk_dly_tsk(long tmo);

void uart_pl011_init(void)      /* 名前は QEMU 版（PL011）と同じにしてある */
{
    UART_IER = 0;               /* 割込みは使わない（いまは） */
}

void uart_putc(char c)
{
    while (!(UART_LSR & LSR_THRE)) { }
    UART_THR = (uint32_t)(unsigned char)c;
}

void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') {
            uart_putc('\r');
        }
        uart_putc(*s++);
    }
}

int uart_getc(void)
{
    while (!(UART_LSR & LSR_DR)) { }
    return (int)(UART_RBR & 0xFFu);
}

/* ---- 受信（見て回る形） ---- */
int rv_uart_try_getc(void)
{
    return (UART_LSR & LSR_DR) ? (int)(UART_RBR & 0xFFu) : -1;
}

int rv_uart_rx_available(void)
{
    return (UART_LSR & LSR_DR) ? 1 : 0;
}

int rv_uart_getc_wait_intr(int (*intr)(void))
{
    int c;
    while ((c = rv_uart_try_getc()) < 0) {
        if (intr != 0 && intr()) {
            return -1;
        }
        tk_dly_tsk(10);
    }
    return c;
}

int rv_uart_getc_wait(void)
{
    return rv_uart_getc_wait_intr(0);
}

void rv_uart_irq_init(void) { }
int  rv_uart_irq(unsigned irq) { (void)irq; return 0; }
void rv_uart_loopback(int on) { (void)on; }
volatile unsigned long rv_uart_irq_count;
