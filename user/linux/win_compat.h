/* win_compat.h — T88-BASIC を Windows（MinGW-w64）で作るための置き換え
 *
 *   basic.c は Linux（と Tamazusa の Linux 互換層）の呼び方で書いてある。Windows で作るときだけ、
 *   このファイルが端末・Ctrl+C・名前のあてはめなどを Windows の仕組みに置き換える。
 *     端末: コンソールの入力を 1 文字ずつ（行の編集・こだまを止める）。矢印などは VT の並び（ESC [ A …）で受ける
 *     文字: 出入りとも UTF-8（basic.c は画面との間で Shift_JIS ⇔ UTF-8 を直す）
 *     Ctrl+C: SetConsoleCtrlHandler で basic.c の on_sigint を呼ぶ
 *   /dev/fb0・/dev/dsp・/dev/mouse・/dev/io は開けないので、basic.c は「装置なし」として動く
 *   （画面と音は、この後の段階で窓と waveOut に置き換える）。
 */
#ifndef WIN_COMPAT_H
#define WIN_COMPAT_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <direct.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>        /* clock_gettime・nanosleep（winpthreads） */

/* ---- 端末（termios の代わり） ---- */
#define ICANON  0x0002
#define ECHO    0x0008
#define VMIN    6
#define VTIME   5
#define TCSANOW 0
typedef unsigned int tcflag_t;
struct termios {
    tcflag_t c_lflag;
    unsigned char c_cc[32];
};

static int win_in_console;      /* 標準入力がコンソールか（パイプ・ファイルでなく） */
static DWORD win_in_mode0;      /* はじめのコンソールの入力の設定 */

static int tcgetattr(int fd, struct termios *t)
{
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    (void)fd;
    if (!GetConsoleMode(h, &win_in_mode0)) {
        return -1;
    }
    win_in_console = 1;
    memset(t, 0, sizeof(*t));
    t->c_lflag = ICANON | ECHO;
    return 0;
}

