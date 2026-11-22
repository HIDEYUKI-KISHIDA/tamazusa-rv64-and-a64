/* kernel/gui/desktop.c — デスクトップの入口（作業の 7 番）
 *
 *   画面全体（1024×768）を、カーネルの中で組み立てて virtio-gpu に出す。
 *     ・上の帯: 「玉梓 Tamazusa OS」と日付・時刻（RTC、日本時間）
 *     ・端末の窓: シリアル（UART）に出た文字を、そのまま 80 桁 × 25 行で映す。VT100 の主な制御（色・位置・消去・
 *       スクロールの範囲）を解釈する。文字は UTF-8 で、漢字・かなは全角（2 桁）。字形は GNU Unifont（font16.c）
 *     ・画面の窓: /dev/fb0（640×400）の絵。初めて絵が書かれたときに現れる
 *     ・マウスの矢印。窓は、題の帯をつかんで動かせ、押すといちばん上に来る
 *   キーボード（USB）の文字は、これまでどおりシリアルの入力に入る（どの窓が上でも、入力は端末へ）。
 *   組み立ては desk_task が 30 ミリ秒ごとに行い、変わった四角だけを装置へ写す。
 *   UART へ出た文字は rv_con_tap が輪の入れ物（64KiB）にためておく（起動の途中の分も、窓ができたら映る）。
 */
#include <stdint.h>
#include <stddef.h>
#include <tk/tkernel.h>
#include <libstr.h>
#include <rv_priv.h>
#include "desktop.h"
#include "font16.h"

extern int virtio_gpu_show(unsigned x, unsigned y, unsigned w, unsigned h);
extern uint32_t rv_now_unix(void);
extern volatile int rv_mouse_x, rv_mouse_y, rv_mouse_buttons;

#define LOCK(st)   ((st) = RV_IRQ_SAVE())
#define UNLOCK(st) RV_IRQ_RESTORE(st)

#define TZ_OFFSET   (9 * 3600)      /* 日本時間（JST）で出す */
#define BAR_H       24              /* 上の帯 */
#define TITLE_H     22              /* 窓の題の帯 */
#define COLS        80
#define ROWS        25
#define CON_W       (COLS * 8)
#define CON_H       (ROWS * 16)

/* 色（0xRRGGBB。装置の形 B8G8R8X8 を 32 ビットで読んだもの） */
#define C_BAR       0x1d2129
#define C_BAR_TEXT  0xe8ecf2
#define C_FRAME     0x101318
#define C_TITLE_ON  0x3a6ea5
#define C_TITLE_OFF 0x5b636e
#define C_TITLE_TXT 0xffffff

static const uint32_t pal[16] = {
    0x000000, 0xc23621, 0x25bc24, 0xadad27, 0x2f5bd8, 0xd338d3, 0x33bbc8, 0xcbcccd,
    0x818383, 0xfc391f, 0x31e722, 0xeaec23, 0x5c86ff, 0xf935f8, 0x14f0f0, 0xe9ebeb,
};

static uint32_t *scr;               /* 画面全体（DESK_W × DESK_H） */
static uint32_t *app;               /* /dev/fb0（APP_W × APP_H） */
static uint32_t con_px[CON_W * CON_H];
static uint32_t bg_row[DESK_H];     /* 背景の色（縦のグラデーション） */
static volatile int running;

/* ---------------------------------------------------------------- 変わった四角 */
struct rect { int x0, y0, x1, y1; };    /* x1・y1 は含まない */
#define NDMG 16
static struct rect dmg[NDMG];
static int ndmg;

static void damage(int x0, int y0, int x1, int y1)
{
    unsigned long st;
    int i;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > DESK_W) x1 = DESK_W;
    if (y1 > DESK_H) y1 = DESK_H;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    LOCK(st);
    for (i = 0; i < ndmg; i++) {        /* 重なる・接するものとはまとめる */
        struct rect *r = &dmg[i];
        if (x0 <= r->x1 && r->x0 <= x1 && y0 <= r->y1 && r->y0 <= y1) {
            if (x0 < r->x0) r->x0 = x0;
            if (y0 < r->y0) r->y0 = y0;
            if (x1 > r->x1) r->x1 = x1;
            if (y1 > r->y1) r->y1 = y1;
            UNLOCK(st);
            return;
        }
    }
    if (ndmg < NDMG) {
        dmg[ndmg].x0 = x0; dmg[ndmg].y0 = y0; dmg[ndmg].x1 = x1; dmg[ndmg].y1 = y1;
        ndmg++;
    } else {                            /* 入りきらなければ全部を 1 つに */
        for (i = 0; i < ndmg; i++) {
            if (dmg[i].x0 < x0) x0 = dmg[i].x0;
            if (dmg[i].y0 < y0) y0 = dmg[i].y0;
            if (dmg[i].x1 > x1) x1 = dmg[i].x1;
            if (dmg[i].y1 > y1) y1 = dmg[i].y1;
        }
        dmg[0].x0 = x0; dmg[0].y0 = y0; dmg[0].x1 = x1; dmg[0].y1 = y1;
        ndmg = 1;
    }
    UNLOCK(st);
}

