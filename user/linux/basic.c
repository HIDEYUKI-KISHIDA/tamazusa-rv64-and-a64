/* basic.c — T88-BASIC（N88-BASIC 互換のインタプリタ）段階 1
 *
 *   Tamazusa OS の Linux 互換層で動く（musl で静的リンク）。ふつうの Linux でも動く。
 *     basic            対話モード（Ok のあとに命令や行番号つきの行を打つ）
 *     basic FILE.BAS   プログラムを読んで実行し、終わったらシェルに戻る
 *     basic -i FILE    読んで実行したあと、対話モードに入る
 *
 *   段階 1 の範囲: 言語の核とテキスト画面
 *     行番号・GOTO・GOSUB/RETURN・FOR/NEXT・WHILE/WEND・IF THEN ELSE・ON GOTO/GOSUB
 *     変数（% ! # $）・配列・DIM・DEFINT など・DEF FN・DATA/READ/RESTORE・SWAP・ERASE
 *     PRINT（; , TAB SPC USING）・INPUT・LINE INPUT・INKEY$・INPUT$
 *     CLS・LOCATE・COLOR・BEEP・CSRLIN・POS
 *     数値と文字列の関数ひととおり、TIME$・DATE$、RND・RANDOMIZE
 *     RUN・LIST・NEW・LOAD・SAVE・CONT・DELETE・RENUM・FILES・TRON/TROFF・SYSTEM
 *   段階 2 の範囲: ファイルとエラー処理
 *     OPEN（FOR INPUT/OUTPUT/APPEND/RANDOM AS #n LEN=、古い形 OPEN "O",#1,"F"）・CLOSE
 *     PRINT #・PRINT # USING・WRITE（#）・INPUT #・LINE INPUT #・INPUT$(n,#f)・EOF・LOF・LOC
 *     ランダムファイル: FIELD・GET・PUT・LSET・RSET・MKI$ MKS$ MKD$・CVI CVS CVD（N88 と同じ MBF 形式）
 *     SAVE・LOAD・MERGE・CHAIN・COMMON・KILL・NAME・FILES
 *     ON ERROR GOTO・RESUME（NEXT・行番号）・ERROR・ERR・ERL
 *   文字列は N88 と同じく Shift_JIS で持つ（LEN("漢")=2、MID$ で 2 バイトずつ）。画面へ出すときと、
 *     キーボード・打ち込んだ行を受け取るときに UTF-8 と直し合う。PRINT # のファイルは Shift_JIS のまま。
 *     LOAD は UTF-8 と Shift_JIS のどちらのファイルも読む。SAVE は読んだときの形式で書く（Shift_JIS なら CR LF）。
 *     SAVE だけ（名前なし）は最後に LOAD・SAVE したファイルへ上書き。SAVE AS は名前を付けて保存。
 *     上書きの前に、前の版を「名前.bak」に残す
 *   AUTO [始め][,増分]（行番号を出す）・KEY n,"文字列"・KEY LIST（F1〜F10 の文字列。初期値は N88-日本語BASIC(86) と同じ）
 *   Windows 版（tools/build_basic_win.sh、user/linux/win_compat.h）: 名前を省いた LOAD と SAVE AS は「開く」
 *     「名前を付けて保存」の画面。← → ↑ ↓ で行を直し、前の行を呼び戻す。F1〜F10 は KEY の文字列。
 *     保存していない変更があれば、NEW・LOAD・RUN "名前"・SYSTEM の前に聞く
 *   書くファイルの置き場: 今のフォルダに書けなければ /tmp（Tamazusa のディスクは読み取り専用）。
 *     読むときは、置き場 → 今のフォルダ → /bas の順に探す。「1:」「A:」のドライブ名は外す。
 *   段階 3 の範囲: 音
 *     PLAY（MML、6 声まで: CDEFGAB # + - 音長 . R N O < > L T V Q @ & X =変数;）・BEEP
 *     /dev/dsp（Tamazusa の音の装置、22050Hz・16 ビット・モノラル）へ書いて鳴らす。装置がなければ同じ時間待つ。
 *     @ は音色: 0 矩形波 1 細い矩形波 2 三角波 3 のこぎり波 4 正弦波（5 以上はくり返し）
 *     SOUND（音源のレジスタ）・MML の S M Y は受け付けるだけ
 *   段階 4 の範囲: グラフィックス（640×400・16 色。/dev/fb0 = Tamazusa の画面に出す）
 *     SCREEN（0・1 は 200 ライン、2・3 は 400 ライン）・CLS 2/3・COLOR（4 番目がグラフィックスの色）・COLOR=(p,c)
 *     PSET・PRESET・LINE（B・BF・線の模様）・CIRCLE（弧・楕円・F で塗る）・PAINT・POINT・VIEW・WINDOW・
 *     GET/PUT（PSET・PRESET・XOR・OR・AND）・STEP
 *   Tamazusa の拡張: MOUSE(n)（0 X、1 Y、2 ボタン: 1 左 2 右 4 中、3 変わった回数）。/dev/mouse を読む
 *   受け付けるが何もしないもの: POKE・DEF SEG・WIDTH・CONSOLE・KEY・機械語の CALL（PEEK・USR は 0 を返す）
 *   INP・OUT: &HE000〜&HE07F は仮想の機器（Tamazusa の /dev/io。ポートの表は include/vio_sim.h）。ほかの番号は何もしない
 *   Tamazusa の拡張: SLEEP ミリ秒（待ち。N88 の FOR の空回しは速い CPU では一瞬で終わるため）
 *   Tamazusa の拡張: MOVE x,y,z,f（XYZ直線移動、mm と mm/min。計算と表示のみ）
 *   Tamazusa の拡張: JOINTS n / MOVEJ q1,...,qn,v（関節の同期動作の計算。仮想のサーボ 0〜n-1 に目標を出す）
 *   拡張（後の BASIC から取り入れたもの。新しい命令は、名前の一部からは切り出さない: DOG は DO G にならない）
 *     構造化: 複数行の IF … ELSEIF … ELSE … END IF（ENDIF）・SELECT CASE（値, a TO b, IS < x, ELSE）・
 *       DO [WHILE|UNTIL] … LOOP [WHILE|UNTIL]・EXIT DO / FOR / SUB / FUNCTION・
 *       SUB 名前(引数) … END SUB（CALL 名前(引数) か 名前 引数 で呼ぶ）・FUNCTION 名前(引数) … END FUNCTION・
 *       LOCAL（引数と LOCAL の変数は呼ぶあいだだけ。ほかの変数は共通。BBC BASIC と同じ）
 *     ON TIMER(秒) GOSUB 行・TIMER ON / OFF / STOP・TIMER（真夜中からの秒）。SLEEP の途中でも処理を呼び、残りを待つ
 *     機器: SENSOR(n)・PWM ch,% / PWM(ch)・SERVO ch,角度[,速さ] / SERVO(ch)・ESTOP [0] / ESTOP・WATCHDOG ミリ秒
 *     エラーや中断でプログラムが止まると、アクチュエータを 0 にし、サーボをその場で止める
 *   キーワードは N88 と同じく、空白なしで続けても読める（FORI=1TO10 STEP2 など）。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <setjmp.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef _WIN32
#include "win_compat.h"         /* Windows（MinGW-w64）で作るとき: 端末・Ctrl+C などの置き換え */
#else
#include <termios.h>
#include <poll.h>
#include <fnmatch.h>
#include <sys/ioctl.h>
#endif
#include "sjis_table.h"
#include "../../include/iomgr_core.h"
#include <stdarg.h>
#include "../../kernel/gui/font16.c"    /* 画面の文字（GNU Unifont、SIL OFL 1.1。docs/licenses/OFL-1.1.txt） */

/* ---------------------------------------------------------------- 文字の面（N88 のテキスト画面）
 *   N88-BASIC の画面は、文字の面（80 桁 × 25 行）がグラフィックスの面の上に重なる。
 *   T88-BASIC も、端末へ出す文字（UTF-8 と ESC の制御）を、そのまま中で文字の面にも書き、
 *   グラフィックスを /dev/fb0 へ出すときに重ねる（文字の点は文字の色、ほかはグラフィックスが透けて見える）。
 *   CLS 1 で文字だけ、CLS 2 でグラフィックスだけ、CLS 3 で両方を消すのも N88 と同じ。
 *   グラフィックスを使わないプログラムでは、画面（/dev/fb0）は開かない。 */
#define TCOLS 80
#define TROWS 25
static uint32_t tx_ch[TROWS][TCOLS];
static unsigned char tx_fg[TROWS][TCOLS], tx_bg[TROWS][TCOLS], tx_wide[TROWS][TCOLS];
static unsigned char tx_px[TROWS * 16][TCOLS * 8];     /* 文字の面の点: 0 透明、1〜8 色（ANSI の 0〜7） */
static int tx_x, tx_y, tx_wrap, tx_cfg = 7, tx_cbg = 0, tx_rev;
static int tx_esc, tx_np, tx_p[8];
static uint32_t tx_u8;
static int tx_u8n;
static int tx_dirty0 = TROWS, tx_dirty1;
static const uint32_t tx_rgb[8] = {             /* ANSI の 8 色（N88 のデジタルの 8 色と同じ明るさ） */
    0x000000, 0xff0000, 0x00ff00, 0xffff00, 0x0000ff, 0xff00ff, 0x00ffff, 0xffffff
};

static int tx_find(const uint16_t *code, unsigned n, uint32_t c)
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

/* 文字の幅（1・2）と形 */
static int tx_glyph(uint32_t c, const uint8_t **nb, const uint16_t **wb)
{
    int i;
    *nb = NULL;
    *wb = NULL;
    if (c >= 0x20 && c < 0x7f) {
        *nb = font16_narrow_bits[c - 0x20];
        return 1;
    }
    if ((i = tx_find(font16_narrow_code, font16_narrow_n, c)) >= 0) {
        *nb = font16_narrow_bits[i];
        return 1;
    }
    if ((i = tx_find(font16_wide_code, font16_wide_n, c)) >= 0) {
        *wb = font16_wide_bits[i];
        return 2;
    }
    *nb = font16_narrow_bits['?' - 0x20];
    return 1;
}

static void tx_mark(int r0, int r1)
{
    if (r0 < tx_dirty0) tx_dirty0 = r0;
    if (r1 > tx_dirty1) tx_dirty1 = r1;
}

/* 桁 c・行 r の文字を点にする */
static void tx_draw(int r, int c)
{
    const uint8_t *nb;
    const uint16_t *wb;
    int w, y, k;
    if (tx_wide[r][c] == 2) {
        return;                                 /* 全角の右半分は左で描く */
    }
    w = tx_glyph(tx_ch[r][c] ? tx_ch[r][c] : ' ', &nb, &wb) * 8;
    if (c * 8 + w > TCOLS * 8) {
        w = TCOLS * 8 - c * 8;
    }
    for (y = 0; y < 16; y++) {
        unsigned bits = nb ? (unsigned)nb[y] << 8 : wb[y];
        unsigned char *d = &tx_px[r * 16 + y][c * 8];
        for (k = 0; k < w; k++) {
            int on = (bits & (0x8000u >> k)) != 0;
            d[k] = on ? (unsigned char)(tx_fg[r][c] + 1) : (unsigned char)(tx_bg[r][c] ? tx_bg[r][c] + 1 : 0);
        }
    }
    tx_mark(r, r + 1);
}

static void tx_clear_cells(int r, int c0, int c1)
{
    int c;
    for (c = c0; c < c1 && c < TCOLS; c++) {
        tx_ch[r][c] = ' ';
        tx_fg[r][c] = 7;
        tx_bg[r][c] = 0;
        tx_wide[r][c] = 0;
    }
    for (c = 0; c < 16; c++) {                  /* ここでは c は点の行 */
        memset(&tx_px[r * 16 + c][c0 * 8], 0, (size_t)((c1 > TCOLS ? TCOLS : c1) - c0) * 8);
    }
    tx_mark(r, r + 1);
}

static void tx_scroll(void)
{
    memmove(tx_ch[0], tx_ch[1], sizeof(tx_ch[0]) * (TROWS - 1));
    memmove(tx_fg[0], tx_fg[1], sizeof(tx_fg[0]) * (TROWS - 1));
    memmove(tx_bg[0], tx_bg[1], sizeof(tx_bg[0]) * (TROWS - 1));
    memmove(tx_wide[0], tx_wide[1], sizeof(tx_wide[0]) * (TROWS - 1));
    memmove(tx_px[0], tx_px[16], sizeof(tx_px[0]) * 16 * (TROWS - 1));
    tx_clear_cells(TROWS - 1, 0, TCOLS);
    tx_mark(0, TROWS);
}

static void tx_lf(void)
{
    if (tx_y >= TROWS - 1) {
        tx_scroll();
        tx_y = TROWS - 1;
    } else {
        tx_y++;
    }
}

static void tx_put(uint32_t cp)
{
    const uint8_t *nb;
    const uint16_t *wb;
    int w = tx_glyph(cp, &nb, &wb);
    unsigned char fg = (unsigned char)tx_cfg, bg = (unsigned char)tx_cbg;
    if (tx_rev) {                               /* 反転: 色の四角に、文字の形だけ透ける */
        bg = fg;
        fg = 0;
    }
    if (tx_wrap || (w == 2 && tx_x == TCOLS - 1)) {
        tx_x = 0;
        tx_wrap = 0;
        tx_lf();
    }
    if (tx_wide[tx_y][tx_x] == 2 && tx_x > 0) {         /* 全角の片方を上書き */
        tx_clear_cells(tx_y, tx_x - 1, tx_x);
    }
    if (tx_wide[tx_y][tx_x] == 1 && tx_x + 1 < TCOLS) {
        tx_clear_cells(tx_y, tx_x + 1, tx_x + 2);
    }
    tx_ch[tx_y][tx_x] = cp;
    tx_fg[tx_y][tx_x] = fg;
    tx_bg[tx_y][tx_x] = bg;
    tx_wide[tx_y][tx_x] = w == 2 ? 1 : 0;
    if (w == 2) {
        tx_ch[tx_y][tx_x + 1] = ' ';
        tx_fg[tx_y][tx_x + 1] = fg;
        tx_bg[tx_y][tx_x + 1] = bg;
        tx_wide[tx_y][tx_x + 1] = 2;
    }
    tx_draw(tx_y, tx_x);
    tx_x += w;
    if (tx_x >= TCOLS) {
        tx_x = TCOLS - 1;
        tx_wrap = 1;
    }
}

static void tx_sgr(void)
{
    int i;
    if (tx_np == 0) {
        tx_np = 1;
        tx_p[0] = 0;
    }
    for (i = 0; i < tx_np; i++) {
        int v = tx_p[i];
        if (v == 0) { tx_cfg = 7; tx_cbg = 0; tx_rev = 0; }
        else if (v == 7) tx_rev = 1;
        else if (v == 27) tx_rev = 0;
        else if (v >= 30 && v <= 37) tx_cfg = v - 30;
        else if (v == 39) tx_cfg = 7;
        else if (v >= 40 && v <= 47) tx_cbg = v - 40;
        else if (v == 49) tx_cbg = 0;
    }
}

/* 端末へ出すバイトを、文字の面にも書く（UTF-8・ESC [ … の主な制御） */
static void tx_feed(const char *s, size_t n)
{
    for (; n > 0; n--, s++) {
        unsigned char b = (unsigned char)*s;
        if (tx_esc == 1) {
            tx_esc = b == '[' ? 2 : 0;
            tx_np = 0;
            continue;
        }
        if (tx_esc == 2) {
            if (b >= '0' && b <= '9') {
                if (tx_np == 0) { tx_np = 1; tx_p[0] = 0; }
                tx_p[tx_np - 1] = tx_p[tx_np - 1] * 10 + (b - '0');
            } else if (b == ';') {
                if (tx_np == 0) { tx_np = 1; tx_p[0] = 0; }
                if (tx_np < 8) tx_p[tx_np++] = 0;
            } else if (b == '?') {
            } else {
                int a = tx_np > 0 && tx_p[0] > 0 ? tx_p[0] : 1, bb = tx_np > 1 && tx_p[1] > 0 ? tx_p[1] : 1, r;
                tx_esc = 0;
                tx_wrap = 0;
                if (b == 'm') tx_sgr();
                else if (b == 'H' || b == 'f') {
                    tx_y = a - 1 < TROWS ? a - 1 : TROWS - 1;
                    tx_x = bb - 1 < TCOLS ? bb - 1 : TCOLS - 1;
                } else if (b == 'J') {
                    if (tx_np && tx_p[0] == 2) for (r = 0; r < TROWS; r++) tx_clear_cells(r, 0, TCOLS);
                    else { tx_clear_cells(tx_y, tx_x, TCOLS); for (r = tx_y + 1; r < TROWS; r++) tx_clear_cells(r, 0, TCOLS); }
                } else if (b == 'K') tx_clear_cells(tx_y, tx_x, TCOLS);
                else if (b == 'A') tx_y = tx_y - a < 0 ? 0 : tx_y - a;
                else if (b == 'B') tx_y = tx_y + a >= TROWS ? TROWS - 1 : tx_y + a;
                else if (b == 'C') tx_x = tx_x + a >= TCOLS ? TCOLS - 1 : tx_x + a;
                else if (b == 'D') tx_x = tx_x - a < 0 ? 0 : tx_x - a;
            }
            continue;
        }
        if (tx_u8n > 0) {
            if ((b & 0xc0) == 0x80) {
                tx_u8 = (tx_u8 << 6) | (b & 0x3f);
                if (--tx_u8n == 0) tx_put(tx_u8);
                continue;
            }
            tx_u8n = 0;
        }
        if (b >= 0xc0) {
            tx_u8n = b >= 0xf0 ? 3 : b >= 0xe0 ? 2 : 1;
            tx_u8 = b & (0x3f >> tx_u8n);
        } else if (b == 0x1b) {
            tx_esc = 1;
        } else if (b == '\r') {
            tx_x = 0;
            tx_wrap = 0;
        } else if (b == '\n') {
            tx_x = 0;                           /* 端末は \n を \r\n にする（ONLCR） */
            tx_wrap = 0;
            tx_lf();
        } else if (b == 8) {
            if (tx_x > 0) tx_x--;
            tx_wrap = 0;
        } else if (b == '\t') {
            tx_x = (tx_x + 8) & ~7;
            if (tx_x >= TCOLS) tx_x = TCOLS - 1;
        } else if (b == 12) {
            int r;
            for (r = 0; r < TROWS; r++) tx_clear_cells(r, 0, TCOLS);
            tx_x = tx_y = 0;
        } else if (b >= 0x20 && b < 0x7f) {
            tx_put(b);
        }
    }
}

/* 端末へ出すものを、文字の面にも写す（標準出力のときだけ） */
static int tx_putchar(int c)
{
    char ch = (char)c;
    tx_feed(&ch, 1);
    return putchar(c);
}

static int tx_fputs(const char *str, FILE *f)
{
    if (f == stdout) {
        tx_feed(str, strlen(str));
    }
    return fputs(str, f);
}

static size_t tx_fwrite(const void *ptr, size_t sz, size_t nm, FILE *f)
{
    if (f == stdout) {
        tx_feed(ptr, sz * nm);
    }
    return fwrite(ptr, sz, nm, f);
}

static int tx_printf(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        tx_feed(buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
        fputs(buf, stdout);
    }
    return n;
}
#define putchar(c) tx_putchar(c)
#define fputs(s, f) tx_fputs((s), (f))
#define fwrite(p, a, b, f) tx_fwrite((p), (a), (b), (f))
#define printf tx_printf

#define VERSION "0.8"
#define MAXSTR 255
#define MAXLINE 512
#define SIM_MAX_JOINTS 64

/* ---------------------------------------------------------------- キーワード */
#define KEYWORDS(X) \
    X(PRINT) X(LET) X(IF) X(THEN) X(ELSE) X(GOTO) X(GOSUB) X(RETURN) X(FOR) X(TO) \
    X(STEP) X(NEXT) X(WHILE) X(WEND) X(END) X(STOP) X(REM) X(DIM) X(INPUT) X(LINE) \
    X(DATA) X(READ) X(RESTORE) X(ON) X(CLS) X(LOCATE) X(COLOR) X(RANDOMIZE) X(DEF) X(FN) \
    X(SWAP) X(BEEP) X(WIDTH) X(CONSOLE) X(KEY) X(OFF) X(SCREEN) X(ERASE) X(CLEAR) \
    X(DEFINT) X(DEFSNG) X(DEFDBL) X(DEFSTR) X(RUN) X(LIST) X(NEW) X(LOAD) X(SAVE) X(CONT) \
    X(SYSTEM) X(FILES) X(DELETE) X(RENUM) X(TRON) X(TROFF) X(SLEEP) X(USING) X(TAB) X(SPC) \
    X(OPEN) X(AS) X(OUTPUT) X(APPEND) X(RANDOM) X(CLOSE) X(WRITE) X(KILL) X(NAME) X(MERGE) \
    X(CHAIN) X(COMMON) X(FIELD) X(GET) X(PUT) X(LSET) X(RSET) X(ERROR) X(RESUME) X(PLAY) X(SOUND) \
    X(PSET) X(PRESET) X(CIRCLE) X(PAINT) X(VIEW) X(WINDOW) \
    X(AND) X(OR) X(XOR) X(NOT) X(MOD) X(EQV) X(IMP) X(POKE) X(OUT) X(SEG) X(CALL) \
    X(PEEK) X(INP) X(USR) X(ABS) X(INT) X(FIX) X(SGN) X(SQR) X(SIN) X(COS) X(TAN) X(ATN) \
    X(LOG) X(EXP) X(RND) X(LEN) X(MID_S) X(LEFT_S) X(RIGHT_S) X(CHR_S) X(ASC) X(STR_S) \
    X(VAL) X(HEX_S) X(OCT_S) X(INSTR) X(STRING_S) X(SPACE_S) X(INKEY_S) X(INPUT_S) \
    X(TIME_S) X(DATE_S) X(CSRLIN) X(POS) X(CINT) X(CSNG) X(CDBL) X(FRE) X(ERR) X(ERL) \
    X(EOF) X(LOF) X(LOC) X(MKI_S) X(MKS_S) X(MKD_S) X(CVI) X(CVS) X(CVD) X(POINT) X(MOUSE) \
    X(ELSEIF) X(ENDIF) X(SELECT) X(CASE) X(IS) X(DO) X(LOOP) X(UNTIL) X(EXIT) X(SUB) X(FUNCTION) \
    X(LOCAL) X(TIMER) X(MOVE) X(JOINTS) X(MOVEJ) X(SENSOR) X(SERVO) X(PWM) X(ESTOP) X(WATCHDOG) \
    X(PIN) X(ADC) X(DEVICE) X(AUTO)

enum {
#define KW_ENUM(n) K_##n,
    KEYWORDS(KW_ENUM)
    K_COUNT
};
static const char *kwname[K_COUNT] = {
#define KW_NAME(n) #n,
    KEYWORDS(KW_NAME)
};
static char kwtext[K_COUNT][12];        /* 表記（MID_S → MID$） */

/* 文の先頭で、空白なしで続けて書かれても切り出す命令 */
static int is_stmt_kw(int k)
{
    switch (k) {
    case K_PRINT: case K_LET: case K_IF: case K_GOTO: case K_GOSUB: case K_RETURN: case K_FOR:
    case K_NEXT: case K_WHILE: case K_WEND: case K_END: case K_STOP: case K_REM: case K_DIM:
    case K_INPUT: case K_LINE: case K_DATA: case K_READ: case K_RESTORE: case K_ON: case K_CLS:
    case K_LOCATE: case K_COLOR: case K_RANDOMIZE: case K_DEF: case K_SWAP: case K_BEEP:
    case K_WIDTH: case K_CONSOLE: case K_KEY: case K_SCREEN: case K_ERASE: case K_CLEAR:
    case K_DEFINT: case K_DEFSNG: case K_DEFDBL: case K_DEFSTR: case K_RUN: case K_LIST:
    case K_POKE: case K_OUT: case K_CALL: case K_TRON: case K_TROFF: case K_SLEEP:
    case K_OPEN: case K_CLOSE: case K_WRITE: case K_KILL: case K_NAME: case K_MERGE: case K_CHAIN:
    case K_COMMON: case K_FIELD: case K_GET: case K_PUT: case K_LSET: case K_RSET: case K_ERROR:
    case K_RESUME: case K_SAVE: case K_LOAD: case K_FILES: case K_PLAY: case K_SOUND:
    case K_PSET: case K_PRESET: case K_CIRCLE: case K_PAINT: case K_VIEW: case K_WINDOW:
    case K_AUTO:
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- 字句 */
enum { T_EOL = 0, T_NUM, T_STR, T_ID, T_KW, T_OP, T_RAW };
/* 2 文字の演算子 */
#define OP_LE 'l'
#define OP_GE 'g'
#define OP_NE 'n'

typedef struct {
    unsigned char t;
    unsigned char prec;     /* T_NUM: 1 整数 2 単精度 3 倍精度 */
    short k;                /* T_KW: キーワード番号、T_OP: 文字 */
    double n;
    char *s;                /* T_STR・T_ID・T_RAW */
    void *cache;            /* T_ID: 変数の場所（vgen が同じ間だけ有効） */
    unsigned gen;
} Tok;

typedef struct {
    int num;
    char *src;              /* LIST で見せる本文（行番号のあと） */
    Tok *tk;
    int nt;
} Line;

/* ---------------------------------------------------------------- 値 */
typedef struct {
    char type;              /* 'n' 数値 / 's' 文字列 */
    char prec;              /* 1 整数 2 単精度 3 倍精度 */
    double n;
    char *s;                /* malloc した文字列（持ち主は受け取った側） */
} Val;

typedef struct Var {
    char *name;             /* 型の文字つき（A! B$ C% D#） */
    char type;              /* '%' '!' '#' '$' */
    double n;
    char *s;
    struct Var *next;
} Var;

typedef struct Arr {
    char *name;
    char type;
    int nd;
    int dim[8];             /* 各次元の上限 */
    long count;
    double *n;
    char **s;
    struct Arr *next;
} Arr;

typedef struct Fn {
    char *name;
    char type;
    int np;
    Tok *param[8];
    Line *line;
    int tp;
    struct Fn *next;
} Fn;

/* ---------------------------------------------------------------- 状態 */
static Line **prog;
static int nprog, capprog;
static Line *direct;            /* 直接モードで打った行 */
static Line *L;                 /* 実行中の行 */
static int li;                  /* その番号（prog の添字、直接モードは -1） */
static int tp;                  /* 行の中の字句の位置 */
static int jumped;

#define VHASH 509
static Var *vars[VHASH];
static Arr *arrs[VHASH];
static Fn *fns;
static unsigned vgen = 1;
static char deftype[26];

typedef struct { int li; int tp; } Pos;
typedef struct { Var *v; double lim, step; Pos at; int ndo, nwhile, nsel; } ForEnt;  /* ndo など: EXIT FOR で戻す深さ */
static ForEnt forstk[64];
static int nfor;
static Pos gosubstk[256];
static int ngosub;
static Pos whilestk[64];
static int nwhile;

static int data_li, data_tp, data_off;  /* 次に READ する DATA（行、字句、文字の位置） */
static int cont_ok;
static Pos cont_at;
static int trace;
static int interactive = 1;
static int last_err, last_erl;
static int failed;               /* 処理されなかったエラーがあった（終了コード 1） */
static int onerr_line;          /* ON ERROR GOTO の行（0 = なし） */
static int in_err;              /* エラー処理の中（RESUME を待っている） */
static Pos cur_stmt;            /* 実行中の文の先頭（RESUME の戻り先） */
static Pos resume_at;
static int report_erl;          /* ON ERROR GOTO 0 で止めるとき、エラーの起きた行で表示する */
static char workdir[256] = "."; /* 書くファイルの置き場 */
static double sim_xyz[3];       /* MOVE の仮想現在位置（mm、起動時は原点） */
static double sim_joint_pos[SIM_MAX_JOINTS];
static int sim_joint_count;     /* JOINTS で設定した仮想関節数 */

/* 開いたファイル（#1〜#16） */
#define MAXFILES 16
typedef struct {
    char **sp;
    long off, w;
} Field;
typedef struct {
    FILE *fp;
    char mode;                  /* 'I' 'O' 'A' 'R'、0 = 閉じている */
    char dev;                   /* 0 ファイル、'S' 画面（SCRN:）、'K' キーボード（KYBD:） */
    int col;
    long reclen;
    char *rec;                  /* ランダム: レコードの入れ物 */
    long recno;                 /* 最後に GET/PUT したレコードの番号 */
    int nfield;
    Field field[64];
} BFile;
static BFile files[MAXFILES + 1];
static BFile *outf;             /* PRINT # の出力先（NULL = 画面） */
static void gfx_flush(int force);
static int try_call_fn(Val *out);
static void frames_drop(void);
static int io_read(unsigned port);
static int io_name_id(const char *s, int want_kind, int want_out);
static double io_named_get(const char *s, int kind);
static int io_device(const char *s);
static double io_sensor(long n);
static double now_sec(void);
static void timer_reset(void);
static void timer_returned(void);
static int call_sub_stmt(void);
static void do_sleep(long ms);
static int block_depth(int which);
static void io_write(unsigned port, double d);
static int g_lines;
static void to_log(double x, double y, int *lx, int *ly);
static int gpoint(int x, int y);
static void ignore_args(void);
static BFile *get_file(long n);
static int f_getc(BFile *f);
static int f_eof(BFile *f);
static void close_all(void);
static void to_mbf4(double d, unsigned char o[4]);
static void to_mbf8(double d, unsigned char o[8]);
static double from_mbf4(const unsigned char *b);
static double from_mbf8(const unsigned char *b);

static jmp_buf errjb;
static volatile sig_atomic_t brkflag;

static int color_used;          /* COLOR を使った（終わりに色を戻す）*/
static int col;                 /* 出力の桁（TAB・POS・カンマの区切りのため） */
static int row;
static unsigned long rnd_state = 0x12345678UL;
static double rnd_last;

/* ---------------------------------------------------------------- エラー */
enum {
    E_NEXT = 1, E_SYNTAX, E_RETURN, E_DATA, E_ILLFUNC, E_OVERFLOW, E_MEM, E_UNDEFLINE,
    E_SUBSCRIPT, E_DUPDEF, E_DIV0, E_DIRECT, E_TYPE, E_STRLONG = 15, E_CONT = 17,
    E_UNDEFFN, E_NORESUME, E_RESUME, E_FORNEXT = 26, E_WHILE = 29, E_WEND,
    /* ファイルのエラーは N88-BASIC(86) リファレンスマニュアルの付録 A と同じ番号 */
    E_FIELD = 50, E_BADFNUM = 52, E_FILENF = 53, E_OPEN = 54, E_PASTEND = 55, E_BADNAME = 56,
    E_SEQONLY = 59, E_NOTOPEN = 60, E_WRITE = 61, E_IO = 64, E_EXISTS = 65, E_FULL = 68,
    /* T88-BASIC の拡張の命令のエラー（N88 にない番号） */
    E_IFEND = 100, E_SELEND, E_CASE, E_DOLOOP, E_LOOP, E_EXIT, E_SUBEND, E_ENDSUB, E_ARGS, E_LOCAL,
    E_NODEV = 110, E_ESTOP, E_DENIED
};
static const char *errmsg(int e)
{
    switch (e) {
    case E_NEXT: return "NEXT without FOR";
    case E_SYNTAX: return "Syntax error";
    case E_RETURN: return "RETURN without GOSUB";
    case E_DATA: return "Out of DATA";
    case E_ILLFUNC: return "Illegal function call";
    case E_OVERFLOW: return "Overflow";
    case E_MEM: return "Out of memory";
    case E_UNDEFLINE: return "Undefined line number";
    case E_SUBSCRIPT: return "Subscript out of range";
    case E_DUPDEF: return "Duplicate Definition";
    case E_DIV0: return "Division by zero";
    case E_DIRECT: return "Illegal direct";
    case E_TYPE: return "Type mismatch";
    case E_STRLONG: return "String too long";
    case E_CONT: return "Can't continue";
    case E_UNDEFFN: return "Undefined user function";
    case E_NORESUME: return "No RESUME";
    case E_RESUME: return "RESUME without error";
    case E_FIELD: return "FIELD overflow";
    case E_BADFNUM: return "Bad file number";
    case E_SEQONLY: return "Sequential I/O only";
    case E_NOTOPEN: return "File not open";
    case E_OPEN: return "File already open";
    case E_IO: return "Disk I/O error";
    case E_EXISTS: return "File already exist";
    case E_FULL: return "Disk full";
    case E_PASTEND: return "Input past end";
    case E_FORNEXT: return "FOR without NEXT";
    case E_WHILE: return "WHILE without WEND";
    case E_WEND: return "WEND without WHILE";
    case E_FILENF: return "File not found";
    case E_BADNAME: return "Bad file name";
    case E_WRITE: return "File write protected";
    case E_IFEND: return "IF without END IF";
    case E_SELEND: return "SELECT without END SELECT";
    case E_CASE: return "CASE without SELECT";
    case E_DOLOOP: return "DO without LOOP";
    case E_LOOP: return "LOOP without DO";
    case E_EXIT: return "EXIT without block";
    case E_SUBEND: return "SUB without END SUB";
    case E_ENDSUB: return "END SUB without SUB";
    case E_ARGS: return "Wrong number of arguments";
    case E_LOCAL: return "LOCAL outside SUB";
    case E_NODEV: return "Device unavailable";
    case E_ESTOP: return "Emergency stop";
    case E_DENIED: return "Permission denied";
    }
    return "Unprintable error";
}

static void err(int e) __attribute__((noreturn));
static void err(int e)
{
    longjmp(errjb, e);
}

#define R_END   1000            /* longjmp の値: END（正常に終わり） */
#define R_STOP  1001            /* STOP・Ctrl-C */
#define R_EXIT  1002            /* SYSTEM */

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        err(E_MEM);
    }
    return p;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s);
    char *p = xmalloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

