/* fsdemo.c — ファイルとプログラム起動のシステムコールを確かめるユーザープログラム（M14）
 *
 *   arg = 0: 次を順に確かめ、すべて合えば 77 で終わる（だめなら失敗した手順の番号）
 *     1  readme.txt を開き、10 バイトずつ読んで表示（オフセットが進む）。
 *        先頭が "B-Free disk test:"、合計が 60〜128 バイト（改行が CRLF に変わっても通る）
 *     2  終わりまで読んだあとの read は 0
 *     3  ないファイルを開く → UERR_NOENT
 *     4  不正な fd で read / close → UERR_BADF
 *     5  カーネルのアドレスや自分のコード（読み取り専用）へ read → UERR_FAULT（止まらない）
 *     6  開けるだけ開く → 9 個目で UERR_MFILE、閉じれば また開ける
 *     7  hello.elf を起動（引数 0）→ 子の終了コード 90
 *     8  ないプログラム → UERR_NOENT、ELF でないファイル（readme.txt）→ UERR_NOEXEC
 *   arg = 1 | (id << 8): hello.elf を引数 2 | (id << 8) で起動し、子の終了コードを返す
 *   arg = 2: readme.txt を開いたまま終わる（閉じ忘れても後始末されることの確認）→ 3
 */
#include "usys.h"

#define README_MAX  128

static char text[README_MAX + 1];

int main(long arg);

static void put_line(const char *s)
{
    u_puts(s);
}

static int step_read_readme(void)
{
    char chunk[11];
    long fd = u_open("readme.txt");
    long total = 0;
    long n;
    int i0;

    if (fd < 0) {
        return 0;
    }
    put_line("[fsdemo] readme.txt: ");
    for (;;) {
        long i;
        n = u_read(fd, chunk, 10);
        if (n <= 0) {
            break;
        }
        for (i = 0; i < n && total + i < README_MAX; i++) {
            text[total + i] = chunk[i];
        }
        chunk[n] = 0;
        u_puts(chunk);                  /* 10 バイトずつ続けて表示 */
        total += n;
    }
    if (n != 0) {
        return 0;
    }
    if (u_read(fd, chunk, 10) != 0) {   /* 2: 終わりのあとは 0 */
        return 0;
    }
    u_close(fd);
    {
        static const char want[] = "B-Free disk test:";
        for (i0 = 0; want[i0] != 0; i0++) {
            if (text[i0] != want[i0]) {
                return 0;
            }
        }
    }
    return total >= 60 && total <= README_MAX;
}

int main(long arg)
{
    long r, fd, i;
    long fds[9];

    if ((arg & 0xff) == 1) {
        return (int)u_spawn("hello.elf", 2 | ((arg >> 8) << 8));
    }
    if ((arg & 0xff) == 2) {
        u_open("readme.txt");
        return 3;
    }

    /* 1, 2 */
    if (!step_read_readme()) {
        return 1;
    }
    /* 3 */
    if (u_open("nothing.txt") != UERR_NOENT) {
        return 3;
    }
    /* 4 */
    if (u_read(5, text, 4) != UERR_BADF || u_close(-1) != UERR_BADF || u_read(100, text, 1) != UERR_BADF) {
        return 4;
    }
    /* 5 */
    fd = u_open("readme.txt");
    if (fd < 0) {
        return 5;
    }
    r = u_read(fd, (void *)0x80200000UL, 8);
    if (r != UERR_FAULT) {
        return 5;
    }
    r = u_read(fd, (void *)main, 8);
    if (r != UERR_FAULT) {
        return 5;
    }
    u_close(fd);
    /* 6 */
    for (i = 0; i < 9; i++) {
        fds[i] = u_open("readme.txt");
    }
    if (fds[7] < 0 || fds[8] != UERR_MFILE) {
        return 6;
    }
    u_close(fds[3]);
    if (u_open("readme.txt") != fds[3]) {
        return 6;
    }
    for (i = 0; i < 8; i++) {
        u_close(fds[i]);
    }
    /* 7 */
    put_line("\r\n[fsdemo] spawning hello.elf\r\n");
    r = u_spawn("hello.elf", 0);
    if (r != 90) {
        return 7;
    }
    /* 8 */
    if (u_spawn("nothing.elf", 0) != UERR_NOENT || u_spawn("readme.txt", 0) != UERR_NOEXEC) {
        return 8;
    }
    put_line("[fsdemo] all file/spawn checks done\r\n");
    return 77;
}