/* ---------------------------------------------------------------- 窓 */
struct win {
    int x, y;                       /* 外側の左上 */
    int cw, ch;                     /* 中身の大きさ */
    const char *title;
    int visible;
    const uint32_t *px;             /* 中身の点（幅 cw） */
};
#define NWIN 2
static struct win wins[NWIN];
static int zo[NWIN] = { 1, 0 };     /* 下から上へ。はじめは端末が上 */

static int win_w(const struct win *w) { return w->cw + 2; }
static int win_h(const struct win *w) { return w->ch + TITLE_H + 2; }

static void damage_win(const struct win *w)
{
    damage(w->x, w->y, w->x + win_w(w), w->y + win_h(w));
}

static int top_visible(void)
{
    int i;
    for (i = NWIN - 1; i >= 0; i--) {
        if (wins[zo[i]].visible) {
            return zo[i];
        }
    }
    return -1;
}

static void raise_win(int id)
{
    int i, j;
    for (i = 0; i < NWIN && zo[i] != id; i++) {
    }
    if (i >= NWIN || i == NWIN - 1) {
        return;
    }
    for (j = i; j < NWIN - 1; j++) {
        zo[j] = zo[j + 1];
    }
    zo[NWIN - 1] = id;
    damage_win(&wins[id]);
}

/* ---------------------------------------------------------------- 文字の形 */
static int find16(const uint16_t *code, unsigned n, uint32_t c)
{
    unsigned lo = 0, hi = n;
    if (c > 0xffff) {
        return -1;
    }
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (code[mid] == c) {
            return (int)mid;
        }
        if (code[mid] < c) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return -1;
}

/* 文字の幅（1 か 2）と形。見つからなければ U+FFFD */
static int glyph(uint32_t c, const uint8_t **nb, const uint16_t **wb)
{
    int i;
    if (c >= 0x20 && c < 0x7f) {
        *nb = font16_narrow_bits[c - 0x20];         /* 表の頭は ASCII が順に並ぶ */
        return 1;
    }
    i = find16(font16_narrow_code, font16_narrow_n, c);
    if (i >= 0) {
        *nb = font16_narrow_bits[i];
        return 1;
    }
    i = find16(font16_wide_code, font16_wide_n, c);
    if (i >= 0) {
        *wb = font16_wide_bits[i];
        return 2;
    }
    if (c != 0xfffd) {
        return glyph(0xfffd, nb, wb);
    }
    *nb = font16_narrow_bits['?' - 0x20];
    return 1;
}

static int char_width(uint32_t c)
{
    const uint8_t *nb;
    const uint16_t *wb;
    return glyph(c, &nb, &wb);
}

/* dst（幅 stride）の (x, y) に 1 文字。clip の外は描かない。bg が 0xffffffff なら背景は描かない。描いた幅を返す */
static int draw_char(uint32_t *dst, int stride, const struct rect *clip, int x, int y, uint32_t c,
                     uint32_t fg, uint32_t bg)
{
    const uint8_t *nb = NULL;
    const uint16_t *wb = NULL;
    int w = glyph(c, &nb, &wb) * 8;
    int r, k;
    for (r = 0; r < 16; r++) {
        int yy = y + r;
        unsigned bits;
        if (yy < clip->y0 || yy >= clip->y1) {
            continue;
        }
        bits = nb ? nb[r] : wb[r];
        for (k = 0; k < w; k++) {
            int xx = x + k;
            int on = (bits >> (w - 1 - k)) & 1;
            if (xx < clip->x0 || xx >= clip->x1) {
                continue;
            }
            if (on) {
                dst[yy * stride + xx] = fg;
            } else if (bg != 0xffffffffu) {
                dst[yy * stride + xx] = bg;
            }
        }
    }
    return w;
}

/* UTF-8 の文字列を描く。描いた幅を返す */
static int draw_text(int x, int y, const char *s, uint32_t fg, const struct rect *clip)
{
    int x0 = x;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t c = *p++;
        if (c >= 0xc0) {
            int n = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
            c &= 0x3f >> n;
            while (n-- > 0 && (*p & 0xc0) == 0x80) {
                c = (c << 6) | (*p++ & 0x3f);
            }
        }
        x += draw_char(scr, DESK_W, clip, x, y, c, fg, 0xffffffffu);
    }
    return x - x0;
}

static int text_width(const char *s)
{
    int w = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t c = *p++;
        if (c >= 0xc0) {
            int n = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
            c &= 0x3f >> n;
            while (n-- > 0 && (*p & 0xc0) == 0x80) {
                c = (c << 6) | (*p++ & 0x3f);
            }
        }
        w += char_width(c) * 8;
    }
    return w;
}

/* ---------------------------------------------------------------- 端末（VT100 の主なところ） */
struct cell { uint32_t ch; uint8_t fg, bg, wide; };     /* wide: 1 = 全角の左、2 = 全角の右 */
static struct cell cells[ROWS][COLS];
static uint8_t row_dirty[ROWS];
static int cx, cy, wrap_pending, cur_fg = 7, cur_bg = 0, bold, reverse, cur_on = 1, blink_on = 1;
static int top, bot = ROWS - 1, save_x, save_y;
static int prev_cx, prev_cy;
static int esc_state, nparam, params[16], priv;
static uint32_t u8_cp;
static int u8_left;

