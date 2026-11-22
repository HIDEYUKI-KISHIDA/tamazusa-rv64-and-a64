/*
 * usys_file.c — ユーザープログラム向けのファイルとプログラム起動のシステムコール
 *               （S-mode + MMU のみ）
 *
 *   SYS_OPEN (5)  a0 = パス              → fd（0 以上）/ 負のエラー
 *   SYS_READ (6)  a0 = fd, a1 = バッファ, a2 = 長さ → 読んだバイト数（終わりなら 0）/ 負のエラー
 *   SYS_CLOSE(7)  a0 = fd                → 0 / 負のエラー
 *   SYS_SPAWN(8)  a0 = パス, a1 = 引数, a2 = 文字列の引数（0 ならなし）
 *                                        → 子の終了コード / 負のエラー
 *                 子は main(引数, 文字列の引数) として受け取る（文字列は子のスタックの上端に写す）
 *   SYS_DIRENT(10) a0 = 番号, a1 = バッファ, a2 = 長さ → ファイルの大きさ（名前を写す）
 *                                        / UERR_NOENT（それ以上ない）
 *
 *   ファイルはディスクの tar（読み取り専用）。開いたファイルの表はプログラム
 *   （rv_uproc のスロット）ごとに持ち、プログラムが終わると全部閉じる。
 *
 *   SYS_SPAWN: ディスクの ELF を新しいユーザー空間に読み込み、新しいタスクで動かす。
 *   呼んだタスクは子が終わるまで眠り（tk_slp_tsk）、子のタスクは終わると
 *   親を起こす（tk_wup_tsk）。親は子のタスクを削除してから終了コードを返す。
 *   ELF を読み込む作業領域は 1 つなので、ミューテックスで 1 本ずつにする。
 */
#include <stdint.h>
#include <stddef.h>
#include <tk/tkernel.h>
#include <libstr.h>
#include "../../../fs/tarfs.h"

#define UERR_NOENT   (-2)       /* ファイルがない */
#define UERR_NOEXEC  (-8)       /* ELF として読み込めない */
#define UERR_BADF    (-9)       /* 不正な fd */
#define UERR_NOMEM   (-12)      /* 資源が足りない */
#define UERR_FAULT   (-14)      /* ユーザーのアドレスが不正 */
#define UERR_IO      (-5)       /* ディスクの読み込みに失敗 */
#define UERR_MFILE   (-24)      /* 開けるファイルの数を超えた */
#define UERR_NOTSUP  (-38)      /* プログラムの空間の外からは使えない */

#define RV_UPROC_MAX    32
#define UFD_MAX         8
#define READ_CHUNK      256
#define SPAWN_MAX       8
#define ELF_BUF_SIZE    (64 * 1024)

struct rv_uproc;
extern int  rv_uproc_current_slot(void);
extern struct rv_uproc *rv_uproc_load(const void *image, size_t len);
extern void rv_uproc_free(struct rv_uproc *p);
extern uint64_t  rv_uproc_satp(const struct rv_uproc *p);
extern uintptr_t rv_uproc_entry(const struct rv_uproc *p);
extern uintptr_t rv_uproc_sp(const struct rv_uproc *p);
extern size_t rv_user_copy_str(char *dst, uintptr_t uva, size_t max);
extern int rv_user_copy_out(uintptr_t uva, const void *src, size_t len);
extern long rv_run_user_in_space2(uint64_t satp, uintptr_t entry, uintptr_t user_sp, long arg, long arg2);
extern uintptr_t rv_uproc_push_string(struct rv_uproc *p, const char *str);

extern ER tk_slp_tsk(TMO tmout);
extern ER tk_wup_tsk(ID tskid);
extern ER tk_del_tsk(ID tskid);
extern ID tk_get_tid(void);

/* ---- 開いたファイルの表 ---- */

struct ufd {
    int used;
    struct tarfs_file f;
    uint64_t offset;
};
static struct ufd ufds[RV_UPROC_MAX][UFD_MAX];

void rv_ufile_release(int slot)
{
    int i;

    if (slot < 0 || slot >= RV_UPROC_MAX) {
        return;
    }
    for (i = 0; i < UFD_MAX; i++) {
        ufds[slot][i].used = 0;
    }
}

