/* kernel/tk/m28_basic_test.c — M28: T88-BASIC（SMODE=1 かつ MMU=1）
 *
 *   ディスクの basic.elf（N88-BASIC 互換のインタプリタ、musl で静的リンク）を対話モードで動かし、
 *   台本を流し込む。
 *   1. /bas/TEST1.BAS（言語の核の自己テスト）の結果がすべて期待どおり
 *   2. /bas/TEST2.BAS（段階 2: 順ファイル・ランダムファイル・MKI$/MKS$・ON ERROR GOTO・RESUME・NAME・KILL）
 *   3. 対話モード: 行の入力・RUN・空白なしの書き方（FORI=1TO3）・INKEY$・SAVE と LOAD（/tmp）・FILES・KILL・
 *      Shift_JIS のプログラムの LOAD・漢字の文字列（N88 と同じく Shift_JIS で持つ）・エラーの表示・書き込めないディスクへの SAVE・SYSTEM で終わる
 *   4. 段階 3: PLAY（2 声）と BEEP の波形が /dev/dsp（virtio-sound）へ流れ、装置が鳴らし終える
 *   5. 段階 4: /bas/GRAPH.BAS の絵（色見本・線・箱・円・楕円・PAINT・弧・PSET）が /dev/fb0（virtio-gpu）に出る。
 *      文字の面（PRINT した文字）が、N88 と同じくグラフィックスの上に重なる
 *   6. 拡張: /bas/TEST3.BAS（複数行の IF・SELECT CASE・DO LOOP・EXIT・SUB・FUNCTION・LOCAL・ON TIMER・
 *      仮想の機器 /dev/io の INP・OUT・SENSOR・PWM・SERVO・ESTOP・WATCHDOG）、MOVE・MOVEJ、
 *      エラーで止まったプログラムの出力が止まり、/dev/io の状態がカーネルに残ること
 *   7. 終わったあと、プロセスの表と /tmp が空になり、ページとカーネルのメモリがすべて返っている
 *
 * 合格の目印: [M28] PASS
 */
#include <stdint.h>
#include <stddef.h>
#include "../fs/tarfs.h"

extern void uart_puts(const char *s);
struct rv_uproc;
extern struct rv_uproc *rv_linux_load_path(const char *path, const char *const argv[],
                                           const char *const envp[], long *err);
extern long rv_linux_run_process(struct rv_uproc *p);
extern int rv_linux_proc_count(void);
extern int rv_page_free_count(void);
extern int rv_tmp_count(void);
extern int virtio_snd_present(void);
extern int virtio_gpu_present(void);
extern uint32_t rv_fb_pixel(unsigned x, unsigned y);
extern volatile unsigned long rv_gpu_flushes, rv_gpu_errors;
extern int rv_snd_inflight(void);
extern volatile unsigned long rv_snd_bytes, rv_snd_periods_done;
extern size_t rv_heap_in_use;
extern void rv_console_set_test_input(const char *s);
extern void rv_console_capture(int on);
extern const char *rv_console_captured(void);

static const char script[] =
    "RUN \"/bas/TEST1\"\n"
    "RUN \"TEST2\"\n"
    "NEW\n"
    "10 FORI=1TO3:PRINT I*I;:NEXT:PRINT\n"
    "20 A$=INKEY$:IF A$=\"\" THEN 20\n"
    "30 PRINT \"KEY=\";A$\n"
    "RUN\n"
    "Z"
    "SAVE \"MYPROG\"\n"
    "NEW\n"
    "LOAD \"myprog\"\n"
    "LIST 30\n"
    "FILES\n"
    "KILL \"MYPROG.BAS\"\n"
    "PRINT \"ST=\";ESTOP:ESTOP 0\n"
    "NEW\n"
    "10 MOVE 100,50,-10,500\n"
    "20 END\n"
    "RUN\n"
    "JOINTS 8\n"
    "NEW\n"
    "10 MOVEJ 0,15,-10,20,5,0,30,10,30\n"
    "20 END\n"
    "RUN\n"
    "RUN \"TEST3\"\n"
    "NEW\n"
    "10 PWM 0,100:PRINT \"H1=\";PWM(0)\n"
    "20 PRINT 1/0\n"
    "RUN\n"
    "PRINT \"H2=\";PWM(0)\n"
    "RUN \"SJIS\"\n"
    "A$=\"漢字かな\":PRINT LEN(A$);MID$(A$,3,2);ASC(A$)\n"
    "PLAY \"T240L8CDEFGAB>C\",\"T240L2O3C\":BEEP\n"
    "RUN \"GRAPH\"\n"
    "PRINT 1/0\n"
    "SAVE \"/X\"\n"
    "SYSTEM\n";