static void clear_cells(int r, int c0, int c1)
{
    int c;
    for (c = c0; c < c1 && c < COLS; c++) {
        cells[r][c].ch = ' ';
        cells[r][c].fg = 7;
        cells[r][c].bg = (uint8_t)cur_bg;
        cells[r][c].wide = 0;
    }
    row_dirty[r] = 1;
}

static int pend_scroll;              /* まだ con_px に写していない、画面全体の上へのスクロール（行） */

static void scroll_up(int t, int b, int n)
{
    int r;
    if (n > b - t + 1) {
        n = b - t + 1;
    }
    if (t == 0 && b == ROWS - 1 && n < ROWS) {         /* 全体: 描いた点ごとずらす（描き直さない） */
        for (r = 0; r + n < ROWS; r++) {
            memcpy(cells[r], cells[r + n], sizeof(cells[r]));
            row_dirty[r] = row_dirty[r + n];
        }
        for (; r < ROWS; r++) {
            clear_cells(r, 0, COLS);
        }
        pend_scroll += n;
        return;
    }
    for (r = t; r + n <= b; r++) {
        memcpy(cells[r], cells[r + n], sizeof(cells[r]));
        row_dirty[r] = 1;
    }
    for (; r <= b; r++) {
        clear_cells(r, 0, COLS);
    }
}

static void scroll_down(int t, int b, int n)
{
    int r;
    if (n > b - t + 1) {
        n = b - t + 1;
    }
    for (r = b; r - n >= t; r--) {
        memcpy(cells[r], cells[r - n], sizeof(cells[r]));
        row_dirty[r] = 1;
    }
    for (; r >= t; r--) {
        clear_cells(r, 0, COLS);
    }
}

static void line_feed(void)
{
    if (cy == bot) {
        scroll_up(top, bot, 1);
    } else if (cy < ROWS - 1) {
        cy++;
    }
}

/* 全角の片方だけを上書きするときは、もう片方を空白にする */
static void break_wide(int r, int c)
{
    if (cells[r][c].wide == 2 && c > 0) {
        cells[r][c - 1].ch = ' ';
        cells[r][c - 1].wide = 0;
    } else if (cells[r][c].wide == 1 && c + 1 < COLS) {
        cells[r][c + 1].ch = ' ';
        cells[r][c + 1].wide = 0;
    }
}

static void put_cp(uint32_t c)
{
    int w = char_width(c);
    uint8_t fg = (uint8_t)cur_fg, bg = (uint8_t)cur_bg;
    if (bold && fg < 8) {
        fg += 8;
    }
    if (reverse) {
        uint8_t t = fg;
        fg = bg;
        bg = t;
    }
    if (wrap_pending || (w == 2 && cx == COLS - 1)) {
        cx = 0;
        wrap_pending = 0;
        line_feed();
    }
    break_wide(cy, cx);
    cells[cy][cx].ch = c;
    cells[cy][cx].fg = fg;
    cells[cy][cx].bg = bg;
    cells[cy][cx].wide = w == 2 ? 1 : 0;
    if (w == 2) {
        break_wide(cy, cx + 1);
        cells[cy][cx + 1].ch = ' ';
        cells[cy][cx + 1].fg = fg;
        cells[cy][cx + 1].bg = bg;
        cells[cy][cx + 1].wide = 2;
    }
    row_dirty[cy] = 1;
    cx += w;
    if (cx >= COLS) {
        cx = COLS - 1;
        wrap_pending = 1;
    }
}

static int P(int i, int def)
{
    return (i < nparam && params[i] > 0) ? params[i] : def;
}

static void sgr(void)
{
    int i;
    if (nparam == 0) {
        nparam = 1;
        params[0] = 0;
    }
    for (i = 0; i < nparam; i++) {
        int v = params[i];
        if (v == 0) {
            cur_fg = 7; cur_bg = 0; bold = 0; reverse = 0;
        } else if (v == 1) {
            bold = 1;
        } else if (v == 22) {
            bold = 0;
        } else if (v == 7) {
            reverse = 1;
        } else if (v == 27) {
            reverse = 0;
        } else if (v >= 30 && v <= 37) {
            cur_fg = v - 30;
        } else if (v == 39) {
            cur_fg = 7;
        } else if (v >= 40 && v <= 47) {
            cur_bg = v - 40;
        } else if (v == 49) {
            cur_bg = 0;
        } else if (v >= 90 && v <= 97) {
            cur_fg = v - 90 + 8;
        } else if (v >= 100 && v <= 107) {
            cur_bg = v - 100 + 8;
        } else if ((v == 38 || v == 48) && i + 1 < nparam) {
            if (params[i + 1] == 5 && i + 2 < nparam) {         /* 256 色: 16 色までだけ */
                int n = params[i + 2];
                if (n < 16) {
                    if (v == 38) cur_fg = n; else cur_bg = n;
                }
                i += 2;
            } else if (params[i + 1] == 2) {                    /* 24 ビット色: 読み飛ばす */
                i += 4;
            }
        }
    }
}