/* テスト用: そのスロットで開いているファイルの数 */
int rv_ufile_open_count(int slot)
{
    int i, n = 0;

    if (slot < 0 || slot >= RV_UPROC_MAX) {
        return 0;
    }
    for (i = 0; i < UFD_MAX; i++) {
        n += ufds[slot][i].used;
    }
    return n;
}

static int copy_path(char *dst, uintptr_t uva)
{
    size_t n = rv_user_copy_str(dst, uva, 101);
    return (n == 0) ? UERR_FAULT : 0;
}

long rv_sys_open(uintptr_t upath)
{
    int slot = rv_uproc_current_slot();
    char path[101];
    struct tarfs_file f;
    int i, r;

    if (slot < 0) {
        return UERR_NOTSUP;
    }
    if (copy_path(path, upath) != 0) {
        return UERR_FAULT;
    }
    r = tarfs_find(path, &f);
    if (r < 0) {
        return UERR_IO;
    }
    if (r == 0) {
        return UERR_NOENT;
    }
    for (i = 0; i < UFD_MAX; i++) {
        if (!ufds[slot][i].used) {
            ufds[slot][i].used = 1;
            ufds[slot][i].f = f;
            ufds[slot][i].offset = 0;
            return i;
        }
    }
    return UERR_MFILE;
}

static struct ufd *get_fd(long fd)
{
    int slot = rv_uproc_current_slot();

    if (slot < 0 || fd < 0 || fd >= UFD_MAX || !ufds[slot][fd].used) {
        return NULL;
    }
    return &ufds[slot][fd];
}

long rv_sys_read(long fd, uintptr_t ubuf, uint64_t len)
{
    struct ufd *u = get_fd(fd);
    uint8_t chunk[READ_CHUNK];
    uint64_t done = 0;

    if (u == NULL) {
        return UERR_BADF;
    }
    while (done < len) {
        uint64_t want = len - done;
        int64_t n;

        if (want > READ_CHUNK) {
            want = READ_CHUNK;
        }
        n = tarfs_read(&u->f, u->offset, chunk, want);
        if (n < 0) {
            return (done > 0) ? (long)done : UERR_IO;
        }
        if (n == 0) {
            break;                      /* ファイルの終わり */
        }
        if (rv_user_copy_out(ubuf + done, chunk, (size_t)n) != 0) {
            return (done > 0) ? (long)done : UERR_FAULT;
        }
        u->offset += (uint64_t)n;
        done += (uint64_t)n;
        if ((uint64_t)n < want) {
            break;
        }
    }
    return (long)done;
}

long rv_sys_close(long fd)
{
    struct ufd *u = get_fd(fd);

    if (u == NULL) {
        return UERR_BADF;
    }
    u->used = 0;
    return 0;
}

/* ---- プログラムの起動 ---- */

struct spawn_rec {
    int used;
    struct rv_uproc *proc;
    long arg;
    long arg2;
    long exit_code;
    ID parent;
};
static struct spawn_rec spawns[SPAWN_MAX];
static uint64_t elf_buf[ELF_BUF_SIZE / 8];      /* 8 バイト境界（ELF ローダの条件） */
static ID loader_mtx;

static void child_task(INT stacd, void *exinf)
{
    struct spawn_rec *s = &spawns[stacd];
    (void)exinf;

    s->exit_code = rv_run_user_in_space2(rv_uproc_satp(s->proc), rv_uproc_entry(s->proc),
                                         rv_uproc_sp(s->proc), s->arg, s->arg2);
    rv_uproc_free(s->proc);
    s->proc = NULL;
    tk_wup_tsk(s->parent);
    tk_ext_tsk();
}

static int loader_lock(void)
{
    if (loader_mtx <= 0) {
        T_CMTX cm;
        ID id;

        memset(&cm, 0, sizeof(cm));
        cm.mtxatr = TA_TFIFO;
        tk_dis_dsp();                   /* 作るのは 1 回だけ */
        if (loader_mtx <= 0) {
            id = tk_cre_mtx(&cm);
            loader_mtx = id;
        }
        tk_ena_dsp();
        if (loader_mtx <= 0) {
            return -1;
        }
    }
    return (tk_loc_mtx(loader_mtx, TMO_FEVR) == E_OK) ? 0 : -1;
}

