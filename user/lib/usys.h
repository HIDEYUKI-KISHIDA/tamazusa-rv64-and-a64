/* usys.h — ユーザープログラム用のシステムコール
 *   RISC-V: ecall（a7 = 番号、a0〜a2 = 引数）／ AArch64: svc #0（x8 = 番号、x0〜x2 = 引数） */
#ifndef USYS_H
#define USYS_H

#define SYS_PUTS    1
#define SYS_EXIT    2
#define SYS_SLEEP   3
#define SYS_GETTID  4
#define SYS_OPEN    5
#define SYS_READ    6
#define SYS_CLOSE   7
#define SYS_SPAWN   8
#define SYS_GETC    9
#define SYS_DIRENT  10

/* エラー（負の値で返る） */
#define UERR_NOENT   (-2)
#define UERR_IO      (-5)
#define UERR_NOEXEC  (-8)
#define UERR_BADF    (-9)
#define UERR_NOMEM   (-12)
#define UERR_FAULT   (-14)
#define UERR_MFILE   (-24)

#if defined(__aarch64__)
static inline long usys_call1(long nr, long arg)
{
    register long r0 asm("x0") = arg;
    register long r8 asm("x8") = nr;
    asm volatile("svc #0" : "+r"(r0) : "r"(r8) : "memory");
    return r0;
}

static inline long usys_call3(long nr, long x0, long x1, long x2)
{
    register long r0 asm("x0") = x0;
    register long r1 asm("x1") = x1;
    register long r2 asm("x2") = x2;
    register long r8 asm("x8") = nr;
    asm volatile("svc #0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r8) : "memory");
    return r0;
}
#else
static inline long usys_call1(long nr, long arg)
{
    register long a0 asm("a0") = arg;
    register long a7 asm("a7") = nr;
    asm volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}

static inline long usys_call3(long nr, long x0, long x1, long x2)
{
    register long a0 asm("a0") = x0;
    register long a1 asm("a1") = x1;
    register long a2 asm("a2") = x2;
    register long a7 asm("a7") = nr;
    asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}
#endif

static inline long u_open(const char *path)                 { return usys_call1(SYS_OPEN, (long)path); }
static inline long u_read(long fd, void *buf, long len)     { return usys_call3(SYS_READ, fd, (long)buf, len); }
static inline long u_close(long fd)                         { return usys_call1(SYS_CLOSE, fd); }
static inline long u_spawn(const char *path, long arg)      { return usys_call3(SYS_SPAWN, (long)path, arg, 0); }
/* 文字列の引数付きで起動（子は main(arg, args) で受け取る） */
static inline long u_spawn_args(const char *path, long arg, const char *args)
{
    return usys_call3(SYS_SPAWN, (long)path, arg, (long)args);
}
static inline long u_getc(void)                             { return usys_call1(SYS_GETC, 0); }
/* ディスクの index 番目のファイル名を buf へ。戻り値は大きさ、それ以上なければ UERR_NOENT */
static inline long u_dirent(long index, char *buf, long len) { return usys_call3(SYS_DIRENT, index, (long)buf, len); }

static inline long u_puts(const char *s)   { return usys_call1(SYS_PUTS, (long)s); }
static inline long u_sleep(long ms)        { return usys_call1(SYS_SLEEP, ms); }
static inline long u_gettid(void)          { return usys_call1(SYS_GETTID, 0); }
static inline void u_exit(long code)
{
    usys_call1(SYS_EXIT, code);
    for (;;) { }
}

#endif
