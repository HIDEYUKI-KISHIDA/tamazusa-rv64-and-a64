/* fstest.c — Linux 互換層のファイルを、ふつうの C のやり方で確かめる（M20）
 *   すべて合えば「fstest: all N checks passed」と出して 0、だめなら失敗した番号で終わる
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

static int n_ok;

static int check(int ok, int no, const char *what)
{
    if (!ok) {
        printf("fstest: check %d failed: %s (errno=%d)\n", no, what, errno);
        exit(no);
    }
    n_ok++;
    return 1;
}

int main(void)
{
    char line[128], buf[16], cwd[64];
    struct stat st, st2;
    FILE *fp;
    DIR *dir;
    struct dirent *e;
    int fd, fd2, found_hello = 0, found_readme = 0, found_dot = 0, count = 0;

    /* 1. fopen / fgets */
    fp = fopen("/readme.txt", "r");
    check(fp != NULL, 1, "fopen /readme.txt");
    check(fgets(line, sizeof(line), fp) != NULL && strncmp(line, "B-Free disk test:", 17) == 0,
          2, "fgets first line");
    fclose(fp);

    /* 2. stat / fstat */
    check(stat("readme.txt", &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 60, 3, "stat readme.txt");
    fd = open("readme.txt", O_RDONLY);
    check(fd >= 3, 4, "open readme.txt");
    check(fstat(fd, &st2) == 0 && st2.st_size == st.st_size && st2.st_ino == st.st_ino, 5, "fstat");

    /* 3. lseek / read / pread */
    check(lseek(fd, 7, SEEK_SET) == 7 && read(fd, buf, 4) == 4 && memcmp(buf, "disk", 4) == 0,
          6, "lseek 7 + read 'disk'");
    check(lseek(fd, 0, SEEK_END) == st.st_size, 7, "lseek SEEK_END = size");
    check(pread(fd, buf, 6, 0) == 6 && memcmp(buf, "B-Free", 6) == 0, 8, "pread at 0");
    check(read(fd, buf, 4) == 0, 9, "read at the end -> 0");

    /* 4. dup */
    fd2 = dup(fd);
    check(fd2 > fd && lseek(fd2, 0, SEEK_SET) == 0 && read(fd2, buf, 6) == 6, 10, "dup");
    close(fd2);
    close(fd);
    check(read(fd, buf, 1) < 0 && errno == EBADF, 11, "read after close -> EBADF");

    /* 5. errors */
    check(open("nothing.txt", O_RDONLY) < 0 && errno == ENOENT, 12, "missing file -> ENOENT");
    check(open("readme.txt", O_WRONLY) < 0 && errno == EROFS, 13, "open for writing -> EROFS");
    check(open("/sub/x", O_RDONLY) < 0 && errno == ENOENT, 14, "no sub folders -> ENOENT");

    /* 6. folder */
    dir = opendir("/");
    check(dir != NULL, 15, "opendir /");
    while ((e = readdir(dir)) != NULL) {
        count++;
        if (strcmp(e->d_name, "hello_musl.elf") == 0) found_hello = 1;
        if (strcmp(e->d_name, "readme.txt") == 0) found_readme = 1;
        if (strcmp(e->d_name, ".") == 0) found_dot = 1;
    }
    closedir(dir);
    check(found_hello && found_readme && found_dot && count >= 6, 16, "readdir lists the disk");
    check(stat("/", &st) == 0 && S_ISDIR(st.st_mode), 17, "stat / is a folder");

    /* 7. cwd / access */
    check(getcwd(cwd, sizeof(cwd)) != NULL && strcmp(cwd, "/") == 0, 18, "getcwd = /");
    check(access("hello_musl.elf", X_OK) == 0 && access("readme.txt", W_OK) < 0, 19, "access");

    printf("fstest: all %d checks passed (%d entries in /)\n", n_ok, count);
    return 0;
}
