/* sh.c — 小さなシェル
 *
 *   組み込みのコマンド:
 *     help          使い方
 *     ls            ディスクのファイル一覧
 *     echo 文字列   そのまま表示
 *     exit          シェルを終わる（終了コード 0）
 *   それ以外: 「名前 引数」→ ディスクの「名前.elf」を、引数を文字列として渡して起動し、
 *             終了コードが 0 でなければ「[exit N]」と表示する
 *
 *   行の編集: Backspace（0x08 / 0x7f）で 1 文字消す。Enter（CR / LF）で実行。
 *   入力の終わり（Ctrl-D = 4）が行の先頭で来たら終わる。
 */
#include "ulib.h"

#define LINE_MAX    128

static char line[LINE_MAX];

static long read_line(void)
{
    long n = 0;

    for (;;) {
        long c = u_getc();

        if (c == 4) {                   /* Ctrl-D */
            if (n == 0) {
                return -1;
            }
            continue;
        }
        if (c == '\r' || c == '\n') {
            u_puts("\r\n");
            line[n] = 0;
            return n;
        }
        if (c == 0x08 || c == 0x7f) {
            if (n > 0) {
                n--;
                u_puts("\b \b");        /* 画面上でも 1 文字消す */
            }
            continue;
        }
        if (c < 0x20 || c > 0x7e) {
            continue;                   /* その他の制御文字は無視 */
        }
        if (n < LINE_MAX - 1) {
            line[n++] = (char)c;
            u_putc((char)c);            /* エコー */
        }
    }
}

static char *skip_spaces(char *s)
{
    while (*s == ' ') {
        s++;
    }
    return s;
}

static void cmd_ls(void)
{
    char name[101];
    long i, sz;

    for (i = 0; (sz = u_dirent(i, name, sizeof(name))) >= 0; i++) {
        u_puts("  ");
        u_puts(name);
        u_puts("  ");
        u_put_num(sz);
        u_puts("\r\n");
    }
}

static void run_program(const char *cmd, const char *args)
{
    char path[64];
    long n = u_strlen(cmd);
    long r;

    if (n > 50) {
        u_puts("sh: name too long\r\n");
        return;
    }
    u_strcpy_n(path, cmd, sizeof(path));
    u_strcpy_n(path + n, ".elf", sizeof(path) - n);
    r = u_spawn_args(path, 0, args);
    if (r == UERR_NOENT) {
        u_puts(cmd);
        u_puts(": not found\r\n");
    } else if (r == UERR_NOEXEC) {
        u_puts(cmd);
        u_puts(": not a program\r\n");
    } else if (r != 0) {
        u_puts("[exit ");
        u_put_num(r);
        u_puts("]\r\n");
    }
}

int main(long arg, const char *args)
{
    (void)arg;
    (void)args;

    u_puts("Tamazusa shell. Type 'help' for commands.\r\n");
    for (;;) {
        char *cmd, *rest;
        long n;

        u_puts("tamazusa> ");
        n = read_line();
        if (n < 0) {
            u_puts("\r\n");
            return 0;
        }
        cmd = skip_spaces(line);
        if (*cmd == 0) {
            continue;
        }
        rest = cmd;
        while (*rest != 0 && *rest != ' ') {
            rest++;
        }
        if (*rest == ' ') {
            *rest++ = 0;
            rest = skip_spaces(rest);
        }

        if (u_strcmp(cmd, "exit") == 0) {
            return 0;
        } else if (u_strcmp(cmd, "help") == 0) {
            u_puts("commands: help, ls, echo TEXT, exit\r\n");
            u_puts("programs: NAME [ARGS] runs NAME.elf from the disk (e.g. hello, cat readme.txt)\r\n");
        } else if (u_strcmp(cmd, "ls") == 0) {
            cmd_ls();
        } else if (u_strcmp(cmd, "echo") == 0) {
            u_puts(rest);
            u_puts("\r\n");
        } else {
            run_program(cmd, rest);
        }
    }
}
