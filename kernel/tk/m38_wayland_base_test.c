/* kernel/tk/m38_wayland_base_test.c — M38: Wayland の下回り（SMODE=1 かつ MMU=1）
 *
 *   user/linux/wltest.c（Linux の上でも同じ結果になる試験）を動かす。Wayland の合成役とアプリがすることを、
 *   同じシステムコールで小さくまねる:
 *   1. memfd_create・mmap(MAP_SHARED)・fork した子と同じページ、無名の MAP_SHARED
 *   2. AF_UNIX の bind・listen・connect・accept4、SO_PEERCRED
 *   3. sendmsg・recvmsg の SCM_RIGHTS で memfd を渡し、合成役が mmap して絵を読む。書いたものが相手にも見える
 *   4. epoll（待ち口・つながり・timerfd・eventfd）
 *   5. mremap で共有の写しを広げる
 *   6. /dev/input/event0・event1（evdev）: ioctl と、できごと（この試験が、プログラムの準備ができたら入れる）
 *   終わったら、memfd・ソケット・ページがすべて返っている。
 *
 * 合格の目印: [M38] PASS
 */
#include <stdint.h>
#include <stddef.h>
#include <tk/tkernel.h>
#include "../fs/tarfs.h"

extern void uart_puts(const char *s);
struct rv_uproc;
extern struct rv_uproc *rv_linux_load_path(const char *path, const char *const argv[],
                                           const char *const envp[], long *err);
extern long rv_linux_run_process(struct rv_uproc *p);
extern void rv_console_set_test_input(const char *s);
extern void rv_console_capture(int on);
extern const char *rv_console_captured(void);
extern int rv_linux_proc_count(void);
extern int rv_tmp_count(void);
extern int rv_usock_count(void);
extern int rv_page_free_count(void);
extern int rv_usb_kbd_on(void);
extern int rv_usb_ptr_on(void);
extern void rv_evdev_kbd(const uint8_t *r, const uint8_t *prev);
extern void rv_evdev_ptr(int x, int y, int buttons, int wheel);
extern volatile unsigned long rv_usock_fds_passed;

static int check(int ok, const char *what)
{
    uart_puts(ok ? "[M38] OK   " : "[M38] NG   ");
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

static volatile int inj_done, inj_stop;

/* プログラムが「input ready」と出したら、キー A を押して離し、位置 (100, 200) で左ボタンを押す */
static void injector(INT stacd, void *exinf)
{
    static const uint8_t none[8] = { 0 }, a_down[8] = { 0, 0, 4, 0, 0, 0, 0, 0 };
    int i;
    (void)stacd;
    (void)exinf;
    for (i = 0; i < 1500 && !inj_stop; i++) {           /* 最大 15 秒 */
        if (contains(rv_console_captured(), "WLTEST input ready")) {
            tk_dly_tsk(50);
            rv_evdev_kbd(a_down, none);
            rv_evdev_kbd(none, a_down);
            rv_evdev_ptr(100, 200, 1, 0);
            rv_evdev_ptr(100, 200, 0, 0);
            inj_done = 1;
            break;
        }
        tk_dly_tsk(10);
    }
    tk_ext_tsk();
}

int m38_run_wayland_base_test(void)
{
    static const char *const argv[] = { "wltest", 0 };
    static const char *const envp[] = { "HOME=/", "PATH=/bin:/", 0 };
    struct tarfs_file f;
    struct rv_uproc *p;
    const char *out;
    long r;
    int pass = 1, free0, tmp0, us0;
    T_CTSK ct;
    ID tid;

    uart_puts("\r\n[M38] Wayland base (shared memory, UNIX sockets, fd passing, epoll, evdev) test\r\n");
    if (tarfs_find("wltest.elf", &f) != 1) {
        uart_puts("[M38] SKIP (no wltest.elf)\r\n");
        return 1;
    }
    if (!rv_usb_kbd_on() || !rv_usb_ptr_on()) {
        uart_puts("[M38] SKIP (no USB keyboard and tablet)\r\n");
        return 1;
    }
    free0 = rv_page_free_count();
    tmp0 = rv_tmp_count();
    us0 = rv_usock_count();
    p = rv_linux_load_path("wltest.elf", argv, envp, &r);
    if (p == NULL) {
        uart_puts("[M38] load failed\r\n[M38] FAIL\r\n");
        return 0;
    }
    inj_done = inj_stop = 0;
    {
        unsigned char *q = (unsigned char *)&ct;
        unsigned k;
        for (k = 0; k < sizeof(ct); k++) {
            q[k] = 0;
        }
    }
    ct.tskatr = TA_HLNG | TA_RNG0;
    ct.itskpri = 8;
    ct.stksz = 2048;
    ct.task = (FP)injector;
    rv_console_set_test_input("");
    rv_console_capture(1);
    tid = tk_cre_tsk(&ct);
    if (tid > 0) {
        tk_sta_tsk(tid, 0);
    }
    r = rv_linux_run_process(p);
    inj_stop = 1;
    tk_dly_tsk(30);
    if (tid > 0) {
        tk_del_tsk(tid);                    /* 終わっていれば E_OBJ（そのまま） */
    }
    rv_console_capture(0);
    rv_console_set_test_input(0);
    out = rv_console_captured();

    pass &= check(contains(out, "WLTEST ok memfd + mmap MAP_SHARED") && contains(out, "WLTEST ok anonymous MAP_SHARED"),
                  "memfd_create + mmap(MAP_SHARED): shared with a forked child and with the file; anonymous MAP_SHARED");
    pass &= check(contains(out, "WLTEST ok AF_UNIX socket") && contains(out, "WLTEST ok accept4 via epoll"),
                  "AF_UNIX bind / listen / connect / accept4, SO_PEERCRED");
    pass &= check(contains(out, "WLTEST ok recvmsg: message and a file") && contains(out, "WLTEST ok the compositor maps")
                  && contains(out, "WLTEST ok the client saw") && rv_usock_fds_passed >= 1,
                  "SCM_RIGHTS: a memfd passed to the compositor, mapped there, writes seen on both sides");
    pass &= check(contains(out, "WLTEST ok epoll_wait wakes"), "epoll with timerfd and eventfd");
    pass &= check(contains(out, "WLTEST ok mremap grows"), "mremap (MREMAP_MAYMOVE) of a shared mapping");
    pass &= check(contains(out, "WLTEST ok evdev: open") && contains(out, "WLTEST ok evdev events") && inj_done,
                  "/dev/input/event0 and event1: ioctl, key and pointer events");
    pass &= check(r == 0 && contains(out, "WLTEST done, 0 failed") && rv_linux_proc_count() == 0
                  && rv_tmp_count() == tmp0 && rv_usock_count() == us0 && rv_page_free_count() == free0,
                  "exit 0; memfds, sockets and pages all returned");
    uart_puts(pass ? "[M38] PASS\r\n" : "[M38] FAIL\r\n");
    return pass;
}