/* BASIC の文字列: 長さを前に持つ（MKI$ などの 0 のバイトも入る）。末尾にも 0 をおくので、
 * C の文字列としても読める（ファイル名など）。作るのは snew、捨てるのは sfree だけ */
static char *snew(const char *p, size_t n)
{
    size_t *h = xmalloc(sizeof(size_t) + n + 1);
    char *s = (char *)(h + 1);
    *h = n;
    if (n && p) {
        memcpy(s, p, n);
    } else if (n) {
        memset(s, 0, n);
    }
    s[n] = 0;
    return s;
}

static size_t slen(const char *s)
{
    return s ? ((const size_t *)s)[-1] : 0;
}

static void sfree(char *s)
{
    if (s) {
        free((size_t *)s - 1);
    }
}

/* ---------------------------------------------------------------- Shift_JIS と UTF-8
 *   BASIC の中の文字列は N88 と同じ Shift_JIS で持つ（LEN("漢")=2、MID$ で 2 バイトずつ取れる）。
 *   端末は UTF-8 なので、画面に出すときと、キーボードや打ち込んだ行を受け取るときに直す。 */
static int is_sjis_lead(unsigned c)
{
    return (c >= 0x81 && c <= 0x9f) || (c >= 0xe0 && c <= 0xfc);
}

/* 2 バイトの文字 → Unicode（0 = ない） */
static unsigned sjis_cp(unsigned lead, unsigned trail)
{
    unsigned li2, ti;
    if (!is_sjis_lead(lead) || trail < 0x40 || trail > 0xfc || trail == 0x7f) {
        return 0;
    }
    li2 = lead <= 0x9f ? lead - 0x81 : lead - 0xe0 + 31;
    ti = trail < 0x7f ? trail - 0x40 : trail - 0x41;
    return sjis_table[li2 * 188 + ti];
}

