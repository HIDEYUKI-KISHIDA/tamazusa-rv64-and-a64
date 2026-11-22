#include <stdint.h>

/* 装置のアドレスのずれ（rv_priv.h の RV_IO）。ページングを有効にすると mmu_sv39.c が設定する */
uintptr_t rv_io_offset = 0;

#define UART0_BASE  (0x10000000UL + rv_io_offset)
#define UART_THR    0
#define UART_RBR    0       /* 受信（読み出し） */
#define UART_LSR    5       /* 状態: ビット 0 = 受信データあり */

/* デスクトップの端末の窓へも写す（kernel/gui/desktop.c。ないときは何もしない） */
extern void rv_con_tap(char c) __attribute__((weak));

void uart_putc(char c)
{
    volatile unsigned char *uart = (volatile unsigned char *)UART0_BASE;
    uart[UART_THR] = (unsigned char)c;
    if (rv_con_tap) {
        rv_con_tap(c);
    }
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

/* 受信した 1 文字（0〜255）。なければ -1（待たない） */
int uart_getc_nb(void)
{
    volatile unsigned char *uart = (volatile unsigned char *)UART0_BASE;

    if (uart[UART_LSR] & 1) {
        return uart[UART_RBR];
    }
    return -1;
}

void uart_pl011_init(void)
{
}
