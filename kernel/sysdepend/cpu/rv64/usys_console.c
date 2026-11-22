/*
 * usys_console.c — ユーザープログラム向けのコンソール（S-mode + MMU のみ）
 *
 *   rv_console_puts : UART へ出す。記録が有効なら記録用の領域にも写す（テスト用）
 *   rv_console_getc : 1 文字読む（届くまで眠って待つ）。
 *                     UART の受信割込み（PLIC）で起こされる（plic_uart.c）。
 *                     テスト用の入力が設定されていれば、UART の代わりにそれを返す
 *                     （使い切ったら 4 = Ctrl-D）
 *
 *   SYS_GETC(9) が rv_console_getc を、SYS_PUTS(1) が rv_console_puts を使う。
 */
#include <stdint.h>
#include <stddef.h>

extern void uart_puts(const char *s);
extern int  rv_uart_getc_wait(void);
extern int  rv_uart_getc_wait_intr(int (*intr)(void));
extern int  rv_uart_try_getc(void);
extern int  rv_uart_rx_available(void);

#define CAPTURE_SIZE    8192

static const char *test_input;
static size_t      test_pos;
static char        capture[CAPTURE_SIZE];
static size_t      capture_len;
static int         capture_on;

void rv_console_set_test_input(const char *s)
{
    test_input = s;
    test_pos = 0;
}

void rv_console_capture(int on)
{
    if (on) {
        capture_len = 0;
        capture[0] = 0;
    }
    capture_on = on;
}

const char *rv_console_captured(void)
{
    return capture;
}

void rv_console_puts(const char *s)
{
    uart_puts(s);
    if (capture_on) {
        while (*s && capture_len < CAPTURE_SIZE - 1) {
            capture[capture_len++] = *s++;
        }
        capture[capture_len] = 0;
    }
}

int rv_console_getc(void)
{
    int c;

    if (test_input != NULL) {
        c = (unsigned char)test_input[test_pos];
        if (c == 0) {
            return 4;                   /* 入力の終わり = Ctrl-D */
        }
        test_pos++;
        return c;
    }
    return rv_uart_getc_wait();
}

/* 1 文字読む。待っている間に intr() が 1 になったら -1（シグナルで中断） */
int rv_console_getc_intr(int (*intr)(void))
{
    if (test_input != NULL) {
        return rv_console_getc();
    }
    return rv_uart_getc_wait_intr(intr);
}

/* 1 文字（なければ -1、待たない）。テスト用の入力を使い切ったら 4 */
int rv_console_try_getc(void)
{
    if (test_input != NULL) {
        return rv_console_getc();
    }
    return rv_uart_try_getc();
}

/* 読める文字があれば 1（テスト用の入力は、使い切っても「終わり」を読めるので 1） */
int rv_console_has_input(void)
{
    if (test_input != NULL) {
        return 1;
    }
    return rv_uart_rx_available();
}