static int check(int ok, const char *what)
{
    uart_puts(ok ? "[M28] OK   " : "[M28] NG   ");
    uart_puts(what);
    uart_puts("\r\n");
    return ok;
}

static int contains(const char *hay, const char *needle)
{
    const char *h;
    for (h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b && *a == *b) {
            a++;
            b++;
        }
        if (*b == 0) {
            return 1;
        }
    }
    return 0;
}

int m28_run_basic_test(void)
{
    static const char *const argv[] = { "basic", 0 };
    static const char *const envp[] = { "HOME=/", "PATH=/bin:/", "TERM=vt100", 0 };
    struct tarfs_file f;
    struct rv_uproc *p;
    const char *out;
    int free0 = rv_page_free_count();
    size_t heap0 = rv_heap_in_use;
    long r;
    int pass = 1;
    unsigned long snd0 = rv_snd_bytes, per0 = rv_snd_periods_done;

    uart_puts("\r\n[M28] T88-BASIC test\r\n");
    if (tarfs_find("basic.elf", &f) != 1) {
        uart_puts("[M28] SKIP (basic.elf is not on the disk: musl is needed)\r\n");
        return 1;
    }
    p = rv_linux_load_path("basic.elf", argv, envp, &r);
    if (p == NULL) {
        uart_puts("[M28] load failed\r\n[M28] FAIL\r\n");
        return 0;
    }
    rv_console_set_test_input(script);
    rv_console_capture(1);
    r = rv_linux_run_process(p);
    rv_console_capture(0);
    rv_console_set_test_input(0);
    out = rv_console_captured();

    pass &= check(contains(out, "T88-BASIC Version") && contains(out, "div .3333333")
                  && contains(out, "int-4 -3  3  1  1024"), "banner, numbers and operators");
    pass &= check(contains(out, "HEELLLO 5  65 B") && contains(out, "str 12 4.5  3 FF10***[  ]")
                  && contains(out, "AxyDE"), "string functions and MID$ statement");
    pass &= check(contains(out, "sum 55") && contains(out, " 10  7  4  1") && contains(out, "skip ok")
                  && contains(out, "nest 6") && contains(out, "while 3"), "FOR/NEXT, STEP, zero-trip, NEXT J,I, WHILE");
    pass &= check(contains(out, "sub\nback") && contains(out, "big") && contains(out, "else ok")
                  && contains(out, "compact ok") && contains(out, "on ok") && contains(out, "sub2\non gosub back"),
                  "GOSUB, IF THEN ELSE, compact keywords, ON GOTO/GOSUB");
    pass &= check(contains(out, "arr 25  4") && contains(out, "ABCD") && contains(out, "data 42 Q,str 7")
                  && contains(out, "restore 42") && contains(out, "fn 10") && contains(out, "swap 7  3"),
                  "arrays, DATA/READ/RESTORE, DEF FN, SWAP");
    pass &= check(contains(out, "rel-1  0 -1  1  7") && contains(out, "  3.14") && contains(out, "ABCDX")
                  && contains(out, " 1E+10 -.5  123456789  100000") && contains(out, "intvar 40000")
                  && contains(out, "tab       x"), "logic, PRINT USING, number format, TAB");
    pass &= check(contains(out, "line[ABC 12 -3.5 ]") && contains(out, "write[X,Y] 42 [Z]")
                  && contains(out, "using 3.1") && contains(out, "append[LAST] eof-1  36"),
                  "sequential files: PRINT #, WRITE #, USING, APPEND, LINE INPUT #, INPUT #, EOF, LOF");
    pass &= check(contains(out, "rec[NAME2   ] 200  .5 [ *] 2  48")
                  && contains(out, "mbf 129  0  .3333333333333333"),
                  "random files: FIELD, LSET/RSET, PUT, GET, MKI$/MKS$/MKD$ (MBF), LOC, LOF");
    pass &= check(contains(out, "err 11 line 220") && contains(out, "resumed next")
                  && contains(out, "err 53 line 230") && contains(out, "after 1")
                  && contains(out, "err 200 line 250") && contains(out, "killed ok\nend"),
                  "ON ERROR GOTO, RESUME NEXT / line, ERROR, ERR, ERL, NAME, KILL");
    pass &= check(contains(out, " 1  4  9") && contains(out, "KEY=Z"), "typed program, RUN, INKEY$");
    pass &= check(contains(out, "MOVE SIM X=100 Y=50 Z=-10 F=500 DIST=")
                  && contains(out, "TIME="),
                  "MOVE command computes 3-axis linear distance and constant-feed duration");
    pass &= check(contains(out, "JOINTS SIM N=8")
                  && contains(out, "MOVEJ SIM DOF=8 VMAX=30deg/s TIME=1s"),
                  "MOVEJ synchronizes an arbitrary joint vector in simulation mode");
    pass &= check(contains(out, "Files are saved in /tmp") && contains(out, "30 PRINT \"KEY=\";A$")
                  && contains(out, "MYPROG.BAS"), "SAVE and LOAD in /tmp (any case), FILES");
    pass &= check(contains(out, "TEST3 ALL OK"),
                  "/bas/TEST3.BAS: block IF, SELECT CASE, DO LOOP, EXIT, SUB, FUNCTION, LOCAL, ON TIMER, devices");
    pass &= check(contains(out, "ST= 3"), "/dev/io starts with outputs off (not armed) until ESTOP 0");
    pass &= check(contains(out, "H1= 100") && contains(out, "H2= 0"),
                  "/dev/io keeps its state in the kernel; outputs stop when a program stops with an error");
    pass &= check(contains(out, "日本語 ｶﾅ OK"), "Shift_JIS program LOAD");
    pass &= check(contains(out, " 8 字 138"), "strings kept in Shift_JIS like N88 (LEN, MID$, ASC on kanji)");
    pass &= check(contains(out, "Division by zero") && contains(out, "File write protected"),
                  "error messages (division by zero, SAVE to the read-only disk)");
    if (virtio_snd_present()) {
        /* PLAY 1 秒（22050Hz・16 ビットで 44100 バイト）と BEEP 0.11 秒。装置が鳴らし終えて全部返している */
        unsigned long bytes = rv_snd_bytes - snd0, per = rv_snd_periods_done - per0;
        pass &= check(bytes >= 44100 + 4800 && bytes <= 44100 + 6000 && per * 2048 >= bytes
                      && rv_snd_inflight() == 0, "PLAY (2 voices) and BEEP played through /dev/dsp (virtio-sound)");
    } else {
        uart_puts("[M28] --   no sound device: PLAY waits without sound\r\n");
    }
    pass &= check(contains(out, "POINT(470,200) = 5  POINT(470,320) = 6"), "graphics: PAINT, CIRCLE and POINT (/bas/GRAPH.BAS)");
    if (virtio_gpu_present()) {
        /* GRAPH.BAS の絵が画面（virtio-gpu）の入れ物に出ている: 色見本・塗った楕円の中・円の下端・枠 */
        pass &= check(rv_fb_pixel(10, 10) == 0x000000 && rv_fb_pixel(50, 10) == 0x0000ff
                      && rv_fb_pixel(90, 10) == 0xff0000 && rv_fb_pixel(610, 10) == 0xbbbbbb
                      && rv_fb_pixel(470, 200) == 0x00ffff && rv_fb_pixel(470, 320) == 0xffff00
                      && rv_fb_pixel(0, 200) == 0xffffff && rv_gpu_errors == 0,
                      "picture shown on /dev/fb0 (virtio-gpu): colors, painted ellipse, circle, box");
        {
            /* 文字の面が絵の上に重なっている（GRAPH.BAS は下の行に PRINT し、そのあとの文字も下の行に流れる）。
             * 箱の中の下の 2 行（線のない黒い所）に白い文字の点がある */
            unsigned x, y, lit = 0;
            for (y = 368; y < 399; y++) {
                for (x = 1; x < 300; x++) {
                    lit += rv_fb_pixel(x, y) == 0xffffff;
                }
            }
            pass &= check(lit > 50, "text screen drawn over the graphics (N88: text plane on top of graphics)");
        }
    } else {
        uart_puts("[M28] --   no display device: graphics drawn in memory only\r\n");
    }
    pass &= check(r == 0, "SYSTEM exits with 0");
    pass &= check(rv_linux_proc_count() == 0 && rv_tmp_count() == 0 && rv_page_free_count() == free0
                  && rv_heap_in_use == heap0, "process table and /tmp empty, pages and kernel memory returned");
    uart_puts(pass ? "[M28] PASS\r\n" : "[M28] FAIL\r\n");
    return pass;
}