static size_t put_utf8(unsigned cp, char *o)
{
    if (cp < 0x80) {
        o[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        o[0] = (char)(0xc0 | (cp >> 6));
        o[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    o[0] = (char)(0xe0 | (cp >> 12));
    o[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
    o[2] = (char)(0x80 | (cp & 0x3f));
    return 3;
}

static int valid_utf8(const unsigned char *p, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned char c = p[i];
        int k;
        if (c < 0x80) {
            i++;
            continue;
        }
        if ((c & 0xe0) == 0xc0) {
            k = 1;
        } else if ((c & 0xf0) == 0xe0) {
            k = 2;
        } else if ((c & 0xf8) == 0xf0) {
            k = 3;
        } else {
            return 0;
        }
        if (i + k >= n) {
            return 0;
        }
        while (k-- > 0) {
            i++;
            if ((p[i] & 0xc0) != 0x80) {
                return 0;
            }
        }
        i++;
    }
    return 1;
}

/* Unicode → Shift_JIS の表（起動のときに sjis_table を裏返して作る） */
typedef struct {
    unsigned short u, s;
} U2S;
static U2S *u2s;
static int nu2s;

static int u2s_cmp(const void *a, const void *b)
{
    const U2S *x = a, *y = b;
    if (x->u != y->u) {
        return (int)x->u - (int)y->u;
    }
    return (int)x->s - (int)y->s;       /* 同じ字が 2 か所にあれば、JIS の側（番号の小さいほう）を使う */
}

static void build_u2s(void)
{
    int l, t, n = 0;
    u2s = malloc(sizeof(U2S) * 11280);
    if (!u2s) {
        return;
    }
    for (l = 0; l < 60; l++) {
        for (t = 0; t < 188; t++) {
            unsigned cp = sjis_table[l * 188 + t];
            unsigned lead = l < 31 ? 0x81 + l : 0xe0 + l - 31;
            unsigned trail = t < 63 ? 0x40 + t : 0x41 + t;
            if (cp && cp < 0x10000) {
                u2s[n].u = (unsigned short)cp;
                u2s[n].s = (unsigned short)((lead << 8) | trail);
                n++;
            }
        }
    }
    qsort(u2s, n, sizeof(U2S), u2s_cmp);
    nu2s = n;
}

static unsigned u2s_find(unsigned cp)
{
    int lo = 0, hi = nu2s;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (u2s[mid].u < cp) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return (lo < nu2s && u2s[lo].u == cp) ? u2s[lo].s : 0;
}

/* UTF-8 → Shift_JIS（out は n バイトあれば足りる）。ない字は 〓 */
static size_t utf8_to_sjis(const char *in, size_t n, char *out)
{
    const unsigned char *p = (const unsigned char *)in;
    size_t i = 0, k = 0;

    while (i < n) {
        unsigned c = p[i], cp, len, j;
        if (c < 0x80) {
            out[k++] = (char)c;
            i++;
            continue;
        }
        if ((c & 0xe0) == 0xc0) {
            cp = c & 0x1f;
            len = 2;
        } else if ((c & 0xf0) == 0xe0) {
            cp = c & 0x0f;
            len = 3;
        } else if ((c & 0xf8) == 0xf0) {
            cp = c & 0x07;
            len = 4;
        } else {
            i++;
            continue;
        }
        if (i + len > n) {
            break;
        }
        for (j = 1; j < len; j++) {
            cp = (cp << 6) | (p[i + j] & 0x3f);
        }
        i += len;
        if (cp >= 0xff61 && cp <= 0xff9f) {
            out[k++] = (char)(0xa1 + (cp - 0xff61));
        } else {
            unsigned sj = cp < 0x10000 ? u2s_find(cp) : 0;
            if (!sj) {
                sj = 0x81ac;            /* 〓 */
            }
            out[k++] = (char)(sj >> 8);
            out[k++] = (char)(sj & 0xff);
        }
    }
    return k;
}

/* Shift_JIS → UTF-8（out は n*3+1 バイトあれば足りる）。0 で終える */
static size_t sjis_to_utf8(const char *in, size_t n, char *out)
{
    const unsigned char *p = (const unsigned char *)in;
    size_t i = 0, k = 0;

    while (i < n) {
        unsigned c = p[i];
        if (c < 0x80) {
            out[k++] = (char)c;
            i++;
        } else if (c >= 0xa1 && c <= 0xdf) {
            k += put_utf8(0xff61 + (c - 0xa1), out + k);
            i++;
        } else if (is_sjis_lead(c) && i + 1 < n) {
            unsigned cp = sjis_cp(c, p[i + 1]);
            k += put_utf8(cp ? cp : 0x3013, out + k);
            i += 2;
        } else {
            k += put_utf8(0x3013, out + k);
            i++;
        }
    }
    out[k] = 0;
    return k;
}

/* C の文字列を UTF-8 にした写し（呼んだ側が free） */
static char *to_utf8_cstr(const char *s)
{
    size_t n = strlen(s);
    char *o = xmalloc(n * 3 + 1);
    sjis_to_utf8(s, n, o);
    return o;
}

/* 比べる（memcmp で、短いほうが小さい） */
static int scmp(const char *a, const char *b)
{
    size_t la = slen(a), lb = slen(b), m = la < lb ? la : lb;
    int c = m ? memcmp(a, b, m) : 0;
    if (c) {
        return c;
    }
    return la < lb ? -1 : la > lb ? 1 : 0;
}

/* ---------------------------------------------------------------- 端末 */
static struct termios tio_saved;
static int tio_have, tio_raw;
static char inbuf[1024];
static int inlen, inpos;
static int out_dirty;
static struct timespec last_flush;

static void tx_to_gfx(void);

static void out_flush(void)
{
    fflush(stdout);
    tx_to_gfx();
    out_dirty = 0;
    clock_gettime(CLOCK_MONOTONIC, &last_flush);
}

/* 画面を動かすプログラムのため、PRINT のたびに少し間をおいて吐き出す */
static void out_maybe_flush(void)
{
    struct timespec now;
    long ms;

    clock_gettime(CLOCK_MONOTONIC, &now);
    ms = (now.tv_sec - last_flush.tv_sec) * 1000 + (now.tv_nsec - last_flush.tv_nsec) / 1000000;
    if (ms >= 20) {
        out_flush();
    }
}

static void tty_cooked(void)
{
    if (tio_have && tio_raw) {
        tcsetattr(0, TCSANOW, &tio_saved);
        tio_raw = 0;
    }
}

static void tty_rawmode(void)
{
    struct termios t;

    if (tio_have && !tio_raw) {
        t = tio_saved;
        t.c_lflag &= ~(ICANON | ECHO);
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
        tcsetattr(0, TCSANOW, &t);
        tio_raw = 1;
    }
}

/* 入力を読み足す。読めた数（0 = 終わり、-1 = 割り込み） */
static int in_fill(void)
{
    ssize_t n;

    out_flush();
    inpos = inlen = 0;
    n = read(0, inbuf, sizeof(inbuf));
    if (n < 0) {
        if (errno == EINTR) {
            return -1;
        }
        return 0;
    }
    inlen = (int)n;
    return inlen;
}

static int in_ready(void)
{
    struct pollfd p;

    if (inpos < inlen) {
        return 1;
    }
    p.fd = 0;
    p.events = POLLIN;
    p.revents = 0;
    return poll(&p, 1, 0) > 0 && (p.revents & POLLIN);
}

/* 1 文字（待つ）。-1 = 入力の終わり */
static int in_getc(void)
{
    if (inpos >= inlen) {
        int r = in_fill();
        if (r < 0) {
            if (brkflag) {
                return -2;
            }
            return in_getc();
        }
        if (r == 0) {
            return -1;
        }
    }
    return (unsigned char)inbuf[inpos++];
}

/* ファンクションキー（N88-日本語BASIC(86) の初期設定。\r は改行まで打つ）。KEY 文で変える */
static char fkey[10][32] = {
    "load\"", "auto", "go to", "list", "run\r",
    "save\"", "key", "print", "edit.\r", "cont\r"
};

#ifdef _WIN32
/* ---- Windows のコンソールでの 1 行の編集 ----
 *   ← → Home End で動き、BS・Del で消す。↑ ↓ で前に打った行を呼び戻す。
 *   F1〜F10（Shift+F1〜F5 は F6〜F10）は fkey の文字列を打つ。中身は UTF-8 で持つ */
#define HIST_MAX 32
static char *hist[HIST_MAX];
static int nhist;

static int u8_len(unsigned char c)
{
    return c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : 4;
}

static int u8_width(const char *s)   /* 1 文字の画面の幅（全角は 2） */
{
    const unsigned char *p = (const unsigned char *)s;
    unsigned cp;
    if (p[0] < 0x80) {
        return 1;
    }
    if ((p[0] & 0xe0) == 0xc0) {
        return 1;
    }
    cp = (p[0] & 0x0f) << 12 | (p[1] & 0x3f) << 6 | (p[2] & 0x3f);
    if ((p[0] & 0xf0) == 0xf0) {
        return 2;
    }
    if ((cp >= 0x1100 && cp <= 0x115f) || (cp >= 0x2e80 && cp <= 0xa4cf) || (cp >= 0xac00 && cp <= 0xd7a3) ||
        (cp >= 0xf900 && cp <= 0xfaff) || (cp >= 0xfe30 && cp <= 0xfe4f) || (cp >= 0xff00 && cp <= 0xff60) ||
        (cp >= 0xffe0 && cp <= 0xffe6)) {
        return 2;
    }
    return 1;
}

static int u8_span(const char *s, int from, int to)   /* s[from..to) の画面の幅 */
{
    int w = 0;
    while (from < to) {
        w += u8_width(s + from);
        from += u8_len((unsigned char)s[from]);
    }
    return w;
}

/* 打ったキーを 1 つ読む。普通の文字は 1 バイトずつ、特別なキーは 256 以上の番号で返す */
enum { KY_UP = 256, KY_DOWN, KY_LEFT, KY_RIGHT, KY_HOME, KY_END, KY_DEL, KY_F1 };
static int key_read(void)
{
    int c = in_getc(), d, n = 0, mod = 0;
    if (c != 0x1b) {
        return c;
    }
    if (!in_ready()) {
        return 0x1b;
    }
    d = in_getc();
    if (d == 'O') {                 /* ESC O P〜S: F1〜F4 */
        d = in_getc();
        return (d >= 'P' && d <= 'S') ? KY_F1 + (d - 'P') : 0;
    }
    if (d != '[') {
        return 0;
    }
    for (;;) {
        d = in_getc();
        if (d >= '0' && d <= '9') {
            n = n * 10 + (d - '0');
        } else if (d == ';') {
            mod = n;
            n = 0;
        } else {
            break;
        }
    }
    if (mod == 1) {                 /* ESC [ 1 ; m P〜S: 修飾キーつきの F1〜F4（m = 2 は Shift） */
        int shift = (n == 2) ? 5 : 0;
        if (d >= 'P' && d <= 'S') {
            return KY_F1 + (d - 'P') + shift;
        }
    }
    switch (d) {
    case 'A': return KY_UP;
    case 'B': return KY_DOWN;
    case 'C': return KY_RIGHT;
    case 'D': return KY_LEFT;
    case 'H': return KY_HOME;
    case 'F': return KY_END;
    case '~': {
        int base = mod ? mod : n, shift = (mod && n == 2) ? 5 : 0;
        switch (base) {
        case 1: return KY_HOME;
        case 3: return KY_DEL;
        case 4: return KY_END;
        case 15: return KY_F1 + 4 + shift;
        case 17: return KY_F1 + 5;
        case 18: return KY_F1 + 6;
        case 19: return KY_F1 + 7;
        case 20: return KY_F1 + 8;
        case 21: return KY_F1 + 9;
        }
        return 0;
    }
    }
    return 0;
}

static int edit_line(char *buf, int max)
{
    int n = 0, cur = 0, hi = nhist;

    buf[0] = 0;
    tty_rawmode();
    for (;;) {
        int c;
        out_flush();
        c = key_read();
        if (c == -2 || c == 3) {
            tty_cooked();
            return -2;
        }
        if (c < 0) {
            tty_cooked();
            return n ? n : -1;
        }
        if (c == '\r' || c == '\n') {
            break;
        }
        if (c >= KY_F1 && c < KY_F1 + 10) {     /* ファンクションキー: 文字列を打つ（\r があればそこで決まり） */
            const char *s = fkey[c - KY_F1];
            int enter = 0;
            for (; *s; s++) {
                if (*s == '\r') {
                    enter = 1;
                    break;
                }
                if (n < max - 1) {
                    memmove(buf + cur + 1, buf + cur, (size_t)(n - cur + 1));
                    buf[cur++] = *s;
                    n++;
                }
            }
            if (enter) {
                fputs("\x1b[u", stdout);
                fputs(buf, stdout);
                fputs("\x1b[K", stdout);
                cur = n;
                break;
            }
        } else if (c == KY_LEFT && cur > 0) {
            do { cur--; } while (cur > 0 && ((unsigned char)buf[cur] & 0xc0) == 0x80);
        } else if (c == KY_RIGHT && cur < n) {
            cur += u8_len((unsigned char)buf[cur]);
        } else if (c == KY_HOME) {
            cur = 0;
        } else if (c == KY_END) {
            cur = n;
        } else if ((c == 8 || c == 127) && cur > 0) {
            int s = cur;
            do { s--; } while (s > 0 && ((unsigned char)buf[s] & 0xc0) == 0x80);
            memmove(buf + s, buf + cur, (size_t)(n - cur + 1));
            n -= cur - s;
            cur = s;
        } else if (c == KY_DEL && cur < n) {
            int l = u8_len((unsigned char)buf[cur]);
            memmove(buf + cur, buf + cur + l, (size_t)(n - cur - l + 1));
            n -= l;
        } else if ((c == KY_UP && hi > 0) || (c == KY_DOWN && hi < nhist)) {
            hi += (c == KY_UP) ? -1 : 1;
            snprintf(buf, (size_t)max, "%s", hi < nhist ? hist[hi] : "");
            n = cur = (int)strlen(buf);
        } else if (c >= 0x20 && c < 256 && c != 127 && n < max - 4) {
            /* 文字（UTF-8 の続きのバイトもまとめて入れる） */
            int l = u8_len((unsigned char)c), i;
            char ch[4];
            ch[0] = (char)c;
            for (i = 1; i < l; i++) {
                ch[i] = (char)in_getc();
            }
            memmove(buf + cur + l, buf + cur, (size_t)(n - cur + 1));
            memcpy(buf + cur, ch, (size_t)l);
            n += l;
            cur += l;
        }
        /* 画面を合わせる: 打ち始めの位置へ戻って行を書き直し、カーソルを置く */
        fputs("\x1b[u", stdout);       /* 打ち始めの位置へ */
        fputs(buf, stdout);
        fputs("\x1b[K", stdout);
        fputs("\x1b[u", stdout);
        if (cur > 0) {
            fprintf(stdout, "\x1b[%dC", u8_span(buf, 0, cur));
        }
    }
    tty_cooked();
    fputs("\n", stdout);
    if (n > 0 && (nhist == 0 || strcmp(hist[nhist - 1], buf) != 0)) {
        if (nhist == HIST_MAX) {
            free(hist[0]);
            memmove(hist, hist + 1, sizeof(hist[0]) * (HIST_MAX - 1));
            nhist--;
        }
        hist[nhist++] = xstrdup(buf);
    }
    return n;
}
#endif

/* 1 行（改行は含めない）。-1 = 入力の終わり、-2 = Ctrl-C */
static int read_line(char *buf, int max)
{
    int n = 0, c;

    gfx_flush(1);
#ifdef _WIN32
    if (win_in_console && tio_have) {
        fputs("\x1b[s", stdout);       /* 打ち始めの位置を覚える（書き直すときに戻る） */
        n = edit_line(buf, max);
        if (n < 0) {
            return n;
        }
        goto got_line;
    }
#endif
    tty_cooked();
    for (;;) {
        c = in_getc();
        if (c == -2) {
            return -2;
        }
        if (c < 0) {
            if (n == 0) {
                return -1;
            }
            break;
        }
        if (c == '\n') {
            break;
        }
        if (c == '\r') {
            continue;
        }
        if (n < max - 1) {
            buf[n++] = (char)c;
        }
    }
    buf[n] = 0;
#ifdef _WIN32
got_line:
#endif
    tx_feed(buf, (size_t)n);            /* 打った行（端末が表示したもの）を文字の面にも */
    tx_feed("\n", 1);
    if (valid_utf8((unsigned char *)buf, (size_t)n)) {     /* 端末の UTF-8 → Shift_JIS */
        char tmp[MAXLINE * 2];
        size_t k = utf8_to_sjis(buf, (size_t)n, tmp);
        if (k > (size_t)max - 1) {
            k = (size_t)max - 1;
        }
        memcpy(buf, tmp, k);
        buf[k] = 0;
        n = (int)k;
    }
    col = 0;
    row++;
    return n;
}

/* 漢字などのキー: UTF-8 の残りを読み、Shift_JIS にして 1 バイトずつ渡す */
static unsigned char kq[4];
static int kqn, kqp;

static int kbd_multibyte(int c)
{
    char u[4], sj[4];
    int len = (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 1, i;
    size_t k;
    u[0] = (char)c;
    for (i = 1; i < len; i++) {
        int d = in_getc();
        if (d < 0) {
            return 0;
        }
        u[i] = (char)d;
    }
    k = utf8_to_sjis(u, (size_t)len, sj);
    if (k == 0) {
        return 0;
    }
    memcpy(kq, sj + 1, k - 1);
    kqn = (int)k - 1;
    kqp = 0;
    return (unsigned char)sj[0];
}

/* キーボードから 1 文字（待つ）。Shift_JIS で */
static int kbd_getc(void)
{
    int c;
    if (kqp < kqn) {
        return kq[kqp++];
    }
    c = in_getc();
    if (c >= 0x80) {
        return kbd_multibyte(c);
    }
    return c;
}

/* INKEY$: 押されていなければ 0。矢印は N88 の文字コードにする */
static int inkey(void)
{
    int c;

    out_flush();
    gfx_flush(0);
    tty_rawmode();
    if (kqp < kqn) {
        return kq[kqp++];
    }
    if (!in_ready()) {
        return 0;
    }
    c = in_getc();
    if (c >= 0x80) {
        return kbd_multibyte(c);
    }
    if (c < 0) {
        return 0;
    }
    if (c == 0x1b && in_ready()) {
        int c2 = in_getc();
        if ((c2 == '[' || c2 == 'O') && in_ready()) {
            int c3 = in_getc();
            switch (c3) {
            case 'A': return 30;
            case 'B': return 31;
            case 'C': return 28;
            case 'D': return 29;
            }
            return 0;
        }
        return 0x1b;
    }
    if (c == '\n') {
        return 13;
    }
    if (c == 0x7f) {
        return 8;
    }
    return c;
}

static void on_sigint(int s)
{
    (void)s;
    brkflag = 1;
}

/* ---------------------------------------------------------------- 出力 */
static void put_mem(const char *s, size_t n)
{
    if (outf && outf->dev != 'S') {
        fwrite(s, 1, n, outf->fp);
        for (; n > 0; n--, s++) {
            unsigned char c = (unsigned char)*s;
            if (c == '\n' || c == '\r') {
                outf->col = 0;
            } else if (c >= 0x20) {
                outf->col++;            /* Shift_JIS の漢字は 2 バイトで 2 桁 */
            }
        }
        return;
    }
    for (; n > 0; n--, s++) {
        unsigned char c = (unsigned char)*s;
        static unsigned pend_lead;      /* 2 バイトの文字の 1 バイト目（次の呼び出しにまたがってもよい） */
        char u[4];
        size_t ul;
        if (pend_lead) {
            unsigned cp = sjis_cp(pend_lead, c);
            pend_lead = 0;
            ul = put_utf8(cp ? cp : 0x3013, u);
            fwrite(u, 1, ul, stdout);
            col += 2;
            if (cp) {
                continue;
            }
        }
        if (is_sjis_lead(c)) {
            pend_lead = c;
            continue;
        }
        if (c >= 0xa1 && c <= 0xdf) {   /* 半角カナ */
            ul = put_utf8(0xff61 + (c - 0xa1), u);
            fwrite(u, 1, ul, stdout);
            col++;
            continue;
        }
        if (c >= 0x80) {
            putchar('?');
            col++;
            continue;
        }
        putchar(c);
        if (c == '\n') {
            col = 0;
            row++;
        } else if (c == '\r') {
            col = 0;
        } else if (c == 8) {
            if (col > 0) {
                col--;
            }
        } else if (c >= 0x20) {
            col++;
        }
    }
    out_dirty = 1;
}

static void put_str(const char *s)
{
    put_mem(s, strlen(s));
}

static void newline(void)
{
    put_str("\n");
}

/* 数値の表記（N88 と同じく、正の数の前に空白をおく。末尾の空白は呼ぶ側で） */
static void fmt_num(char *buf, double n, int prec)
{
    char t[64], *p, *e;

    if (prec == 1 && n == floor(n) && fabs(n) < 2147483648.0) {
        sprintf(t, "%ld", (long)n);
    } else if (prec == 3) {
        sprintf(t, "%.16G", n);
    } else {
        float f = (float)n;
        sprintf(t, "%.7G", (double)f);
    }
    /* 0.5 → .5、-0.5 → -.5 */
    p = t;
    if (p[0] == '0' && p[1] == '.') {
        memmove(p, p + 1, strlen(p));
    } else if (p[0] == '-' && p[1] == '0' && p[2] == '.') {
        memmove(p + 1, p + 2, strlen(p + 1));
    }
    /* 指数: 1E+07 の形。倍精度は D */
    e = strchr(t, 'E');
    if (e) {
        char mant[40], *m;
        int ex = atoi(e + 1);
        *e = 0;
        strcpy(mant, t);
        m = strchr(mant, '.');
        if (m) {                /* 仮数の末尾の 0 を落とす */
            char *z = mant + strlen(mant) - 1;
            while (z > m && *z == '0') {
                *z-- = 0;
            }
            if (z == m) {
                *z = 0;
            }
        }
        sprintf(t, "%s%c%c%02d", mant, prec == 3 ? 'D' : 'E', ex < 0 ? '-' : '+', ex < 0 ? -ex : ex);
    }
    if (n >= 0 || t[0] != '-') {
        if (t[0] == '-') {
            strcpy(buf, t);
        } else {
            buf[0] = ' ';
            strcpy(buf + 1, t);
        }
    } else {
        strcpy(buf, t);
    }
}

/* ---------------------------------------------------------------- 字句の切り出し */
static int kw_match(const char *p, int *len)
{
    int best = -1, bl = 0, k;

    for (k = 0; k < K_ELSEIF; k++) {     /* K_ELSEIF から後ろ（新しい命令）は、名前の一部からは切り出さない */
        const char *w = kwtext[k];
        int n = (int)strlen(w);
        if (n > bl && strncasecmp(p, w, n) == 0) {
            best = k;
            bl = n;
        }
    }
    *len = bl;
    return best;
}

static int kw_exact(const char *s, int n)
{
    int k;

    for (k = 0; k < K_COUNT; k++) {
        if ((int)strlen(kwtext[k]) == n && strncasecmp(s, kwtext[k], n) == 0) {
            return k;
        }
    }
    return -1;
}

static int is_idch(int c)
{
    return isalnum(c) || c == '.';
}

/* 行の本文を字句にする。誤りは E_SYNTAX */
static Tok *tokenize(const char *s, int *ntok)
{
    Tok *v = NULL;
    int n = 0, cap = 0;
    int ctx = 0;        /* 0 文の先頭 1 値が来る所 2 値のあと */
    const char *p = s;

#define PUSH(tt) do { if (n >= cap) { cap = cap ? cap * 2 : 16; v = realloc(v, cap * sizeof(Tok)); if (!v) err(E_MEM); } \
                      memset(&v[n], 0, sizeof(Tok)); v[n].t = (tt); n++; } while (0)
    for (;;) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == 0) {
            break;
        }
        if (*p == '"') {
            const char *q = ++p;
            char *str;
            while (*p && *p != '"') {
                p++;
            }
            str = xmalloc(p - q + 1);
            memcpy(str, q, p - q);
            str[p - q] = 0;
            if (*p == '"') {
                p++;
            }
            PUSH(T_STR);
            v[n - 1].s = str;
            ctx = 2;
            continue;
        }
        if (*p == '\'') {               /* ' は REM */
            PUSH(T_KW);
            v[n - 1].k = K_REM;
            PUSH(T_RAW);
            v[n - 1].s = xstrdup(p + 1);
            break;
        }
        if (isdigit((unsigned char)*p) || (*p == '.' && isdigit((unsigned char)p[1]))
            || (*p == '&' && (toupper((unsigned char)p[1]) == 'H' || toupper((unsigned char)p[1]) == 'O'))) {
            double val;
            int prec = 1, digits = 0;
            if (*p == '&') {
                int base = toupper((unsigned char)p[1]) == 'H' ? 16 : 8;
                p += 2;
                val = (double)strtol(p, (char **)&p, base);
                if (val > 32767 && val < 65536) {
                    val -= 65536;   /* N88 の &H は 16 ビット */
                }
            } else {
                const char *q = p;
                char numbuf[64];
                int k = 0, dexp = 0;
                while (isdigit((unsigned char)*p) || *p == '.') {
                    if (*p == '.') {
                        prec = 2;
                    } else {
                        digits++;
                    }
                    p++;
                }
                if ((toupper((unsigned char)*p) == 'E' || toupper((unsigned char)*p) == 'D')
                    && (isdigit((unsigned char)p[1])
                        || ((p[1] == '+' || p[1] == '-') && isdigit((unsigned char)p[2])))) {
                    dexp = toupper((unsigned char)*p) == 'D';
                    prec = dexp ? 3 : 2;
                    p += 2;
                    while (isdigit((unsigned char)*p)) {
                        p++;
                    }
                }
                for (; q < p && k < 62; q++) {
                    numbuf[k++] = (toupper((unsigned char)*q) == 'D') ? 'E' : *q;
                }
                numbuf[k] = 0;
                val = strtod(numbuf, NULL);
                if (*p == '%') {
                    prec = 1;
                    p++;
                } else if (*p == '!') {
                    prec = 2;
                    p++;
                } else if (*p == '#') {
                    prec = 3;
                    p++;
                } else if (prec == 1 && (val > 32767 || digits > 7)) {
                    prec = digits > 7 ? 3 : 2;
                } else if (prec == 2 && digits > 7) {
                    prec = 3;
                }
            }
            PUSH(T_NUM);
            v[n - 1].n = val;
            v[n - 1].prec = (unsigned char)prec;
            ctx = 2;
            continue;
        }
        if (isalpha((unsigned char)*p)) {
            const char *q = p;
            int len, k, rl;
            char sfx = 0;
            while (is_idch((unsigned char)*p)) {
                p++;
            }
            rl = (int)(p - q);
            if (*p == '$' || *p == '%' || *p == '!' || *p == '#') {
                sfx = *p;
            }
            /* 1) 名前全体がキーワード（$ つきも含む） */
            k = sfx == '$' ? kw_exact(q, rl + 1) : -1;
            if (k >= 0) {
                p++;
                len = rl + 1;
            } else {
                k = kw_exact(q, rl);
                len = rl;
            }
            if (k < 0) {
                /* 2) 先頭がキーワードなら切り出すか */
                int pk = kw_match(q, &len);
                if (pk >= 0 && len < rl + (sfx == '$')) {
                    const char *after = p + (sfx ? 1 : 0);
                    while (*after == ' ') {
                        after++;
                    }
                    if (pk == K_FN) {
                        k = pk;
                    } else if (ctx == 2) {
                        k = pk;                 /* 値のあとは演算子か TO・THEN など */
                    } else if (ctx == 0 && is_stmt_kw(pk)
                               && (*after != '=' || pk == K_FOR || pk == K_IF)) {
                        k = pk;                 /* 文の先頭の命令（FORI=1 など） */
                    } else if (ctx == 1 && kwtext[pk][strlen(kwtext[pk]) - 1] == '$') {
                        k = pk;                 /* CHR$ など */
                    }
                    if (k >= 0) {
                        p = q + len;
                    }
                }
            }
            if (k >= 0) {
                PUSH(T_KW);
                v[n - 1].k = (short)k;
                if (k == K_REM) {
                    PUSH(T_RAW);
                    v[n - 1].s = xstrdup(p);
                    break;
                }
                if (k == K_DATA) {
                    const char *d = p;
                    int inq = 0;
                    char *raw;
                    while (*p && (inq || *p != ':')) {
                        if (*p == '"') {
                            inq = !inq;
                        }
                        p++;
                    }
                    raw = xmalloc(p - d + 1);
                    memcpy(raw, d, p - d);
                    raw[p - d] = 0;
                    PUSH(T_RAW);
                    v[n - 1].s = raw;
                    ctx = 2;
                    continue;
                }
                switch (k) {
                case K_THEN: case K_ELSE:
                    ctx = 0;
                    break;
                case K_INKEY_S: case K_TIME_S: case K_DATE_S: case K_CSRLIN: case K_ERR: case K_ERL:
                case K_TIMER: case K_ESTOP:
                    ctx = 2;
                    break;
                default:
                    ctx = 1;
                }
                continue;
            }
            /* 3) 名前（変数） */
            {
                char name[48];
                int m = 0;
                const char *r;
                for (r = q; r < p && m < 40; r++) {
                    name[m++] = (char)toupper((unsigned char)*r);
                }
                if (sfx) {
                    name[m++] = sfx;
                    p++;
                }
                name[m] = 0;
                PUSH(T_ID);
                v[n - 1].s = xstrdup(name);
                ctx = 2;
            }
            continue;
        }
        /* 記号 */
        {
            int c = *p++;
            if (c == '?') {
                PUSH(T_KW);
                v[n - 1].k = K_PRINT;
                ctx = 1;
                continue;
            }
            if (c == '<' && *p == '=') {
                c = OP_LE;
                p++;
            } else if (c == '=' && *p == '<') {
                c = OP_LE;
                p++;
            } else if (c == '>' && *p == '=') {
                c = OP_GE;
                p++;
            } else if (c == '=' && *p == '>') {
                c = OP_GE;
                p++;
            } else if (c == '<' && *p == '>') {
                c = OP_NE;
                p++;
            } else if (c == '>' && *p == '<') {
                c = OP_NE;
                p++;
            }
            if (!strchr("+-*/\\^=<>(),;:#lgn", c)) {
                free(v);
                err(E_SYNTAX);
            }
            PUSH(T_OP);
            v[n - 1].k = (short)c;
            ctx = (c == ')') ? 2 : (c == ':') ? 0 : 1;
        }
    }
    PUSH(T_EOL);
    *ntok = n;
    return v;
#undef PUSH
}

static void free_line(Line *l)
{
    int i;

    if (!l) {
        return;
    }
    for (i = 0; i < l->nt; i++) {
        free(l->tk[i].s);
    }
    free(l->tk);
    free(l->src);
    free(l);
}

/* 本文を大文字にする（文字列と REM・' のあとはそのまま） */
static void upcase_src(char *s)
{
    int inq = 0;
    char *p;

    for (p = s; *p; p++) {
        if (*p == '"') {
            inq = !inq;
        } else if (!inq) {
            if (*p == '\'') {
                break;
            }
            if ((p == s || !isalnum((unsigned char)p[-1])) && strncasecmp(p, "REM", 3) == 0) {
                p[0] = 'R'; p[1] = 'E'; p[2] = 'M';
                break;
            }
            if ((p == s || !isalnum((unsigned char)p[-1])) && strncasecmp(p, "DATA", 4) == 0) {
                p[0] = 'D'; p[1] = 'A'; p[2] = 'T'; p[3] = 'A';
                p += 3;
                while (p[1] && (inq || p[1] != ':')) {
                    p++;
                    if (*p == '"') {
                        inq = !inq;
                    }
                }
                continue;
            }
            *p = (char)toupper((unsigned char)*p);
        }
    }
}

static Line *make_line(int num, const char *text)
{
    Line *l = xmalloc(sizeof(Line));
    l->num = num;
    l->src = xstrdup(text);
    upcase_src(l->src);
    l->tk = NULL;
    l->nt = 0;
    l->tk = tokenize(text, &l->nt);
    return l;
}

/* ---------------------------------------------------------------- プログラム */
static int find_line(int num, int *exact)
{
    int lo = 0, hi = nprog;

    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (prog[mid]->num < num) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    *exact = lo < nprog && prog[lo]->num == num;
    return lo;
}

static void clear_vars(void);

static unsigned prog_gen = 1;   /* プログラムが変わるたびに増える（SUB・FUNCTION の表を作り直す） */
/* プログラムのファイルの状態（保存し忘れの確認・名前を省いた SAVE のため） */
static char cur_file[600];      /* 最後に LOAD・SAVE した名前（Shift_JIS）。空 = まだない */
static int cur_sjis;            /* そのファイルが Shift_JIS だった（保存も Shift_JIS で） */
static int prog_dirty;          /* 読んだ・保存したあとに、行を変えた */

static void prog_changed(void)
{
    Fn *f;

    prog_gen++;
    prog_dirty = 1;
    while (fns) {               /* DEF FN は行の字句を指しているので、行が変わったら捨てる */
        f = fns->next;
        free(fns->name);
        free(fns);
        fns = f;
    }
    cont_ok = 0;
    nfor = ngosub = nwhile = 0;
}

static void store_line(int num, const char *text)
{
    int ex, i = find_line(num, &ex);

    prog_changed();
    while (*text == ' ') {
        text++;
    }
    if (*text == 0) {
        if (ex) {
            free_line(prog[i]);
            memmove(&prog[i], &prog[i + 1], (nprog - i - 1) * sizeof(Line *));
            nprog--;
        }
        return;
    }
    {
        Line *l = make_line(num, text);
        if (ex) {
            free_line(prog[i]);
            prog[i] = l;
            return;
        }
        if (nprog >= capprog) {
            capprog = capprog ? capprog * 2 : 256;
            prog = realloc(prog, capprog * sizeof(Line *));
            if (!prog) {
                err(E_MEM);
            }
        }
        memmove(&prog[i + 1], &prog[i], (nprog - i) * sizeof(Line *));
        prog[i] = l;
        nprog++;
    }
}

static void prog_new(void)
{
    int i;

    for (i = 0; i < nprog; i++) {
        free_line(prog[i]);
    }
    nprog = 0;
    prog_changed();
    clear_vars();
}

/* ---------------------------------------------------------------- 変数 */
static unsigned hash(const char *s)
{
    unsigned h = 5381;
    while (*s) {
        h = h * 33 + (unsigned char)*s++;
    }
    return h % VHASH;
}

static void clear_vars(void)
{
    int i;
    Fn *f;

    frames_drop();
    for (i = 0; i < VHASH; i++) {
        Var *v = vars[i];
        Arr *a = arrs[i];
        while (v) {
            Var *nx = v->next;
            free(v->name);
            sfree(v->s);
            free(v);
            v = nx;
        }
        while (a) {
            Arr *nx = a->next;
            long j;
            if (a->s) {
                for (j = 0; j < a->count; j++) {
                    sfree(a->s[j]);
                }
            }
            free(a->s);
            free(a->n);
            free(a->name);
            free(a);
            a = nx;
        }
        vars[i] = NULL;
        arrs[i] = NULL;
    }
    while (fns) {
        f = fns->next;
        free(fns->name);
        free(fns);
        fns = f;
    }
    memset(deftype, '!', sizeof(deftype));
    vgen++;
    onerr_line = 0;
    in_err = 0;
    close_all();
    data_li = 0;
    data_tp = -1;
    data_off = 0;
    nfor = ngosub = nwhile = 0;
}

/* 型の文字つきの名前にする */
static void full_name(const char *id, char *out, char *type)
{
    size_t n = strlen(id);
    char last = id[n - 1];

    strcpy(out, id);
    if (last == '$' || last == '%' || last == '!' || last == '#') {
        *type = last;
    } else {
        *type = deftype[toupper((unsigned char)id[0]) - 'A'];
        out[n] = *type;
        out[n + 1] = 0;
    }
}

static Var *var_get_name(const char *full, char type)
{
    unsigned h = hash(full);
    Var *v;

    for (v = vars[h]; v; v = v->next) {
        if (strcmp(v->name, full) == 0) {
            return v;
        }
    }
    v = xmalloc(sizeof(Var));
    v->name = xstrdup(full);
    v->type = type;
    v->n = 0;
    v->s = NULL;
    v->next = vars[h];
    vars[h] = v;
    return v;
}

static Var *var_of(Tok *t)
{
    char full[64], type;

    if (t->cache && t->gen == vgen) {
        return t->cache;
    }
    full_name(t->s, full, &type);
    t->cache = var_get_name(full, type);
    t->gen = vgen;
    return t->cache;
}

static Arr *arr_find(const char *full)
{
    Arr *a;
    for (a = arrs[hash(full)]; a; a = a->next) {
        if (strcmp(a->name, full) == 0) {
            return a;
        }
    }
    return NULL;
}

static Arr *arr_make(const char *full, char type, int nd, const int *dim)
{
    unsigned h = hash(full);
    Arr *a;
    long cnt = 1;
    int i;

    if (arr_find(full)) {
        err(E_DUPDEF);
    }
    for (i = 0; i < nd; i++) {
        if (dim[i] < 0) {
            err(E_ILLFUNC);
        }
        cnt *= dim[i] + 1;
        if (cnt > 4000000) {
            err(E_MEM);
        }
    }
    a = xmalloc(sizeof(Arr));
    a->name = xstrdup(full);
    a->type = type;
    a->nd = nd;
    memcpy(a->dim, dim, nd * sizeof(int));
    a->count = cnt;
    if (type == '$') {
        a->s = calloc(cnt, sizeof(char *));
        a->n = NULL;
        if (!a->s) {
            err(E_MEM);
        }
    } else {
        a->n = calloc(cnt, sizeof(double));
        a->s = NULL;
        if (!a->n) {
            err(E_MEM);
        }
    }
    a->next = arrs[h];
    arrs[h] = a;
    return a;
}

/* ---------------------------------------------------------------- 字句を読む道具 */
#define CUR (&L->tk[tp])

static int is_op(int c)
{
    return CUR->t == T_OP && CUR->k == c;
}

static int is_kw(int k)
{
    return CUR->t == T_KW && CUR->k == k;
}

static void expect_op(int c)
{
    if (!is_op(c)) {
        err(E_SYNTAX);
    }
    tp++;
}

static int accept_op(int c)
{
    if (is_op(c)) {
        tp++;
        return 1;
    }
    return 0;
}

static int accept_kw(int k)
{
    if (is_kw(k)) {
        tp++;
        return 1;
    }
    return 0;
}

static int at_end(void)
{
    return CUR->t == T_EOL || is_op(':') || is_kw(K_ELSE);
}

/* ---------------------------------------------------------------- 式 */
static Val expr(void);

static Val num_val(double n, int prec)
{
    Val v;
    v.type = 'n';
    v.prec = (char)prec;
    v.n = n;
    v.s = NULL;
    return v;
}

static Val str_val(char *s)
{
    Val v;
    v.type = 's';
    v.prec = 0;
    v.n = 0;
    v.s = s;
    return v;
}

/* C の文字列から */
static Val str_copy(const char *s)
{
    return str_val(snew(s ? s : "", s ? strlen(s) : 0));
}

/* BASIC の文字列の写し */
static Val str_dup(const char *s)
{
    return str_val(snew(s, slen(s)));
}

static double num_expr(void)
{
    Val v = expr();
    if (v.type != 'n') {
        sfree(v.s);
        err(E_TYPE);
    }
    return v.n;
}

static char *str_expr(void)
{
    Val v = expr();
    if (v.type != 's') {
        err(E_TYPE);
    }
    return v.s;
}

static long int_expr(void)
{
    double d = num_expr();
    d = floor(d + 0.5);
    if (d < -2147483648.0 || d > 2147483647.0) {
        err(E_OVERFLOW);
    }
    return (long)d;
}

static long to_int16(double d)
{
    d = floor(d + 0.5);
    if (d < -32768 || d > 65535) {
        err(E_OVERFLOW);
    }
    return (long)d;
}

/* 配列の添字を読んで、要素の番号にする */
static long arr_index(Arr *a, const int *idx, int nd)
{
    long off = 0;
    int i;

    if (nd != a->nd) {
        err(E_SUBSCRIPT);
    }
    for (i = 0; i < nd; i++) {
        if (idx[i] < 0 || idx[i] > a->dim[i]) {
            err(E_SUBSCRIPT);
        }
        off = off * (a->dim[i] + 1) + idx[i];
    }
    return off;
}

typedef struct {
    char type;              /* '%' '!' '#' '$' */
    double *np;
    char **sp;
} LV;

/* 変数か配列の要素（名前の字句にいるとき） */
static LV lvalue(void)
{
    Tok *t = CUR;
    LV lv;

    if (t->t != T_ID) {
        err(E_SYNTAX);
    }
    tp++;
    if (is_op('(')) {
        char full[64], type;
        int idx[8], nd = 0;
        Arr *a;
        long off;

        tp++;
        do {
            if (nd >= 8) {
                err(E_SUBSCRIPT);
            }
            idx[nd++] = (int)int_expr();
        } while (accept_op(','));
        expect_op(')');
        full_name(t->s, full, &type);
        a = arr_find(full);
        if (!a) {
            int d[8], i;
            for (i = 0; i < nd; i++) {
                d[i] = 10;
            }
            a = arr_make(full, type, nd, d);
        }
        off = arr_index(a, idx, nd);
        lv.type = a->type;
        if (a->type == '$') {
            lv.sp = &a->s[off];
            lv.np = NULL;
        } else {
            lv.np = &a->n[off];
            lv.sp = NULL;
        }
        return lv;
    }
    {
        Var *v = var_of(t);
        lv.type = v->type;
        if (v->type == '$') {
            lv.sp = &v->s;
            lv.np = NULL;
        } else {
            lv.np = &v->n;
            lv.sp = NULL;
        }
    }
    return lv;
}

static void assign(LV lv, Val v)
{
    if (lv.type == '$') {
        if (v.type != 's') {
            err(E_TYPE);
        }
        sfree(*lv.sp);
        *lv.sp = v.s;
        return;
    }
    if (v.type != 'n') {
        sfree(v.s);
        err(E_TYPE);
    }
    if (lv.type == '%') {
        double d = floor(v.n + 0.5);
        if (d < -32768 || d > 32767) {
            err(E_OVERFLOW);
        }
        *lv.np = d;
    } else if (lv.type == '!') {
        *lv.np = (double)(float)v.n;
    } else {
        *lv.np = v.n;
    }
}

static int prec_of(char type)
{
    return type == '%' ? 1 : type == '#' ? 3 : 2;
}

static unsigned long rnd_next(void)
{
    rnd_state = rnd_state * 1103515245UL + 12345UL;
    rnd_state &= 0xffffffffUL;
    return rnd_state;
}

static Val call_fn(void);

static char *substr(const char *s, long start, long len)
{
    long n = (long)slen(s);

    if (start < 0) {
        start = 0;
    }
    if (start > n) {
        start = n;
    }
    if (len < 0 || start + len > n) {
        len = n - start;
    }
    return snew(s + start, (size_t)len);
}

static char *repeat_ch(int c, long n)
{
    char tmp[MAXSTR + 1];
    if (n < 0 || n > MAXSTR) {
        err(E_ILLFUNC);
    }
    memset(tmp, c, n);
    return snew(tmp, (size_t)n);
}

/* 引数 1 つのかっこ */
static double arg1n(void)
{
    double d;
    expect_op('(');
    d = num_expr();
    expect_op(')');
    return d;
}

static Val func(int k)
{
    double d;
    char *s, *s2;
    long a, b;
    char buf[64];

    switch (k) {
    case K_ABS: d = arg1n(); return num_val(fabs(d), 2);
    case K_INT: d = arg1n(); return num_val(floor(d), 2);
    case K_FIX: d = arg1n(); return num_val(trunc(d), 2);
    case K_SGN: d = arg1n(); return num_val(d > 0 ? 1 : d < 0 ? -1 : 0, 1);
    case K_SQR: d = arg1n(); if (d < 0) err(E_ILLFUNC); return num_val(sqrt(d), 2);
    case K_SIN: d = arg1n(); return num_val(sin(d), 2);
    case K_COS: d = arg1n(); return num_val(cos(d), 2);
    case K_TAN: d = arg1n(); return num_val(tan(d), 2);
    case K_ATN: d = arg1n(); return num_val(atan(d), 2);
    case K_LOG: d = arg1n(); if (d <= 0) err(E_ILLFUNC); return num_val(log(d), 2);
    case K_EXP: d = arg1n(); d = exp(d); if (isinf(d)) err(E_OVERFLOW); return num_val(d, 2);
    case K_CINT: d = arg1n(); d = floor(d + 0.5); if (d < -32768 || d > 32767) err(E_OVERFLOW); return num_val(d, 1);
    case K_CSNG: d = arg1n(); return num_val((float)d, 2);
    case K_CDBL: d = arg1n(); return num_val(d, 3);
    case K_FRE: expect_op('('); sfree(expr().s); expect_op(')'); return num_val(60000, 2);
    case K_INP: {               /* &HE000〜&HE07F は仮想の機器（/dev/io）。ほかの番号は 0 */
        long port = (long)floor(arg1n() + 0.5) & 0xffff;
        if (port >= 0xe000 && port < 0xe080) {
            return num_val(io_read((unsigned)(port - 0xe000)), 1);
        }
        return num_val(0, 1);
    }
    case K_TIMER:               /* 真夜中からの秒（QBasic と同じ） */
        {
            time_t now = time(NULL);
            struct tm *tm = localtime(&now);
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            return num_val(tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec + ts.tv_nsec / 1e9, 2);
        }
    case K_SENSOR: return num_val(io_sensor((long)floor(arg1n() + 0.5)), 2);
    case K_SERVO: case K_PWM: case K_PIN: case K_ADC: case K_DEVICE: {
        /* 名前（文字列）なら /etc/io.conf の口、番号なら仮想の機器の決まった口 */
        Val a;
        long ch;
        double r;
        expect_op('(');
        a = expr();
        expect_op(')');
        if (a.type == 's') {
            char nm[64];
            int n = slen(a.s) < 63 ? (int)slen(a.s) : 63;
            memcpy(nm, a.s ? a.s : "", (size_t)n);
            nm[n] = 0;
            sfree(a.s);
            if (k == K_DEVICE) {
                return num_val(io_device(nm) ? -1 : 0, 1);
            }
            r = io_named_get(nm, k == K_SERVO ? IO_K_SERVO : k == K_PWM ? IO_K_PWM
                                 : k == K_PIN ? IO_K_PIN : IO_K_ADC);
            return num_val(r, 2);
        }
        if (k == K_DEVICE) {
            err(E_TYPE);
        }
        ch = (long)floor(a.n + 0.5);
        if (k == K_SERVO) {
            if (ch < 0 || ch > 63) {
                err(E_ILLFUNC);
            }
            return num_val(io_read(0x40 + (unsigned)ch) / 10.0, 2);
        }
        if (k == K_PWM) {
            if (ch < 0 || ch > 4) {
                err(E_ILLFUNC);
            }
            return num_val(io_read(0x10 + (unsigned)ch), 1);
        }
        if (k == K_PIN) {
            if (ch < 0 || ch > 15) {
                err(E_ILLFUNC);
            }
            return num_val((io_read(VIO_P_DOUT) >> ch) & 1, 1);
        }
        return num_val(io_sensor(ch), 2);
    }
    case K_ESTOP: return num_val(io_read(0x3f), 1);
    case K_PEEK: case K_USR:
        /* 実機の I/O はまだない（デバイス層で読み替える予定） */
        if (k == K_USR && CUR->t == T_NUM) {
            tp++;
        }
        arg1n();
        return num_val(0, 1);
    case K_RND:
        d = 1;
        if (is_op('(')) {
            d = arg1n();
        }
        if (d < 0) {
            rnd_state = (unsigned long)(-d * 65536.0);
        }
        if (d != 0) {
            rnd_last = (double)(rnd_next() >> 8) / 16777216.0;
        }
        return num_val(rnd_last, 2);
    case K_LEN: expect_op('('); s = str_expr(); expect_op(')'); d = (double)slen(s); sfree(s); return num_val(d, 1);
    case K_ASC:
        expect_op('(');
        s = str_expr();
        expect_op(')');
        if (slen(s) == 0) {
            sfree(s);
            err(E_ILLFUNC);
        }
        d = (unsigned char)s[0];
        sfree(s);
        return num_val(d, 1);
    case K_VAL:
        expect_op('(');
        s = str_expr();
        expect_op(')');
        {
            const char *p = s;
            while (*p == ' ') {
                p++;
            }
            if (*p == '&' && (toupper((unsigned char)p[1]) == 'H' || toupper((unsigned char)p[1]) == 'O')) {
                d = (double)strtol(p + 2, NULL, toupper((unsigned char)p[1]) == 'H' ? 16 : 8);
            } else {
                d = strtod(p, NULL);
            }
        }
        sfree(s);
        return num_val(d, 2);
    case K_CHR_S:
        a = (long)floor(arg1n() + 0.5);
        if (a < 0 || a > 255) {
            err(E_ILLFUNC);
        }
        {
            char c = (char)a;
            return str_val(snew(&c, 1));
        }
    case K_STR_S: {
        Val v;
        expect_op('(');
        v = expr();
        expect_op(')');
        if (v.type != 'n') {
            err(E_TYPE);
        }
        fmt_num(buf, v.n, v.prec);
        return str_copy(buf);
    }
    case K_HEX_S: case K_OCT_S:
        a = (long)floor(arg1n() + 0.5);
        if (a < -32768 || a > 65535) {
            err(E_OVERFLOW);
        }
        sprintf(buf, k == K_HEX_S ? "%lX" : "%lo", a & 0xffffL);
        return str_copy(buf);
    case K_LEFT_S: case K_RIGHT_S:
        expect_op('(');
        s = str_expr();
        expect_op(',');
        a = int_expr();
        expect_op(')');
        if (a < 0) {
            err(E_ILLFUNC);
        }
        b = (long)slen(s);
        s2 = k == K_LEFT_S ? substr(s, 0, a) : substr(s, a >= b ? 0 : b - a, a);
        sfree(s);
        return str_val(s2);
    case K_MID_S:
        expect_op('(');
        s = str_expr();
        expect_op(',');
        a = int_expr();
        b = -1;
        if (accept_op(',')) {
            b = int_expr();
            if (b < 0) {
                err(E_ILLFUNC);
            }
        }
        expect_op(')');
        if (a < 1) {
            err(E_ILLFUNC);
        }
        s2 = substr(s, a - 1, b);
        sfree(s);
        return str_val(s2);
    case K_INSTR: {
        Val v;
        long start = 1;
        char *f;
        expect_op('(');
        v = expr();
        if (v.type == 'n') {
            start = (long)v.n;
            expect_op(',');
            s = str_expr();
        } else {
            s = v.s;
        }
        expect_op(',');
        s2 = str_expr();
        expect_op(')');
        if (start < 1) {
            err(E_ILLFUNC);
        }
        d = 0;
        {
            long n = (long)slen(s), m = (long)slen(s2), i;
            (void)f;
            if (start <= n || (m == 0 && start <= n + 1)) {
                for (i = start - 1; i + m <= n; i++) {
                    if (m == 0 || memcmp(s + i, s2, m) == 0) {
                        d = (double)(i + 1);
                        break;
                    }
                }
            }
        }
        sfree(s);
        sfree(s2);
        return num_val(d, 1);
    }
    case K_STRING_S: {
        Val v;
        expect_op('(');
        a = int_expr();
        expect_op(',');
        v = expr();
        expect_op(')');
        if (v.type == 's') {
            int c = (unsigned char)v.s[0];
            size_t n = slen(v.s);
            sfree(v.s);
            if (!n) {
                err(E_ILLFUNC);
            }
            return str_val(repeat_ch(c, a));
        }
        return str_val(repeat_ch((int)v.n & 255, a));
    }
    case K_SPACE_S:
        a = (long)floor(arg1n() + 0.5);
        return str_val(repeat_ch(' ', a));
    case K_INKEY_S:
        a = inkey();
        if (!a) {
            return str_copy("");
        }
        {
            char c = (char)a;
            return str_val(snew(&c, 1));
        }
    case K_INPUT_S: {
        long i;
        BFile *fi = NULL;
        char tmp[MAXSTR + 1];
        expect_op('(');
        a = int_expr();
        if (accept_op(',')) {           /* INPUT$(n,#f) */
            accept_op('#');
            fi = get_file(int_expr());
            if (fi->mode != 'I' && fi->dev != 'K') {
                err(E_ILLFUNC);
            }
        }
        expect_op(')');
        if (a < 1 || a > MAXSTR) {
            err(E_ILLFUNC);
        }
        if (!fi) {
            out_flush();
            tty_rawmode();
        }
        for (i = 0; i < a; i++) {
            int c = fi ? f_getc(fi) : kbd_getc();
            if (c == -2) {
                longjmp(errjb, R_STOP);
            }
            if (c < 0) {
                if (fi) {
                    err(E_PASTEND);
                }
                break;
            }
            tmp[i] = (char)(!fi && c == '\n' ? 13 : c);
        }
        return str_val(snew(tmp, (size_t)i));
    }
    case K_EOF:
        return num_val(f_eof(get_file((long)floor(arg1n() + 0.5))) ? -1 : 0, 1);
    case K_LOF: case K_LOC: {
        BFile *fi = get_file((long)floor(arg1n() + 0.5));
        long pos, end;
        if (fi->dev) {
            return num_val(0, 1);
        }
        if (k == K_LOC) {
            if (fi->mode == 'R') {
                return num_val(fi->recno, 2);
            }
            return num_val(ftell(fi->fp) / 128, 2);
        }
        fflush(fi->fp);
        pos = ftell(fi->fp);
        fseek(fi->fp, 0, SEEK_END);
        end = ftell(fi->fp);
        fseek(fi->fp, pos, SEEK_SET);
        return num_val(end, 2);
    }
    case K_MKI_S: case K_MKS_S: case K_MKD_S: {
        unsigned char b8[8];
        d = arg1n();
        if (k == K_MKI_S) {
            long v = (long)floor(d + 0.5);
            if (v < -32768 || v > 32767) {
                err(E_OVERFLOW);
            }
            b8[0] = (unsigned char)(v & 0xff);
            b8[1] = (unsigned char)((v >> 8) & 0xff);
            return str_val(snew((char *)b8, 2));
        }
        if (k == K_MKS_S) {
            to_mbf4(d, b8);
            return str_val(snew((char *)b8, 4));
        }
        to_mbf8(d, b8);
        return str_val(snew((char *)b8, 8));
    }
    case K_CVI: case K_CVS: case K_CVD: {
        size_t need = k == K_CVI ? 2 : k == K_CVS ? 4 : 8;
        unsigned char *u;
        expect_op('(');
        s = str_expr();
        expect_op(')');
        if (slen(s) < need) {
            sfree(s);
            err(E_ILLFUNC);
        }
        u = (unsigned char *)s;
        if (k == K_CVI) {
            long v = u[0] | (u[1] << 8);
            d = v >= 32768 ? v - 65536 : v;
        } else {
            d = k == K_CVS ? from_mbf4(u) : from_mbf8(u);
        }
        sfree(s);
        return num_val(d, k == K_CVI ? 1 : k == K_CVS ? 2 : 3);
    }
    case K_POINT: {
        double px, py;
        int lx, ly;
        expect_op('(');
        px = num_expr();
        expect_op(',');
        py = num_expr();
        expect_op(')');
        to_log(px, py, &lx, &ly);
        return num_val(gpoint(lx, ly), 1);
    }
    case K_MOUSE: {             /* Tamazusa の拡張: MOUSE(0) X、(1) Y、(2) ボタン、(3) 変わった回数 */
        static int mfd = -2;
        int32_t m[4] = { 0, 0, 0, 0 };
        long n = (long)floor(arg1n() + 0.5);
        if (n < 0 || n > 3) {
            err(E_ILLFUNC);
        }
        gfx_flush(0);
        if (mfd == -2) {
            mfd = open("/dev/mouse", O_RDONLY);
            if (mfd < 0) {
                mfd = -1;
            }
        }
        if (mfd >= 0 && read(mfd, m, sizeof(m)) != (ssize_t)sizeof(m)) {
            m[0] = m[1] = m[2] = m[3] = 0;
        }
        if (n <= 1 && g_lines == 200) {
            return num_val(n == 1 ? m[1] / 2 : m[0], 1);   /* 200 ラインの画面では Y を半分に */
        }
        return num_val(m[n], 1);
    }
    case K_TIME_S: case K_DATE_S: {
        time_t t = time(NULL);
        struct tm *tm = localtime(&t);
        if (k == K_TIME_S) {
            sprintf(buf, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
        } else {
            sprintf(buf, "%02d/%02d/%02d", tm->tm_year % 100, tm->tm_mon + 1, tm->tm_mday);
        }
        return str_copy(buf);
    }
    case K_CSRLIN: return num_val(row, 1);
    case K_POS: arg1n(); return num_val(col, 1);
    case K_ERR: return num_val(last_err, 1);
    case K_ERL: return num_val(last_erl, 1);
    }
    err(E_SYNTAX);
    return num_val(0, 1);
}

static Val primary(void)
{
    Tok *t = CUR;

    switch (t->t) {
    case T_NUM:
        tp++;
        return num_val(t->n, t->prec);
    case T_STR:
        tp++;
        return str_copy(t->s);
    case T_ID: {
        Val v;
        LV lv;
        if (L->tk[tp + 1].t == T_OP && L->tk[tp + 1].k == '(' && try_call_fn(&v)) {
            return v;                   /* FUNCTION の呼び出し */
        }
        if (L->tk[tp + 1].t != T_OP || L->tk[tp + 1].k != '(') {
            Var *var = var_of(t);       /* 速い道: ただの変数 */
            tp++;
            if (var->type == '$') {
                return str_dup(var->s);
            }
            return num_val(var->n, prec_of(var->type));
        }
        lv = lvalue();
        if (lv.type == '$') {
            return str_dup(*lv.sp);
        }
        v = num_val(*lv.np, prec_of(lv.type));
        return v;
    }
    case T_OP:
        if (t->k == '(') {
            Val v;
            tp++;
            v = expr();
            expect_op(')');
            return v;
        }
        break;
    case T_KW:
        if (t->k == K_FN) {
            tp++;
            return call_fn();
        }
        if ((t->k >= K_PEEK && t->k <= K_MOUSE) || t->k == K_TIMER || t->k == K_SENSOR
            || t->k == K_SERVO || t->k == K_PWM || t->k == K_ESTOP || t->k == K_PIN || t->k == K_ADC
            || t->k == K_DEVICE) {
            tp++;
            return func(t->k);
        }
        break;
    }
    err(E_SYNTAX);
    return num_val(0, 1);
}

static Val unary(void);

static Val power(void)
{
    Val a = primary();
    while (is_op('^')) {
        Val b;
        tp++;
        b = unary();
        if (a.type != 'n' || b.type != 'n') {
            err(E_TYPE);
        }
        if (a.n == 0 && b.n < 0) {
            err(E_DIV0);
        }
        a.n = pow(a.n, b.n);
        a.prec = a.prec == 3 || b.prec == 3 ? 3 : 2;
    }
    return a;
}

static Val unary(void)
{
    if (is_op('-')) {
        Val v;
        tp++;
        v = unary();
        if (v.type != 'n') {
            err(E_TYPE);
        }
        v.n = -v.n;
        return v;
    }
    if (is_op('+')) {
        tp++;
        return unary();
    }
    return power();
}

static int maxp(int a, int b)
{
    return a > b ? a : b;
}

static Val term(void)
{
    Val a = unary();
    while (is_op('*') || is_op('/')) {
        int op = CUR->k;
        Val b;
        tp++;
        b = unary();
        if (a.type != 'n' || b.type != 'n') {
            err(E_TYPE);
        }
        if (op == '*') {
            a.n *= b.n;
            a.prec = (char)maxp(a.prec, b.prec);
            if (a.prec == 1 && fabs(a.n) > 32767) {
                a.prec = 2;
            }
        } else {
            if (b.n == 0) {
                err(E_DIV0);
            }
            a.n /= b.n;
            a.prec = (char)(a.prec == 3 || b.prec == 3 ? 3 : 2);
        }
    }
    return a;
}

static Val idiv(void)
{
    Val a = term();
    while (is_op('\\')) {
        Val b;
        long x, y;
        tp++;
        b = term();
        if (a.type != 'n' || b.type != 'n') {
            err(E_TYPE);
        }
        x = (long)floor(a.n + 0.5);
        y = (long)floor(b.n + 0.5);
        if (y == 0) {
            err(E_DIV0);
        }
        a = num_val((double)(x / y), 1);
    }
    return a;
}

static Val modexp(void)
{
    Val a = idiv();
    while (is_kw(K_MOD)) {
        Val b;
        long x, y;
        tp++;
        b = idiv();
        if (a.type != 'n' || b.type != 'n') {
            err(E_TYPE);
        }
        x = (long)floor(a.n + 0.5);
        y = (long)floor(b.n + 0.5);
        if (y == 0) {
            err(E_DIV0);
        }
        a = num_val((double)(x % y), 1);
    }
    return a;
}

static Val addexp(void)
{
    Val a = modexp();
    while (is_op('+') || is_op('-')) {
        int op = CUR->k;
        Val b;
        tp++;
        b = modexp();
        if (a.type == 's' && b.type == 's' && op == '+') {
            size_t la = slen(a.s), lb = slen(b.s);
            char *r;
            if (la + lb > MAXSTR) {
                err(E_STRLONG);
            }
            r = snew(NULL, la + lb);
            memcpy(r, a.s, la);
            memcpy(r + la, b.s, lb);
            sfree(a.s);
            sfree(b.s);
            a.s = r;
            continue;
        }
        if (a.type != 'n' || b.type != 'n') {
            err(E_TYPE);
        }
        a.n = op == '+' ? a.n + b.n : a.n - b.n;
        a.prec = (char)maxp(a.prec, b.prec);
        if (a.prec == 1 && fabs(a.n) > 32767) {
            a.prec = 2;
        }
    }
    return a;
}

static Val relexp(void)
{
    Val a = addexp();
    for (;;) {
        int op;
        Val b;
        int r;
        if (CUR->t != T_OP || !strchr("=<>lgn", CUR->k)) {
            return a;
        }
        op = CUR->k;
        tp++;
        b = addexp();
        if (a.type != b.type) {
            err(E_TYPE);
        }
        if (a.type == 's') {
            int c = scmp(a.s, b.s);
            sfree(a.s);
            sfree(b.s);
            r = op == '=' ? c == 0 : op == '<' ? c < 0 : op == '>' ? c > 0
              : op == OP_LE ? c <= 0 : op == OP_GE ? c >= 0 : c != 0;
        } else {
            double x = a.n, y = b.n;
            if (a.prec < 3 && b.prec < 3) {     /* 単精度どうしは単精度で比べる */
                x = (float)x;
                y = (float)y;
            }
            r = op == '=' ? x == y : op == '<' ? x < y : op == '>' ? x > y
              : op == OP_LE ? x <= y : op == OP_GE ? x >= y : x != y;
        }
        a = num_val(r ? -1 : 0, 1);
    }
}

static Val notexp(void)
{
    if (is_kw(K_NOT)) {
        Val v;
        tp++;
        v = notexp();
        if (v.type != 'n') {
            err(E_TYPE);
        }
        return num_val((double)(~to_int16(v.n) & 0xffff) - ((~to_int16(v.n) & 0x8000) ? 65536 : 0), 1);
    }
    return relexp();
}

static long s16(long x)
{
    x &= 0xffff;
    return x >= 0x8000 ? x - 0x10000 : x;
}

static Val logexp(int level)
{
    static const int ops[] = { K_IMP, K_EQV, K_XOR, K_OR, K_AND };
    Val a = level == 5 ? notexp() : logexp(level + 1);
    if (level == 5) {
        return a;
    }
    while (is_kw(ops[level])) {
        Val b;
        long x, y, r;
        int k = ops[level];
        tp++;
        b = logexp(level + 1);
        if (a.type != 'n' || b.type != 'n') {
            err(E_TYPE);
        }
        x = to_int16(a.n);
        y = to_int16(b.n);
        switch (k) {
        case K_AND: r = x & y; break;
        case K_OR: r = x | y; break;
        case K_XOR: r = x ^ y; break;
        case K_EQV: r = ~(x ^ y); break;
        default: r = (~x) | y; break;
        }
        a = num_val((double)s16(r), 1);
    }
    return a;
}

static Val expr(void)
{
    return logexp(0);
}

/* ---------------------------------------------------------------- 行への移動 */
static int line_index(int num)
{
    int ex, i = find_line(num, &ex);
    if (!ex) {
        err(E_UNDEFLINE);
    }
    return i;
}

static void go_line(int idx)
{
    li = idx;
    L = prog[idx];
    tp = 0;
    jumped = 1;
    if (trace) {
        printf("[%d]", L->num);
        out_dirty = 1;
    }
}

static void go_pos(Pos p)
{
    li = p.li;
    L = p.li < 0 ? direct : prog[p.li];
    tp = p.tp;
    jumped = 1;
}

static Pos here(void)
{
    Pos p;
    p.li = li;
    p.tp = tp;
    return p;
}

/* ひとつ前の字句が、そのキーワードか（EXIT FOR・DO WHILE などを数え違えないため） */
static int prev_is(int k)
{
    return tp > 0 && L->tk[tp - 1].t == T_KW && L->tk[tp - 1].k == k;
}

/* 文の先頭か（行の最初か、: のすぐあと） */
static int at_stmt_start(void)
{
    return tp == 0 || (L->tk[tp - 1].t == T_OP && L->tk[tp - 1].k == ':');
}

/* 行の終わりまで飛ばす */
static void skip_line(void)
{
    while (CUR->t != T_EOL) {
        tp++;
    }
}

/* 文の終わり（: か行末か ELSE）まで飛ばす */
static void skip_stmt(void)
{
    while (!at_end()) {
        tp++;
    }
}

/* 次の文へ進む（行をまたぐ）。0 = プログラムの終わり */
static int advance_token(void)
{
    if (CUR->t != T_EOL) {
        tp++;
        return 1;
    }
    if (li < 0 || li + 1 >= nprog) {
        return 0;
    }
    li++;
    L = prog[li];
    tp = 0;
    return 1;
}

static void do_next(void);

/* FOR に対応する NEXT のあとまで飛ばす（くり返しが 0 回のとき） */
static void skip_for(void)
{
    int depth = 0;

    for (;;) {
        if (!advance_token()) {
            err(E_FORNEXT);
        }
        if (CUR->t == T_KW && CUR->k == K_FOR && !prev_is(K_EXIT)) {
            depth++;
            continue;
        }
        if (CUR->t == T_KW && CUR->k == K_NEXT) {
            tp++;
            if (at_end()) {
                if (depth == 0) {
                    jumped = 1;
                    return;
                }
                depth--;
                tp--;
                continue;
            }
            for (;;) {
                if (CUR->t != T_ID) {
                    err(E_SYNTAX);
                }
                if (depth == 0) {
                    tp++;
                    jumped = 1;
                    if (accept_op(',')) {
                        do_next();          /* NEXT J,I の残り（外のループ） */
                    }
                    return;
                }
                depth--;
                tp++;
                if (!accept_op(',')) {
                    break;
                }
            }
            tp--;
        }
    }
}

/* WHILE に対応する WEND のあとまで飛ばす */
static void skip_while(void)
{
    int depth = 0;

    for (;;) {
        if (!advance_token()) {
            err(E_WHILE);
        }
        if (CUR->t == T_KW && CUR->k == K_WHILE && !prev_is(K_DO) && !prev_is(K_LOOP)) {
            depth++;
        } else if (CUR->t == T_KW && CUR->k == K_WEND) {
            if (depth == 0) {
                tp++;
                jumped = 1;
                return;
            }
            depth--;
        }
    }
}

/* ---------------------------------------------------------------- 文 */
static void stmt(void);

static BFile *file_arg(void);
#define CURCOL (outf && outf->dev != 'S' ? outf->col : col)

static void do_print(void)
{
    int nl = 1;
    char buf[80];

    outf = NULL;
    if (accept_op('#')) {
        BFile *f = file_arg();
        if (f->mode != 'O' && f->mode != 'A') {
            err(E_ILLFUNC);
        }
        if (!at_end()) {
            expect_op(',');
        }
        outf = f;
    }
    if (accept_kw(K_USING)) {
        char *fmt = str_expr();
        void print_using(const char *fmt);
        if (!accept_op(';')) {
            expect_op(',');
        }
        print_using(fmt);
        sfree(fmt);
        outf = NULL;
        out_maybe_flush();
        return;
    }
    while (!at_end()) {
        if (accept_op(';')) {
            nl = 0;
            continue;
        }
        if (accept_op(',')) {
            int next = (CURCOL / 14 + 1) * 14;
            if (next >= 80 && !outf) {
                newline();
            } else {
                while (CURCOL < next) {
                    put_str(" ");
                }
            }
            nl = 0;
            continue;
        }
        nl = 1;
        if (is_kw(K_TAB) || is_kw(K_SPC)) {
            int k = CUR->k;
            long n;
            tp++;
            n = (long)floor(arg1n() + 0.5);
            if (n < 0 || n > 255) {
                err(E_ILLFUNC);
            }
            if (k == K_TAB) {
                if (CURCOL > n) {
                    newline();
                }
                while (CURCOL < n) {
                    put_str(" ");
                }
            } else {
                while (n-- > 0) {
                    put_str(" ");
                }
            }
            nl = 0;
            continue;
        }
        {
            Val v = expr();
            if (v.type == 's') {
                put_mem(v.s, slen(v.s));
                sfree(v.s);
            } else {
                fmt_num(buf, v.n, v.prec);
                strcat(buf, " ");
                put_str(buf);
            }
        }
    }
    if (nl) {
        newline();
    }
    outf = NULL;
    out_maybe_flush();
}

/* PRINT USING（# . , + - ** $$ ^^^^ と、文字列の ! & \  \） */
static int using_field(const char *f, int *pos)
{
    const char *p = f + *pos;
    return *p == '#' || (*p == '.' && p[1] == '#') || (*p == '+' && (p[1] == '#' || p[1] == '.'))
        || (*p == '*' && p[1] == '*') || (*p == '$' && p[1] == '$');
}

void print_using(const char *fmt)
{
    int pos = 0, used = 0;
    int flen = (int)strlen(fmt);

    for (;;) {
        if (pos >= flen) {
            if (!used || at_end() || is_op(';') || is_op(',')) {
                break;
            }
            pos = 0;
        }
        if (fmt[pos] == '_' && pos + 1 < flen) {
            char c[2] = { fmt[pos + 1], 0 };
            put_str(c);
            pos += 2;
            continue;
        }
        if (fmt[pos] == '!' || fmt[pos] == '&' || fmt[pos] == '\\') {
            int w;
            char *s;
            if (at_end()) {
                break;
            }
            if (fmt[pos] == '!') {
                w = 1;
                pos++;
            } else if (fmt[pos] == '&') {
                w = -1;
                pos++;
            } else {
                int e = pos + 1;
                while (e < flen && fmt[e] == ' ') {
                    e++;
                }
                if (e >= flen || fmt[e] != '\\') {
                    char c[2] = { fmt[pos], 0 };
                    put_str(c);
                    pos++;
                    continue;
                }
                w = e - pos + 1;
                pos = e + 1;
            }
            s = str_expr();
            used = 1;
            if (w < 0) {
                put_mem(s, slen(s));
            } else {
                int i, n = (int)slen(s);
                for (i = 0; i < w; i++) {
                    char c[2] = { i < n ? s[i] : ' ', 0 };
                    put_str(c);
                }
            }
            sfree(s);
            if (!accept_op(';')) {
                accept_op(',');
            }
            continue;
        }
        if (using_field(fmt, &pos)) {
            int plus = 0, minus_after = 0, star = 0, dollar = 0, comma = 0, expo = 0;
            int before = 0, after = -1;
            double v;
            char num[128], out[160];
            int w;

            if (at_end()) {
                break;
            }
            if (fmt[pos] == '+') {
                plus = 1;
                pos++;
            }
            if (fmt[pos] == '*' && fmt[pos + 1] == '*') {
                star = 1;
                before += 2;
                pos += 2;
                if (fmt[pos] == '$') {
                    dollar = 1;
                    before++;
                    pos++;
                }
            } else if (fmt[pos] == '$' && fmt[pos + 1] == '$') {
                dollar = 1;
                before += 1;
                pos += 2;
            }
            while (pos < flen && (fmt[pos] == '#' || fmt[pos] == ',')) {
                if (fmt[pos] == ',') {
                    comma = 1;
                }
                before++;
                pos++;
            }
            if (pos < flen && fmt[pos] == '.') {
                pos++;
                after = 0;
                while (pos < flen && fmt[pos] == '#') {
                    after++;
                    pos++;
                }
            }
            if (pos + 3 < flen && strncmp(fmt + pos, "^^^^", 4) == 0) {
                expo = 1;
                pos += 4;
            }
            if (pos < flen && fmt[pos] == '+' && !plus) {
                plus = 2;
                pos++;
            } else if (pos < flen && fmt[pos] == '-') {
                minus_after = 1;
                pos++;
            }
            v = num_expr();
            used = 1;
            if (expo) {
                sprintf(num, "%.*E", after < 0 ? 0 : after, fabs(v));
            } else {
                sprintf(num, "%.*f", after < 0 ? 0 : after, fabs(v));
                if (after < 0 && strchr(num, '.')) {
                    *strchr(num, '.') = 0;
                }
                if (num[0] == '0' && num[1] == '.' && before > 0) {
                    /* 0.5 は「0.50」のまま */
                }
                if (comma) {
                    char tmp[128];
                    char *dot = strchr(num, '.');
                    int ip = dot ? (int)(dot - num) : (int)strlen(num);
                    int i, j = 0;
                    for (i = 0; i < ip; i++) {
                        tmp[j++] = num[i];
                        if ((ip - i - 1) % 3 == 0 && i != ip - 1) {
                            tmp[j++] = ',';
                        }
                    }
                    strcpy(tmp + j, dot ? dot : "");
                    strcpy(num, tmp);
                }
            }
            out[0] = 0;
            if (v < 0 && !minus_after && plus != 2) {
                strcat(out, "-");
            } else if (plus == 1) {
                strcat(out, "+");
            }
            if (dollar) {
                strcat(out, "$");
            }
            strcat(out, num);
            if (minus_after) {
                strcat(out, v < 0 ? "-" : " ");
            } else if (plus == 2) {
                strcat(out, v < 0 ? "-" : "+");
            }
            w = before + (after >= 0 ? after + 1 : 0) + (plus == 1) + (expo ? 4 : 0);
            {
                int len = (int)strlen(out) - (minus_after || plus == 2);
                if (len > w) {
                    put_str("%");
                } else {
                    while (len < w) {
                        put_str(star ? "*" : " ");
                        len++;
                    }
                }
            }
            put_str(out);
            if (!accept_op(';')) {
                accept_op(',');
            }
            continue;
        }
        {
            char c[2] = { fmt[pos], 0 };
            put_str(c);
            pos++;
        }
    }
    if (is_op(';') || is_op(',')) {
        tp++;
    } else {
        newline();
    }
    {
        /* 末尾が ; なら改行しない（上で処理済み） */
    }
}

/* INPUT の 1 項目を読む。, 区切り。引用符は外す */
static const char *input_item(const char *p, char *out, int max)
{
    int n = 0;

    while (*p == ' ') {
        p++;
    }
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && n < max - 1) {
            out[n++] = *p++;
        }
        if (*p == '"') {
            p++;
        }
        while (*p && *p != ',') {
            p++;
        }
    } else {
        while (*p && *p != ',' && n < max - 1) {
            out[n++] = *p++;
        }
        while (n > 0 && out[n - 1] == ' ') {
            n--;
        }
    }
    out[n] = 0;
    return p;
}