static int tcsetattr(int fd, int act, const struct termios *t)
{
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD m = win_in_mode0;
    (void)fd;
    (void)act;
    if (!win_in_console) {
        return -1;
    }
    if (!(t->c_lflag & ICANON)) {
        m &= ~(DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
        m |= ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_PROCESSED_INPUT;
    }
    return SetConsoleMode(h, m) ? 0 : -1;
}

/* 標準入力から読む（コンソールなら UTF-16 で受けて UTF-8 にする） */
static int win_read0(char *buf, size_t len)
{
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    WCHAR w[256];
    DWORD got = 0;
    int n;

    if (!win_in_console) {
        return _read(0, buf, (unsigned)len);
    }
    if (len < 4) {
        return 0;
    }
    if (!ReadConsoleW(h, w, (DWORD)((len / 4 < 256) ? len / 4 : 256), &got, NULL) || got == 0) {
        errno = EINTR;          /* Ctrl+C で止まったとき */
        return -1;
    }
    n = WideCharToMultiByte(CP_UTF8, 0, w, (int)got, buf, (int)len, NULL, NULL);
    return n;
}
#define read(fd, b, n) ((fd) == 0 ? win_read0((char *)(b), (n)) : _read((fd), (b), (unsigned)(n)))

/* ---- poll（標準入力に打たれたキーがあるか、だけ） ---- */
#define POLLIN 1
struct pollfd { int fd; short events, revents; };
static int poll(struct pollfd *p, unsigned n, int tmo)
{
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    INPUT_RECORD r[32];
    DWORD k = 0, i;
    (void)n;
    (void)tmo;
    p->revents = 0;
    if (!win_in_console) {
        return 0;
    }
    if (!PeekConsoleInputW(h, r, 32, &k)) {
        return 0;
    }
    for (i = 0; i < k; i++) {
        if (r[i].EventType == KEY_EVENT && r[i].Event.KeyEvent.bKeyDown &&
            (r[i].Event.KeyEvent.uChar.UnicodeChar != 0 ||
             (r[i].Event.KeyEvent.wVirtualKeyCode >= VK_PRIOR && r[i].Event.KeyEvent.wVirtualKeyCode <= VK_DOWN) ||
             (r[i].Event.KeyEvent.wVirtualKeyCode >= VK_F1 && r[i].Event.KeyEvent.wVirtualKeyCode <= VK_F12))) {
            p->revents = POLLIN;
            return 1;
        }
    }
    /* キーの文字にならない知らせ（窓の大きさ・Shift だけ など）は捨てる */
    if (k > 0) {
        ReadConsoleInputW(h, r, k, &k);
    }
    return 0;
}

/* ---- Ctrl+C（sigaction の代わり） ---- */
struct sigaction { void (*sa_handler)(int); int sa_flags; int sa_mask; };
#define sigemptyset(m) (*(m) = 0)
static void (*win_sigint_fn)(int);
static BOOL WINAPI win_ctrl_handler(DWORD ev)
{
    if ((ev == CTRL_C_EVENT || ev == CTRL_BREAK_EVENT) && win_sigint_fn) {
        win_sigint_fn(2);
        return TRUE;
    }
    return FALSE;
}
static int win_sigaction(int sig, const struct sigaction *sa, void *old)
{
    (void)sig;
    (void)old;
    win_sigint_fn = sa->sa_handler;
    SetConsoleCtrlHandler(win_ctrl_handler, TRUE);
    return 0;
}
#define sigaction(s, a, o) win_sigaction((s), (a), (o))

/* ---- 名前のあてはめ（* と ?。大文字・小文字は区別しない） ---- */
#define FNM_CASEFOLD 16
static int fnmatch(const char *pat, const char *s, int flags)
{
    (void)flags;
    if (*pat == 0) {
        return *s ? 1 : 0;
    }
    if (*pat == '*') {
        do {
            if (fnmatch(pat + 1, s, flags) == 0) {
                return 0;
            }
        } while (*s++);
        return 1;
    }
    if (*s == 0) {
        return 1;
    }
    if (*pat == '?' || tolower((unsigned char)*pat) == tolower((unsigned char)*s)) {
        return fnmatch(pat + 1, s + 1, flags);
    }
    return 1;
}

/* ---- そのほか ---- */
static inline int ioctl(int fd, unsigned long req, void *arg) { (void)fd; (void)req; (void)arg; return -1; }
#define mkdir(p, m) _mkdir(p)
/* 画面（/dev/fb0）・入出力（/dev/io）は開けないので、使われない */
static inline long pread(int fd, void *b, size_t n, long off) { (void)fd; (void)b; (void)n; (void)off; return -1; }
static inline long pwrite(int fd, const void *b, size_t n, long off) { (void)fd; (void)b; (void)n; (void)off; return -1; }

/* ---- ファイル名は UTF-8 で受け、Windows の UTF-16 の API で開く（日本語の名前のため） ---- */
static WCHAR *win_w(const char *u, WCHAR *w, int max)
{
    if (!MultiByteToWideChar(CP_UTF8, 0, u, -1, w, max)) {
        w[0] = 0;
    }
    return w;
}
static FILE *win_fopen(const char *p, const char *m)
{
    WCHAR wp[MAX_PATH * 2], wm[16];
    return _wfopen(win_w(p, wp, MAX_PATH * 2), win_w(m, wm, 16));
}
static int win_remove(const char *p) { WCHAR w[MAX_PATH * 2]; return _wremove(win_w(p, w, MAX_PATH * 2)); }
static int win_rename(const char *a, const char *b)
{
    WCHAR wa[MAX_PATH * 2], wb[MAX_PATH * 2];
    return _wrename(win_w(a, wa, MAX_PATH * 2), win_w(b, wb, MAX_PATH * 2));
}
static int win_access(const char *p, int mode) { WCHAR w[MAX_PATH * 2]; return _waccess(win_w(p, w, MAX_PATH * 2), mode & 6); }
static int win_stat(const char *p, struct stat *st)
{
    WCHAR w[MAX_PATH * 2];
    struct _stat64 s;
    if (_wstat64(win_w(p, w, MAX_PATH * 2), &s) != 0) {
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->st_mode = s.st_mode;
    st->st_size = (off_t)s.st_size;
    st->st_mtime = (time_t)s.st_mtime;
    return 0;
}
typedef struct { _WDIR *w; struct dirent e; } WIN_DIR;
static WIN_DIR *win_opendir(const char *p)
{
    WCHAR w[MAX_PATH * 2];
    WIN_DIR *d;
    _WDIR *wd = _wopendir(win_w(p, w, MAX_PATH * 2));
    if (!wd) {
        return NULL;
    }
    d = calloc(1, sizeof(*d));
    d->w = wd;
    return d;
}
static struct dirent *win_readdir(WIN_DIR *d)
{
    struct _wdirent *we = _wreaddir(d->w);
    if (!we) {
        return NULL;
    }
    WideCharToMultiByte(CP_UTF8, 0, we->d_name, -1, d->e.d_name, sizeof(d->e.d_name), NULL, NULL);
    return &d->e;
}
static int win_closedir(WIN_DIR *d) { _wclosedir(d->w); free(d); return 0; }
#define fopen(p, m)     win_fopen((p), (m))
#define remove(p)       win_remove(p)
#define rename(a, b)    win_rename((a), (b))
#define access(p, m)    win_access((p), (m))
#define stat(p, s)      win_stat((p), (s))
#define DIR             WIN_DIR
#define opendir(p)      win_opendir(p)
#define readdir(d)      win_readdir(d)
#define closedir(d)     win_closedir(d)

/* コンソールの出力を UTF-8 と VT の並び（色・カーソルの移動）にする。main の前に一度 */
static void win_console_setup(void)
{
    HANDLE ho = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD m;
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    if (GetConsoleMode(ho, &m)) {
        SetConsoleMode(ho, m | ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT);
    }
    _setmode(0, _O_BINARY);
    _setmode(1, _O_BINARY);
}

#endif