struct rv_uproc *rv_load_program(const char *path, long *err)
{
    struct tarfs_file f;
    struct rv_uproc *p;
    int64_t n;
    int r;

    r = tarfs_find(path, &f);
    if (r <= 0) {
        *err = (r == 0) ? UERR_NOENT : UERR_IO;
        return NULL;
    }
    if (f.size > ELF_BUF_SIZE) {
        *err = UERR_NOMEM;
        return NULL;
    }
    if (loader_lock() != 0) {
        *err = UERR_NOMEM;
        return NULL;
    }
    n = tarfs_read_all(&f, elf_buf, ELF_BUF_SIZE);
    p = (n == (int64_t)f.size) ? rv_uproc_load(elf_buf, (size_t)n) : NULL;
    tk_unl_mtx(loader_mtx);
    if (p == NULL) {
        *err = (n == (int64_t)f.size) ? UERR_NOEXEC : UERR_IO;
    }
    return p;
}

long rv_sys_spawn(uintptr_t upath, long arg, uintptr_t uargs)
{
    char path[101];
    char args[129];
    uintptr_t args_va = 0;
    struct rv_uproc *p;
    struct spawn_rec *s = NULL;
    T_CTSK ctsk;
    ID tid;
    long err = 0;
    int i;

    if (rv_uproc_current_slot() < 0) {
        return UERR_NOTSUP;
    }
    if (copy_path(path, upath) != 0) {
        return UERR_FAULT;
    }
    if (uargs != 0) {
        rv_user_copy_str(args, uargs, sizeof(args));   /* 読めなければ空文字列 */
    }
    p = rv_load_program(path, &err);
    if (p == NULL) {
        return err;
    }
    if (uargs != 0) {
        args_va = rv_uproc_push_string(p, args);
        if (args_va == 0) {
            rv_uproc_free(p);
            return UERR_NOMEM;
        }
    }

    tk_dis_dsp();
    for (i = 0; i < SPAWN_MAX; i++) {
        if (!spawns[i].used) {
            s = &spawns[i];
            s->used = 1;
            break;
        }
    }
    tk_ena_dsp();
    if (s == NULL) {
        rv_uproc_free(p);
        return UERR_NOMEM;
    }
    s->proc = p;
    s->arg = arg;
    s->arg2 = (long)args_va;
    s->exit_code = 0;
    s->parent = tk_get_tid();

    memset(&ctsk, 0, sizeof(ctsk));
    ctsk.tskatr = TA_HLNG | TA_RNG0;
    ctsk.itskpri = 10;
    ctsk.stksz = 8192;
    ctsk.task = (FP)child_task;
    tid = tk_cre_tsk(&ctsk);
    if (tid <= 0 || tk_sta_tsk(tid, (INT)(s - spawns)) != E_OK) {
        if (tid > 0) {
            tk_del_tsk(tid);
        }
        rv_uproc_free(p);
        s->used = 0;
        return UERR_NOMEM;
    }

    tk_slp_tsk(TMO_FEVR);               /* 子が終わると起こされる */
    /* 子が休止状態（tk_ext_tsk の後）になるまで待ってから削除する */
    while (tk_del_tsk(tid) == E_OBJ) {
        tk_dly_tsk(1);
    }
    err = s->exit_code;
    s->used = 0;
    return err;
}

/* ディスクの index 番目のファイル名を ubuf に写し、大きさを返す */
long rv_sys_dirent(long index, uintptr_t ubuf, uint64_t len)
{
    struct tarfs_file f;
    size_t n = 0;
    int r;

    if (rv_uproc_current_slot() < 0) {
        return UERR_NOTSUP;
    }
    if (index < 0) {
        return UERR_NOENT;
    }
    r = tarfs_entry((int)index, &f);
    if (r < 0) {
        return UERR_IO;
    }
    if (r == 0) {
        return UERR_NOENT;
    }
    while (f.name[n] != 0) {
        n++;
    }
    if (len < n + 1 || rv_user_copy_out(ubuf, f.name, n + 1) != 0) {
        return UERR_FAULT;
    }
    return (long)f.size;
}