static int is_number_text(const char *s)
{
    char *e;
    while (*s == ' ') {
        s++;
    }
    if (*s == 0) {
        return 1;           /* 空は 0 */
    }
    strtod(s, &e);
    while (*e == ' ') {
        e++;
    }
    return *e == 0 && e != s;
}

static void file_input(int line_input);

static void do_input(int line_input)
{
    char prompt[MAXLINE] = "";
    int qmark = 1;
    char buf[MAXLINE];
    int start_tp;

    if (is_op('#')) {
        file_input(line_input);
        return;
    }

    if (li < 0) {
        /* 直接モードでも使える（N88 は Illegal direct だが便利なので許す） */
    }
    if (is_op(';')) {
        tp++;               /* INPUT; は改行しない（ここでは無視） */
    }
    if (CUR->t == T_STR) {
        strncpy(prompt, CUR->s, sizeof(prompt) - 1);
        tp++;
        if (accept_op(',')) {
            qmark = 0;
        } else {
            expect_op(';');
        }
    }
    if (line_input) {
        LV lv;
        int r;
        put_str(prompt);
        lv = lvalue();
        if (lv.type != '$') {
            err(E_TYPE);
        }
        r = read_line(buf, sizeof(buf));
        if (r == -2) {
            longjmp(errjb, R_STOP);
        }
        if (r < 0) {
            buf[0] = 0;
        }
        assign(lv, str_copy(buf));
        return;
    }
    start_tp = tp;
    for (;;) {
        const char *p;
        int ok = 1, r;
        put_str(prompt);
        if (qmark) {
            put_str("? ");
        }
        r = read_line(buf, sizeof(buf));
        if (r == -2) {
            longjmp(errjb, R_STOP);
        }
        if (r < 0) {
            longjmp(errjb, R_END);
        }
        /* まず形を確かめ（数か、数の数）、よければ代入する */
        p = buf;
        tp = start_tp;
        for (;;) {
            char item[MAXLINE];
            Tok *t = CUR;
            char full[64], type;
            if (t->t != T_ID) {
                err(E_SYNTAX);
            }
            full_name(t->s, full, &type);
            p = input_item(p, item, sizeof(item));
            if (type != '$' && !is_number_text(item)) {
                ok = 0;
            }
            tp++;
            if (is_op('(')) {           /* 添字は読み飛ばす（あとで評価する） */
                int depth = 0;
                do {
                    if (is_op('(')) depth++;
                    if (is_op(')')) depth--;
                    tp++;
                } while (depth > 0 && CUR->t != T_EOL);
            }
            if (!accept_op(',')) {
                break;
            }
            if (*p != ',') {
                ok = 0;
                break;
            }
            p++;
        }
        if (ok && *p == ',') {
            ok = 0;
        }
        if (!ok) {
            put_str("?Redo from start\n");
            continue;
        }
        p = buf;
        tp = start_tp;
        for (;;) {
            char item[MAXLINE];
            LV lv = lvalue();
            p = input_item(p, item, sizeof(item));
            if (lv.type == '$') {
                assign(lv, str_copy(item));
            } else {
                assign(lv, num_val(strtod(item, NULL), 2));
            }
            if (!accept_op(',')) {
                break;
            }
            if (*p == ',') {
                p++;
            }
        }
        return;
    }
}

/* DATA を 1 つ読む */
static int data_next(char *out, int max, int *was_quoted)
{
    for (;;) {
        Line *l;
        const char *s;
        if (data_li >= nprog) {
            return 0;
        }
        l = prog[data_li];
        if (data_tp < 0) {
            /* この行の DATA を探す */
            int i;
            for (i = 0; i < l->nt; i++) {
                if (l->tk[i].t == T_KW && l->tk[i].k == K_DATA) {
                    break;
                }
            }
            if (i >= l->nt) {
                data_li++;
                continue;
            }
            data_tp = i + 1;
            data_off = 0;
        }
        if (data_tp >= l->nt || l->tk[data_tp].t != T_RAW) {
            /* 同じ行の次の DATA */
            int i;
            for (i = data_tp; i < l->nt; i++) {
                if (l->tk[i].t == T_KW && l->tk[i].k == K_DATA) {
                    break;
                }
            }
            if (i >= l->nt) {
                data_li++;
                data_tp = -1;
                continue;
            }
            data_tp = i + 1;
            data_off = 0;
            continue;
        }
        s = l->tk[data_tp].s;
        if (data_off > (int)strlen(s)) {
            data_tp++;
            continue;
        }
        {
            const char *p = s + data_off;
            const char *q;
            int n = 0;
            while (*p == ' ') {
                p++;
            }
            *was_quoted = *p == '"';
            q = input_item(p, out, max);
            (void)n;
            data_off = (int)(q - s) + 1;   /* 「,」の次 */
            if (*q == 0) {
                data_off = (int)strlen(s) + 1;
            }
            return 1;
        }
    }
}

static void do_read(void)
{
    do {
        char item[MAXLINE];
        int q;
        LV lv = lvalue();
        if (!data_next(item, sizeof(item), &q)) {
            err(E_DATA);
        }
        if (lv.type == '$') {
            assign(lv, str_copy(item));
        } else {
            if (!is_number_text(item)) {
                err(E_SYNTAX);
            }
            assign(lv, num_val(strtod(item, NULL), 2));
        }
    } while (accept_op(','));
}

static void do_restore(void)
{
    data_li = 0;
    data_tp = -1;
    data_off = 0;
    if (CUR->t == T_NUM) {
        int ex;
        data_li = find_line((int)CUR->n, &ex);
        if (!ex) {
            err(E_UNDEFLINE);
        }
        tp++;
    }
}

static void do_dim(void)
{
    do {
        Tok *t = CUR;
        char full[64], type;
        int d[8], nd = 0;
        if (t->t != T_ID) {
            err(E_SYNTAX);
        }
        tp++;
        expect_op('(');
        do {
            if (nd >= 8) {
                err(E_SUBSCRIPT);
            }
            d[nd++] = (int)int_expr();
        } while (accept_op(','));
        expect_op(')');
        full_name(t->s, full, &type);
        arr_make(full, type, nd, d);
    } while (accept_op(','));
}

static void do_erase(void)
{
    do {
        Tok *t = CUR;
        char full[64], type;
        unsigned h;
        Arr **pp;
        if (t->t != T_ID) {
            err(E_SYNTAX);
        }
        tp++;
        full_name(t->s, full, &type);
        h = hash(full);
        for (pp = &arrs[h]; *pp; pp = &(*pp)->next) {
            if (strcmp((*pp)->name, full) == 0) {
                Arr *a = *pp;
                long j;
                *pp = a->next;
                if (a->s) {
                    for (j = 0; j < a->count; j++) {
                        sfree(a->s[j]);
                    }
                }
                free(a->s);
                free(a->n);
                free(a->name);
                free(a);
                break;
            }
        }
        if (!*pp && 0) {
            err(E_ILLFUNC);
        }
    } while (accept_op(','));
}

static void do_deftype(int k)
{
    char t = k == K_DEFINT ? '%' : k == K_DEFSNG ? '!' : k == K_DEFDBL ? '#' : '$';
    do {
        int a, b;
        if (CUR->t != T_ID || strlen(CUR->s) != 1) {
            err(E_SYNTAX);
        }
        a = b = CUR->s[0] - 'A';
        tp++;
        if (accept_op('-')) {
            if (CUR->t != T_ID || strlen(CUR->s) != 1) {
                err(E_SYNTAX);
            }
            b = CUR->s[0] - 'A';
            tp++;
        }
        for (; a <= b; a++) {
            deftype[a] = t;
        }
    } while (accept_op(','));
    vgen++;
}

static void do_def(void)
{
    Fn *f;
    char full[64], type;

    if (accept_kw(K_SEG)) {         /* DEF SEG: 段階 1 は何もしない */
        if (accept_op('=')) {
            num_expr();
        }
        return;
    }
    if (li < 0) {
        err(E_DIRECT);
    }
    if (!accept_kw(K_FN)) {
        err(E_SYNTAX);
    }
    if (CUR->t != T_ID) {
        err(E_SYNTAX);
    }
    full_name(CUR->s, full, &type);
    tp++;
    for (f = fns; f; f = f->next) {
        if (strcmp(f->name, full) == 0) {
            break;
        }
    }
    if (!f) {
        f = xmalloc(sizeof(Fn));
        f->name = xstrdup(full);
        f->next = fns;
        fns = f;
    }
    f->type = type;
    f->np = 0;
    if (accept_op('(')) {
        do {
            if (CUR->t != T_ID || f->np >= 8) {
                err(E_SYNTAX);
            }
            f->param[f->np++] = CUR;
            tp++;
        } while (accept_op(','));
        expect_op(')');
    }
    expect_op('=');
    f->line = L;
    f->tp = tp;
    skip_stmt();
}

static Val call_fn(void)
{
    char full[64], type;
    Fn *f;
    Val args[8], saved[8], r;
    int n = 0, i;
    Line *sL;
    int sli, stp;

    if (CUR->t != T_ID) {
        err(E_SYNTAX);
    }
    full_name(CUR->s, full, &type);
    tp++;
    for (f = fns; f; f = f->next) {
        if (strcmp(f->name, full) == 0) {
            break;
        }
    }
    if (!f) {
        err(E_UNDEFFN);
    }
    if (accept_op('(')) {
        do {
            if (n >= 8) {
                err(E_SYNTAX);
            }
            args[n++] = expr();
        } while (accept_op(','));
        expect_op(')');
    }
    if (n != f->np) {
        err(E_SYNTAX);
    }
    for (i = 0; i < n; i++) {
        Var *v = var_of(f->param[i]);
        saved[i].type = v->type == '$' ? 's' : 'n';
        saved[i].n = v->n;
        saved[i].s = v->s;
        v->s = NULL;
        if (v->type == '$') {
            if (args[i].type != 's') {
                err(E_TYPE);
            }
            v->s = args[i].s;
        } else {
            if (args[i].type != 'n') {
                err(E_TYPE);
            }
            v->n = args[i].n;
        }
    }
    sL = L;
    sli = li;
    stp = tp;
    L = f->line;
    tp = f->tp;
    r = expr();
    L = sL;
    li = sli;
    tp = stp;
    for (i = 0; i < n; i++) {
        Var *v = var_of(f->param[i]);
        sfree(v->s);
        v->s = saved[i].s;
        v->n = saved[i].n;
    }
    if ((type == '$') != (r.type == 's')) {
        err(E_TYPE);
    }
    if (r.type == 'n') {
        r.prec = (char)prec_of(type);
    }
    return r;
}

static void do_for(void)
{
    LV lv;
    Var *v;
    Val start;
    double lim, step = 1;
    ForEnt *fe;
    int i;

    if (CUR->t != T_ID || (L->tk[tp + 1].t == T_OP && L->tk[tp + 1].k == '(')) {
        err(E_SYNTAX);
    }
    v = var_of(CUR);
    if (v->type == '$') {
        err(E_TYPE);
    }
    lv = lvalue();
    expect_op('=');
    start = expr();
    assign(lv, start);
    if (!accept_kw(K_TO)) {
        err(E_SYNTAX);
    }
    lim = num_expr();
    if (accept_kw(K_STEP)) {
        step = num_expr();
    }
    /* 同じ変数の古い FOR は捨てる */
    for (i = nfor - 1; i >= 0; i--) {
        if (forstk[i].v == v) {
            nfor = i;
            break;
        }
    }
    if ((step >= 0 && v->n > lim) || (step < 0 && v->n < lim)) {
        skip_for();
        return;
    }
    if (nfor >= 64) {
        err(E_MEM);
    }
    fe = &forstk[nfor++];
    fe->v = v;
    fe->lim = lim;
    fe->step = step;
    fe->at = here();
    fe->ndo = block_depth(0);
    fe->nwhile = nwhile;
    fe->nsel = block_depth(1);
}

static void do_next(void)
{
    for (;;) {
        ForEnt *fe;
        Var *v = NULL;
        if (CUR->t == T_ID) {
            v = var_of(CUR);
            tp++;
            while (nfor > 0 && forstk[nfor - 1].v != v) {
                nfor--;
            }
        }
        if (nfor == 0) {
            err(E_NEXT);
        }
        fe = &forstk[nfor - 1];
        fe->v->n += fe->step;
        if (fe->v->type == '%') {
            if (fe->v->n > 32767 || fe->v->n < -32768) {
                err(E_OVERFLOW);
            }
        } else if (fe->v->type == '!') {
            fe->v->n = (float)fe->v->n;
        }
        if ((fe->step >= 0 && fe->v->n <= fe->lim) || (fe->step < 0 && fe->v->n >= fe->lim)) {
            go_pos(fe->at);
            return;
        }
        nfor--;
        if (!accept_op(',')) {
            return;
        }
    }
}

static void do_goto_num(void)
{
    if (CUR->t != T_NUM) {
        err(E_SYNTAX);
    }
    go_line(line_index((int)CUR->n));
}

static void do_gosub_to(int num)
{
    Pos p;
    int idx = line_index(num);

    if (ngosub >= 256) {
        err(E_MEM);
    }
    p = here();
    gosubstk[ngosub++] = p;
    go_line(idx);
}

static void if_scan(int want_branch);

static void do_if(void)
{
    Val c = expr();
    int truth;

    if (c.type == 's') {
        truth = slen(c.s) != 0;
        sfree(c.s);
    } else {
        truth = c.n != 0;
    }
    if (is_kw(K_GOTO)) {
        tp++;
        if (truth) {
            do_goto_num();
            return;
        }
    } else if (accept_kw(K_THEN)) {
        if (CUR->t == T_EOL || is_kw(K_REM)) {      /* 複数行の IF（END IF まで） */
            if (!truth) {
                if_scan(1);
            }
            jumped = 1;
            return;
        }
        if (truth) {
            if (CUR->t == T_NUM) {
                do_goto_num();
            }
            jumped = 1;     /* THEN のあとの文はふつうに続けて実行する */
            return;
        }
    } else {
        err(E_SYNTAX);
    }
    /* 偽: 対応する ELSE を探す */
    {
        int depth = 0;
        while (CUR->t != T_EOL) {
            if (CUR->t == T_KW && CUR->k == K_IF) {
                depth++;
            } else if (CUR->t == T_KW && CUR->k == K_ELSE) {
                if (depth == 0) {
                    tp++;
                    if (CUR->t == T_NUM) {
                        do_goto_num();
                    } else {
                        jumped = 1;
                    }
                    return;
                }
                depth--;
            }
            tp++;
        }
        jumped = 1;
    }
}

static void set_timer(double sec, int line);

static void do_on(void)
{
    long n;
    int is_sub;
    long i = 1;

    if (accept_kw(K_TIMER)) {           /* ON TIMER(秒) GOSUB 行番号（TIMER ON で始まる） */
        double sec = arg1n();
        if (!accept_kw(K_GOSUB) || CUR->t != T_NUM || !(sec > 0) || sec > 86400) {
            err(E_SYNTAX);
        }
        line_index((int)CUR->n);
        set_timer(sec, (int)CUR->n);
        tp++;
        return;
    }
    if (accept_kw(K_ERROR)) {           /* ON ERROR GOTO 行番号（0 でやめる） */
        int num;
        if (!accept_kw(K_GOTO) || CUR->t != T_NUM) {
            err(E_SYNTAX);
        }
        num = (int)CUR->n;
        tp++;
        if (num == 0) {
            onerr_line = 0;
            if (in_err) {               /* 処理の中で 0: 元のエラーを表示して止める */
                in_err = 0;
                report_erl = 1;
                longjmp(errjb, last_err);
            }
            return;
        }
        line_index(num);
        onerr_line = num;
        return;
    }
    n = int_expr();

    if (accept_kw(K_GOTO)) {
        is_sub = 0;
    } else if (accept_kw(K_GOSUB)) {
        is_sub = 1;
    } else {
        err(E_SYNTAX);
        return;
    }
    if (n < 0 || n > 255) {
        err(E_ILLFUNC);
    }
    for (;;) {
        if (CUR->t != T_NUM) {
            err(E_SYNTAX);
        }
        if (i == n) {
            int num = (int)CUR->n;
            if (is_sub) {
                skip_stmt();        /* 戻り先は ON 文の次 */
                do_gosub_to(num);
            } else {
                go_line(line_index(num));
            }
            return;
        }
        tp++;
        i++;
        if (!accept_op(',')) {
            return;             /* 範囲外は次の文へ */
        }
    }
}

static const int ansi_of_n88[8] = { 0, 4, 1, 5, 2, 6, 3, 7 };

static void do_palette(void);
static int gfx_fg_set(int c);

