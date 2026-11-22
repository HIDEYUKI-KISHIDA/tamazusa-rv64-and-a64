/* kernel/gui/desktop.h — デスクトップ（画面全体の合成）。desktop.c */
#ifndef DESKTOP_H
#define DESKTOP_H
#include <stdint.h>

#define DESK_W  1024                /* 画面全体（virtio-gpu の資源 2 番） */
#define DESK_H  768
#define APP_W   640                 /* /dev/fb0 の絵（PC-9801 と同じ 640×400）。「画面」の窓の中に出す */
#define APP_H   400

void desktop_start(uint32_t *screen, uint32_t *app);   /* virtio_gpu.c の初期化のあと 1 回 */
void desktop_app_damage(unsigned y0, unsigned y1);     /* /dev/fb0 の行 y0〜y1-1 が変わった */
void desktop_pointer(int x, int y, int buttons);       /* USB のマウス・タブレット（画面全体の座標） */
void rv_con_tap(char c);                               /* UART へ出た 1 文字（uart_virt.c から） */

/* テスト用 */
int desktop_running(void);
uint32_t desktop_pixel(unsigned x, unsigned y);
int desktop_cell(int col, int row);                    /* 端末の窓の文字（Unicode）。範囲外 -1 */
void desktop_wait_idle(void);                          /* たまっている文字を画面まで出し終えるまで待つ */
void desktop_window_rect(int which, int *x, int *y, int *w, int *h);   /* 0 端末・1 画面の、中身の位置 */
int desktop_top_window(void);                          /* いちばん上の窓（0 端末・1 画面） */
int desktop_cell_color(int col, int row);              /* 文字の色（下 4 ビット 前景、次の 4 ビット 背景） */
void desktop_raise(int which);                         /* 窓をいちばん上に（0 端末・1 画面） */

#endif