static void csi(char f)
{
    int n = P(0, 1), r;
    wrap_pending = 0;
    switch (f) {
    case 'A': cy -= n; if (cy < 0) cy = 0; break;
    case 'B': cy += n; if (cy >= ROWS) cy = ROWS - 1; break;
    case 'C': cx += n; if (cx >= COLS) cx = COLS - 1; break;
    case 'D': cx -= n; if (cx < 0) cx = 0; break;
    case 'E': cy += n; if (cy >= ROWS) cy = ROWS - 1; cx = 0; break;
    case 'F': cy -= n; if (cy < 0) cy = 0; cx = 0; break;
    case 'G': case '`': cx = n - 1; if (cx >= COLS) cx = COLS - 1; break;
    case 'd': cy = n - 1; if (cy >= ROWS) cy = ROWS - 1; break;
    case 'H': case 'f':
        cy = P(0, 1) - 1;
        cx = P(1, 1) - 1;
        if (cy >= ROWS) cy = ROWS - 1;
        if (cx >= COLS) cx = COLS - 1;
        break;
    case 'J':
        n = nparam ? params[0] : 0;
        if (n == 0) {
            clear_cells(cy, cx, COLS);
            for (r = cy + 1; r < ROWS; r++) clear_cells(r, 0, COLS);
        } else if (n == 1) {
            clear_cells(cy, 0, cx + 1);
            for (r = 0; r < cy; r++) clear_cells(r, 0, COLS);
        } else {
            for (r = 0; r < ROWS; r++) clear_cells(r, 0, COLS);
        }
        break;
    case 'K':
        n = nparam ? params[0] : 0;
        if (n == 0) clear_cells(cy, cx, COLS);
        else if (n == 1) clear_cells(cy, 0, cx + 1);
        else clear_cells(cy, 0, COLS);
        break;
    case 'L': if (cy >= top && cy <= bot) scroll_down(cy, bot, n); break;
    case 'M': if (cy >= top && cy <= bot) scroll_up(cy, bot, n); break;
    case 'S': scroll_up(top, bot, n); break;
    case 'T': scroll_down(top, bot, n); break;
    case 'P':
        if (n > COLS - cx) n = COLS - cx;
        memmove(&cells[cy][cx], &cells[cy][cx + n], sizeof(struct cell) * (size_t)(COLS - cx - n));
        clear_cells(cy, COLS - n, COLS);
        break;
    case '@':
        if (n > COLS - cx) n = COLS - cx;
        memmove(&cells[cy][cx + n], &cells[cy][cx], sizeof(struct cell) * (size_t)(COLS - cx - n));
        clear_cells(cy, cx, cx + n);
        break;
    case 'X': clear_cells(cy, cx, cx + n); break;
    case 'm': sgr(); break;
    case 'r':
        top = P(0, 1) - 1;
        bot = P(1, ROWS) - 1;
        if (top < 0 || top >= ROWS) top = 0;
        if (bot >= ROWS || bot <= top) { top = 0; bot = ROWS - 1; }
        cx = 0;
        cy = 0;
        break;
    case 's': save_x = cx; save_y = cy; break;
    case 'u': cx = save_x; cy = save_y; break;
    case 'h': case 'l':
        if (priv && nparam && params[0] == 25) {
            cur_on = f == 'h';
            row_dirty[cy] = 1;
        }
        break;
    default:                            /* n（位置の問い合わせ）などは答えない（シリアルの端末が答える） */
        break;
    }
}

static void con_reset(void)
{
    int r;
    cur_fg = 7; cur_bg = 0; bold = 0; reverse = 0; cur_on = 1;
    top = 0; bot = ROWS - 1; cx = 0; cy = 0; wrap_pending = 0;
    for (r = 0; r < ROWS; r++) clear_cells(r, 0, COLS);
}

static void con_byte(unsigned char b)
{
    if (esc_state == 0 && u8_left > 0) {
        if ((b & 0xc0) == 0x80) {
            u8_cp = (u8_cp << 6) | (b & 0x3f);
            if (--u8_left == 0) {
                put_cp(u8_cp);
            }
            return;
        }
        u8_left = 0;
        put_cp(0xfffd);
    }
    switch (esc_state) {
    case 1:                             /* ESC の次 */
        esc_state = 0;
        if (b == '[') {
            esc_state = 2;
            nparam = 0;
            priv = 0;
            params[0] = 0;
        } else if (b == ']') {
            esc_state = 3;
        } else if (b == '(' || b == ')') {
            esc_state = 4;
        } else if (b == '7') {
            save_x = cx; save_y = cy;
        } else if (b == '8') {
            cx = save_x; cy = save_y;
        } else if (b == 'D') {
            line_feed();
        } else if (b == 'E') {
            cx = 0;
            line_feed();
        } else if (b == 'M') {
            if (cy == top) scroll_down(top, bot, 1); else if (cy > 0) cy--;
        } else if (b == 'c') {
            con_reset();
        }
        return;
    case 2:                             /* ESC [ … */
        if (b >= '0' && b <= '9') {
            if (nparam == 0) {
                nparam = 1;
                params[0] = 0;
            }
            if (params[nparam - 1] < 10000) {
                params[nparam - 1] = params[nparam - 1] * 10 + (b - '0');
            }
        } else if (b == ';') {
            if (nparam == 0) {
                nparam = 1;
                params[0] = 0;
            }
            if (nparam < 16) {
                params[nparam++] = 0;
            }
        } else if (b == '?' || b == '>' || b == '=') {
            priv = 1;
        } else if (b >= 0x40 && b <= 0x7e) {
            esc_state = 0;
            csi((char)b);
        } else if (b < 0x20) {
            esc_state = 0;
        }
        return;
    case 3:                             /* ESC ] …（窓の題など）: BEL か ESC で終わり。使わない */
        if (b == 7) {
            esc_state = 0;
        } else if (b == 0x1b) {
            esc_state = 1;
        }
        return;
    case 4:                             /* ESC ( の次の 1 文字（文字の組）: 使わない */
        esc_state = 0;
        return;
    }
    if (b >= 0x80) {
        if (b >= 0xc2 && b < 0xf5) {
            u8_left = b >= 0xf0 ? 3 : b >= 0xe0 ? 2 : 1;
            u8_cp = b & (0x3f >> u8_left);
        } else {
            put_cp(0xfffd);
        }
        return;
    }
    switch (b) {
    case 0x1b: esc_state = 1; break;
    case '\r': cx = 0; wrap_pending = 0; break;
    case '\n': case 0x0b: case 0x0c: line_feed(); break;
    case '\b': if (cx > 0) cx--; wrap_pending = 0; break;
    case '\t': cx = (cx + 8) & ~7; if (cx >= COLS) cx = COLS - 1; break;
    default:
        if (b >= 0x20 && b < 0x7f) {
            put_cp(b);
        }
        break;
    }
}