static void do_color(void)
{
    char b[32];
    if (accept_op('=')) {
        do_palette();
        return;
    }
    if (!at_end() && !is_op(',')) {
        long f = int_expr();
        if (f < 0 || f > 7) {
            err(E_ILLFUNC);
        }
        sprintf(b, "\033[%dm", 30 + ansi_of_n88[f]);
        color_used = 1;
        fputs(b, stdout);
    }
    if (accept_op(',')) {
        if (!at_end() && !is_op(',')) {
            long bg = int_expr();
            if (bg < 0 || bg > 7) {
                err(E_ILLFUNC);
            }
            sprintf(b, "\033[%dm", 40 + ansi_of_n88[bg]);
            color_used = 1;
            fputs(b, stdout);
        }
        if (accept_op(',')) {       /* ボーダーの色は受け付けるだけ */
            if (!is_op(',') && !at_end()) {
                num_expr();
            }
            if (accept_op(',')) {   /* グラフィックスの色 */
                if (!is_op(',') && !at_end()) {
                    long g = int_expr();
                    if (g < 0 || g > 15) {
                        err(E_ILLFUNC);
                    }
                    gfx_fg_set((int)g);
                }
                ignore_args();      /* パレットの形は受け付けるだけ */
            }
        }
    }
}

static void do_locate(void)
{
    long x = -1, y = -1;
    char b[32];

    if (!is_op(',')) {
        x = int_expr();
    }
    if (accept_op(',')) {
        if (!is_op(',') && !at_end()) {
            y = int_expr();
        }
        if (accept_op(',')) {
            long cur = int_expr();
            fputs(cur ? "\033[?25h" : "\033[?25l", stdout);
        }
    }
    if (x < 0) {
        x = col;
    }
    if (y < 0) {
        y = row;
    }
    if (x > 255 || y > 255) {
        err(E_ILLFUNC);
    }
    sprintf(b, "\033[%ld;%ldH", y + 1, x + 1);
    fputs(b, stdout);
    col = (int)x;
    row = (int)y;
}

static void ignore_args(void)
{
    while (!at_end()) {
        tp++;
    }
}

static void do_list(void)
{
    int from = 0, to = 0x7fffffff, i;

    if (CUR->t == T_NUM) {
        from = to = (int)CUR->n;
        tp++;
    }
    if (accept_op('-')) {
        to = 0x7fffffff;
        if (CUR->t == T_NUM) {
            to = (int)CUR->n;
            tp++;
        }
    }
    for (i = 0; i < nprog; i++) {
        if (prog[i]->num >= from && prog[i]->num <= to) {
            char *u = to_utf8_cstr(prog[i]->src);
            printf("%d %s\n", prog[i]->num, u);
            free(u);
            if (brkflag) {
                break;
            }
        }
    }
    col = 0;
}

static void do_delete(void)
{
    int from, to, i;

    if (CUR->t == T_NUM) {
        from = to = (int)CUR->n;
        tp++;
    } else {
        from = 0;
        to = 0;
    }
    if (accept_op('-')) {
        to = 0x7fffffff;
        if (CUR->t == T_NUM) {
            to = (int)CUR->n;
            tp++;
        }
    }
    if (CUR->t != T_EOL && !is_op(':')) {
        err(E_SYNTAX);
    }
    prog_changed();
    for (i = nprog - 1; i >= 0; i--) {
        if (prog[i]->num >= from && prog[i]->num <= to) {
            free_line(prog[i]);
            memmove(&prog[i], &prog[i + 1], (nprog - i - 1) * sizeof(Line *));
            nprog--;
        }
    }
}

/* RENUM [新しい先頭[,古い先頭[,増分]]] — 行番号の参照（GOTO など）も書き換える */
static void do_renum(void)
{
    long nstart = 10, ostart = 0, inc = 10;
    int i, *newnum;

    if (CUR->t == T_NUM) {
        nstart = (long)CUR->n;
        tp++;
    }
    if (accept_op(',')) {
        if (CUR->t == T_NUM) {
            ostart = (long)CUR->n;
            tp++;
        }
        if (accept_op(',')) {
            inc = int_expr();
        }
    }
    if (inc <= 0) {
        err(E_ILLFUNC);
    }
    newnum = xmalloc((nprog + 1) * sizeof(int));
    {
        long n = nstart;
        for (i = 0; i < nprog; i++) {
            if (prog[i]->num < ostart) {
                newnum[i] = prog[i]->num;
            } else {
                newnum[i] = (int)n;
                n += inc;
            }
            if (newnum[i] > 65529 || (i > 0 && newnum[i] <= newnum[i - 1])) {
                free(newnum);
                err(E_ILLFUNC);
            }
        }
    }
    /* 本文を作り直す: 字句から書き戻すのでなく、本文の数字を置き換える */
    for (i = 0; i < nprog; i++) {
        Line *l = prog[i];
        char out[MAXLINE * 2];
        const char *p = l->src;
        int o = 0, inq = 0, refmode = 0;
        while (*p && o < (int)sizeof(out) - 16) {
            if (*p == '"') {
                inq = !inq;
                out[o++] = *p++;
                continue;
            }
            if (!inq && (strncmp(p, "REM", 3) == 0 || *p == '\'')) {
                break;
            }
            if (!inq && isalpha((unsigned char)*p)) {
                int len, k = kw_match(p, &len);
                if (k == K_GOTO || k == K_GOSUB || k == K_THEN || k == K_ELSE || k == K_RESTORE
                    || (k == K_LIST && 0)) {
                    refmode = 1;
                    memcpy(out + o, p, len);
                    o += len;
                    p += len;
                    continue;
                }
                if (k >= 0) {
                    refmode = 0;
                    memcpy(out + o, p, len);
                    o += len;
                    p += len;
                    continue;
                }
                refmode = 0;
                out[o++] = *p++;
                continue;
            }
            if (!inq && refmode && isdigit((unsigned char)*p)) {
                int num = 0, j, ex;
                while (isdigit((unsigned char)*p)) {
                    num = num * 10 + (*p++ - '0');
                }
                j = find_line(num, &ex);
                o += sprintf(out + o, "%d", ex ? newnum[j] : num);
                continue;
            }
            if (!inq && *p != ' ' && *p != ',') {
                refmode = 0;
            }
            out[o++] = *p++;
        }
        strcpy(out + o, p);
        {
            Line *nl = make_line(newnum[i], out);
            free_line(l);
            prog[i] = nl;
        }
    }
    free(newnum);
    prog_changed();
}

/* ---------------------------------------------------------------- ファイルの名前 */
static int exists(const char *p)
{
    return access(p, F_OK) == 0;
}

static void set_workdir(void)
{
    if (access(".", W_OK) == 0) {
        strcpy(workdir, ".");
    } else if (access("/tmp", W_OK) == 0) {
        strcpy(workdir, "/tmp");
    } else {
        strcpy(workdir, ".");
    }
}

/* フォルダの中で名前を探す（大文字・小文字の違いは問わない。N88 と同じ） */
static int find_ci(const char *dir, const char *base, char *out, size_t max)
{
    char cand[600];
    DIR *d;
    struct dirent *e;

    snprintf(cand, sizeof(cand), "%s/%s", dir, base);
    if (exists(cand)) {
        snprintf(out, max, "%s", cand);
        return 1;
    }
    d = opendir(dir);
    if (!d) {
        return 0;
    }
    while ((e = readdir(d)) != NULL) {
        if (strcasecmp(e->d_name, base) == 0) {
            snprintf(out, max, "%s/%s", dir, e->d_name);
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
}

/* BASIC の名前を本当のパスにする。
 *   write: 書くため（置き場の中）。読むときは 置き場 → 今のフォルダ → /bas の順に、大文字・小文字も試す
 *   bas: 拡張子がなければ .BAS をつける（プログラムのファイル） */
static void resolve_name_u(const char *name, int write, int bas, char *out, size_t max);

static void resolve_name(const char *name, int write, int bas, char *out, size_t max)
{
    char *u = to_utf8_cstr(name);          /* 漢字の名前は UTF-8 にしてから探す */
    resolve_name_u(u, write, bas, out, max);
    free(u);
}

static void resolve_name_u(const char *name, int write, int bas, char *out, size_t max)
{
    char base[256];
    const char *dirs[3];
    int i;
    const char *slash;

    if (strlen(name) > 200 || !*name) {
        err(E_BADNAME);
    }
#ifdef _WIN32
    /* Windows の完全な場所（C:\… や \\…。「開く」の画面から来る）は、そのまま使う */
    if ((isalpha((unsigned char)name[0]) && name[1] == ':' && (name[2] == '\\' || name[2] == '/')) ||
        (name[0] == '\\' && name[1] == '\\')) {
        const char *b = strrchr(name, '\\');
        snprintf(out, max, "%s%s", name, (bas && !strchr(b ? b : name, '.')) ? ".BAS" : "");
        return;
    }
#endif
    if (isalnum((unsigned char)name[0]) && name[1] == ':' && name[2]) {
        name += 2;                  /* 「1:」「A:」のドライブ名 */
    }
    strcpy(base, name);
    slash = strrchr(base, '/');
    if (bas && !strchr(slash ? slash : base, '.')) {
        strcat(base, ".BAS");
    }
    if (strchr(base, '/')) {
        char dir[256];
        const char *sl = strrchr(base, '/');
        snprintf(dir, sizeof(dir), "%.*s", sl == base ? 1 : (int)(sl - base), base);
        if (!write && !exists(base) && find_ci(dir, sl + 1, out, max)) {
            return;
        }
        snprintf(out, max, "%s", base);
        return;
    }
    if (write) {
        snprintf(out, max, "%s/%s", workdir, base);
        return;
    }
    dirs[0] = workdir;
    dirs[1] = ".";
    dirs[2] = "/bas";
    for (i = 0; i < 3; i++) {
        if (find_ci(dirs[i], base, out, max)) {
            return;
        }
    }
    snprintf(out, max, "%s/%s", workdir, base);
}

static int write_err(void)
{
    switch (errno) {
    case EROFS: case EACCES: case EPERM: return E_WRITE;
    case ENOSPC: case EFBIG: return E_FULL;
    case ENOENT: return E_FILENF;
    case EEXIST: return E_EXISTS;
    case EMFILE: case ENFILE: return E_BADFNUM;
    }
    return E_BADNAME;
}

/* ---------------------------------------------------------------- プログラムのファイル */
static void prog_clear_lines(void)
{
    int i;
    for (i = 0; i < nprog; i++) {
        free_line(prog[i]);
    }
    nprog = 0;
    prog_changed();
}

/* 読み込む。merge = 0 なら今のプログラムを消してから */
static char *load_tail_name;
static int auto_on, auto_num, auto_step;     /* AUTO の途中 */
static int load_tail_merge, load_tail_sjis;

static void load_file(const char *name, int merge, int keep_vars)
{
    char path[600];
    FILE *f;
    char *raw, *text, *p;
    size_t n = 0, cap = 4096, tn;
    int conv;

    resolve_name(name, 0, 1, path, sizeof(path));
    f = fopen(path, "rb");
    if (!f) {
        err(E_FILENF);
    }
    raw = xmalloc(cap);
    for (;;) {
        size_t r;
        if (n + 4096 > cap) {
            cap *= 2;
            raw = realloc(raw, cap);
            if (!raw) {
                fclose(f);
                err(E_MEM);
            }
        }
        r = fread(raw + n, 1, 4096, f);
        n += r;
        if (r == 0) {
            break;
        }
    }
    fclose(f);
    if (valid_utf8((unsigned char *)raw, n)) {     /* UTF-8 のファイル → Shift_JIS。Shift_JIS ならそのまま */
        text = xmalloc(n + 1);
        tn = utf8_to_sjis(raw, n, text);
        conv = 1;
    } else {
        text = xmalloc(n + 1);
        memcpy(text, raw, n);
        tn = n;
        conv = 0;
    }
    text[tn] = 0;
    free(raw);
    if (!merge) {
        if (keep_vars) {
            prog_clear_lines();
        } else {
            prog_new();
        }
    }
    /* 下の行の読み込みが終わってから、名前と形式を覚える（merge は今の名前のまま、変えたものとする） */
    /* 覚えるのは実際に開いたファイルの場所（大文字・小文字や .BAS の付け足しで別のファイルにしないため）。
     * 場所は UTF-8 なので、名前の決まり（Shift_JIS）に直す */
    load_tail_name = xmalloc(strlen(path) + 1);
    load_tail_name[utf8_to_sjis(path, strlen(path), load_tail_name)] = 0;
    load_tail_merge = merge;
    load_tail_sjis = !conv;
    p = text;
    while (*p) {
        char *e = strchr(p, '\n'), *q;
        int num;
        if (e) {
            *e = 0;
        }
        q = p + strlen(p);
        while (q > p && (q[-1] == '\r' || q[-1] == 0x1a)) {
            *--q = 0;
        }
        q = p;
        while (*q == ' ') {
            q++;
        }
        if (isdigit((unsigned char)*q)) {
            num = (int)strtol(q, &q, 10);
            if (*q == ' ') {
                q++;
            }
            if (num <= 65529 && strlen(q) < MAXLINE) {
                store_line(num, q);
            }
        }
        if (!e) {
            break;
        }
        p = e + 1;
    }
    free(text);
    if (load_tail_name) {
        if (load_tail_merge) {
            prog_dirty = 1;
        } else {
            snprintf(cur_file, sizeof(cur_file), "%s", load_tail_name);
            cur_sjis = load_tail_sjis;
            prog_dirty = 0;
        }
        free(load_tail_name);
        load_tail_name = NULL;
    }
}

/* 保存: 読んだときの形式で書く（Shift_JIS で読んだら Shift_JIS、ほかは UTF-8）。
 * 前からあるファイルは「名前.bak」に残してから書く */
static void save_file(const char *name)
{
    char path[600];
    FILE *f;
    int i;

    resolve_name(name, 1, 1, path, sizeof(path));
    if (exists(path)) {
        char bak[620];
        snprintf(bak, sizeof(bak), "%s.bak", path);
        remove(bak);
        rename(path, bak);
    }
    f = fopen(path, "wb");
    if (!f) {
        err(write_err());
    }
    for (i = 0; i < nprog; i++) {
        if (cur_sjis) {
            fprintf(f, "%d %s\r\n", prog[i]->num, prog[i]->src);   /* Shift_JIS（N88 と同じ CR LF） */
        } else {
            char *u = to_utf8_cstr(prog[i]->src);
            fprintf(f, "%d %s\n", prog[i]->num, u);
            free(u);
        }
    }
    if (fclose(f) != 0) {
        err(E_FULL);
    }
    if (name != cur_file) {
        snprintf(cur_file, sizeof(cur_file), "%s", name);
    }
    prog_dirty = 0;
}

/* ---- 名前を聞く（Windows は「開く」「名前を付けて保存」の画面、ほかは 1 行で打ってもらう） ----
 * 返すのは Shift_JIS の名前（呼んだ側で free）。やめたら NULL */
static int read_line(char *buf, int max);
#ifdef _WIN32
#include <commdlg.h>
static char *win_file_dialog(int save)
{
    WCHAR wfile[600] = L"";
    OPENFILENAMEW o;
    char u[1200], *s;
    BOOL ok;

    if (save && cur_file[0]) {
        char *cu = to_utf8_cstr(cur_file);
        MultiByteToWideChar(CP_UTF8, 0, cu, -1, wfile, 600);
        free(cu);
    }
    memset(&o, 0, sizeof(o));
    o.lStructSize = sizeof(o);
    o.hwndOwner = GetConsoleWindow();
    o.lpstrFilter = L"BASIC (*.bas)\0*.bas;*.BAS\0All files (*.*)\0*.*\0";
    o.lpstrFile = wfile;
    o.nMaxFile = 600;
    o.lpstrDefExt = L"bas";
    o.Flags = save ? (OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR)
                   : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR);
    ok = save ? GetSaveFileNameW(&o) : GetOpenFileNameW(&o);
    if (!ok) {
        return NULL;
    }
    WideCharToMultiByte(CP_UTF8, 0, wfile, -1, u, sizeof(u), NULL, NULL);
    s = xmalloc(strlen(u) + 1);
    s[utf8_to_sjis(u, strlen(u), s)] = 0;
    return s;
}
#endif

static char *ask_file_name(int save)
{
#ifdef _WIN32
    return win_file_dialog(save);
#else
    char buf[MAXLINE];
    int r;
    put_str(save ? "Save as? " : "Load? ");
    out_flush();
    r = read_line(buf, sizeof(buf));
    if (r <= 0) {
        return NULL;
    }
    return xstrdup(buf);
#endif
}

/* 直接打った NEW・LOAD・RUN "名前"・SYSTEM の前: 保存していない変更があれば聞く（Windows 版）。
 * 1 = 続けてよい、0 = やめる */
static int confirm_discard(void)
{
    char buf[MAXLINE];
    int r;

#ifndef _WIN32
    /* いまは Windows 版だけで聞く（Tamazusa の試験は打つ行を決めて流すので、問いがはさまると崩れる） */
    (void)buf;
    (void)r;
    return 1;
#else
    if (!interactive || li >= 0 || !prog_dirty || nprog == 0 || !win_in_console) {
        return 1;               /* パイプなどから打つ行を流しているときは聞かない */
    }
    for (;;) {
        put_str("Program not saved. Save it? (Y/N/C) ");
        out_flush();
        r = read_line(buf, sizeof(buf));
        if (r < 0 || r == -2) {
            return 0;
        }
        switch (toupper((unsigned char)buf[0])) {
        case 'Y': {
            char *n = cur_file[0] ? xstrdup(cur_file) : ask_file_name(1);
            if (!n) {
                return 0;
            }
            save_file(n);
            free(n);
            return 1;
        }
        case 'N':
            return 1;
        case 'C':
            return 0;
        }
    }
#endif
}

static void do_files(void)
{
    char dir[600], pat[256] = "*";
    DIR *d;
    struct dirent *e;
    int n = 0;

    strcpy(dir, workdir);
    if (!at_end()) {
        char *s0 = str_expr();
        char *s = to_utf8_cstr(s0);
        const char *a = s;
        struct stat st;
        sfree(s0);
        if (isalnum((unsigned char)a[0]) && a[1] == ':') {
            a += 2;
        }
        if (*a && stat(a, &st) == 0 && S_ISDIR(st.st_mode)) {
            snprintf(dir, sizeof(dir), "%s", a);
        } else if (*a) {
            const char *sl = strrchr(a, '/');
            if (sl) {
                snprintf(dir, sizeof(dir), "%.*s", (int)(sl - a) > 0 ? (int)(sl - a) : 1, a);
                snprintf(pat, sizeof(pat), "%s", sl + 1);
            } else {
                snprintf(pat, sizeof(pat), "%s", a);
            }
        }
        free(s);
    }
    d = opendir(dir);
    if (!d) {
        err(E_FILENF);
    }
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') {
            continue;
        }
        if (fnmatch(pat, e->d_name, FNM_CASEFOLD) != 0) {
            continue;
        }
        printf("%-18s", e->d_name);
        if (++n % 4 == 0) {
            putchar('\n');
        }
    }
    if (n % 4) {
        putchar('\n');
    }
    closedir(d);
    if (n == 0) {
        err(E_FILENF);
    }
    col = 0;
}

static void do_kill(void)
{
    char path[600];
    char *s = str_expr();

    resolve_name(s, 0, 0, path, sizeof(path));
    sfree(s);
    if (!exists(path)) {
        err(E_FILENF);
    }
    if (remove(path) != 0) {
        err(write_err());
    }
}

static void do_name(void)
{
    char from[600], to[600];
    char *a = str_expr(), *b;

    if (!accept_kw(K_AS)) {
        sfree(a);
        err(E_SYNTAX);
    }
    b = str_expr();
    resolve_name(a, 0, 0, from, sizeof(from));
    {
        /* 行き先は元と同じフォルダ（名前だけのとき） */
        const char *sl = strrchr(from, '/');
        if (!strchr(b, '/') && sl) {
            char *ub = to_utf8_cstr(b);
            snprintf(to, sizeof(to), "%.*s/%s", (int)(sl - from), from, ub);
            free(ub);
        } else {
            resolve_name(b, 1, 0, to, sizeof(to));
        }
    }
    sfree(a);
    sfree(b);
    if (!exists(from)) {
        err(E_FILENF);
    }
    if (exists(to)) {
        err(E_EXISTS);
    }
    if (rename(from, to) != 0) {
        err(write_err());
    }
}

/* ---------------------------------------------------------------- データのファイル */
static BFile *get_file(long n)
{
    if (n < 1 || n > MAXFILES) {
        err(E_BADFNUM);
    }
    if (!files[n].mode) {
        err(E_NOTOPEN);
    }
    return &files[n];
}

/* #n（# は省ける） */
static BFile *file_arg(void)
{
    accept_op('#');
    return get_file(int_expr());
}

static void close_file(BFile *f)
{
    if (!f->mode) {
        return;
    }
    if (f->fp && !f->dev) {
        fclose(f->fp);
    } else if (f->fp) {
        fflush(f->fp);
    }
    free(f->rec);
    memset(f, 0, sizeof(*f));
}

static void close_all(void)
{
    int i;
    for (i = 1; i <= MAXFILES; i++) {
        close_file(&files[i]);
    }
    outf = NULL;
}

static void do_open(void)
{
    Val first = expr();
    char mode = 0, *fname, path[600];
    long n, reclen = 128;
    BFile *f;

    if (first.type != 's') {
        err(E_TYPE);
    }
    if (accept_op(',')) {                   /* 古い形: OPEN "O",#1,"FILE"[,LEN] */
        mode = (char)toupper((unsigned char)first.s[0]);
        sfree(first.s);
        accept_op('#');
        n = int_expr();
        expect_op(',');
        fname = str_expr();
        if (accept_op(',')) {
            reclen = int_expr();
        }
    } else {
        fname = first.s;
        if (accept_kw(K_FOR)) {
            if (accept_kw(K_INPUT)) {
                mode = 'I';
            } else if (accept_kw(K_OUTPUT)) {
                mode = 'O';
            } else if (accept_kw(K_APPEND)) {
                mode = 'A';
            } else if (accept_kw(K_RANDOM)) {
                mode = 'R';
            } else {
                sfree(fname);
                err(E_SYNTAX);
            }
        } else {
            mode = 'R';
        }
        if (!accept_kw(K_AS)) {
            sfree(fname);
            err(E_SYNTAX);
        }
        accept_op('#');
        n = int_expr();
        if (accept_kw(K_LEN)) {
            expect_op('=');
            reclen = int_expr();
        }
    }
    if (mode != 'I' && mode != 'O' && mode != 'A' && mode != 'R') {
        sfree(fname);
        err(E_ILLFUNC);
    }
    if (n < 1 || n > MAXFILES) {
        sfree(fname);
        err(E_BADFNUM);
    }
    if (reclen < 1 || reclen > 32767) {
        sfree(fname);
        err(E_ILLFUNC);
    }
    f = &files[n];
    if (f->mode) {
        sfree(fname);
        err(E_OPEN);
    }
    if (strcasecmp(fname, "SCRN:") == 0 || strcasecmp(fname, "KYBD:") == 0) {
        int scrn = toupper((unsigned char)fname[0]) == 'S';
        sfree(fname);
        if ((scrn && mode != 'O' && mode != 'A') || (!scrn && mode != 'I')) {
            err(E_ILLFUNC);
        }
        f->dev = scrn ? 'S' : 'K';
        f->fp = scrn ? stdout : stdin;
        f->mode = mode;
        return;
    }
    resolve_name(fname, mode != 'I', 0, path, sizeof(path));
    sfree(fname);
    if (mode == 'I') {
        f->fp = fopen(path, "rb");
        if (!f->fp) {
            err(E_FILENF);
        }
    } else if (mode == 'R') {
        f->fp = fopen(path, "r+b");
        if (!f->fp) {
            f->fp = fopen(path, "w+b");
        }
    } else {
        f->fp = fopen(path, mode == 'O' ? "wb" : "ab");
    }
    if (!f->fp) {
        err(write_err());
    }
    f->mode = mode;
    f->col = 0;
    f->reclen = reclen;
    f->recno = 0;
    f->nfield = 0;
    if (mode == 'R') {
        f->rec = xmalloc((size_t)reclen);
        memset(f->rec, ' ', (size_t)reclen);
    }
}

static void do_close(void)
{
    if (at_end()) {
        close_all();
        return;
    }
    do {
        long n;
        accept_op('#');
        n = int_expr();
        if (n < 1 || n > MAXFILES) {
            err(E_BADFNUM);
        }
        close_file(&files[n]);
    } while (accept_op(','));
}

static int f_getc(BFile *f)
{
    int c;
    if (f->dev == 'K') {
        c = in_getc();
        if (c == -2) {
            longjmp(errjb, R_STOP);
        }
        return c;
    }
    c = fgetc(f->fp);
    if (c == 0x1a) {                /* Ctrl-Z はファイルの終わり（N88 の書き方） */
        ungetc(c, f->fp);
        return EOF;
    }
    return c;
}

static int f_eof(BFile *f)
{
    int c;
    if (f->dev) {
        return 0;
    }
    if (f->mode == 'R') {
        long pos = ftell(f->fp), end;
        fseek(f->fp, 0, SEEK_END);
        end = ftell(f->fp);
        fseek(f->fp, pos, SEEK_SET);
        return f->recno * f->reclen >= end;
    }
    if (f->mode != 'I') {
        err(E_ILLFUNC);
    }
    c = fgetc(f->fp);
    if (c == EOF || c == 0x1a) {
        if (c != EOF) {
            ungetc(c, f->fp);
        }
        return 1;
    }
    ungetc(c, f->fp);
    return 0;
}

/* INPUT # の 1 項目。0 = ファイルの終わり */
static int file_item(BFile *f, char *out, int max, int str)
{
    int c, n = 0;

    do {
        c = f_getc(f);
    } while (c == ' ' || c == '\t' || c == '\r' || c == '\n');
    if (c == EOF) {
        return 0;
    }
    if (str && c == '"') {
        while ((c = f_getc(f)) != EOF && c != '"' && c != '\n') {
            if (n < max - 1) {
                out[n++] = (char)c;
            }
        }
        if (c == '"') {
            do {
                c = f_getc(f);
            } while (c == ' ');
        }
    } else {
        while (c != EOF && c != ',' && c != '\n' && c != '\r' && (str || c != ' ')) {
            if (n < max - 1) {
                out[n++] = (char)c;
            }
            c = f_getc(f);
        }
        while (str && n > 0 && out[n - 1] == ' ') {
            n--;
        }
        if (!str && c == ' ') {
            while (c == ' ') {
                c = f_getc(f);
            }
            if (c != ',' && c != EOF && c != '\n' && c != '\r' && !f->dev) {
                ungetc(c, f->fp);
            }
        }
    }
    if (c == '\r' && !f->dev) {
        int d = fgetc(f->fp);
        if (d != '\n' && d != EOF) {
            ungetc(d, f->fp);
        }
    }
    out[n] = 0;
    return 1;
}

static void file_input(int line_input)
{
    BFile *f = file_arg();

    if (f->mode != 'I') {
        err(E_ILLFUNC);
    }
    expect_op(',');
    if (line_input) {
        char buf[MAXSTR + 1];
        int c, n = 0;
        LV lv = lvalue();
        if (lv.type != '$') {
            err(E_TYPE);
        }
        c = f_getc(f);
        if (c == EOF) {
            err(E_PASTEND);
        }
        while (c != EOF && c != '\n') {
            if (c != '\r' && n < MAXSTR) {
                buf[n++] = (char)c;
            }
            c = f_getc(f);
        }
        assign(lv, str_val(snew(buf, (size_t)n)));
        return;
    }
    do {
        char item[MAXLINE];
        LV lv = lvalue();
        if (!file_item(f, item, sizeof(item), lv.type == '$')) {
            err(E_PASTEND);
        }
        if (lv.type == '$') {
            assign(lv, str_copy(item));
        } else {
            assign(lv, num_val(strtod(item, NULL), 2));
        }
    } while (accept_op(','));
}

static void do_writestmt(void)
{
    int first = 1;
    char buf[80];

    outf = NULL;
    if (accept_op('#')) {
        BFile *f = file_arg();
        if (f->mode != 'O' && f->mode != 'A') {
            err(E_ILLFUNC);
        }
        expect_op(',');
        outf = f;
    }
    while (!at_end()) {
        Val v = expr();
        if (!first) {
            put_str(",");
        }
        first = 0;
        if (v.type == 's') {
            put_str("\"");
            put_mem(v.s, slen(v.s));
            put_str("\"");
            sfree(v.s);
        } else {
            fmt_num(buf, v.n, v.prec);
            put_str(buf[0] == ' ' ? buf + 1 : buf);
        }
        if (!accept_op(',') && !accept_op(';')) {
            break;
        }
    }
    newline();
    outf = NULL;
}

static void do_field(void)
{
    BFile *f = file_arg();
    long off = 0;

    if (f->mode != 'R') {
        err(E_SEQONLY);
    }
    f->nfield = 0;
    while (accept_op(',')) {
        long w = int_expr();
        LV lv;
        if (!accept_kw(K_AS)) {
            err(E_SYNTAX);
        }
        lv = lvalue();
        if (lv.type != '$') {
            err(E_TYPE);
        }
        if (w < 0 || off + w > f->reclen || f->nfield >= 64) {
            err(E_FIELD);
        }
        f->field[f->nfield].sp = lv.sp;
        f->field[f->nfield].off = off;
        f->field[f->nfield].w = w;
        f->nfield++;
        sfree(*lv.sp);
        *lv.sp = snew(f->rec + off, (size_t)w);
        off += w;
    }
}

static void getput(int put)
{
    BFile *f = file_arg();
    long r;
    int i;

    if (f->mode != 'R') {
        err(E_SEQONLY);
    }
    r = f->recno + 1;
    if (accept_op(',')) {
        r = int_expr();
    }
    if (r < 1) {
        err(E_ILLFUNC);
    }
    if (fseek(f->fp, (r - 1) * f->reclen, SEEK_SET) != 0) {
        err(E_ILLFUNC);
    }
    if (put) {
        if (fwrite(f->rec, 1, (size_t)f->reclen, f->fp) != (size_t)f->reclen) {
            err(E_FULL);
        }
        fflush(f->fp);
    } else {
        size_t got = fread(f->rec, 1, (size_t)f->reclen, f->fp);
        memset(f->rec + got, 0, (size_t)f->reclen - got);
        for (i = 0; i < f->nfield; i++) {
            Field *fd = &f->field[i];
            sfree(*fd->sp);
            *fd->sp = snew(f->rec + fd->off, (size_t)fd->w);
        }
    }
    f->recno = r;
}

/* LSET・RSET: FIELD の変数ならレコードの入れ物へ、そうでなければ今の長さのまま詰める */
static void do_lrset(int right)
{
    LV lv = lvalue();
    char *v, *dst;
    long w, n, i;
    int k;

    if (lv.type != '$') {
        err(E_TYPE);
    }
    expect_op('=');
    v = str_expr();
    dst = NULL;
    w = (long)slen(*lv.sp);
    for (k = 1; k <= MAXFILES && !dst; k++) {
        for (i = 0; i < files[k].nfield; i++) {
            if (files[k].mode == 'R' && files[k].field[i].sp == lv.sp) {
                dst = files[k].rec + files[k].field[i].off;
                w = files[k].field[i].w;
                break;
            }
        }
    }
    {
        char tmp[MAXSTR + 1];
        n = (long)slen(v);
        if (n > w) {
            n = w;
        }
        memset(tmp, ' ', (size_t)w);
        memcpy(tmp + (right ? w - n : 0), v, (size_t)n);
        if (dst) {
            memcpy(dst, tmp, (size_t)w);
        }
        sfree(*lv.sp);
        *lv.sp = snew(tmp, (size_t)w);
    }
    sfree(v);
}

