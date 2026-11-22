/* kernel/tk/m37_desktop_test.c — M37: デスクトップの入口（作業の 7 番）（SMODE=1 かつ MMU=1）
 *
 *   画面全体（1024×768）をカーネルの中で組み立てている（kernel/gui/desktop.c）。
 *   1. 端末の窓: UART に出した文字（ASCII・漢字・半角カナ・罫線、UTF-8）が、桁と行に正しく入る（全角は 2 桁）。
 *      VT100 の色（ESC [31m）と位置（ESC [行;桁H、ESC 7・8）
 *   2. 漢字の点が窓の中に描かれている。上の帯の色と、時計の文字がある
 *   3. マウス: 題の帯をつかんで窓を動かし、戻す
 *   4. 画面の窓: 絵が書かれると現れて上に来る。窓の中の位置が /dev/mouse の位置（640×400）になり、
 *      ボタンは中身の上でだけ渡る。端末の窓を押すと端末が上に来て、重なったところではボタンが渡らない
 *   5. 矢印が描かれている
 *
 * 合格の目印: [M37] PASS
 */
#include <stdint.h>
#include <stddef.h>
#include "../gui/desktop.h"

extern void uart_puts(const char *s);
extern int virtio_gpu_present(void);
extern volatile int rv_mouse_x, rv_mouse_y, rv_mouse_buttons;
extern volatile unsigned long rv_gpu_errors;

static int check(int ok, const char *what)
{
    uart_puts(ok ? "[M37] OK   " : "[M37] NG   ");
    uart_puts(what);
    uart_puts("\r\n");
    return ok;
}

/* 端末の窓で "M37MARK" のある行（なければ -1） */
static int find_mark(void)
{
    static const char m[] = "M37MARK";
    int r, c, k;
    for (r = 24; r >= 0; r--) {
        for (c = 0; c + 7 <= 80; c++) {
            for (k = 0; k < 7 && desktop_cell(c + k, r) == m[k]; k++) {
            }
            if (k == 7) {
                return r * 100 + c;
            }
        }
    }
    return -1;
}

int m37_run_desktop_test(void)
{
    int pass = 1, pos, r, c, x, y, w, h, x2, y2, gx, gy, lit, i, j;

    uart_puts("\r\n[M37] desktop (screen composed in the kernel) test\r\n");
    if (!virtio_gpu_present() || !desktop_running()) {
        uart_puts("[M37] SKIP (no display device)\r\n");
        return 1;
    }
    desktop_raise(0);

    /* 1. 文字 */
    uart_puts("[M37] M37MARK 玉梓ｱ\x1b[31mRED\x1b[0m ─ 端\r\n");
    uart_puts("\x1b" "7\x1b[1;70HTOPRIGHT\x1b" "8");
    desktop_wait_idle();
    pos = find_mark();
    r = pos / 100;
    c = pos % 100;
    {
        /* 次の行を出すと上へ流れるので、先に全部見る */
        int vt = desktop_cell(69, 0) == 'T' && desktop_cell(75, 0) == 'H' && desktop_cell(76, 0) == 'T'
                 && (desktop_cell_color(c + 13, r) & 15) == 1 && (desktop_cell_color(c + 16, r) & 15) == 7;
        desktop_window_rect(0, &x, &y, &w, &h);
        lit = 0;
        if (pos >= 0) {
            for (j = 0; j < 16; j++) {
                for (i = 0; i < 16; i++) {
                    lit += desktop_pixel((unsigned)(x + (c + 8) * 8 + i), (unsigned)(y + r * 16 + j)) == 0xcbcccd;
                }
            }
        }
    pass &= check(pos >= 0 && desktop_cell(c + 7, r) == ' ' && desktop_cell(c + 8, r) == 0x7389    /* 玉 */
                  && desktop_cell(c + 10, r) == 0x6893 && desktop_cell(c + 12, r) == 0xff71      /* 梓・ｱ */
                  && desktop_cell(c + 13, r) == 'R' && desktop_cell(c + 16, r) == ' '
                  && desktop_cell(c + 17, r) == 0x2500 && desktop_cell(c + 19, r) == 0x7aef,      /* ─・端 */
                  "UTF-8 text in the terminal window: kanji take 2 columns, halfwidth kana and box lines 1");
    pass &= check(pos >= 0 && vt,
                  "VT100: color (ESC [31m / [0m), cursor position (ESC [1;70H) with save and restore (ESC 7 / 8)");
    }

    /* 2. 点（lit は上で数えた） */
    {
        int clock = 0;
        for (j = 0; j < 24; j++) {
            for (i = DESK_W - 200; i < DESK_W; i++) {
                clock += desktop_pixel((unsigned)i, (unsigned)j) == 0xe8ecf2;
            }
        }
        pass &= check(lit >= 20 && lit <= 200 && desktop_pixel(DESK_W / 2, 2) == 0x1d2129 && clock > 50
                      && rv_gpu_errors == 0,
                      "kanji drawn in the window (Unifont 16 dots), top bar with clock, no device errors");
    }

    /* 3. 窓を動かす */
    desktop_pointer(x + 100, y - 12, 0);
    desktop_pointer(x + 100, y - 12, 1);
    desktop_pointer(x + 130, y + 8, 1);
    desktop_pointer(x + 130, y + 8, 0);
    desktop_window_rect(0, &x2, &y2, &w, &h);
    desktop_pointer(x2 + 100, y2 - 12, 1);
    desktop_pointer(x2 + 70, y2 - 32, 1);
    desktop_pointer(x2 + 70, y2 - 32, 0);
    desktop_window_rect(0, &i, &j, &w, &h);
    pass &= check(x2 == x + 30 && y2 == y + 20 && i == x && j == y,
                  "mouse: drag the terminal window by its title bar, and back");

    /* 4. 画面の窓 */
    desktop_app_damage(0, 1);
    desktop_window_rect(1, &gx, &gy, &w, &h);
    {
        int top_after_draw = desktop_top_window(), mx, my, mb, mb2, top_after_click;
        desktop_pointer(gx + 100, gy + 50, 1);
        mx = rv_mouse_x;
        my = rv_mouse_y;
        mb = rv_mouse_buttons;
        desktop_pointer(gx + 100, gy + 50, 0);
        desktop_pointer(x + 5, y + 5, 1);                       /* 端末の中身を押す */
        desktop_pointer(x + 5, y + 5, 0);
        top_after_click = desktop_top_window();
        desktop_pointer(gx + 10, gy + 10, 1);                   /* 画面の中身だが、端末が上に重なっている */
        mb2 = rv_mouse_buttons;
        desktop_pointer(gx + 10, gy + 10, 0);
        pass &= check(top_after_draw == 1 && w == APP_W && h == APP_H && mx == 100 && my == 50 && mb == 1,
                      "graphics window appears on top when drawn; /dev/mouse gets the position inside it (640x400)");
        pass &= check(top_after_click == 0 && mb2 == 0,
                      "click raises the terminal window; no buttons to the program where another window covers it");
    }

    /* 5. 矢印 */
    desktop_pointer(900, 100, 0);
    desktop_wait_idle();
    pass &= check(desktop_pixel(900, 100) == 0x000000 && desktop_pixel(901, 102) == 0xffffff
                  && desktop_pixel(899, 100) != 0x000000,
                  "mouse arrow drawn at the pointer");

    uart_puts(pass ? "[M37] PASS\r\n" : "[M37] FAIL\r\n");
    return pass;
}