/* 1 文字を con_px の桁 c・行 r に（はみ出さないので切り取りなし） */
static void cell_draw(int c, int r, uint32_t ch, uint32_t fg, uint32_t bg)
{
    const uint8_t *nb = NULL;
    const uint16_t *wb = NULL;
    int w = glyph(ch, &nb, &wb) * 8, y, k;
    uint32_t *d = con_px + (r * 16) * CON_W + c * 8;
    if (c * 8 + w > CON_W) {
        w = CON_W - c * 8;
    }
    for (y = 0; y < 16; y++, d += CON_W) {
        unsigned bits = nb ? (unsigned)nb[y] << 8 : wb[y];
        for (k = 0; k < w; k++) {
            d[k] = (bits & (0x8000u >> k)) ? fg : bg;
        }
    }
}

/* 行 r を con_px に描く */
static void render_row(int r)
{
    int c;
    for (c = 0; c < COLS; c++) {
        struct cell *e = &cells[r][c];
        uint32_t fg = pal[e->fg & 15], bg = pal[e->bg & 15];
        int is_cur = cur_on && blink_on && r == cy && (c == cx || (e->wide == 2 && c - 1 == cx));
        if (e->wide == 2) {
            if (c == 0 || cells[r][c - 1].wide != 1) {          /* 左がないときは空白 */
                cell_draw(c, r, ' ', fg, bg);
            }
            continue;
        }
        if (is_cur) {
            uint32_t t = fg;
            fg = bg;
            bg = t;
            if (fg == bg) {
                fg = 0x000000;
                bg = 0xcbcccd;
            }
        }
        cell_draw(c, r, e->ch ? e->ch : ' ', fg, bg);
    }
}

/* ---------------------------------------------------------------- UART から来た文字 */
#define RING 65536
static uint8_t ring[RING];
static volatile unsigned ring_head, ring_tail;

void rv_con_tap(char c)
{
    unsigned long st;
    LOCK(st);
    ring[ring_head % RING] = (uint8_t)c;
    ring_head++;
    if (ring_head - ring_tail > RING) {
        ring_tail = ring_head - RING;   /* 古いものから捨てる */
    }
    UNLOCK(st);
}

/* たまった分を解釈する（1 回に max バイトまで）。解釈したバイト数 */
static unsigned con_drain(unsigned max)
{
    unsigned n = 0;
    while (n < max) {
        unsigned long st;
        unsigned char b;
        LOCK(st);
        if (ring_tail == ring_head) {
            UNLOCK(st);
            break;
        }
        b = ring[ring_tail % RING];
        ring_tail++;
        UNLOCK(st);
        con_byte(b);
        n++;
    }
    return n;
}

/* ---------------------------------------------------------------- 組み立て */
static char clock_text[40];
static int clock_min = -1;

static void put2(char *p, int v)
{
    p[0] = (char)('0' + v / 10 % 10);
    p[1] = (char)('0' + v % 10);
}

static void make_clock(uint32_t t)
{
    /* 1970-01-01 からの日数を年月日に（Howard Hinnant の days_from_civil の逆） */
    long z = (long)(t / 86400) + 719468;
    long era = z / 146097;
    long doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = yoe + era * 400;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    long d = doy - (153 * mp + 2) / 5 + 1;
    long m = mp < 10 ? mp + 3 : mp - 9;
    int sec = (int)(t % 86400);
    char *p = clock_text;
    if (m <= 2) {
        y++;
    }
    p[0] = (char)('0' + y / 1000 % 10);
    p[1] = (char)('0' + y / 100 % 10);
    put2(p + 2, (int)(y % 100));
    p[4] = '-';
    put2(p + 5, (int)m);
    p[7] = '-';
    put2(p + 8, (int)d);
    p[10] = ' ';
    put2(p + 11, sec / 3600);
    p[13] = ':';
    put2(p + 14, sec / 60 % 60);
    memcpy(p + 16, " JST", 5);
}