/* MKS$・MKD$・CVS・CVD: N88 と同じ Microsoft Binary Format */
static void to_mbf4(double d, unsigned char o[4])
{
    float f = (float)d;
    uint32_t u;
    unsigned e;

    memcpy(&u, &f, 4);
    e = (u >> 23) & 0xff;
    if (f == 0 || e == 0) {
        memset(o, 0, 4);
        return;
    }
    if (e + 2 > 255) {
        err(E_OVERFLOW);
    }
    o[0] = (unsigned char)u;
    o[1] = (unsigned char)(u >> 8);
    o[2] = (unsigned char)(((u >> 16) & 0x7f) | ((u >> 31) << 7));
    o[3] = (unsigned char)(e + 2);
}

static double from_mbf4(const unsigned char *b)
{
    uint32_t u;
    float f;
    if (b[3] < 3) {
        return 0;
    }
    u = ((uint32_t)(b[2] >> 7) << 31) | ((uint32_t)(b[3] - 2) << 23)
      | ((uint32_t)(b[2] & 0x7f) << 16) | ((uint32_t)b[1] << 8) | b[0];
    memcpy(&f, &u, 4);
    return f;
}

static void to_mbf8(double d, unsigned char o[8])
{
    uint64_t u, m;
    int e, me, i;

    memcpy(&u, &d, 8);
    e = (int)((u >> 52) & 0x7ff);
    if (d == 0 || e == 0) {
        memset(o, 0, 8);
        return;
    }
    me = e - 1023 + 129;
    if (me > 255) {
        err(E_OVERFLOW);
    }
    if (me < 1) {
        memset(o, 0, 8);
        return;
    }
    m = (u & ((1ULL << 52) - 1)) << 3;
    for (i = 0; i < 7; i++) {
        o[i] = (unsigned char)(m >> (8 * i));
    }
    o[6] = (unsigned char)((o[6] & 0x7f) | ((u >> 63) << 7));
    o[7] = (unsigned char)me;
}

static double from_mbf8(const unsigned char *b)
{
    uint64_t m = 0, u;
    double d;
    int i;
    if (b[7] == 0) {
        return 0;
    }
    for (i = 0; i < 7; i++) {
        m |= (uint64_t)b[i] << (8 * i);
    }
    m &= (1ULL << 55) - 1;
    u = ((uint64_t)(b[6] >> 7) << 63) | ((uint64_t)(b[7] - 129 + 1023) << 52) | (m >> 3);
    memcpy(&d, &u, 8);
    return d;
}

/* ---------------------------------------------------------------- 音（段階 3）
 *   PLAY の MML を音符の並びにし、矩形波などで 22050Hz・16 ビット・モノラルの波形を作って
 *   /dev/dsp（Tamazusa の音の装置）に書く。装置がなければ、鳴らさずに同じ時間だけ待つ
 *   （音楽で間を取るプログラムの速さが変わらないように）。
 *   PLAY は 6 声まで（PLAY A$,B$,C$）。声ごとのオクターブ・音長・テンポ・音量は次の PLAY に持ち越す。 */
#define SR 22050
typedef struct {
    int oct, len, tempo, vol, q, tone;
} Voice;
static Voice voice[6];
static int dsp_fd = -2;             /* -2 まだ開いていない、-1 装置がない */
static unsigned long snd_written;   /* 書いたサンプルの数（テスト用の表示） */

typedef struct {
    long start, len;                /* サンプル */
    double freq;
    double amp;
    int tone, gate;                 /* gate: 鳴っている長さ（サンプル） */
} Note;

static void voices_reset(void)
{
    int i;
    for (i = 0; i < 6; i++) {
        voice[i].oct = 4;
        voice[i].len = 4;
        voice[i].tempo = 120;
        voice[i].vol = 8;
        voice[i].q = 8;
        voice[i].tone = 0;
    }
}

/* 波形を鳴らす（装置へ書く。なければ同じ時間待つ） */
static void snd_out(const short *pcm, size_t n)
{
    const char *p = (const char *)pcm;
    size_t left = n * sizeof(short);

    out_flush();
    gfx_flush(1);
    if (dsp_fd == -2) {
        dsp_fd = open("/dev/dsp", O_WRONLY);
        if (dsp_fd < 0) {
            dsp_fd = -1;
        }
    }
    if (dsp_fd < 0) {
        struct timespec ts;
        ts.tv_sec = (time_t)(n / SR);
        ts.tv_nsec = (long)((n % SR) * (1000000000L / SR));
        while (nanosleep(&ts, &ts) != 0 && errno == EINTR && !brkflag) {
        }
        return;
    }
    while (left > 0) {
        ssize_t w = write(dsp_fd, p, left);
        if (w < 0) {
            if (errno == EINTR && !brkflag) {
                continue;
            }
            return;
        }
        p += w;
        left -= (size_t)w;
    }
    snd_written += n;
}

static void snd_close(void)
{
    if (dsp_fd >= 0) {
        close(dsp_fd);
    }
    dsp_fd = -2;
}

/* MML の数（=変数; の形も） */
static long mml_num(const char *s, size_t n, size_t *i, long dflt)
{
    long v = 0;
    int any = 0;

    if (*i < n && s[*i] == '=') {
        char name[48];
        int k = 0;
        char full[64], type;
        Var *var;
        (*i)++;
        while (*i < n && s[*i] != ';' && k < 40) {
            name[k++] = (char)toupper((unsigned char)s[(*i)++]);
        }
        if (*i < n && s[*i] == ';') {
            (*i)++;
        }
        name[k] = 0;
        if (!k) {
            err(E_ILLFUNC);
        }
        full_name(name, full, &type);
        if (type == '$') {
            err(E_TYPE);
        }
        var = var_get_name(full, type);
        return (long)floor(var->n + 0.5);
    }
    while (*i < n && isdigit((unsigned char)s[*i])) {
        v = v * 10 + (s[(*i)++] - '0');
        any = 1;
        if (v > 100000) {
            err(E_ILLFUNC);
        }
    }
    return any ? v : dflt;
}

static double vol_amp(int v)
{
    if (v <= 0) {
        return 0;
    }
    if (v > 15) {
        v = 15;
    }
    return 8000.0 * pow(10.0, (v - 15) * 1.5 / 20.0);   /* 1 段で約 1.5dB（PSG に近い） */
}

/* 1 声の MML を音符にする。t は声の時刻（サンプル、小数も持つ） */
static void mml_voice(const char *s, size_t n, Voice *v, Note **notes, int *nn, int *cap, double *t, int depth)
{
    static const int semi[7] = { 9, 11, 0, 2, 4, 5, 7 };   /* A B C D E F G */
    size_t i = 0;
    int tie = 0;

    if (depth > 8) {
        err(E_ILLFUNC);
    }
    while (i < n) {
        int c = toupper((unsigned char)s[i++]);
        double freq = -1;
        long len;
        double dur;
        int dots = 0;

        switch (c) {
        case ' ': case '\t': case ';':
            continue;
        case 'O': v->oct = (int)mml_num(s, n, &i, 4); if (v->oct < 1 || v->oct > 8) err(E_ILLFUNC); continue;
        case '<': if (v->oct > 1) v->oct--; continue;
        case '>': if (v->oct < 8) v->oct++; continue;
        case 'L': v->len = (int)mml_num(s, n, &i, 4); if (v->len < 1 || v->len > 64) err(E_ILLFUNC); continue;
        case 'T': v->tempo = (int)mml_num(s, n, &i, 120); if (v->tempo < 32 || v->tempo > 255) err(E_ILLFUNC); continue;
        case 'V': v->vol = (int)mml_num(s, n, &i, 8); if (v->vol < 0 || v->vol > 15) err(E_ILLFUNC); continue;
        case 'Q': v->q = (int)mml_num(s, n, &i, 8); if (v->q < 1 || v->q > 8) err(E_ILLFUNC); continue;
        case '@': v->tone = (int)mml_num(s, n, &i, 0); continue;
        case 'S': case 'M': case 'Y': case 'K':
            mml_num(s, n, &i, 0);       /* エンベロープ・FM のレジスタは受け付けるだけ */
            if (c == 'Y' && i < n && s[i] == ',') {
                i++;
                mml_num(s, n, &i, 0);
            }
            continue;
        case '&':
            tie = 1;
            continue;
        case 'X': {                     /* X 変数名; … 文字列の変数の中身を演奏 */
            char name[48], full[64], type;
            int k = 0;
            Var *var;
            while (i < n && s[i] != ';' && k < 40) {
                name[k++] = (char)toupper((unsigned char)s[i++]);
            }
            if (i < n) {
                i++;
            }
            name[k] = 0;
            if (k == 0) {
                err(E_ILLFUNC);
            }
            if (name[k - 1] != '$') {
                name[k++] = '$';
                name[k] = 0;
            }
            full_name(name, full, &type);
            var = var_get_name(full, type);
            mml_voice(var->s ? var->s : "", slen(var->s), v, notes, nn, cap, t, depth + 1);
            continue;
        }
        case 'R':
            freq = 0;
            break;
        case 'N': {
            long nn2 = mml_num(s, n, &i, -1);
            if (nn2 < 0 || nn2 > 96) {
                err(E_ILLFUNC);
            }
            freq = nn2 == 0 ? 0 : 440.0 * pow(2.0, ((double)(nn2 + 24) - 69.0) / 12.0);
            len = v->len;
            goto have_len;
        }
        default:
            if (c >= 'A' && c <= 'G') {
                int sm = semi[c - 'A'];
                int oct = v->oct;
                if (i < n && (s[i] == '#' || s[i] == '+')) {
                    sm++;
                    i++;
                } else if (i < n && s[i] == '-') {
                    sm--;
                    i++;
                }
                freq = 440.0 * pow(2.0, ((oct + 1) * 12 + sm - 69) / 12.0);
                break;
            }
            err(E_ILLFUNC);
        }
        len = mml_num(s, n, &i, v->len);
have_len:
        if (len < 1 || len > 64) {
            err(E_ILLFUNC);
        }
        while (i < n && s[i] == '.') {
            dots++;
            i++;
        }
        dur = (double)SR * 240.0 / ((double)v->tempo * (double)len);
        {
            double add = dur / 2;
            int d;
            for (d = 0; d < dots; d++) {
                dur += add;
                add /= 2;
            }
        }
        if (tie && *nn > 0 && freq > 0 && (*notes)[*nn - 1].freq == freq) {
            Note *pv = &(*notes)[*nn - 1];   /* & でつないだ同じ高さ: 前の音を伸ばす */
            pv->len += (long)(*t + dur) - (long)*t;
            pv->gate = (int)(pv->len - (long)(dur * (8 - v->q) / 8));
        } else if (freq > 0) {
            Note *no;
            if (*nn >= *cap) {
                *cap = *cap ? *cap * 2 : 256;
                *notes = realloc(*notes, (size_t)*cap * sizeof(Note));
                if (!*notes) {
                    err(E_MEM);
                }
            }
            no = &(*notes)[(*nn)++];
            no->start = (long)*t;
            no->len = (long)(*t + dur) - (long)*t;
            no->gate = (int)(no->len * v->q / 8);
            no->freq = freq;
            no->amp = vol_amp(v->vol);
            no->tone = v->tone;
        }
        tie = 0;
        *t += dur;
        if (*t > (double)SR * 600) {
            err(E_ILLFUNC);             /* 1 回の PLAY は 10 分まで */
        }
    }
}

/* 音符の並びを波形にして鳴らす */
static void render_play(Note *notes, int nn, long total)
{
    int *mix;
    short *pcm;
    long i;
    int k;

    if (total <= 0) {
        return;
    }
    mix = calloc((size_t)total, sizeof(int));
    pcm = malloc((size_t)total * sizeof(short));
    if (!mix || !pcm) {
        free(mix);
        free(pcm);
        err(E_MEM);
    }
    for (k = 0; k < nn; k++) {
        Note *no = &notes[k];
        double ph = 0, step = no->freq / SR;
        long g = no->gate, att = SR / 300, rel = SR / 100;
        int wave = no->tone % 5;
        if (g > no->len) {
            g = no->len;
        }
        for (i = 0; i < g + rel && no->start + i < total; i++) {
            double w, e = 1.0;
            switch (wave) {
            case 1: w = ph < 0.25 ? 1 : -1; break;                     /* 細い矩形波 */
            case 2: w = ph < 0.5 ? 4 * ph - 1 : 3 - 4 * ph; break;     /* 三角波 */
            case 3: w = 2 * ph - 1; break;                             /* のこぎり波 */
            case 4: w = sin(2 * M_PI * ph); break;                     /* 正弦波 */
            default: w = ph < 0.5 ? 1 : -1; break;                     /* 矩形波（PSG の音） */
            }
            if (i < att) {
                e = (double)i / att;
            } else if (i >= g) {
                e = 1.0 - (double)(i - g) / rel;
            }
            mix[no->start + i] += (int)(w * no->amp * e);
            ph += step;
            if (ph >= 1) {
                ph -= 1;
            }
        }
    }
    for (i = 0; i < total; i++) {
        int x = mix[i];
        pcm[i] = (short)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
    }
    free(mix);
    snd_out(pcm, (size_t)total);
    free(pcm);
}

static void do_play(void)
{
    Note *notes = NULL;
    int nn = 0, cap = 0, ch = 0;
    long total = 0;

    if (at_end()) {
        return;
    }
    do {
        double t = 0;
        if (ch >= 6) {
            free(notes);
            err(E_SYNTAX);
        }
        if (!is_op(',') && !at_end()) {
            char *m = str_expr();
            mml_voice(m, slen(m), &voice[ch], &notes, &nn, &cap, &t, 0);
            sfree(m);
        }
        if ((long)t > total) {
            total = (long)t;
        }
        ch++;
    } while (accept_op(','));
    render_play(notes, nn, total);
    free(notes);
}

static void do_beep(void)
{
    Note no;
    long n = SR / 10;               /* 0.1 秒、2kHz（PC-98 のブザーに近い高さ） */

    if (!at_end()) {
        if (num_expr() == 0) {
            return;                 /* BEEP 0: 止める（鳴らし続けはしないので何もしない） */
        }
    }
    no.start = 0;
    no.len = n;
    no.gate = (int)n;
    no.freq = 2000;
    no.amp = vol_amp(12);
    no.tone = 0;
    render_play(&no, 1, n + SR / 100);
}

/* ---------------------------------------------------------------- グラフィックス（段階 4）
 *   640×400 の 16 色の絵を BASIC の中に持ち、変わった行を /dev/fb0（Tamazusa の画面）へ書く。
 *   画面がなければ絵は中だけで描く（POINT は使える）。
 *   SCREEN 0・1 は PC-8801 と同じ 200 ライン（1 行を 2 本に引き伸ばす）、SCREEN 2・3 は 400 ライン。 */
#define GW 640
#define GH 400
static unsigned char gbuf[GH][GW];
static uint32_t palette[16];
static const uint32_t default_pal[16] = {
    0x000000, 0x0000ff, 0xff0000, 0xff00ff, 0x00ff00, 0x00ffff, 0xffff00, 0xffffff,
    0x777777, 0x000077, 0x770000, 0x770077, 0x007700, 0x007777, 0x777700, 0xbbbbbb
};
static int g_lines = 400;
static int gfx_fg = 7, gfx_bg = 0;
static double last_x, last_y;           /* 最後の点（STEP と LINE -(x,y) のため） */
static int fb_fd = -2;                  /* -2 まだ開いていない、-1 画面がない */
static int dirty_y0 = GH, dirty_y1 = 0;
static struct timespec last_gflush;
static int vx0, vy0, vx1 = GW - 1, vy1 = GH - 1;        /* VIEW（論理の座標） */
static int win_on, win_screen;
static double wx0, wy0, wx1, wy1;                       /* WINDOW */
static unsigned long gfx_ops;           /* 描いた回数（テスト用） */

static void gfx_reset(void)
{
    memcpy(palette, default_pal, sizeof(palette));
    vx0 = vy0 = 0;
    vx1 = GW - 1;
    vy1 = g_lines - 1;
    win_on = 0;
    gfx_fg = 7;
    gfx_bg = 0;
    last_x = last_y = 0;
}

static void gfx_dirty(int py0, int py1)
{
    if (py0 < dirty_y0) {
        dirty_y0 = py0;
    }
    if (py1 > dirty_y1) {
        dirty_y1 = py1;
    }
}

/* 文字の面の変わった行を、グラフィックスの変わった行に加える（画面を開いているときだけ） */
static void tx_to_gfx(void)
{
    if (tx_dirty0 < tx_dirty1 && fb_fd >= 0) {
        gfx_dirty(tx_dirty0 * 16, tx_dirty1 * 16);
        tx_dirty0 = TROWS;
        tx_dirty1 = 0;
        gfx_flush(0);
    }
}

/* 変わった行を画面へ書く。force でなければ 30 ミリ秒に 1 回まで。文字の面を重ねる */
static void gfx_flush(int force)
{
    static uint32_t row[GW * 8];
    int y;

    if (fb_fd >= 0 && tx_dirty0 < tx_dirty1) {
        gfx_dirty(tx_dirty0 * 16, tx_dirty1 * 16);
        tx_dirty0 = TROWS;
        tx_dirty1 = 0;
    }

    if (dirty_y0 >= dirty_y1) {
        return;
    }
    if (!force) {
        struct timespec now;
        long ms;
        clock_gettime(CLOCK_MONOTONIC, &now);
        ms = (now.tv_sec - last_gflush.tv_sec) * 1000 + (now.tv_nsec - last_gflush.tv_nsec) / 1000000;
        if (ms < 30) {
            return;
        }
    }
    if (fb_fd == -2) {
        fb_fd = open("/dev/fb0", O_RDWR);
        if (fb_fd < 0) {
            fb_fd = -1;
        } else {
            dirty_y0 = 0;                       /* 初めて開いた: 文字の面も含めて全部を出す */
            dirty_y1 = GH;
            tx_dirty0 = TROWS;
            tx_dirty1 = 0;
        }
    }
    if (fb_fd >= 0) {
        for (y = dirty_y0; y < dirty_y1; y += 8) {
            int n = dirty_y1 - y < 8 ? dirty_y1 - y : 8, k, x;
            for (k = 0; k < n; k++) {
                const unsigned char *t = tx_px[y + k];
                for (x = 0; x < GW; x++) {
                    row[k * GW + x] = t[x] ? tx_rgb[(t[x] - 1) & 7] : palette[gbuf[y + k][x] & 15];
                }
            }
            if (pwrite(fb_fd, row, (size_t)n * GW * 4, (off_t)y * GW * 4) < 0) {
                break;
            }
        }
    }
    dirty_y0 = GH;
    dirty_y1 = 0;
    clock_gettime(CLOCK_MONOTONIC, &last_gflush);
}

/* 論理の座標の点（VIEW の外は描かない） */
static void gpset(int x, int y, int c)
{
    if (x < vx0 || x > vx1 || y < vy0 || y > vy1 || x < 0 || x >= GW || y < 0 || y >= g_lines) {
        return;
    }
    if (g_lines == 200) {
        gbuf[y * 2][x] = gbuf[y * 2 + 1][x] = (unsigned char)c;
        gfx_dirty(y * 2, y * 2 + 2);
    } else {
        gbuf[y][x] = (unsigned char)c;
        gfx_dirty(y, y + 1);
    }
}

static int gpoint(int x, int y)
{
    if (x < 0 || x >= GW || y < 0 || y >= g_lines) {
        return -1;
    }
    return gbuf[g_lines == 200 ? y * 2 : y][x];
}

/* 使う側の座標 → 論理の座標 */
static void to_log(double x, double y, int *lx, int *ly)
{
    if (win_on) {
        double fx = (x - wx0) / (wx1 - wx0), fy = (y - wy0) / (wy1 - wy0);
        x = vx0 + fx * (vx1 - vx0);
        y = win_screen ? vy0 + fy * (vy1 - vy0) : vy1 - fy * (vy1 - vy0);
    } else {
        x += vx0;
        y += vy0;
    }
    *lx = (int)floor(x + 0.5);
    *ly = (int)floor(y + 0.5);
}

static double scale_x(void)
{
    return win_on ? (vx1 - vx0) / fabs(wx1 - wx0) : 1.0;
}

static void gline(int x0, int y0, int x1, int y1, int c, unsigned style)
{
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx + dy;
    unsigned bit = 0x8000;
    for (;;) {
        if (style == 0xffff || (style & bit)) {
            gpset(x0, y0, c);
        }
        bit = bit == 1 ? 0x8000 : bit >> 1;
        if (x0 == x1 && y0 == y1) {
            break;
        }
        {
            int e2 = 2 * e;
            if (e2 >= dy) {
                e += dy;
                x0 += sx;
            }
            if (e2 <= dx) {
                e += dx;
                y0 += sy;
            }
        }
        if (brkflag) {
            break;
        }
    }
}

static void gbox(int x0, int y0, int x1, int y1, int c, int fill)
{
    int x, y;
    if (x0 > x1) {
        int t = x0; x0 = x1; x1 = t;
    }
    if (y0 > y1) {
        int t = y0; y0 = y1; y1 = t;
    }
    if (fill) {
        for (y = y0; y <= y1; y++) {
            for (x = x0; x <= x1; x++) {
                gpset(x, y, c);
            }
        }
        return;
    }
    gline(x0, y0, x1, y0, c, 0xffff);
    gline(x1, y0, x1, y1, c, 0xffff);
    gline(x1, y1, x0, y1, c, 0xffff);
    gline(x0, y1, x0, y0, c, 0xffff);
}

/* 塗りつぶし: 境界の色か塗る色に当たるまで（行ごとに広げる） */
static void gpaint(int x, int y, int c, int border)
{
    typedef struct { short x, y; } P;
    P *st;
    int n = 0, cap = 4096;
    int xmin = vx0 > 0 ? vx0 : 0, xmax = vx1 < GW - 1 ? vx1 : GW - 1;
    int ymin = vy0 > 0 ? vy0 : 0, ymax = vy1 < g_lines - 1 ? vy1 : g_lines - 1;

#define STOP(px, py) (gpoint(px, py) == border || gpoint(px, py) == c)
    if (x < xmin || x > xmax || y < ymin || y > ymax || STOP(x, y)) {
        return;
    }
    st = malloc(sizeof(P) * cap);
    if (!st) {
        err(E_MEM);
    }
    st[n].x = (short)x;
    st[n].y = (short)y;
    n++;
    while (n > 0) {
        int lx, rx, i, up, dn;
        n--;
        x = st[n].x;
        y = st[n].y;
        if (STOP(x, y)) {
            continue;
        }
        lx = rx = x;
        while (lx > xmin && !STOP(lx - 1, y)) {
            lx--;
        }
        while (rx < xmax && !STOP(rx + 1, y)) {
            rx++;
        }
        for (i = lx; i <= rx; i++) {
            gpset(i, y, c);
        }
        for (up = -1; up <= 1; up += 2) {
            int ny = y + up, inrun = 0;
            if (ny < ymin || ny > ymax) {
                continue;
            }
            for (i = lx; i <= rx; i++) {
                if (!STOP(i, ny)) {
                    if (!inrun) {
                        if (n + 2 >= cap) {
                            cap *= 2;
                            st = realloc(st, sizeof(P) * cap);
                            if (!st) {
                                err(E_MEM);
                            }
                        }
                        st[n].x = (short)i;
                        st[n].y = (short)ny;
                        n++;
                        inrun = 1;
                    }
                } else {
                    inrun = 0;
                }
            }
        }
        (void)dn;
    }
    free(st);
#undef STOP
}

/* 円・楕円・弧（角度はラジアン。負の角度は中心との線も引く）。fill で中を塗る */
static void gcircle(int cx, int cy, double r, int c, double a0, double a1, int arc, double ratio, int fill)
{
    double rx, ry, t, step, end;
    int px = 0, py = 0, first = 1;
    int l0 = 0, l1 = 0;

    if (ratio <= 0) {
        ratio = g_lines == 200 ? 0.5 : 1.0;
    }
    if (ratio < 1) {
        rx = r;
        ry = r * ratio;
    } else {
        ry = r;
        rx = r / ratio;
    }
    if (fill && !arc) {
        int y, x, yr = (int)floor(ry + 0.5);
        for (y = -yr; y <= yr; y++) {
            double w = ry > 0 ? rx * sqrt(1.0 - (double)(y * y) / (ry * ry)) : rx;
            int xw = (int)floor(w + 0.5);
            for (x = -xw; x <= xw; x++) {
                gpset(cx + x, cy + y, c);
            }
        }
        return;
    }
    if (arc) {
        if (a0 < 0) {
            l0 = 1;
            a0 = -a0;
        }
        if (a1 < 0) {
            l1 = 1;
            a1 = -a1;
        }
        end = a1 < a0 ? a1 + 2 * M_PI : a1;
    } else {
        a0 = 0;
        end = 2 * M_PI;
    }
    step = 0.7 / (rx > ry ? (rx > 1 ? rx : 1) : (ry > 1 ? ry : 1));
    for (t = a0;; t += step) {
        int x, y;
        if (t > end) {
            t = end;
        }
        x = cx + (int)floor(rx * cos(t) + 0.5);
        y = cy - (int)floor(ry * sin(t) + 0.5);
        if (first) {
            gpset(x, y, c);
            if (l0) {
                gline(cx, cy, x, y, c, 0xffff);
            }
            first = 0;
        } else if (x != px || y != py) {
            gline(px, py, x, y, c, 0xffff);
        }
        px = x;
        py = y;
        if (t >= end || brkflag) {
            break;
        }
    }
    if (l1) {
        gline(cx, cy, px, py, c, 0xffff);
    }
}

/* (x,y) か STEP(x,y)。使う側の座標で返す */
static void coord(double *x, double *y)
{
    int step = accept_kw(K_STEP);
    double a, b;
    expect_op('(');
    a = num_expr();
    expect_op(',');
    b = num_expr();
    expect_op(')');
    if (step) {
        a += last_x;
        b += last_y;
    }
    *x = a;
    *y = b;
}

static int color_arg(int dflt)
{
    long c;
    if (is_op(',') || at_end()) {
        return dflt;
    }
    c = int_expr();
    if (c < 0 || c > 15) {
        err(E_ILLFUNC);
    }
    return (int)c;
}

static void gfx_done(void)
{
    gfx_ops++;
    gfx_flush(0);
}

static void do_pset(int preset)
{
    double x, y;
    int lx, ly, c;
    coord(&x, &y);
    c = preset ? gfx_bg : gfx_fg;
    if (accept_op(',')) {
        c = color_arg(c);
    }
    to_log(x, y, &lx, &ly);
    gpset(lx, ly, c);
    last_x = x;
    last_y = y;
    gfx_done();
}

static int is_id(const char *name)
{
    return CUR->t == T_ID && strcmp(CUR->s, name) == 0;
}

static void do_gline(void)
{
    double x0 = last_x, y0 = last_y, x1, y1;
    int c = gfx_fg, box = 0, lx0, ly0, lx1, ly1;
    unsigned style = 0xffff;

    if (!is_op('-')) {
        coord(&x0, &y0);
    }
    expect_op('-');
    coord(&x1, &y1);
    if (accept_op(',')) {
        c = color_arg(gfx_fg);
        if (accept_op(',')) {
            if (is_id("B")) {
                box = 1;
                tp++;
            } else if (is_id("BF")) {
                box = 2;
                tp++;
            }
            if (accept_op(',')) {
                style = (unsigned)(int_expr() & 0xffff);
            }
        }
    }
    to_log(x0, y0, &lx0, &ly0);
    to_log(x1, y1, &lx1, &ly1);
    if (box) {
        gbox(lx0, ly0, lx1, ly1, c, box == 2);
    } else {
        gline(lx0, ly0, lx1, ly1, c, style);
    }
    last_x = x1;
    last_y = y1;
    gfx_done();
}

static void do_circle(void)
{
    double x, y, r, a0 = 0, a1 = 2 * M_PI, ratio = 0;
    int c = gfx_fg, arc = 0, fill = 0, lx, ly;

    coord(&x, &y);
    expect_op(',');
    r = num_expr() * scale_x();
    if (accept_op(',')) {
        c = color_arg(gfx_fg);
        if (accept_op(',')) {
            if (!is_op(',')) {
                a0 = num_expr();
                arc = 1;
            }
            if (accept_op(',')) {
                if (!is_op(',')) {
                    a1 = num_expr();
                    arc = 1;
                }
                if (accept_op(',')) {
                    if (!is_op(',') && !at_end()) {
                        ratio = num_expr();
                    }
                    if (accept_op(',')) {
                        if (is_id("F")) {
                            fill = 1;
                            tp++;
                        }
                    }
                }
            }
        }
    }
    to_log(x, y, &lx, &ly);
    gcircle(lx, ly, r, c, a0, a1, arc, ratio, fill);
    last_x = x;
    last_y = y;
    gfx_done();
}

static void do_paint(void)
{
    double x, y;
    int c = gfx_fg, border, lx, ly;

    coord(&x, &y);
    if (accept_op(',')) {
        if (CUR->t == T_STR) {          /* タイルの模様は段階 4 では単色で塗る */
            tp++;
        } else {
            c = color_arg(gfx_fg);
        }
    }
    border = c;
    if (accept_op(',')) {
        border = color_arg(c);
    }
    ignore_args();
    to_log(x, y, &lx, &ly);
    gpaint(lx, ly, c, border);
    last_x = x;
    last_y = y;
    gfx_done();
}

static void do_view(void)
{
    double x0, y0, x1, y1;
    if (at_end()) {
        vx0 = vy0 = 0;
        vx1 = GW - 1;
        vy1 = g_lines - 1;
        return;
    }
    accept_kw(K_SCREEN);
    expect_op('(');
    x0 = num_expr();
    expect_op(',');
    y0 = num_expr();
    expect_op(')');
    expect_op('-');
    expect_op('(');
    x1 = num_expr();
    expect_op(',');
    y1 = num_expr();
    expect_op(')');
    if (x0 > x1) {
        double t = x0; x0 = x1; x1 = t;
    }
    if (y0 > y1) {
        double t = y0; y0 = y1; y1 = t;
    }
    if (x0 < 0 || y0 < 0 || x1 >= GW || y1 >= g_lines) {
        err(E_ILLFUNC);
    }
    vx0 = (int)x0;
    vy0 = (int)y0;
    vx1 = (int)x1;
    vy1 = (int)y1;
    if (accept_op(',')) {
        int fillc = -1, bord = -1;
        if (!is_op(',') && !at_end()) {
            fillc = (int)int_expr();
        }
        if (accept_op(',') && !at_end()) {
            bord = (int)int_expr();
        }
        if (fillc >= 0) {
            gbox(vx0, vy0, vx1, vy1, fillc & 15, 1);
        }
        if (bord >= 0) {
            int sx0 = vx0, sy0 = vy0, sx1 = vx1, sy1 = vy1;
            vx0 = 0; vy0 = 0; vx1 = GW - 1; vy1 = g_lines - 1;
            gbox(sx0 - 1, sy0 - 1, sx1 + 1, sy1 + 1, bord & 15, 0);
            vx0 = sx0; vy0 = sy0; vx1 = sx1; vy1 = sy1;
        }
        gfx_done();
    }
}

static void do_window(void)
{
    if (at_end()) {
        win_on = 0;
        return;
    }
    win_screen = accept_kw(K_SCREEN);
    expect_op('(');
    wx0 = num_expr();
    expect_op(',');
    wy0 = num_expr();
    expect_op(')');
    expect_op('-');
    expect_op('(');
    wx1 = num_expr();
    expect_op(',');
    wy1 = num_expr();
    expect_op(')');
    if (wx0 == wx1 || wy0 == wy1) {
        err(E_ILLFUNC);
    }
    if (wx0 > wx1) {
        double t = wx0; wx0 = wx1; wx1 = t;
    }
    if (wy0 > wy1) {
        double t = wy0; wy0 = wy1; wy1 = t;
    }
    win_on = 1;
}

/* 数の配列（GET・PUT の入れ物） */
static Arr *num_array_arg(void)
{
    char full[64], type;
    Arr *a;
    if (CUR->t != T_ID) {
        err(E_SYNTAX);
    }
    full_name(CUR->s, full, &type);
    tp++;
    if (accept_op('(')) {           /* A(0) の形も受け付ける（先頭から使う） */
        int depth = 1;
        while (depth > 0 && CUR->t != T_EOL) {
            if (is_op('(')) depth++;
            if (is_op(')')) depth--;
            tp++;
        }
    }
    if (type == '$') {
        err(E_TYPE);
    }
    a = arr_find(full);
    if (!a) {
        err(E_ILLFUNC);
    }
    return a;
}

