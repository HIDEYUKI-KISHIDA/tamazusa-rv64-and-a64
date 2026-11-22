/* a64_stubs.c — AArch64 版の途中の段階で、まだ移していない部分の空の関数
 *   uspace.c がプログラムを片づけるときに呼ぶ（ファイル・Linux 互換層は段階 A3・A4 で入る）。
 *   本物が入れば、そちらが使われる（weak） */
struct rv_uproc;
__attribute__((weak)) void rv_ufile_release(int slot) { (void)slot; }
__attribute__((weak)) void rv_linux_fs_release(int slot) { (void)slot; }
__attribute__((weak)) void rv_linux_proc_forget(struct rv_uproc *p) { (void)p; }

__attribute__((weak)) void desktop_raise(int which) { (void)which; }

/* ディスクがない構成（実機の最初の段階）: ネイティブのファイルのシステムコールは「ない」と答える。
 * 画面の出力は UART へ */
#include <stdint.h>
extern void uart_puts(const char *s);
extern int  rv_uart_getc_wait(void);
__attribute__((weak)) void rv_console_puts(const char *s) { uart_puts(s); }
__attribute__((weak)) int  rv_console_getc(void) { return rv_uart_getc_wait(); }
__attribute__((weak)) long rv_sys_open(uintptr_t upath) { (void)upath; return -1; }
__attribute__((weak)) long rv_sys_read(long fd, uintptr_t ubuf, uint64_t len) { (void)fd; (void)ubuf; (void)len; return -1; }
__attribute__((weak)) long rv_sys_close(long fd) { (void)fd; return -1; }
__attribute__((weak)) long rv_sys_spawn(uintptr_t upath, long arg, uintptr_t uargs) { (void)upath; (void)arg; (void)uargs; return -1; }
__attribute__((weak)) long rv_sys_dirent(long index, uintptr_t ubuf, uint64_t len) { (void)index; (void)ubuf; (void)len; return -1; }