static void compose_win(const struct win *w, int active, const struct rect *c)
{
    int ox = w->x, oy = w->y, ww = win_w(w), wh = win_h(w);
    int x, y;
    struct rect in = { ox, oy, ox + ww, oy + wh };
    struct rect tb;
    if (in.x0 < c->x0) in.x0 = c->x0;
    if (in.y0 < c->y0) in.y0 = c->y0;
    if (in.x1 > c->x1) in.x1 = c->x1;
    if (in.y1 > c->y1) in.y1 = c->y1;
    if (in.x0 >= in.x1 || in.y0 >= in.y1) {
        return;
    }
    for (y = in.y0; y < in.y1; y++) {
        uint32_t *d = scr + y * DESK_W;
        int ly = y - oy;
        if (ly > TITLE_H && ly < wh - 1) {             /* 中身の行: 枠の 2 点のほかはまとめて写す */
            int a = in.x0, b = in.x1;
            if (a == ox) {
                d[a++] = C_FRAME;
            }
            if (b == ox + ww) {
                d[--b] = C_FRAME;
            }
            if (a < b) {
                memcpy(d + a, w->px + (ly - TITLE_H - 1) * w->cw + (a - ox - 1), sizeof(uint32_t) * (size_t)(b - a));
            }
            continue;
        }
        for (x = in.x0; x < in.x1; x++) {
            int lx = x - ox;
            if (lx == 0 || ly == 0 || lx == ww - 1 || ly == wh - 1) {
                d[x] = C_FRAME;
            } else {
                d[x] = active ? C_TITLE_ON : C_TITLE_OFF;
            }
        }
    }
    tb = in;
    if (tb.y1 > oy + 1 + TITLE_H) {
        tb.y1 = oy + 1 + TITLE_H;
    }
    if (tb.x1 > ox + ww - 1) {
        tb.x1 = ox + ww - 1;
    }
    if (tb.y0 < tb.y1) {
        draw_text(ox + 10, oy + 4, w->title, C_TITLE_TXT, &tb);
    }
}

/* 矢印（12×19。# 黒、. 白） */
static const char *const arrow[19] = {
    "#           ", "##          ", "#.#         ", "#..#        ", "#...#       ",
    "#....#      ", "#.....#     ", "#......#    ", "#.......#   ", "#........#  ",
    "#.........# ", "#..........#", "#......#####", "#...#..#    ", "#..# #..#   ",
    "#.#  #..#   ", "##    #..#  ", "      #..#  ", "       ##   ",
};
#define ARROW_W 12
#define ARROW_H 19
static volatile int ptr_x = DESK_W / 2, ptr_y = DESK_H / 2, ptr_shown;
static int ptr_drawn_x = -1000, ptr_drawn_y;

static void compose(const struct rect *c)
{
    int x, y, i, k, top_id;
    struct rect bar;
    for (y = c->y0; y < c->y1; y++) {
        uint32_t *d = scr + y * DESK_W;
        uint32_t col = y < BAR_H ? C_BAR : bg_row[y];
        for (x = c->x0; x < c->x1; x++) {
            d[x] = col;
        }
    }
    bar = *c;
    if (bar.y1 > BAR_H) {
        bar.y1 = BAR_H;
    }
    if (bar.y0 < bar.y1) {
        draw_text(10, 4, "玉梓 Tamazusa OS", C_BAR_TEXT, &bar);
        draw_text(DESK_W - 10 - text_width(clock_text), 4, clock_text, C_BAR_TEXT, &bar);
    }
    top_id = top_visible();
    for (i = 0; i < NWIN; i++) {
        const struct win *w = &wins[zo[i]];
        if (w->visible) {
            compose_win(w, zo[i] == top_id, c);
        }
    }
    if (ptr_shown) {
        int px = ptr_drawn_x, py = ptr_drawn_y;
        for (k = 0; k < ARROW_H; k++) {
            int yy = py + k;
            if (yy < c->y0 || yy >= c->y1) {
                continue;
            }
            for (x = 0; x < ARROW_W; x++) {
                int xx = px + x;
                char a = arrow[k][x];
                if (a == ' ' || xx < c->x0 || xx >= c->x1) {
                    continue;
                }
                scr[yy * DESK_W + xx] = a == '#' ? 0x000000 : 0xffffff;
            }
        }
    }
}

/* ---------------------------------------------------------------- マウス */
static int drag = -1, drag_dx, drag_dy, prev_btn;
volatile unsigned long rv_desk_frames, rv_desk_rects;