/* GET (x1,y1)-(x2,y2),配列: 先頭 2 つに幅と高さ、そのあと 1 要素に 8 点（4 ビットずつ） */
static void do_gget(void)
{
    double x0, y0, x1, y1;
    int lx0, ly0, lx1, ly1, w, h, i, n;
    Arr *a;
    coord(&x0, &y0);
    expect_op('-');
    coord(&x1, &y1);
    expect_op(',');
    a = num_array_arg();
    to_log(x0, y0, &lx0, &ly0);
    to_log(x1, y1, &lx1, &ly1);
    if (lx0 > lx1) { int t = lx0; lx0 = lx1; lx1 = t; }
    if (ly0 > ly1) { int t = ly0; ly0 = ly1; ly1 = t; }
    w = lx1 - lx0 + 1;
    h = ly1 - ly0 + 1;
    n = w * h;
    if (a->count < 2 + (n + 7) / 8) {
        err(E_ILLFUNC);
    }
    a->n[0] = w;
    a->n[1] = h;
    for (i = 0; i < (n + 7) / 8; i++) {
        a->n[2 + i] = 0;
    }
    for (i = 0; i < n; i++) {
        int p = gpoint(lx0 + i % w, ly0 + i / w);
        if (p < 0) {
            p = 0;
        }
        a->n[2 + i / 8] += (double)((uint32_t)p << (4 * (i % 8)));
    }
}

static void do_gput(void)
{
    double x, y;
    int lx, ly, w, h, i, mode = K_XOR;
    Arr *a;
    coord(&x, &y);
    expect_op(',');
    a = num_array_arg();
    if (accept_op(',')) {
        if (CUR->t == T_KW && (CUR->k == K_PSET || CUR->k == K_PRESET || CUR->k == K_XOR
                               || CUR->k == K_OR || CUR->k == K_AND)) {
            mode = CUR->k;
            tp++;
        } else {
            err(E_SYNTAX);
        }
    }
    ignore_args();
    if (a->count < 2) {
        err(E_ILLFUNC);
    }
    w = (int)a->n[0];
    h = (int)a->n[1];
    if (w <= 0 || h <= 0 || a->count < 2 + (w * h + 7) / 8) {
        err(E_ILLFUNC);
    }
    to_log(x, y, &lx, &ly);
    for (i = 0; i < w * h; i++) {
        int s = (int)(((uint32_t)a->n[2 + i / 8] >> (4 * (i % 8))) & 15);
        int px = lx + i % w, py = ly + i / w, d = gpoint(px, py), c;
        if (d < 0) {
            continue;
        }
        switch (mode) {
        case K_PSET: c = s; break;
        case K_PRESET: c = (~s) & 15; break;
        case K_OR: c = d | s; break;
        case K_AND: c = d & s; break;
        default: c = d ^ s; break;
        }
        gpset(px, py, c);
    }
    gfx_done();
}

static int gfx_fg_set(int c)
{
    gfx_fg = c;
    return c;
}

static void gfx_cls(void)
{
    memset(gbuf, 0, sizeof(gbuf));
    gfx_dirty(0, GH);
    gfx_flush(1);
}

static void do_screen(void)
{
    long mode;
    if (at_end()) {
        return;
    }
    if (!is_op(',')) {
        mode = int_expr();
        if (mode < 0 || mode > 3) {
            err(E_ILLFUNC);
        }
        g_lines = mode <= 1 ? 200 : 400;
        vx0 = vy0 = 0;
        vx1 = GW - 1;
        vy1 = g_lines - 1;
    }
    ignore_args();              /* 切り替え・ページの番号は受け付けるだけ */
}

/* COLOR=(パレット,色): 色は 0〜7（デジタル）か &Hgrb（16 色、4 ビットずつ） */
static void do_palette(void)
{
    long p, c;
    expect_op('(');
    p = int_expr();
    expect_op(',');
    c = int_expr();
    expect_op(')');
    if (p < 0 || p > 15 || c < 0 || c > 0xfff) {
        err(E_ILLFUNC);
    }
    if (c < 8) {
        palette[p] = default_pal[c];
    } else {
        unsigned g = (unsigned)(c >> 8) & 15, r = (unsigned)(c >> 4) & 15, b = (unsigned)c & 15;
        palette[p] = (r * 17u) << 16 | (g * 17u) << 8 | (b * 17u);
    }
    gfx_dirty(0, GH);           /* 色を変えたら全部描き直す（パレットの効果） */
    gfx_done();
}

static void run_program(int start_idx)
{
    clear_vars();
    timer_reset();
    gfx_reset();
    srand((unsigned)time(NULL));
    if (nprog == 0) {
        longjmp(errjb, R_END);
    }
    go_line(start_idx);
}

/* ---------------------------------------------------------------- 機器（INP・OUT・SENSOR・PWM・SERVO・PIN・ADC） */
/* Tamazusa では /dev/io（カーネルの入出力の管理）を読み書きする。開けなければ（ふつうの Linux など）、
 * 同じ管理（include/iomgr_core.h、/etc/io.conf か組み込みの結びつけ）と仮想の機器を、このプログラムの中で動かす */
static int io_fd = -2;
static VioState io_local;
static IoMgr io_mgr;
static IoBackend io_vbe;
static int io_used;

struct io_req {                 /* kernel/io/iomgr.c と同じ形 */
    char name[32];
    int32_t id, kind, speed, flags;
    int64_t value;
};
#define IOC_LOOKUP  0x4f01
#define IOC_GET     0x4f02
#define IOC_SET     0x4f03
#define IOC_SAFE    0x4f04

static int64_t io_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void io_open(void)
{
    if (io_fd == -2) {
        io_fd = open("/dev/io", O_RDWR);
        if (io_fd < 0) {
            static char buf[8192];
            int fd = open("/etc/io.conf", O_RDONLY), n = -1;
            io_fd = -1;
            vio_init(&io_local, io_now_ms());
            io_init(&io_mgr);
            if (fd >= 0) {
                n = (int)read(fd, buf, sizeof(buf) - 1);
                close(fd);
            }
            if (n < 0) {
                n = (int)strlen(io_default_conf);
                memcpy(buf, io_default_conf, (size_t)n);
            }
            io_parse_conf(&io_mgr, buf, (size_t)n);
            io_vio_now = io_now_ms();
            io_vio_backend(&io_vbe, &io_local);
            io_add_backend(&io_mgr, &io_vbe);
        }
    }
    io_used = 1;
}

/* 入出力の管理の答えを、BASIC のエラーにする */
static void io_check(int r)
{
    switch (r) {
    case IO_OK: return;
    case IO_ENODEV: err(E_NODEV);
    case IO_ERANGE: case IO_EKIND: err(E_ILLFUNC);
    case IO_ESTOP: err(E_ESTOP);
    case IO_EPERM: err(E_DENIED);           /* 動かす権限がない・ネットワークから（安全の決まり 6） */
    }
    err(E_NODEV);
}

static int io_from_errno(int e)
{
    return e == ENODEV ? IO_ENODEV : e == ERANGE ? IO_ERANGE : e == EPERM ? IO_ESTOP
         : e == EINVAL ? IO_EKIND : e == EACCES ? IO_EPERM : IO_EIO;
}

/* 番号の書き方（仮想の機器のポート） */
static int io_read(unsigned port)
{
    int32_t v = 0;
    io_open();
    if (io_fd >= 0) {
        if (pread(io_fd, &v, 4, (off_t)port * 4) != 4) {
            io_check(io_from_errno(errno));
        }
        return v;
    }
    io_vio_now = io_now_ms();
    io_check(io_raw_read(&io_mgr, &io_local, port, &v, io_vio_now));
    return v;
}

static void io_write(unsigned port, double d)
{
    int32_t v;
    if (!isfinite(d) || d < -2147483648.0 || d > 2147483647.0) {
        err(E_ILLFUNC);
    }
    v = (int32_t)floor(d + 0.5);
    io_open();
    if (io_fd >= 0) {
        if (pwrite(io_fd, &v, 4, (off_t)port * 4) != 4) {
            io_check(io_from_errno(errno));
        }
        return;
    }
    io_vio_now = io_now_ms();
    io_check(io_raw_write(&io_mgr, &io_local, port, v, io_vio_now));
}

/* 名前の口。名前から番号を引き（覚えておく）、種類が合わなければエラー */
static struct { char name[IO_NAMELEN]; int id, kind, out; } io_names[IO_MAXENT];
static int io_nnames;

static int io_name_id(const char *s, int want_kind, int want_out)
{
    int i, id = -1, kind = 0, out = 0;
    char nm[IO_NAMELEN];

    for (i = 0; s[i]; i++) {
        if (i >= IO_NAMELEN - 1) {
            err(E_NODEV);
        }
        nm[i] = (char)toupper((unsigned char)s[i]);
    }
    nm[i] = 0;
    for (i = 0; i < io_nnames; i++) {
        if (strcmp(io_names[i].name, nm) == 0) {
            id = io_names[i].id;
            kind = io_names[i].kind;
            out = io_names[i].out;
            break;
        }
    }
    if (id < 0) {
        io_open();
        if (io_fd >= 0) {
            struct io_req q;
            memset(&q, 0, sizeof(q));
            snprintf(q.name, sizeof(q.name), "%s", nm);
            if (ioctl(io_fd, IOC_LOOKUP, &q) != 0) {
                return -1;
            }
            id = q.id;
            kind = q.kind;
            out = q.flags & 1;
        } else {
            id = io_lookup(&io_mgr, nm);
            if (id < 0) {
                return -1;
            }
            kind = io_mgr.ent[id].kind;
            out = io_mgr.ent[id].output;
        }
        if (io_nnames < IO_MAXENT) {
            snprintf(io_names[io_nnames].name, IO_NAMELEN, "%s", nm);
            io_names[io_nnames].id = id;
            io_names[io_nnames].kind = kind;
            io_names[io_nnames].out = out;
            io_nnames++;
        }
    }
    if ((want_kind && kind != want_kind) || (want_out && !out)) {
        err(E_ILLFUNC);
    }
    return id;
}

static double io_named_get(const char *s, int kind)
{
    int id = io_name_id(s, kind, 0);
    int64_t v = 0;
    if (id < 0) {
        err(E_NODEV);
    }
    if (io_fd >= 0) {
        struct io_req q;
        memset(&q, 0, sizeof(q));
        q.id = id;
        if (ioctl(io_fd, IOC_GET, &q) != 0) {
            io_check(io_from_errno(errno));
        }
        return q.value / 1000.0;
    }
    io_vio_now = io_now_ms();
    io_check(io_get(&io_mgr, id, &v, io_vio_now));
    return v / 1000.0;
}

static void io_named_set(const char *s, int kind, double d, int32_t speed)
{
    int id = io_name_id(s, kind, 1);
    int64_t v;
    if (id < 0) {
        err(E_NODEV);
    }
    if (!isfinite(d) || fabs(d) > 1e9) {
        err(E_ILLFUNC);
    }
    v = (int64_t)floor(d * 1000 + 0.5);
    if (io_fd >= 0) {
        struct io_req q;
        memset(&q, 0, sizeof(q));
        q.id = id;
        q.value = v;
        q.speed = speed;
        if (ioctl(io_fd, IOC_SET, &q) != 0) {
            io_check(io_from_errno(errno));
        }
        return;
    }
    io_vio_now = io_now_ms();
    io_check(io_set(&io_mgr, id, v, speed, io_vio_now));
}

/* DEVICE(名前): その名前があり、実物（中身）もつながっていれば 1 */
static int io_device(const char *s)
{
    char nm[IO_NAMELEN];
    int i;
    for (i = 0; s[i] && i < IO_NAMELEN - 1; i++) {
        nm[i] = (char)toupper((unsigned char)s[i]);
    }
    nm[i] = 0;
    if (s[i]) {
        return 0;
    }
    io_open();
    if (io_fd >= 0) {
        struct io_req q;
        memset(&q, 0, sizeof(q));
        snprintf(q.name, sizeof(q.name), "%s", nm);
        return ioctl(io_fd, IOC_LOOKUP, &q) == 0 && (q.flags & 2);
    }
    i = io_lookup(&io_mgr, nm);
    return i >= 0 && io_find_be(&io_mgr, io_mgr.ent[i].be) != NULL;
}

/* SENSOR(n): 0 温度℃ 1 湿度% 2 水位mm 3 距離mm 4 明るさlx 5 音dB 6 触覚 7 CO2ppm 8 pH */
static double io_sensor(long n)
{
    if (n < 0 || n >= VIO_NSENS) {
        err(E_ILLFUNC);
    }
    if (n == VIO_TEMP || n == VIO_PH) {
        return io_read((unsigned)n) / 10.0;
    }
    return io_read((unsigned)n);
}

/* プログラムがエラーや中断で止まったら、すべての中身の出力を 0 にし、サーボをその場で止める */
static void io_safe(void)
{
    if (!io_used) {
        return;
    }
    if (io_fd >= 0) {
        struct io_req q;
        memset(&q, 0, sizeof(q));
        ioctl(io_fd, IOC_SAFE, &q);
        return;
    }
    io_vio_now = io_now_ms();
    io_safe_all(&io_mgr);
}

/* ---------------------------------------------------------------- ON TIMER（QBasic と同じ形） */
static double tmr_interval, tmr_next;
static int tmr_line;            /* 0 = 決めていない */
static int tmr_state;           /* 0 OFF 1 ON 2 STOP（起きたことを覚えておき、ON で呼ぶ） */
static int tmr_pending;
static int tmr_busy = -1;       /* 処理の中（その GOSUB の深さ）。-1 = ちがう */
static int tmr_force;           /* 次の文の前に必ず調べる */
static Pos sleep_pos;           /* タイマーで途中から抜けた SLEEP */
static double sleep_until;
static int sleep_pending;

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void set_timer(double sec, int line)
{
    tmr_interval = sec;
    tmr_line = line;
    tmr_next = now_sec() + sec;
}

static void timer_reset(void)
{
    tmr_line = 0;
    tmr_state = 0;
    tmr_pending = 0;
    tmr_busy = -1;
    sleep_pending = 0;
}

static void timer_returned(void)
{
    if (tmr_busy >= 0 && ngosub < tmr_busy) {
        tmr_busy = -1;
    }
}

/* 文と文のあいだで呼ぶ。時間が来ていたら処理の行へ GOSUB する。呼んだら 1 */
static int timer_check(void)
{
    double now;
    if (tmr_state == 0 || tmr_line == 0 || tmr_busy >= 0 || in_err || li < 0) {
        return 0;
    }
    now = now_sec();
    if (now >= tmr_next) {
        tmr_next += tmr_interval;
        if (tmr_next < now) {
            tmr_next = now + tmr_interval;      /* 遅れた分をまとめて呼ばない */
        }
        tmr_pending = 1;
    }
    if (!tmr_pending || tmr_state != 1) {
        return 0;
    }
    tmr_pending = 0;
    if (ngosub >= 256) {
        err(E_MEM);
    }
    gosubstk[ngosub++] = here();
    tmr_busy = ngosub;
    go_line(line_index(tmr_line));
    return 1;
}

/* SLEEP。待つあいだにタイマーの時間が来たら、処理を先に呼び、戻ったら残りを待つ */
static void do_sleep(long ms)
{
    double until = now_sec() + ms / 1000.0;
    struct timespec ts;

    if (sleep_pending && sleep_pos.li == cur_stmt.li && sleep_pos.tp == cur_stmt.tp) {
        until = sleep_until;                    /* タイマーの処理から戻ってきた: 残りを待つ */
    }
    sleep_pending = 0;
    for (;;) {
        double now = now_sec(), end = until, left;
        int by_timer = 0;
        if (tmr_state == 1 && tmr_line && tmr_busy < 0 && !in_err && li >= 0 && tmr_next < end) {
            end = tmr_next;
            by_timer = 1;
        }
        left = end - now;
        if (left > 0) {
            ts.tv_sec = (time_t)left;
            ts.tv_nsec = (long)((left - (double)ts.tv_sec) * 1e9);
            while (nanosleep(&ts, &ts) != 0 && errno == EINTR && !brkflag) {
            }
        }
        if (brkflag) {
            return;
        }
        if (by_timer && now_sec() + 0.0005 < until) {
            sleep_pending = 1;                  /* この SLEEP をあとでもう一度 */
            sleep_pos = cur_stmt;
            sleep_until = until;
            go_pos(cur_stmt);
            tmr_force = 1;
            return;
        }
        if (!by_timer || now_sec() >= until) {
            return;
        }
    }
}

/* ---------------------------------------------------------------- 複数行の IF */
/* want_branch = 1: 偽だったので、成り立つ ELSEIF か ELSE か END IF を探す。0: END IF のあとへ */
static void if_scan(int want_branch)
{
    int depth = 0;

    for (;;) {
        Tok *t;
        if (!advance_token()) {
            err(E_IFEND);
        }
        t = CUR;
        if (t->t != T_KW) {
            continue;
        }
        if (t->k == K_THEN && (L->tk[tp + 1].t == T_EOL
                               || (L->tk[tp + 1].t == T_KW && L->tk[tp + 1].k == K_REM))) {
            int j = tp;                     /* 行末の THEN: その文が IF で始まれば、入れ子の IF */
            while (j > 0 && !(L->tk[j - 1].t == T_OP && L->tk[j - 1].k == ':')) {
                j--;
            }
            if (L->tk[j].t == T_KW && L->tk[j].k == K_IF) {
                depth++;
            }
            continue;
        }
        if (!at_stmt_start()) {
            continue;
        }
        if (t->k == K_ENDIF || (t->k == K_END && L->tk[tp + 1].t == T_KW && L->tk[tp + 1].k == K_IF)) {
            if (depth == 0) {
                tp += t->k == K_END ? 2 : 1;
                jumped = 1;
                return;
            }
            depth--;
            continue;
        }
        if (depth == 0 && want_branch) {
            if (t->k == K_ELSEIF) {
                Val c;
                int truth;
                tp++;
                c = expr();
                truth = c.type == 's' ? slen(c.s) != 0 : c.n != 0;
                sfree(c.s);
                if (!accept_kw(K_THEN)) {
                    err(E_SYNTAX);
                }
                if (truth) {
                    jumped = 1;
                    return;
                }
                tp--;
                continue;
            }
            if (t->k == K_ELSE && tp == 0) {
                tp++;
                jumped = 1;
                return;
            }
        }
    }
}

/* ---------------------------------------------------------------- SELECT CASE */
static Val selstk[32];
static int nsel;

static void sel_pop(void)
{
    nsel--;
    if (selstk[nsel].type == 's') {
        sfree(selstk[nsel].s);
    }
    selstk[nsel].s = NULL;
}

static int val_cmp(Val *a, Val *b)
{
    if (a->type != b->type) {
        err(E_TYPE);
    }
    if (a->type == 's') {
        return scmp(a->s, b->s);
    }
    return a->n < b->n ? -1 : a->n > b->n;
}

/* CASE のあとの条件（CASE ELSE、値、a TO b、IS < x を , で並べる）。合えば 1 で、文の終わりにいる */
static int case_match(Val *v)
{
    if (accept_kw(K_ELSE)) {
        return 1;
    }
    for (;;) {
        int hit, c;
        if (accept_kw(K_IS)) {
            int op;
            Val b;
            if (CUR->t != T_OP) {
                err(E_SYNTAX);
            }
            op = CUR->k;
            tp++;
            b = expr();
            c = val_cmp(v, &b);
            sfree(b.type == 's' ? b.s : NULL);
            hit = op == '=' ? c == 0 : op == '<' ? c < 0 : op == '>' ? c > 0
                : op == OP_LE ? c <= 0 : op == OP_GE ? c >= 0 : op == OP_NE ? c != 0 : (err(E_SYNTAX), 0);
        } else {
            Val a = expr();
            if (accept_kw(K_TO)) {
                Val b = expr();
                hit = val_cmp(v, &a) >= 0 && val_cmp(v, &b) <= 0;
                sfree(b.type == 's' ? b.s : NULL);
            } else {
                hit = val_cmp(v, &a) == 0;
            }
            sfree(a.type == 's' ? a.s : NULL);
        }
        if (hit) {
            skip_stmt();
            return 1;
        }
        if (!accept_op(',')) {
            return 0;
        }
    }
}

/* want = 1: 合う CASE を探す。0: END SELECT のあとへ */
static void select_scan(int want)
{
    int depth = 0;

    for (;;) {
        Tok *t;
        if (!advance_token()) {
            err(E_SELEND);
        }
        t = CUR;
        if (t->t != T_KW || !at_stmt_start()) {
            continue;
        }
        if (t->k == K_SELECT) {
            depth++;
        } else if (t->k == K_END && L->tk[tp + 1].t == T_KW && L->tk[tp + 1].k == K_SELECT) {
            if (depth == 0) {
                tp += 2;
                sel_pop();
                jumped = 1;
                return;
            }
            depth--;
        } else if (t->k == K_CASE && depth == 0 && want) {
            tp++;
            if (case_match(&selstk[nsel - 1])) {
                jumped = 1;
                return;
            }
        }
    }
}

/* ---------------------------------------------------------------- DO ... LOOP */
typedef struct { Pos at; int nfor, nwhile, nsel; } DoEnt;   /* nfor など: EXIT DO で戻す深さ */
static DoEnt dostk[64];
static int ndo;

static int block_depth(int which)
{
    return which == 0 ? ndo : nsel;
}

static void sel_pop(void);

static void sel_cut(int n)
{
    while (nsel > n) {
        sel_pop();
    }
}

static void skip_do(void)
{
    int depth = 0;

    for (;;) {
        if (!advance_token()) {
            err(E_DOLOOP);
        }
        if (CUR->t != T_KW) {
            continue;
        }
        if (CUR->k == K_DO && !prev_is(K_EXIT)) {
            depth++;
        } else if (CUR->k == K_LOOP) {
            if (depth == 0) {
                tp++;
                skip_stmt();            /* LOOP WHILE・UNTIL の条件 */
                jumped = 1;
                return;
            }
            depth--;
        }
    }
}

/* WHILE c なら c、UNTIL c なら NOT c、条件がなければ 1 */
static int loop_cond(void)
{
    int w = accept_kw(K_WHILE), u = !w && accept_kw(K_UNTIL), truth;
    Val c;
    if (!w && !u) {
        return 1;
    }
    c = expr();
    truth = c.type == 's' ? slen(c.s) != 0 : c.n != 0;
    sfree(c.type == 's' ? c.s : NULL);
    return w ? truth : !truth;
}

/* ---------------------------------------------------------------- SUB・FUNCTION */
/* 引数と LOCAL の変数は、呼ぶときに元の値をしまい、戻るときに戻す（BBC BASIC と同じやり方）。
 * ほかの変数はすべてのところで共通。引数は値で渡す（配列は渡せない） */
#define MAXPROC 128
#define MAXPARAM 16
typedef struct {
    char name[48];
    char kind;                  /* 'S' SUB 'F' FUNCTION */
    int li, tp;                 /* 本体の始まり */
    int np;
    char *param[MAXPARAM];
} Proc;
static Proc procs[MAXPROC];
static int nproc;
static unsigned proc_gen;

typedef struct {
    char kind;
    Pos ret;                    /* 戻る所 */
    Pos call_stmt;              /* 呼んだ文 */
    int save_base, nfor, nwhile, ndo, ngosub, nsel;
    Var *retvar;
} Frame;
static Frame frames[64];
static int nframe;
typedef struct { Var *v; double n; char *s; } Saved;
static Saved saves[1024];
static int nsave;
static Val fn_result;

static void build_procs(void)
{
    int i, j;

    nproc = 0;
    for (i = 0; i < nprog; i++) {
        Line *l = prog[i];
        for (j = 0; j < l->nt; j++) {
            Tok *t = &l->tk[j];
            Proc *p;
            int k;
            if (t->t != T_KW || (t->k != K_SUB && t->k != K_FUNCTION)
                || (j > 0 && !(l->tk[j - 1].t == T_OP && l->tk[j - 1].k == ':'))) {
                continue;
            }
            if (l->tk[j + 1].t != T_ID || nproc >= MAXPROC) {
                continue;               /* 形がおかしい: 実行したときにエラーにする */
            }
            p = &procs[nproc];
            for (k = 0; k < nproc; k++) {
                if (strcmp(procs[k].name, l->tk[j + 1].s) == 0) {
                    break;
                }
            }
            if (k < nproc) {
                continue;               /* 同じ名前は最初のもの */
            }
            snprintf(p->name, sizeof(p->name), "%s", l->tk[j + 1].s);
            p->kind = t->k == K_SUB ? 'S' : 'F';
            p->np = 0;
            k = j + 2;
            if (l->tk[k].t == T_OP && l->tk[k].k == '(') {
                k++;
                while (l->tk[k].t == T_ID && p->np < MAXPARAM) {
                    p->param[p->np++] = l->tk[k].s;
                    k++;
                    if (l->tk[k].t == T_OP && l->tk[k].k == ',') {
                        k++;
                    }
                }
                if (l->tk[k].t == T_OP && l->tk[k].k == ')') {
                    k++;
                }
            }
            p->li = i;
            p->tp = k;
            nproc++;
        }
    }
    proc_gen = prog_gen;
}

static Proc *find_proc(const char *name, char kind)
{
    int i;
    if (proc_gen != prog_gen) {
        build_procs();
    }
    for (i = 0; i < nproc; i++) {
        if (procs[i].kind == kind && strcmp(procs[i].name, name) == 0) {
            return &procs[i];
        }
    }
    return NULL;
}

static Var *named_var(const char *id)
{
    char full[64], type;
    full_name(id, full, &type);
    return var_get_name(full, type);
}

static void save_local(Var *v)
{
    if (nsave >= (int)(sizeof(saves) / sizeof(saves[0]))) {
        err(E_MEM);
    }
    saves[nsave].v = v;
    saves[nsave].n = v->n;
    saves[nsave].s = v->s;
    nsave++;
    v->n = 0;
    v->s = NULL;
}

static void restore_to(int base)
{
    while (nsave > base) {
        Saved *e = &saves[--nsave];
        sfree(e->v->s);
        e->v->n = e->n;
        e->v->s = e->s;
    }
}

/* 変数が消えるとき（RUN・CLEAR・NEW）: しまった値は捨てるだけ */
static void frames_drop(void)
{
    while (nsave > 0) {
        sfree(saves[--nsave].s);
    }
    nframe = 0;
    while (nsel > 0) {
        sel_pop();
    }
    ndo = 0;
}

/* プログラムが終わったとき: 呼んでいた途中なら、しまった値を戻す */
static void frames_unwind(void)
{
    restore_to(0);
    nframe = 0;
    while (nsel > 0) {
        sel_pop();
    }
    ndo = 0;
}

static int fn_active(void)
{
    int i;
    for (i = 0; i < nframe; i++) {
        if (frames[i].kind == 'F') {
            return 1;
        }
    }
    return 0;
}

static void exec_until(int level);

/* 引数を読んで、呼ぶ。paren = 1 なら ( ) で囲む形 */
static void proc_enter(Proc *p, int paren)
{
    Val args[MAXPARAM];
    int n = 0, i;
    Frame *f;

    if (paren) {
        expect_op('(');
        if (!is_op(')')) {
            do {
                if (n >= MAXPARAM) {
                    err(E_ARGS);
                }
                args[n++] = expr();
            } while (accept_op(','));
        }
        expect_op(')');
    } else if (!at_end()) {
        do {
            if (n >= MAXPARAM) {
                err(E_ARGS);
            }
            args[n++] = expr();
        } while (accept_op(','));
    }
    if (n != p->np) {
        for (i = 0; i < n; i++) {
            sfree(args[i].type == 's' ? args[i].s : NULL);
        }
        err(E_ARGS);
    }
    if (nframe >= 64) {
        err(E_MEM);
    }
    f = &frames[nframe];
    f->kind = p->kind;
    f->ret = here();
    f->call_stmt = cur_stmt;
    f->save_base = nsave;
    f->nfor = nfor;
    f->nwhile = nwhile;
    f->ndo = ndo;
    f->ngosub = ngosub;
    f->nsel = nsel;
    f->retvar = NULL;
    nframe++;
    for (i = 0; i < n; i++) {
        Var *v = named_var(p->param[i]);
        LV lv;
        save_local(v);
        lv.type = v->type;
        lv.np = &v->n;
        lv.sp = &v->s;
        assign(lv, args[i]);
    }
    if (p->kind == 'F') {
        f->retvar = named_var(p->name);
        save_local(f->retvar);
    }
    go_pos((Pos){ p->li, p->tp });
}

/* END SUB・EXIT SUB・END FUNCTION・EXIT FUNCTION */
static void proc_return(char kind)
{
    Frame *f;
    if (nframe == 0 || frames[nframe - 1].kind != kind) {
        err(E_ENDSUB);
    }
    f = &frames[--nframe];
    if (kind == 'F') {
        Var *v = f->retvar;
        fn_result = v->type == '$' ? str_dup(v->s) : num_val(v->n, prec_of(v->type));
    }
    restore_to(f->save_base);
    nfor = f->nfor;
    nwhile = f->nwhile;
    ndo = f->ndo;
    ngosub = f->ngosub;
    while (nsel > f->nsel) {
        sel_pop();
    }
    go_pos(f->ret);
}

/* 文の中の SUB の呼び出し（CALL 名前(引数)・名前 引数）。SUB でなければ 0（何も読まない） */
static int call_sub_stmt(void)
{
    Proc *p = find_proc(CUR->s, 'S');
    if (!p) {
        return 0;
    }
    tp++;
    proc_enter(p, is_op('('));
    if (!at_end()) {
        err(E_SYNTAX);
    }
    return 1;
}

/* 式の中の FUNCTION の呼び出し。FUNCTION でなければ 0 */
static int try_call_fn(Val *out)
{
    Proc *p = find_proc(CUR->s, 'F');
    int level;
    Pos keep_stmt;

    if (!p) {
        return 0;
    }
    tp++;
    keep_stmt = cur_stmt;
    proc_enter(p, 1);
    level = nframe;
    exec_until(level);          /* END FUNCTION で戻ってくる（位置は呼んだ式の中） */
    cur_stmt = keep_stmt;
    jumped = 0;
    *out = fn_result;
    fn_result.s = NULL;
    return 1;
}

/* 定義の SUB・FUNCTION の文に、上から流れてきた: END SUB・END FUNCTION のあとへ飛ばす */
static void skip_proc(int kind_kw)
{
    for (;;) {
        if (!advance_token()) {
            err(E_SUBEND);
        }
        if (CUR->t == T_KW && CUR->k == K_END && at_stmt_start()
            && L->tk[tp + 1].t == T_KW && L->tk[tp + 1].k == kind_kw) {
            tp += 2;
            jumped = 1;
            return;
        }
    }
}

/* EXIT DO・EXIT FOR・EXIT SUB・EXIT FUNCTION */
static void do_exit(void)
{
    if (accept_kw(K_DO)) {
        if (ndo == 0 || (nframe && ndo <= frames[nframe - 1].ndo)) {
            err(E_EXIT);
        }
        ndo--;
        nfor = dostk[ndo].nfor;         /* DO の中で始めた FOR・WHILE・SELECT もやめる */
        nwhile = dostk[ndo].nwhile;
        sel_cut(dostk[ndo].nsel);
        skip_do();
    } else if (accept_kw(K_FOR)) {
        if (nfor == 0 || (nframe && nfor <= frames[nframe - 1].nfor)) {
            err(E_EXIT);
        }
        nfor--;
        ndo = forstk[nfor].ndo;         /* FOR の中で始めた DO・WHILE・SELECT もやめる */
        nwhile = forstk[nfor].nwhile;
        sel_cut(forstk[nfor].nsel);
        skip_for();
    } else if (accept_kw(K_SUB)) {
        proc_return('S');
    } else if (accept_kw(K_FUNCTION)) {
        proc_return('F');
    } else {
        err(E_SYNTAX);
    }
}

