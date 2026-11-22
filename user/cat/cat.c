/* cat.c — ファイルの中身を表示する（シェルから「cat ファイル名」で起動）
 *   main(arg, args): args = ファイル名（シェルが渡す文字列の引数）
 *   成功 0、引数なし 1、開けない 2
 */
#include "ulib.h"

int main(long arg, const char *args)
{
    char buf[129];
    long fd, n;

    (void)arg;
    if (args == 0 || args[0] == 0) {
        u_puts("usage: cat FILE\r\n");
        return 1;
    }
    fd = u_open(args);
    if (fd < 0) {
        u_puts("cat: ");
        u_puts(args);
        u_puts(": cannot open\r\n");
        return 2;
    }
    while ((n = u_read(fd, buf, 128)) > 0) {
        buf[n] = 0;
        u_puts(buf);
    }
    u_close(fd);
    return 0;
}