void desktop_pointer(int x, int y, int buttons)
{
    unsigned long st;
    struct win *g = &wins[1];
    int ax, ay, in_app;
    if (!running) {
        rv_mouse_x = x * APP_W / DESK_W;
        rv_mouse_y = y * APP_H / DESK_H;
        rv_mouse_buttons = buttons;
        return;
    }
    LOCK(st);
    ptr_x = x;
    ptr_y = y;
    ptr_shown = 1;
    if ((buttons & 1) && !(prev_btn & 1)) {             /* 左を押した: いちばん上の窓を探す */
        int i;
        for (i = NWIN - 1; i >= 0; i--) {
            struct win *w = &wins[zo[i]];
            if (w->visible && x >= w->x && x < w->x + win_w(w) && y >= w->y && y < w->y + win_h(w)) {
                raise_win(zo[i]);
                if (y < w->y + 1 + TITLE_H) {
                    drag = zo[NWIN - 1];
                    drag_dx = x - w->x;
                    drag_dy = y - w->y;
                }
                break;
            }
        }
    } else if (!(buttons & 1)) {
        drag = -1;
    }
    if (drag >= 0 && (buttons & 1)) {
        struct win *w = &wins[drag];
        int nx = x - drag_dx, ny = y - drag_dy;
        if (ny < BAR_H) ny = BAR_H;                     /* 題の帯は上の帯の下に */
        if (ny > DESK_H - TITLE_H) ny = DESK_H - TITLE_H;
        if (nx < -win_w(w) + 40) nx = -win_w(w) + 40;   /* 少しは見えているように */
        if (nx > DESK_W - 40) nx = DESK_W - 40;
        if (nx != w->x || ny != w->y) {
            damage_win(w);
            w->x = nx;
            w->y = ny;
            damage_win(w);
        }
    }
    prev_btn = buttons;
    /* /dev/mouse には「画面」の窓の中の位置を渡す。ボタンは、窓の中身の上で、窓を動かしていないときだけ */
    if (g->visible) {
        ax = x - (g->x + 1);
        ay = y - (g->y + 1 + TITLE_H);
        in_app = ax >= 0 && ay >= 0 && ax < APP_W && ay < APP_H && drag < 0 && top_visible() == 1;
        rv_mouse_x = ax < 0 ? 0 : ax >= APP_W ? APP_W - 1 : ax;
        rv_mouse_y = ay < 0 ? 0 : ay >= APP_H ? APP_H - 1 : ay;
        rv_mouse_buttons = in_app ? buttons : 0;
    } else {
        rv_mouse_x = x * APP_W / DESK_W;
        rv_mouse_y = y * APP_H / DESK_H;
        rv_mouse_buttons = 0;
    }
    UNLOCK(st);
}

void desktop_app_damage(unsigned y0, unsigned y1)
{
    struct win *g = &wins[1];
    if (!running) {
        return;
    }
    if (!g->visible) {                                  /* 初めて絵が書かれたら、画面の窓を出して上にする */
        unsigned long st;
        LOCK(st);
        g->visible = 1;
        raise_win(1);
        UNLOCK(st);
        damage_win(g);
        return;
    }
    damage(g->x + 1, g->y + 1 + TITLE_H + (int)y0, g->x + 1 + APP_W, g->y + 1 + TITLE_H + (int)y1);
}

/* ---------------------------------------------------------------- 組み立ての仕事 */
static volatile int idle;

static void desk_task(INT stacd, void *exinf)
{
    unsigned long st;
    unsigned tick = 0;
    struct win *cw = &wins[0];
    (void)stacd;
    (void)exinf;
    for (;;) {
        int r, i, n;
        struct rect list[NDMG];
        uint32_t now = rv_now_unix() + TZ_OFFSET;
        unsigned got;

        got = con_drain(RING);
        if (now / 60 != (uint32_t)clock_min) {
            clock_min = (int)(now / 60);
            make_clock(now);
            damage(DESK_W / 2, 0, DESK_W, BAR_H);
        }
        if (++tick % 16 == 0 || got) {                  /* 文字の点滅（約 0.5 秒）。打っているあいだは点けておく */
            blink_on = got ? 1 : !blink_on;
            row_dirty[cy] = 1;
        }
        if (pend_scroll > 0) {                          /* 点ごと上へずらす */
            int k = pend_scroll < ROWS ? pend_scroll : ROWS;
            if (k < ROWS) {
                memmove(con_px, con_px + k * 16 * CON_W, sizeof(uint32_t) * (size_t)((ROWS - k) * 16 * CON_W));
            }
            for (r = ROWS - k; r < ROWS; r++) {
                row_dirty[r] = 1;
            }
            prev_cy -= k;                               /* 前の文字の印もずれた */
            if (prev_cy >= 0) {
                row_dirty[prev_cy] = 1;
            }
            prev_cy = cy;
            prev_cx = cx;
            row_dirty[cy] = 1;
            pend_scroll = 0;
            damage(cw->x + 1, cw->y + 1 + TITLE_H, cw->x + 1 + CON_W, cw->y + 1 + TITLE_H + CON_H);
        }
        if (prev_cx != cx || prev_cy != cy) {
            row_dirty[prev_cy] = 1;
            row_dirty[cy] = 1;
            prev_cx = cx;
            prev_cy = cy;
        }
        for (r = 0; r < ROWS; r++) {
            if (row_dirty[r]) {
                int r0 = r;
                while (r < ROWS && row_dirty[r]) {
                    row_dirty[r] = 0;
                    render_row(r);
                    r++;
                }
                damage(cw->x + 1, cw->y + 1 + TITLE_H + r0 * 16, cw->x + 1 + CON_W, cw->y + 1 + TITLE_H + r * 16);
            }
        }
        LOCK(st);
        if (ptr_shown && (ptr_x != ptr_drawn_x || ptr_y != ptr_drawn_y)) {
            int ox = ptr_drawn_x, oy = ptr_drawn_y;
            ptr_drawn_x = ptr_x;
            ptr_drawn_y = ptr_y;
            UNLOCK(st);
            damage(ox, oy, ox + ARROW_W, oy + ARROW_H);
            damage(ptr_drawn_x, ptr_drawn_y, ptr_drawn_x + ARROW_W, ptr_drawn_y + ARROW_H);
            LOCK(st);
        }
        n = ndmg;
        for (i = 0; i < n; i++) {
            list[i] = dmg[i];
        }
        ndmg = 0;
        UNLOCK(st);
        for (i = 0; i < n; i++) {
            int y0;
            compose(&list[i]);
            /* 装置へは 48 行ずつ写す（QEMU は 1 回の写しのあいだタイマを遅らせるので、1 回を短くする） */
            for (y0 = list[i].y0; y0 < list[i].y1; y0 += 48) {
                int y1 = y0 + 48 < list[i].y1 ? y0 + 48 : list[i].y1;
                virtio_gpu_show((unsigned)list[i].x0, (unsigned)y0,
                                (unsigned)(list[i].x1 - list[i].x0), (unsigned)(y1 - y0));
            }
            rv_desk_rects++;
        }
        rv_desk_frames++;
        idle = (n == 0 && ring_tail == ring_head);
        tk_dly_tsk(30);                                 /* たくさん出ていても、プロセスの時間を取りすぎない */
    }
}