/* 文の始まりのキーワードで、拡張の命令なら実行して 1 */
static int ext_stmt(int k)
{
    switch (k) {
    case K_ELSEIF:              /* 成り立った枝の終わり: END IF のあとへ */
        if_scan(0);
        return 1;
    case K_ENDIF:
        return 1;
    case K_SELECT: {
        Val v;
        if (!accept_kw(K_CASE)) {
            err(E_SYNTAX);
        }
        v = expr();
        if (nsel >= 32) {
            sfree(v.type == 's' ? v.s : NULL);
            err(E_MEM);
        }
        selstk[nsel++] = v;
        skip_stmt();
        select_scan(1);
        return 1;
    }
    case K_CASE:                /* 選ばれた枝の終わり: END SELECT のあとへ */
        if (nsel == 0) {
            err(E_CASE);
        }
        select_scan(0);
        return 1;
    case K_DO: {
        Pos at;
        at.li = li;
        at.tp = tp - 1;
        if (ndo > 0 && dostk[ndo - 1].at.li == at.li && dostk[ndo - 1].at.tp == at.tp) {
            ndo--;
        }
        if (loop_cond()) {
            if (ndo >= 64) {
                err(E_MEM);
            }
            dostk[ndo].at = at;
            dostk[ndo].nfor = nfor;
            dostk[ndo].nwhile = nwhile;
            dostk[ndo].nsel = nsel;
            ndo++;
        } else {
            skip_do();
        }
        return 1;
    }
    case K_LOOP:
        if (ndo == 0) {
            err(E_LOOP);
        }
        if (loop_cond()) {
            go_pos(dostk[ndo - 1].at);
        } else {
            ndo--;
        }
        return 1;
    case K_EXIT: do_exit(); return 1;
    case K_SUB: skip_proc(K_SUB); return 1;
    case K_FUNCTION: skip_proc(K_FUNCTION); return 1;
    case K_LOCAL:
        if (nframe == 0) {
            err(E_LOCAL);
        }
        do {
            if (CUR->t != T_ID) {
                err(E_SYNTAX);
            }
            save_local(named_var(CUR->s));
            tp++;
        } while (accept_op(','));
        return 1;
    case K_TIMER:               /* TIMER ON・OFF・STOP */
        if (accept_kw(K_ON)) {
            if (tmr_state == 0) {
                tmr_next = now_sec() + tmr_interval;
            }
            tmr_state = 1;
            tmr_force = 1;
        } else if (accept_kw(K_OFF)) {
            tmr_state = 0;
            tmr_pending = 0;
        } else if (accept_kw(K_STOP)) {
            tmr_state = 2;
        } else {
            err(E_SYNTAX);
        }
        return 1;
    case K_PWM: case K_SERVO: case K_PIN: {
        /* PWM 口,割合%・SERVO 口,角度[,速さ 度/秒]・PIN 口,0か1。
         * 口は名前（"HEATER"、/etc/io.conf）か、番号（仮想の機器: PWM 0 ヒーター〜4 モーター、SERVO 0〜63、PIN 0〜15） */
        Val a = expr();
        double v, sp = -1;
        expect_op(',');
        v = num_expr();
        if (k == K_SERVO && accept_op(',')) {
            sp = num_expr();
            if (sp < 1 || sp > 3600) {
                err(E_ILLFUNC);
            }
        }
        if (a.type == 's') {
            char nm[64];
            int n = slen(a.s) < 63 ? (int)slen(a.s) : 63;
            memcpy(nm, a.s ? a.s : "", (size_t)n);
            nm[n] = 0;
            sfree(a.s);
            io_named_set(nm, k == K_PWM ? IO_K_PWM : k == K_SERVO ? IO_K_SERVO : IO_K_PIN,
                         k == K_PIN ? (v != 0) : v, sp > 0 ? (int32_t)sp : 0);
            return 1;
        }
        {
            long ch = (long)floor(a.n + 0.5);
            if (k == K_PWM) {
                if (ch < 0 || ch > 4 || v < 0 || v > 100) {
                    err(E_ILLFUNC);
                }
                io_write(VIO_P_ACT + (unsigned)ch, v);
            } else if (k == K_SERVO) {
                if (ch < 0 || ch > 63 || v < -180 || v > 180) {
                    err(E_ILLFUNC);
                }
                if (sp > 0) {
                    io_write(VIO_P_SPEED, sp);
                }
                io_write(VIO_P_SERVO + (unsigned)ch, v * 10);
            } else {
                int bits;
                if (ch < 0 || ch > 15) {
                    err(E_ILLFUNC);
                }
                bits = io_read(VIO_P_DOUT);
                io_write(VIO_P_DOUT, v != 0 ? (bits | (1 << ch)) : (bits & ~(1 << ch)));
            }
        }
        return 1;
    }
    case K_ESTOP:               /* ESTOP（止める）・ESTOP 0（解く） */
        io_write(VIO_P_ESTOP, at_end() ? 1 : num_expr());
        return 1;
    case K_WATCHDOG: {          /* WATCHDOG ミリ秒（0 でやめる） */
        double ms = num_expr();
        if (ms < 0) {
            err(E_ILLFUNC);
        }
        io_write(VIO_P_WDT, ms);
        return 1;
    }
    }
    return 0;
}

/* QEMU 上のXYZ直線移動の計算試験。距離と定速時の所要時間を計算し、実機 I/O は行わない。 */
static void do_move(void)
{
    double x = num_expr();
    double y, z, feed, dx, dy, dz, distance, duration;
    char msg[384];

    expect_op(',');
    y = num_expr();
    expect_op(',');
    z = num_expr();
    expect_op(',');
    feed = num_expr();
    if (!isfinite(x) || !isfinite(y) || !isfinite(z) || !isfinite(feed) || feed <= 0) {
        err(E_ILLFUNC);
    }
    dx = x - sim_xyz[0];
    dy = y - sim_xyz[1];
    dz = z - sim_xyz[2];
    distance = sqrt(dx * dx + dy * dy + dz * dz);
    duration = distance * 60.0 / feed;
    if (!isfinite(distance) || !isfinite(duration)) {
        err(E_ILLFUNC);
    }
    snprintf(msg, sizeof(msg),
             "MOVE SIM X=%.15g Y=%.15g Z=%.15g F=%.15g DIST=%.9gmm TIME=%.9gs",
             x, y, z, feed, distance, duration);
    put_str(msg);
    newline();
    sim_xyz[0] = x;
    sim_xyz[1] = y;
    sim_xyz[2] = z;
}

/* 任意関節数（最大64）の仮想モデルを設定し、全関節をシミュレーション原点に置く。 */
static void do_joints(void)
{
    long n = int_expr();
    char msg[64];

    if (n < 1 || n > SIM_MAX_JOINTS) {
        err(E_ILLFUNC);
    }
    sim_joint_count = (int)n;
    for (int i = 0; i < SIM_MAX_JOINTS; i++) {          /* 今のサーボの角度から始める */
        sim_joint_pos[i] = io_read(VIO_P_SERVO + (unsigned)i) / 10.0;
    }
    snprintf(msg, sizeof(msg), "JOINTS SIM N=%d", sim_joint_count);
    put_str(msg);
    newline();
}

/* 同期する関節空間移動の所要時間を見積もる。時刻ごとの再生や実機 I/O はまだ行わない。 */
static void do_movej(void)
{
    double target[SIM_MAX_JOINTS];
    double speed, max_delta = 0.0, duration;
    char msg[128];
    int i;

    if (sim_joint_count < 1) {
        err(E_ILLFUNC);
    }
    for (i = 0; i < sim_joint_count; i++) {
        target[i] = num_expr();
        if (!isfinite(target[i])) {
            err(E_ILLFUNC);
        }
        expect_op(',');
    }
    speed = num_expr();                 /* deg/s */
    if (!isfinite(speed) || speed <= 0) {
        err(E_ILLFUNC);
    }
    for (i = 0; i < sim_joint_count; i++) {
        double delta = fabs(target[i] - sim_joint_pos[i]);
        if (delta > max_delta) {
            max_delta = delta;
        }
    }
    duration = max_delta / speed;
    if (!isfinite(duration)) {
        err(E_ILLFUNC);
    }
    snprintf(msg, sizeof(msg),
             "MOVEJ SIM DOF=%d VMAX=%.9gdeg/s TIME=%.9gs",
             sim_joint_count, speed, duration);
    put_str(msg);
    newline();
    memcpy(sim_joint_pos, target, (size_t)sim_joint_count * sizeof(target[0]));
    /* 仮想の機器のサーボ 0〜n-1 に目標を出す（速さはすべてのサーボで共通） */
    io_write(VIO_P_SPEED, speed < 1 ? 1 : speed > 3600 ? 3600 : speed);
    for (i = 0; i < sim_joint_count; i++) {
        double a = target[i] < -180 ? -180 : target[i] > 180 ? 180 : target[i];
        io_write(VIO_P_SERVO + (unsigned)i, a * 10);
    }
}

static void stmt(void)
{
    Tok *t = CUR;

    if (t->t == T_ID) {             /* LET を省いた代入 */
        LV lv;
        if (!(L->tk[tp + 1].t == T_OP && L->tk[tp + 1].k == '=') && call_sub_stmt()) {
            return;                 /* SUB の名前だけ（CALL を省いた形） */
        }
        lv = lvalue();
        expect_op('=');
        assign(lv, expr());
        return;
    }
    if (t->t != T_KW) {
        err(E_SYNTAX);
    }
    tp++;
    switch (t->k) {
    case K_LET: {
        LV lv = lvalue();
        expect_op('=');
        assign(lv, expr());
        return;
    }
    case K_PRINT: do_print(); return;
    case K_IF: do_if(); return;
    case K_GOTO: do_goto_num(); return;
    case K_GOSUB:
        if (CUR->t != T_NUM) {
            err(E_SYNTAX);
        }
        {
            int num = (int)CUR->n;
            tp++;
            do_gosub_to(num);
        }
        return;
    case K_RETURN:
        if (ngosub == 0) {
            err(E_RETURN);
        }
        ngosub--;
        timer_returned();
        if (CUR->t == T_NUM) {
            go_line(line_index((int)CUR->n));
        } else {
            go_pos(gosubstk[ngosub]);
        }
        return;
    case K_FOR: do_for(); return;
    case K_NEXT: do_next(); return;
    case K_WHILE: {
        Pos at;
        Val c;
        int truth;
        at.li = li;
        at.tp = tp - 1;
        c = expr();
        truth = c.type == 's' ? slen(c.s) != 0 : c.n != 0;
        sfree(c.s);
        if (nwhile > 0 && whilestk[nwhile - 1].li == at.li && whilestk[nwhile - 1].tp == at.tp) {
            nwhile--;
        }
        if (truth) {
            if (nwhile >= 64) {
                err(E_MEM);
            }
            whilestk[nwhile++] = at;
        } else {
            skip_while();
        }
        return;
    }
    case K_WEND:
        if (nwhile == 0) {
            err(E_WEND);
        }
        go_pos(whilestk[nwhile - 1]);
        return;
    case K_END:
        if (CUR->t == T_KW) {
            switch (CUR->k) {
            case K_IF: tp++; return;                    /* END IF */
            case K_SELECT:                              /* 選ばれた枝の終わりの END SELECT */
                tp++;
                if (nsel == 0) {
                    err(E_CASE);
                }
                sel_pop();
                return;
            case K_SUB: tp++; proc_return('S'); return;
            case K_FUNCTION: tp++; proc_return('F'); return;
            }
        }
        longjmp(errjb, R_END);
    case K_STOP:
        longjmp(errjb, R_STOP);
    case K_REM:
        skip_line();
        return;
    case K_DATA:
        tp++;               /* 中身（T_RAW）を飛ばす */
        return;
    case K_DIM: do_dim(); return;
    case K_INPUT: do_input(0); return;
    case K_LINE:
        if (accept_kw(K_INPUT)) {
            do_input(1);
            return;
        }
        do_gline();
        return;
    case K_READ: do_read(); return;
    case K_RESTORE: do_restore(); return;
    case K_ON: do_on(); return;
    case K_CLS: {               /* CLS 1 文字、2 グラフィックス、3 両方（省くと 1） */
        long n = 1;
        if (!at_end()) {
            n = int_expr();
        }
        if (n & 1) {
            fputs("\033[2J\033[H", stdout);
            col = row = 0;
        }
        if (n & 2) {
            gfx_cls();
        }
        return;
    }
    case K_LOCATE: do_locate(); return;
    case K_COLOR: do_color(); return;
    case K_RANDOMIZE:
        if (!at_end()) {
            rnd_state = (unsigned long)(int_expr() & 0xffffffffL);
        } else {
            rnd_state = (unsigned long)time(NULL);
        }
        return;
    case K_DEF: do_def(); return;
    case K_SWAP: {
        LV a = lvalue(), b;
        expect_op(',');
        b = lvalue();
        if ((a.type == '$') != (b.type == '$')) {
            err(E_TYPE);
        }
        if (a.type == '$') {
            char *s = *a.sp;
            *a.sp = *b.sp;
            *b.sp = s;
        } else {
            double d = *a.np;
            *a.np = *b.np;
            *b.np = d;
        }
        return;
    }
    case K_BEEP: do_beep(); return;
    case K_MOVE: do_move(); return;
    case K_JOINTS: do_joints(); return;
    case K_MOVEJ: do_movej(); return;
    case K_PLAY: do_play(); return;
    case K_PSET: do_pset(0); return;
    case K_PRESET: do_pset(1); return;
    case K_CIRCLE: do_circle(); return;
    case K_PAINT: do_paint(); return;
    case K_VIEW: do_view(); return;
    case K_WINDOW: do_window(); return;
    case K_SOUND:
        ignore_args();          /* SOUND（音源のレジスタに直接書く）は受け付けるだけ */
        return;
    case K_SCREEN: do_screen(); return;
    case K_CALL:
        if (CUR->t == T_ID && call_sub_stmt()) {
            return;             /* CALL 名前(引数): SUB を呼ぶ */
        }
        ignore_args();          /* 機械語の CALL は受け付けるだけ */
        return;
    case K_KEY:
        /* KEY n,"文字列"（CHR$(13) で改行まで打つ）・KEY LIST・KEY ON / OFF（見出しの表示。受け付けるだけ） */
        if (accept_kw(K_LIST)) {
            int i;
            for (i = 0; i < 10; i++) {
                char line[80], *q;
                snprintf(line, sizeof(line), "F%-2d %.31s", i + 1, fkey[i]);
                if ((q = strchr(line, '\r')) != NULL) {
                    strcpy(q, "  <CR>");
                }
                put_str(line);
                put_str("\n");
            }
            return;
        }
        if (accept_kw(K_ON) || accept_kw(K_OFF)) {
            return;
        }
        {
            int k = (int)floor(num_expr() + 0.5);
            char *s;
            expect_op(',');
            s = str_expr();
            if (k < 1 || k > 10) {
                sfree(s);
                err(E_ILLFUNC);
            }
            snprintf(fkey[k - 1], sizeof(fkey[0]), "%.15s", s);     /* N88 と同じく 15 文字まで */
            sfree(s);
        }
        return;
    case K_WIDTH: case K_CONSOLE:
        ignore_args();      /* 段階 1 では受け付けるだけ */
        return;
    case K_OUT: {               /* &HE000〜&HE07F は仮想の機器（/dev/io）。ほかの番号は受け付けるだけ */
        long port = (long)floor(num_expr() + 0.5) & 0xffff;
        double v;
        expect_op(',');
        v = num_expr();
        if (port >= 0xe000 && port < 0xe080) {
            io_write((unsigned)(port - 0xe000), v);
        }
        return;
    }
    case K_POKE:
        num_expr();         /* 実機の I/O はまだない */
        expect_op(',');
        num_expr();
        return;
    case K_ERASE: do_erase(); return;
    case K_CLEAR:
        ignore_args();
        clear_vars();
        return;
    case K_DEFINT: case K_DEFSNG: case K_DEFDBL: case K_DEFSTR: do_deftype(t->k); return;
    case K_SLEEP: {         /* Tamazusa の拡張: SLEEP ミリ秒（FOR の空回しの代わり） */
        long ms = int_expr();
        if (ms < 0) {
            err(E_ILLFUNC);
        }
        out_flush();
        gfx_flush(1);
        do_sleep(ms);
        return;
    }
    case K_TRON: trace = 1; return;
    case K_TROFF: trace = 0; return;
    case K_RUN:
        if (CUR->t == T_STR) {
            char *n = xstrdup(CUR->s);
            tp++;
            if (!confirm_discard()) {
                free(n);
                longjmp(errjb, R_END);
            }
            load_file(n, 0, 0);
            free(n);
            run_program(0);
        } else if (CUR->t == T_NUM) {
            int idx = line_index((int)CUR->n);
            run_program(idx);
        } else {
            run_program(0);
        }
        return;
    case K_LIST: do_list(); return;
    case K_NEW:
        if (!confirm_discard()) {
            longjmp(errjb, R_END);
        }
        prog_new();
        cur_file[0] = 0;
        cur_sjis = 0;
        prog_dirty = 0;
        longjmp(errjb, R_END);
    case K_LOAD: {
        char *s;
        int run = 0;
        if (at_end()) {             /* 名前を省くと「開く」の画面（Windows）か、名前を聞く */
            char *n;
            if (!confirm_discard() || (n = ask_file_name(0)) == NULL) {
                longjmp(errjb, R_END);
            }
            load_file(n, 0, 0);
            free(n);
            longjmp(errjb, R_END);
        }
        s = str_expr();
        if (!confirm_discard()) {
            sfree(s);
            longjmp(errjb, R_END);
        }
        if (accept_op(',')) {
            if (CUR->t == T_ID && strcmp(CUR->s, "R") == 0) {
                run = 1;
                tp++;
            } else {
                err(E_SYNTAX);
            }
        }
        load_file(s, 0, 0);
        sfree(s);
        if (run) {
            run_program(0);
            return;
        }
        longjmp(errjb, R_END);
    }
    case K_SAVE: {
        char *s;
        if (accept_kw(K_AS) || (at_end() && !cur_file[0])) {
            /* SAVE AS: 名前を付けて保存（Windows は画面）。名前のない SAVE で、まだ名前がないときも */
            char *n = ask_file_name(1);
            if (n) {
                save_file(n);
                free(n);
            }
            return;
        }
        if (at_end()) {             /* SAVE だけ: 最後に LOAD・SAVE した名前に上書き */
            save_file(cur_file);
            return;
        }
        s = str_expr();
        if (accept_op(',')) {
            tp++;           /* ,A（テキストで保存）: いつもテキスト */
        }
        save_file(s);
        sfree(s);
        return;
    }
    case K_CONT:
        if (!cont_ok) {
            err(E_CONT);
        }
        go_pos(cont_at);
        return;
    case K_AUTO: {
        /* AUTO [始めの番号][,増分]: 行番号を出して打ってもらう。何も打たずに改行か Ctrl+C でやめる */
        int a = 10, s = 10;
        if (CUR->t == T_NUM) {
            a = (int)CUR->n;
            tp++;
        }
        if (accept_op(',')) {
            s = (int)floor(num_expr() + 0.5);
        }
        if (a < 0 || a > 65529 || s <= 0) {
            err(E_ILLFUNC);
        }
        auto_on = 1;
        auto_num = a;
        auto_step = s;
        longjmp(errjb, R_END);
    }
    case K_SYSTEM:
        if (!confirm_discard()) {
            longjmp(errjb, R_END);
        }
        longjmp(errjb, R_EXIT);
    case K_FILES: do_files(); return;
    case K_DELETE: do_delete(); longjmp(errjb, R_END);
    case K_RENUM: do_renum(); longjmp(errjb, R_END);
    case K_ELSE:
        if (cur_stmt.tp == 0) {     /* 行の最初の ELSE: 複数行の IF の、成り立った枝の終わり */
            if_scan(0);
            return;
        }
        skip_line();
        return;
    case K_OPEN: do_open(); return;
    case K_CLOSE: do_close(); return;
    case K_WRITE: do_writestmt(); return;
    case K_KILL: do_kill(); return;
    case K_NAME: do_name(); return;
    case K_FIELD: do_field(); return;
    case K_GET: case K_PUT:
        if (is_op('(') || is_kw(K_STEP)) {
            if (t->k == K_GET) {
                do_gget();
            } else {
                do_gput();
            }
            return;
        }
        getput(t->k == K_PUT);
        return;
    case K_LSET: do_lrset(0); return;
    case K_RSET: do_lrset(1); return;
    case K_COMMON: ignore_args(); return;
    case K_MERGE: {
        char *s = str_expr();
        load_file(s, 1, 1);
        sfree(s);
        longjmp(errjb, R_END);
    }
    case K_CHAIN: {             /* CHAIN [MERGE] "ファイル"[,行][,ALL]: 変数を残したまま次のプログラムへ */
        int merge = accept_kw(K_MERGE), idx = 0;
        long line = -1;
        char *s = str_expr();
        if (accept_op(',')) {
            if (!is_op(',') && !at_end()) {
                line = int_expr();
            }
            ignore_args();
        }
        load_file(s, merge, 1);
        sfree(s);
        nfor = ngosub = nwhile = 0;
        if (nprog == 0) {
            longjmp(errjb, R_END);
        }
        if (line >= 0) {
            idx = line_index((int)line);
        }
        go_line(idx);
        return;
    }
    case K_ERROR: {
        long n = int_expr();
        if (n < 1 || n > 255) {
            err(E_ILLFUNC);
        }
        err((int)n);
    }
    case K_RESUME:
        if (!in_err) {
            err(E_RESUME);
        }
        in_err = 0;
        if (accept_kw(K_NEXT)) {
            go_pos(resume_at);
            skip_stmt();
        } else if (CUR->t == T_NUM && CUR->n != 0) {
            go_line(line_index((int)CUR->n));
        } else {
            go_pos(resume_at);
        }
        jumped = 1;
        return;
    case K_MID_S: {         /* MID$(A$,n[,m])=B$ */
        LV lv;
        long a, b = -1;
        char *s;
        expect_op('(');
        lv = lvalue();
        if (lv.type != '$') {
            err(E_TYPE);
        }
        expect_op(',');
        a = int_expr();
        if (accept_op(',')) {
            b = int_expr();
        }
        expect_op(')');
        expect_op('=');
        s = str_expr();
        if (a < 1 || !*lv.sp || a > (long)slen(*lv.sp)) {
            sfree(s);
            err(E_ILLFUNC);
        }
        {
            char *d = *lv.sp + a - 1;
            long n = (long)slen(s), room = (long)slen(*lv.sp) - (a - 1);
            if (b >= 0 && b < n) {
                n = b;
            }
            if (n > room) {
                n = room;
            }
            memcpy(d, s, n);
        }
        sfree(s);
        return;
    }
    }
    if (ext_stmt(t->k)) {
        return;
    }
    err(E_SYNTAX);
}

/* 実行の本体。直接モードの行か、プログラムの途中から。
 * level > 0 は FUNCTION の中: その呼び出しが戻ったら（nframe < level）返る */
static void exec_until(int level)
{
    unsigned count = 0;
    for (;;) {
        Tok *t;
        if ((++count & 255) == 0) {
            gfx_flush(0);       /* 描いたあと計算だけ続くときも、ときどき画面に出す */
        }
        if (brkflag) {
            brkflag = 0;
            longjmp(errjb, R_STOP);
        }
        if (tmr_state && ((count & 15) == 0 || tmr_force)) {
            tmr_force = 0;
            if (timer_check()) {
                continue;
            }
        }
        t = CUR;
        if (t->t == T_EOL) {
            if (li < 0) {
                return;                 /* 直接モードの行が終わった */
            }
            if (li + 1 >= nprog) {
                longjmp(errjb, R_END);
            }
            go_line(li + 1);
            jumped = 0;
            continue;
        }
        if (t->t == T_OP && t->k == ':') {
            tp++;
            continue;
        }
        jumped = 0;
        cur_stmt.li = li;
        cur_stmt.tp = tp;
        stmt();
        if (level > 0 && nframe < level) {
            return;                     /* FUNCTION が戻った */
        }
        if (jumped) {
            continue;
        }
        t = CUR;
        if (t->t == T_EOL || (t->t == T_OP && t->k == ':')) {
            continue;
        }
        if (t->t == T_KW && t->k == K_ELSE) {   /* THEN の文が終わった: ELSE 以降は飛ばす */
            skip_line();
            continue;
        }
        err(E_SYNTAX);
    }
}

static void exec(void)
{
    exec_until(0);
}

/* 1 行を処理する（行番号つきなら記憶し、なければ実行する）。SYSTEM なら 0 */
static int stop_in_fn;

/* 止まるときの安全のための出力の停止（ここでのエラーは無視する） */
static void io_safe_quiet(void)
{
    jmp_buf keep;
    memcpy(keep, errjb, sizeof(keep));
    if (setjmp(errjb) == 0) {
        io_safe();
    }
    memcpy(errjb, keep, sizeof(keep));
}

static int do_command(const char *text, int *show_ok)
{
    const char *p = text;
    int r;
    volatile int is_direct = 0;

    while (*p == ' ') {
        p++;
    }
    *show_ok = 0;
    r = setjmp(errjb);
    if (r == 0) {
        if (isdigit((unsigned char)*p)) {
            int num = (int)strtol(p, (char **)&p, 10);
            if (num > 65529) {
                err(E_SYNTAX);
            }
            if (*p == ' ') {
                p++;
            }
            store_line(num, p);
            return 1;
        }
        if (*p == 0) {
            return 1;
        }
        is_direct = 1;
        free_line(direct);
        direct = NULL;
        direct = make_line(-1, p);
        L = direct;
        li = -1;
        tp = 0;
        exec();
        r = R_END;
    } else if (r != R_END && r != R_EXIT && fn_active()) {
        /* FUNCTION の中で止まった: C の呼び出しはもう戻れないので、しまった値を戻す。
         * エラーなら、いちばん外の呼び出しの文で起きたこととする（RESUME はその文から） */
        if (r < R_END) {
            cur_stmt = frames[0].call_stmt;
        }
        frames_unwind();
        stop_in_fn = 1;
    }
    if (r < R_END && cur_stmt.li >= 0 && li >= 0 && onerr_line && !in_err && !report_erl) {
        /* ON ERROR GOTO: エラー処理の行へ */
        outf = NULL;
        last_err = r;
        last_erl = prog[cur_stmt.li]->num;
        in_err = 1;
        resume_at = cur_stmt;
        go_line(line_index(onerr_line));
        exec();
        r = R_END;
    }
    outf = NULL;
    gfx_flush(1);
    tty_cooked();
    if (color_used) {
        fputs("\033[0m", stdout);
        color_used = 0;
    }
    if (r == R_EXIT) {
        return 0;
    }
    if (r == R_END) {
        if (li >= 0) {
            frames_unwind();
            timer_reset();
            cont_ok = 0;
            close_all();            /* プログラムが終わったらファイルを閉じる（N88 と同じ） */
        }
        in_err = 0;
        if (col > 0) {
            newline();
        }
        *show_ok = is_direct;
        out_flush();
        return 1;
    }
    if (r == R_STOP) {
        if (col > 0) {
            newline();
        }
        if (li >= 0) {
            io_safe_quiet();            /* 中断で止まる: 機器の出力を止める */
        }
        if (li >= 0 && stop_in_fn) {
            stop_in_fn = 0;
            printf("Break in %d\n", L->num);
            cont_ok = 0;
        } else if (li >= 0) {
            cont_ok = 1;
            /* 次の文から続ける */
            if (CUR->t != T_EOL) {
                skip_stmt();
            }
            cont_at = here();
            printf("Break in %d\n", L->num);
        } else {
            printf("Break\n");
        }
        col = 0;
        *show_ok = 1;
        out_flush();
        return 1;
    }
    /* エラー */
    stop_in_fn = 0;
    if (li >= 0) {
        frames_unwind();
        timer_reset();
        io_safe_quiet();                /* エラーで止まる: 機器の出力を止める */
    }
    if (!report_erl) {
        last_err = r;
        last_erl = li >= 0 && L ? L->num : 0;
    }
    if (col > 0) {
        newline();
    }
    if (report_erl || (li >= 0 && L)) {
        printf("%s in %d\n", errmsg(r), last_erl);
    } else {
        printf("%s\n", errmsg(r));
    }
    report_erl = 0;
    in_err = 0;
    failed = 1;
    col = 0;
    cont_ok = 0;
    *show_ok = 1;
    out_flush();
    return 1;
}

static void init(void)
{
    int k;
    struct sigaction sa;

#ifdef _WIN32
    win_console_setup();
#endif
    for (k = 0; k < K_COUNT; k++) {
        const char *n = kwname[k];
        size_t m = strlen(n);
        if (m > 2 && strcmp(n + m - 2, "_S") == 0) {
            memcpy(kwtext[k], n, m - 2);
            kwtext[k][m - 2] = '$';
            kwtext[k][m - 1] = 0;
        } else {
            strcpy(kwtext[k], n);
        }
    }
    memset(deftype, '!', sizeof(deftype));
    if (tcgetattr(0, &tio_saved) == 0) {
        tio_have = 1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sa.sa_flags = 0;            /* read を EINTR で止める */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    setvbuf(stdout, NULL, _IOFBF, 8192);
    clock_gettime(CLOCK_MONOTONIC, &last_flush);
    rnd_state = (unsigned long)time(NULL);
    voices_reset();
    gfx_reset();
    set_workdir();
    build_u2s();
}

int main(int argc, char **argv)
{
    char buf[MAXLINE];
    int show_ok = 1, argi = 1, stay = 0;

    init();
    if (argi < argc && strcmp(argv[argi], "-i") == 0) {
        stay = 1;
        argi++;
    }
    if (argi < argc) {
        char cmd[MAXLINE];
        interactive = stay;
        snprintf(cmd, sizeof(cmd), "RUN \"%s\"", argv[argi]);
        if (!do_command(cmd, &show_ok)) {
            out_flush();
            snd_close();
            return 0;
        }
        if (!stay) {
            out_flush();
            snd_close();
            tty_cooked();
            return failed ? 1 : 0;
        }
    } else {
#ifdef _WIN32
        printf("T88-BASIC Version %s for Windows  Copyright (c) 2026 Hideyuki Kishida\n", VERSION);
        printf("A BASIC for device control on Tamazusa OS: N88-BASIC compatible, with extensions\n");
        printf("for research and education.  Non-commercial use only (PolyForm Noncommercial 1.0.0).\n");
        printf("N88-BASIC is a product of NEC Corporation. This software is not affiliated with NEC.\n");
        printf("SYSTEM to exit.  See LICENSE.txt for details.\n");
#else
        printf("T88-BASIC Version %s (Tamazusa OS)  Copyright (c) 2026 Hideyuki Kishida\n", VERSION);
        printf("N88-BASIC compatible.  SYSTEM to exit.\n");
#endif
        if (strcmp(workdir, ".") != 0) {
            printf("Files are saved in %s\n", workdir);
        }
    }
    show_ok = 1;
    for (;;) {
        int r;
        if (auto_on) {              /* AUTO: 行番号を出して 1 行。すでにある行には * を付ける（N88 と同じ） */
            char pre[16], line[MAXLINE + 16];
            int ex = 0;
            find_line(auto_num, &ex);
            snprintf(pre, sizeof(pre), "%d%c", auto_num, ex ? '*' : ' ');
            put_str(pre);
            out_flush();
            brkflag = 0;
            r = read_line(buf, sizeof(buf));
            if (r < 0 || buf[0] == 0) {
                if (r == -2) {
                    put_str("\n");
                }
                auto_on = 0;
                show_ok = 1;
                if (r == -1) {
                    break;
                }
                continue;
            }
            snprintf(line, sizeof(line), "%d %s", auto_num, buf);
            do_command(line, &show_ok);
            auto_num += auto_step;
            if (auto_num > 65529) {
                auto_on = 0;
            }
            show_ok = 0;
            continue;
        }
        if (show_ok) {
            put_str("Ok\n");
        }
        out_flush();
        brkflag = 0;
        r = read_line(buf, sizeof(buf));
        if (r == -2) {
            put_str("\n");
            show_ok = 1;
            continue;
        }
        if (r < 0) {
            break;
        }
        if (!do_command(buf, &show_ok)) {
            break;
        }
    }
    out_flush();
    snd_close();
    tty_cooked();
    return 0;
}