void desktop_start(uint32_t *screen, uint32_t *appfb)
{
    T_CTSK ct;
    ID tid;
    int y;
    if (running) {
        return;
    }
    scr = screen;
    app = appfb;
    for (y = 0; y < DESK_H; y++) {                      /* 上 #2e5578 → 下 #16263a */
        int t = y * 256 / DESK_H;
        uint32_t r = (uint32_t)(0x2e + (0x16 - 0x2e) * t / 256);
        uint32_t g = (uint32_t)(0x55 + (0x26 - 0x55) * t / 256);
        uint32_t b = (uint32_t)(0x78 + (0x3a - 0x78) * t / 256);
        bg_row[y] = (r << 16) | (g << 8) | b;
    }
    con_reset();
    wins[0].x = 16;
    wins[0].y = BAR_H + 14;
    wins[0].cw = CON_W;
    wins[0].ch = CON_H;
    wins[0].title = "端末（シリアル）";
    wins[0].visible = 1;
    wins[0].px = con_px;
    wins[1].x = DESK_W - (APP_W + 2) - 16;
    wins[1].y = DESK_H - (APP_H + TITLE_H + 2) - 16;
    wins[1].cw = APP_W;
    wins[1].ch = APP_H;
    wins[1].title = "画面（/dev/fb0  640×400）";
    wins[1].visible = 0;
    wins[1].px = app;
    make_clock(rv_now_unix() + TZ_OFFSET);
    clock_min = (int)((rv_now_unix() + TZ_OFFSET) / 60);
    damage(0, 0, DESK_W, DESK_H);
    running = 1;

    memset(&ct, 0, sizeof(ct));
    ct.tskatr = TA_HLNG | TA_RNG0;
    ct.itskpri = 9;                     /* プロセス（10）が計算し続けても画面は動く */
    ct.stksz = 8192;
    ct.task = (FP)desk_task;
    tid = tk_cre_tsk(&ct);
    if (tid > 0) {
        tk_sta_tsk(tid, 0);
    }
}

/* ---------------------------------------------------------------- テスト用 */
int desktop_running(void)
{
    return running;
}

uint32_t desktop_pixel(unsigned x, unsigned y)
{
    if (!scr || x >= DESK_W || y >= DESK_H) {
        return 0;
    }
    return scr[y * DESK_W + x] & 0xffffff;
}

int desktop_cell(int col, int row)
{
    if (col < 0 || row < 0 || col >= COLS || row >= ROWS) {
        return -1;
    }
    return (int)cells[row][col].ch;
}

void desktop_wait_idle(void)
{
    int i;
    unsigned long f;
    for (i = 0; i < 300 && running; i++) {              /* 最大 3 秒 */
        f = rv_desk_frames;
        while (rv_desk_frames == f && i < 300) {
            tk_dly_tsk(10);
            i++;
        }
        if (idle) {
            f = rv_desk_frames;                         /* もう 1 回組み立て終わるまで */
            while (rv_desk_frames == f && i < 300) {
                tk_dly_tsk(10);
                i++;
            }
            if (idle) {
                return;
            }
        }
    }
}

void desktop_window_rect(int which, int *x, int *y, int *w, int *h)
{
    const struct win *v = &wins[which & 1];
    *x = v->x + 1;
    *y = v->y + 1 + TITLE_H;
    *w = v->cw;
    *h = v->ch;
}

int desktop_top_window(void)
{
    return top_visible();
}

int desktop_cell_color(int col, int row)
{
    if (col < 0 || row < 0 || col >= COLS || row >= ROWS) {
        return -1;
    }
    return cells[row][col].fg | (cells[row][col].bg << 4);
}

void desktop_raise(int which)
{
    unsigned long st;
    if (!running || !wins[which & 1].visible) {
        return;
    }
    LOCK(st);
    raise_win(which & 1);
    UNLOCK(st);
}
