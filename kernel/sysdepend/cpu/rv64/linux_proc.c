/*
 * linux_proc.c — Linux 互換層のプロセス（S-mode + MMU のみ）
 *
 *   1 つのプロセス = 1 つの T-Kernel のタスク + 1 つのユーザー空間（rv_uproc）。
 *
 *   clone（fork）: 親のユーザー空間を丸ごと複製し（ページをすべて写す）、fd の表を写し、
 *     浮動小数点・ベクトルのレジスタを写して、新しいタスクで子を動かす。子は親の
 *     トラップフレームのレジスタのまま（a0 = 0 にして）U-mode へ戻る。親には子の pid を返す。
 *     CLONE_VM・CLONE_VFORK 付き（vfork・posix_spawn）も、複製として扱う。スレッド（CLONE_THREAD）はまだ。
 *   execve: ディスクの ELF を新しいユーザー空間に読み込み、pid と fd の表は引き継ぐ
 *     （「exec で閉じる」印の fd は閉じる）。トラップフレームを書き換えて、新しいプログラムの
 *     入口へ戻る。古い空間は返す。
 *   exit: 子のタスクは終了コードを残して「ゾンビ」になり、待っている親を起こす。
 *   wait4: 終わった子を探し、終了コード（Linux の形: コード << 8、強制終了は 11 = SIGSEGV）を返し、
 *     子のタスクを削除する。いなければ眠って待つ（WNOHANG なら 0）。
 *
 *   カーネル（テストやシェルの起動）からは rv_linux_run_process でプロセスを動かす。
 */
#include <stdint.h>
#include <stddef.h>
#include <tk/tkernel.h>
#include <libstr.h>
#include <rv_priv.h>
#include <mmu_arch.h>
#include "../../../fs/tarfs.h"

struct rv_uproc;
extern struct rv_uproc *rv_uproc_current(void);
extern struct rv_uproc *rv_uproc_fork(const struct rv_uproc *p);
extern struct rv_uproc *rv_uproc_load_linux(const void *image, size_t len,
                                            const char *const argv[], const char *const envp[]);
extern void rv_uproc_free(struct rv_uproc *p);
extern uint64_t  rv_uproc_satp(const struct rv_uproc *p);
extern uintptr_t rv_uproc_entry(const struct rv_uproc *p);
extern uintptr_t rv_uproc_sp(const struct rv_uproc *p);
extern int  rv_uproc_slot(const struct rv_uproc *p);
extern int  rv_uproc_pid(const struct rv_uproc *p);
extern void rv_uproc_set_pid(struct rv_uproc *p, int pid);
extern int  rv_uproc_next_pid(void);
extern int  rv_user_copy_in(void *dst, uintptr_t uva, size_t len);
extern int  rv_user_copy_out(uintptr_t uva, const void *src, size_t len);
extern size_t rv_user_copy_str(char *dst, uintptr_t uva, size_t max);
extern void rv_linux_fs_fork(int pslot, int cslot);
extern void rv_linux_fs_exec(int oslot, int nslot);
extern long rv_run_user_in_space(uint64_t satp, uintptr_t entry, uintptr_t user_sp, long arg);
extern long rv_run_user_frame_in_space(uint64_t satp, const uint64_t *frame);
extern void rv_set_current_space(uint64_t satp);
extern void rv_fpu_copy_current_to(void *tcb);
extern void *rv_tcb_of(ID tskid);
extern ER tk_slp_tsk(TMO tmout);
extern ER tk_wup_tsk(ID tskid);
extern ER tk_del_tsk(ID tskid);
extern ID tk_get_tid(void);
extern void uart_puts(const char *s);
extern void rv_tty_set_foreground(int pgid);
extern struct rv_uproc *rv_uproc_current(void);
extern int  rv_uproc_is_linux(const struct rv_uproc *p);

#define ECHILD  10
#define ENOMEM  12
#define EFAULT  14
#define ENOENT  2
#define ENOEXEC 8
#define E2BIG   7
#define ENOSYS  38
#define EINTR   4
#define EINVAL  22
#define ESRCH   3
#define EPERM   1
#define SIG_DFL 0
#define SIG_IGN 1

#define CLONE_VM        0x00000100
#define CLONE_THREAD    0x00010000
#define WNOHANG         1
#define SIGSEGV         11

#define LPROC_MAX   32
#define LINUX_PRI   10              /* Linux のプロセスのタスクの優先度（タイマで時分割する） */
enum { LP_FREE = 0, LP_RUN, LP_ZOMBIE };

struct lproc {
    int state;
    int adhoc;                      /* 表になかったので、システムコールの中で登録したもの */
    int pid;
    int ppid;
    int status;                     /* wait の形の終了状態 */
    struct rv_uproc *u;             /* 実行中のユーザー空間（execve で入れ替わる） */
    ID  task;                       /* 子を動かしているタスク（カーネルが動かすプロセスは 0） */
    ID  waiter;                     /* wait4 で眠っているタスク（0 = なし） */
    ID  runtid;                     /* いまこのプロセスを動かしているタスク（シグナルで起こす） */
    int pgid, sid;                  /* プロセスグループとセッション */
    int killsig;                    /* シグナルで終わったときの番号（0 = 普通の終了） */
    uint64_t frame[38];             /* fork の子の最初のレジスタ */
    /* シグナル（ビット n-1 がシグナル n） */
    volatile uint64_t sig_pending;
    uint64_t sig_blocked;
    uint64_t sig_saved;             /* rt_sigsuspend の前のマスク（sig_restore のとき使う） */
    int sig_restore;
    int sys_restartable;            /* 直前のシステムコールが -EINTR で、やり直せるもの */
    uint64_t sys_orig_a0;
    struct lsigact { uint64_t handler, flags, mask; } act[65];
    int ctty;                       /* 制御端末: -1 = コンソール、-2 = なし、0 以上 = 疑似端末の番号 */
    uint64_t alarm_at, alarm_iv;    /* setitimer(ITIMER_REAL): 次の時刻と間隔（マイクロ秒、0 = なし） */
    /* 利用者（uid・gid）。fork・execve で引き継ぐ。初めは root（0） */
    int uid, euid, suid, gid, egid, sgid;
    int ngroups;
    int groups[8];
    int net;                        /* ネットワークのソケットを開いた（このプロセスか親）。機器を動かせない（安全の決まり 6） */
    char comm[16];                  /* プログラムの名前（記録のため） */
};
static struct lproc procs[LPROC_MAX];
static int sig_deliverable(const struct lproc *lp);

volatile unsigned long rv_linux_forks, rv_linux_execs, rv_linux_reaped;   /* テスト用 */

static struct lproc *by_pid(int pid)
{
    int i;
    for (i = 0; i < LPROC_MAX; i++) {
        if (procs[i].state != LP_FREE && procs[i].pid == pid) {
            return &procs[i];
        }
    }
    return NULL;
}

static struct lproc *alloc_proc(void)
{
    int i;
    for (i = 0; i < LPROC_MAX; i++) {
        if (procs[i].state == LP_FREE) {
            memset(&procs[i], 0, sizeof(procs[i]));
            procs[i].ctty = -1;
            return &procs[i];
        }
    }
    return NULL;
}

/* 実行中のプロセス（表になければ、親なしとして登録する） */
static struct lproc *current_proc(struct rv_uproc *u)
{
    int i;
    struct lproc *lp;
    for (i = 0; i < LPROC_MAX; i++) {
        if (procs[i].state == LP_RUN && procs[i].u == u) {
            return &procs[i];
        }
    }
    lp = alloc_proc();
    if (lp != NULL) {
        lp->state = LP_RUN;
        lp->pid = rv_uproc_pid(u);
        lp->ppid = 0;
        lp->u = u;
        lp->task = 0;
        lp->adhoc = 1;
        lp->pgid = lp->sid = lp->pid;
        lp->runtid = tk_get_tid();
    }
    return lp;
}

/* ユーザー空間 u が返されるとき（uspace.c の rv_uproc_free から）: 表の参照を外す */
void rv_linux_proc_forget(struct rv_uproc *u)
{
    int i;
    for (i = 0; i < LPROC_MAX; i++) {
        if (procs[i].state == LP_RUN && procs[i].u == u) {
            if (procs[i].adhoc) {
                procs[i].state = LP_FREE;
            } else {
                procs[i].u = NULL;
            }
        }
    }
}

static int exit_status(const struct lproc *lp, long code)
{
    if (lp->killsig > 0) {
        return lp->killsig;         /* シグナルで終わった（wait の形: 下位 7 ビット） */
    }
    return (code < 0) ? SIGSEGV : (int)((code & 0xff) << 8);
}

static void post_signal(struct lproc *lp, int sig);

int rv_linux_getppid(struct rv_uproc *u)
{
    struct lproc *lp = current_proc(u);
    return lp ? lp->ppid : 0;
}

/* ---- fork ---- */

static void child_task(INT stacd, void *exinf)
{
    struct lproc *lp = &procs[stacd];
    struct lproc *parent;
    long code;
    (void)exinf;

    lp->runtid = tk_get_tid();
    code = rv_run_user_frame_in_space(rv_uproc_satp(lp->u), lp->frame);

    rv_uproc_free(lp->u);           /* 空間と fd を返す（パイプの相手もここで起きる） */
    tk_dis_dsp();                   /* T-Kernel のディスパッチ禁止は入れ子にできない: 上の解放の外で */
    lp->u = NULL;
    lp->status = exit_status(lp, code);
    lp->state = LP_ZOMBIE;
    lp->runtid = 0;
    parent = by_pid(lp->ppid);
    if (parent != NULL) {
        if (parent->waiter > 0) {
            ID w = parent->waiter;
            parent->waiter = 0;
            tk_wup_tsk(w);
        }
        post_signal(parent, 17);    /* SIGCHLD（既定では捨てられる） */
    }
    tk_ena_dsp();
    tk_ext_tsk();
}

long rv_linux_clone(struct rv_uproc *u, const uint64_t *f, unsigned long flags, uintptr_t newsp)
{
    struct lproc *parent = current_proc(u);
    struct lproc *lp;
    struct rv_uproc *cu;
    T_CTSK ctsk;
    ID tid;
    int pid;

    if (flags & CLONE_THREAD) {
        return -ENOSYS;             /* スレッドは後の段 */
    }
    if (parent == NULL || (lp = alloc_proc()) == NULL) {
        return -ENOMEM;
    }
    cu = rv_uproc_fork(u);
    if (cu == NULL) {
        return -ENOMEM;
    }
    pid = rv_uproc_next_pid();
    rv_uproc_set_pid(cu, pid);
    rv_linux_fs_fork(rv_uproc_slot(u), rv_uproc_slot(cu));

    lp->state = LP_RUN;
    lp->pid = pid;
    lp->ppid = parent->pid;
    lp->u = cu;
    lp->pgid = parent->pgid;
    lp->sid = parent->sid;
    lp->ctty = parent->ctty;
    lp->uid = parent->uid;
    lp->euid = parent->euid;
    lp->suid = parent->suid;
    lp->gid = parent->gid;
    lp->egid = parent->egid;
    lp->sgid = parent->sgid;
    lp->ngroups = parent->ngroups;
    memcpy(lp->groups, parent->groups, sizeof(lp->groups));
    lp->net = parent->net;
    memcpy(lp->comm, parent->comm, sizeof(lp->comm));
    lp->sig_blocked = parent->sig_blocked;
    memcpy(lp->act, parent->act, sizeof(lp->act));
    memcpy(lp->frame, f, TF_WORDS * sizeof(uint64_t));
    lp->frame[TF_A0] = 0;               /* 子の戻り値（a0）は 0 */
    if ((flags & CLONE_VM) && newsp != 0) {
        lp->frame[TF_SP] = newsp;       /* posix_spawn: 子は新しいスタックで（複製した空間の中） */
    } else if (newsp != 0) {
        lp->frame[TF_SP] = newsp;
    }

    memset(&ctsk, 0, sizeof(ctsk));
    ctsk.tskatr = TA_HLNG | TA_RNG0;
    ctsk.itskpri = LINUX_PRI;
    ctsk.stksz = 16384;
    ctsk.task = (FP)child_task;
    tid = tk_cre_tsk(&ctsk);
    if (tid <= 0) {
        rv_uproc_free(cu);
        lp->state = LP_FREE;
        return -ENOMEM;
    }
    lp->task = tid;
    rv_fpu_copy_current_to(rv_tcb_of(tid));   /* 親の浮動小数点・ベクトルのレジスタを子へ */
    if (tk_sta_tsk(tid, (INT)(lp - procs)) != E_OK) {
        tk_del_tsk(tid);
        rv_uproc_free(cu);
        lp->state = LP_FREE;
        return -ENOMEM;
    }
    rv_linux_forks++;
    return pid;
}

/* ---- wait4 ---- */

long rv_linux_wait4(struct rv_uproc *u, long pid, uintptr_t ustatus, long options)
{
    struct lproc *me = current_proc(u);
    int i;

    if (me == NULL) {
        return -ECHILD;
    }
    for (;;) {
        struct lproc *z = NULL;
        int have_child = 0;

        tk_dis_dsp();
        me->waiter = tk_get_tid();      /* 先に記録（子の終了と行き違っても起床要求が残る） */
        for (i = 0; i < LPROC_MAX; i++) {
            struct lproc *c = &procs[i];
            if (c->state == LP_FREE || c->ppid != me->pid || c == me) {
                continue;
            }
            if (pid > 0 && c->pid != pid) {
                continue;
            }
            have_child = 1;
            if (c->state == LP_ZOMBIE) {
                z = c;
                break;
            }
        }
        if (z != NULL || !have_child || (options & WNOHANG)) {
            me->waiter = 0;
        }
        tk_ena_dsp();

        if (z != NULL) {
            int cpid = z->pid;
            int32_t st = z->status;
            while (tk_del_tsk(z->task) == E_OBJ) {  /* 子のタスクが休止状態になるまで */
                tk_dly_tsk(1);
            }
            z->state = LP_FREE;
            rv_linux_reaped++;
            if (ustatus != 0 && rv_user_copy_out(ustatus, &st, sizeof(st)) != 0) {
                return -EFAULT;
            }
            return cpid;
        }
        if (!have_child) {
            return -ECHILD;
        }
        if (options & WNOHANG) {
            return 0;
        }
        tk_slp_tsk(TMO_FEVR);
        if (sig_deliverable(me)) {
            tk_dis_dsp();
            me->waiter = 0;
            tk_ena_dsp();
            return -EINTR;
        }
    }
}

/* ---- execve ---- */

#define EXEC_ELF_MAX    (4 * 1024 * 1024)   /* busybox・bash の全部入りが入る大きさ */
#define EXEC_ARGS_MAX   16
#define EXEC_STR_MAX    1024

static uint64_t exec_buf[EXEC_ELF_MAX / 8];     /* 読み込みの作業領域（1 本ずつ） */
static ID exec_mtx;

static int exec_lock(void)
{
    if (exec_mtx <= 0) {
        T_CMTX cm;
        memset(&cm, 0, sizeof(cm));
        cm.mtxatr = TA_TFIFO;
        tk_dis_dsp();
        if (exec_mtx <= 0) {
            exec_mtx = tk_cre_mtx(&cm);
        }
        tk_ena_dsp();
    }
    return (exec_mtx > 0 && tk_loc_mtx(exec_mtx, TMO_FEVR) == E_OK) ? 0 : -1;
}

/* ユーザーの文字列の配列（NULL で終わり）を、カーネルの領域へ写す */
static int copy_strv(uintptr_t uv, char *store, size_t room, const char **out, int max)
{
    size_t used = 0;
    int n = 0;
    if (uv == 0) {
        out[0] = NULL;
        return 0;
    }
    for (;;) {
        uint64_t ptr;
        size_t l;
        if (rv_user_copy_in(&ptr, uv + (uintptr_t)n * 8, 8) != 0) {
            return -EFAULT;
        }
        if (ptr == 0) {
            break;
        }
        if (n >= max - 1 || used >= room - 1) {
            return -E2BIG;
        }
        l = rv_user_copy_str(store + used, (uintptr_t)ptr, room - used);
        out[n++] = store + used;
        used += l + 1;
    }
    out[n] = NULL;
    return n;
}

/* パス → ディスクのファイル名（"/bin/ls" → "ls"、見つからなければ ".elf" を足して探す） */
extern int rv_linux_resolve(const char *path, struct tarfs_file *f, int64_t *ino);

/* プログラムを探す（linux_fs.c と同じ規則: "bash" → bash.elf、"/bin/ls" → busybox.elf） */
static int find_program(const char *path, struct tarfs_file *f)
{
    int64_t ino;
    return rv_linux_resolve(path, f, &ino) == 1;
}

/*
 * exec_mtx を持った状態で、ディスクのプログラムを新しい空間に読み込む。
 * 先頭が「#!」のファイル（スクリプト）なら、その行の解釈プログラムを読み込み、
 * argv を「解釈プログラム [引数] スクリプトのパス 元の argv[1..]」にする。
 */
extern int rv_linux_exec_read(int slot, const char *path, char *buf, size_t max, int64_t *size, char *npath);
static int exec_slot = -1;              /* いまのディレクトリを使うスロット（execve のとき）。−1 = 「/」 */

static struct rv_uproc *load_locked(const char *path, const char *const argv[],
                                    const char *const envp[], long *err)
{
    static char interp[128], iarg[128], spath[128], npath[256];
    int64_t elf_size = 0;
    static const char *nargv[EXEC_ARGS_MAX + 3];
    struct tarfs_file tf;
    struct rv_uproc *nu;
    int depth;

    for (depth = 0; ; depth++) {
        const uint8_t *b = (const uint8_t *)exec_buf;
        int64_t n;
        int dk = rv_linux_exec_read(exec_slot, path, (char *)exec_buf, EXEC_ELF_MAX, &n, npath);
        if (dk < 0) {
            *err = dk;
            return NULL;
        }
        if (dk == 1) {                  /* /data（書き込めるディスク）のプログラム */
            elf_size = n;
            goto loaded;
        }
        path = npath;                   /* いまのディレクトリからの相対を、絶対のパスにしたもの */
        if (!find_program(path, &tf)) {
            /* bash がディスクにないときは busybox の sh で代わりに動かす
             * （/etc/passwd の root のシェルは /bin/bash。sshd のログインがそれで止まらないように） */
            const char *base = path, *q;
            for (q = path; *q; q++) {
                if (*q == '/') {
                    base = q + 1;
                }
            }
            if (base[0] == 'b' && base[1] == 'a' && base[2] == 's' && base[3] == 'h' && base[4] == 0
                && argv[0] != 0 && find_program("busybox.elf", &tf)) {
                int a, j = 0;
                nargv[j++] = (argv[0][0] == '-') ? "-sh" : "sh";      /* ログインシェルの印「-」は残す */
                for (a = 1; argv[a] != 0 && j < EXEC_ARGS_MAX + 2; a++) {
                    nargv[j++] = argv[a];
                }
                nargv[j] = 0;
                argv = nargv;
                path = "busybox.elf";
                continue;
            }
            *err = -ENOENT;
            return NULL;
        }
        if (tf.size > EXEC_ELF_MAX || (n = tarfs_read_all(&tf, exec_buf, EXEC_ELF_MAX)) != (int64_t)tf.size) {
            *err = -ENOMEM;
            return NULL;
        }
        elf_size = n;
loaded:
        if (n >= 2 && b[0] == '#' && b[1] == '!' && depth == 0) {
            int i = 2, k, j = 0, a = 0;
            while (i < n && (b[i] == ' ' || b[i] == '\t')) i++;
            for (k = 0; i < n && b[i] != ' ' && b[i] != '\t' && b[i] != '\n' && k < 127; i++) interp[k++] = (char)b[i];
            interp[k] = 0;
            while (i < n && (b[i] == ' ' || b[i] == '\t')) i++;
            for (k = 0; i < n && b[i] != '\n' && k < 127; i++) iarg[k++] = (char)b[i];
            while (k > 0 && (iarg[k - 1] == ' ' || iarg[k - 1] == '\t' || iarg[k - 1] == '\r')) k--;
            iarg[k] = 0;
            if (interp[0] == 0) {
                *err = -ENOEXEC;
                return NULL;
            }
            for (k = 0; path[k] && k < 127; k++) spath[k] = path[k];
            spath[k] = 0;
            nargv[j++] = interp;
            if (iarg[0]) nargv[j++] = iarg;
            nargv[j++] = spath;
            for (a = 1; argv[a] != 0 && j < EXEC_ARGS_MAX + 2; a++) nargv[j++] = argv[a];
            nargv[j] = 0;
            argv = nargv;
            path = interp;
            continue;
        }
        break;
    }
    nu = rv_uproc_load_linux(exec_buf, (size_t)elf_size, argv, envp);
    if (nu == NULL) {
        *err = -ENOEXEC;
    }
    return nu;
}

/*
 * カーネルから: ディスクのプログラムを Linux 用のプロセスの空間として読み込む
 * （rv_linux_run_process で動かす）。なければ NULL（*err に負のエラー）
 */
static char last_loaded[16];

struct rv_uproc *rv_linux_load_path(const char *path, const char *const argv[],
                                    const char *const envp[], long *err)
{
    struct rv_uproc *nu;

    if (exec_lock() != 0) {
        *err = -ENOMEM;
        return NULL;
    }
    nu = load_locked(path, argv, envp, err);
    tk_unl_mtx(exec_mtx);
    {
        int k;                          /* rv_linux_run_process で、プログラムの名前にする */
        const char *b = path, *q;
        for (q = path; *q; q++) {
            if (*q == '/') b = q + 1;
        }
        for (k = 0; k < 15 && b[k]; k++) last_loaded[k] = b[k];
        last_loaded[k] = 0;
        if (k > 4 && last_loaded[k - 4] == '.' && last_loaded[k - 3] == 'e' && last_loaded[k - 2] == 'l' && last_loaded[k - 1] == 'f') {
            last_loaded[k - 4] = 0;
        }
    }
    return nu;
}

/* パスの最後の名前を、プログラムの名前にする（15 文字まで） */
static void set_comm(struct lproc *lp, const char *path)
{
    const char *b = path, *q;
    int k;
    for (q = path; *q; q++) {
        if (*q == '/') {
            b = q + 1;
        }
    }
    for (k = 0; k < 15 && b[k]; k++) {
        lp->comm[k] = b[k];
    }
    lp->comm[k] = 0;
    if (k > 4 && lp->comm[k - 4] == '.' && lp->comm[k - 3] == 'e' && lp->comm[k - 2] == 'l' && lp->comm[k - 1] == 'f') {
        lp->comm[k - 4] = 0;            /* ディスクのプログラムの「.elf」は名前に含めない */
    }
}

long rv_linux_execve(struct rv_uproc *u, uint64_t *f, uintptr_t upath, uintptr_t uargv, uintptr_t uenvp)
{
    static char argstore[EXEC_STR_MAX], envstore[EXEC_STR_MAX];
    const char *argv[EXEC_ARGS_MAX], *envp[EXEC_ARGS_MAX];
    char path[128];
    struct rv_uproc *nu;
    struct lproc *me = current_proc(u);
    int i, pid;
    long r;

    if (me == NULL || rv_user_copy_str(path, upath, sizeof(path)) == 0) {
        return -EFAULT;
    }
    if (exec_lock() != 0) {
        return -ENOMEM;
    }
    r = copy_strv(uargv, argstore, sizeof(argstore), argv, EXEC_ARGS_MAX);
    if (r >= 0) {
        r = copy_strv(uenvp, envstore, sizeof(envstore), envp, EXEC_ARGS_MAX);
    }
    if (r < 0) {
        tk_unl_mtx(exec_mtx);
        return r;
    }
    exec_slot = rv_uproc_slot(u);
    nu = load_locked(path, argv, envp, &r);
    exec_slot = -1;
    tk_unl_mtx(exec_mtx);
    if (nu == NULL) {
        return r;
    }

    /* ここから先は戻れない: pid と fd を引き継ぎ、空間を入れ替える */
    set_comm(me, path);
    pid = rv_uproc_pid(u);
    rv_uproc_set_pid(nu, pid);
    rv_linux_fs_exec(rv_uproc_slot(u), rv_uproc_slot(nu));
    me->u = nu;
    for (i = 1; i <= 64; i++) {      /* 処理関数は新しいプログラムにないので既定に戻す（無視はそのまま） */
        if (me->act[i].handler != SIG_IGN) {
            me->act[i].handler = SIG_DFL;
            me->act[i].flags = 0;
            me->act[i].mask = 0;
        }
    }
    rv_set_current_space(rv_uproc_satp(nu));
    rv_uproc_free(u);

    for (i = 0; i < 31; i++) {
        f[i] = 0;                   /* x1〜x31 を 0 に */
    }
    f[TF_SP] = rv_uproc_sp(nu);         /* sp */
    f[TF_PC] = rv_uproc_entry(nu);     /* 入口 */
    rv_linux_execs++;
    return 0;                       /* f[9]（a0）= 0 で新しいプログラムが始まる */
}

/* ---- カーネルからプロセスを動かす ---- */

/*
 * p をプロセスとして動かし、終わるまで待って終了コードを返す（強制終了は -1）。
 * execve で空間が入れ替わっても、最後に使っていた空間を返す。
 * 子が残っていれば、親なし（ppid = 0）にする。
 */
long rv_linux_run_process(struct rv_uproc *p)
{
    struct lproc *lp = alloc_proc();
    long code;
    int i;

    if (lp == NULL) {
        rv_uproc_free(p);
        return -ENOMEM;
    }
    lp->state = LP_RUN;
    lp->pid = rv_uproc_pid(p);
    lp->ppid = 0;
    lp->u = p;
    lp->task = 0;
    lp->pgid = lp->sid = lp->pid;
    lp->runtid = tk_get_tid();
    memcpy(lp->comm, last_loaded, sizeof(lp->comm));
    rv_tty_set_foreground(lp->pgid);    /* 端末の Ctrl-C はこのプロセスのグループへ */
    {
        /* 子のプロセスと同じ優先度で動かす（時分割で交互に動けるように） */
        T_RTSK rt;
        PRI old = (tk_ref_tsk(TSK_SELF, &rt) == E_OK) ? rt.tskpri : 0;
        if (old > 0) {
            tk_chg_pri(TSK_SELF, LINUX_PRI);
        }
        code = rv_run_user_in_space(rv_uproc_satp(p), rv_uproc_entry(p), rv_uproc_sp(p), 0);
        if (old > 0) {
            tk_chg_pri(TSK_SELF, old);
        }
    }
    rv_tty_set_foreground(0);
    if (lp->killsig > 0) {
        code = 128 + lp->killsig;
    }
    rv_uproc_free(lp->u);
    for (i = 0; i < LPROC_MAX; i++) {
        if (procs[i].state != LP_FREE && procs[i].ppid == lp->pid) {
            procs[i].ppid = 0;
        }
    }
    lp->state = LP_FREE;
    return code;
}

/* テスト用: 表に残っているプロセスの数 */
int rv_linux_proc_count(void)
{
    int i, n = 0;
    for (i = 0; i < LPROC_MAX; i++) {
        n += procs[i].state != LP_FREE;
    }
    return n;
}

/* ==================================================================
 * シグナル
 *
 *   保留（sig_pending）にビットを立て、そのプロセスを動かしているタスクを起こす。
 *   眠っているシステムコール（端末・パイプ・wait4・nanosleep・ppoll・rt_sigsuspend）は
 *   起きて -EINTR を返す。ユーザーへ戻る直前（exception.S から rv_signal_user_return）に
 *   保留を見て、
 *     既定の動作が「終了」なら、プロセスを終わらせる（wait の状態 = シグナルの番号）
 *     処理関数があれば、ユーザーのスタックに Linux の形の rt_sigframe（siginfo + ucontext）
 *     を積み、処理関数へ飛ぶ。戻り先は信号用の小さなコード（li a7,139; ecall）で、
 *     rt_sigreturn がレジスタとマスクを戻す。
 *   無視されるシグナル（SIG_IGN、既定で無視の SIGCHLD・SIGWINCH・SIGURG）は送った時点で捨てる。
 *   停止（SIGSTOP・SIGTSTP・SIGTTIN・SIGTTOU）と再開（SIGCONT）はまだ: 無視として扱う。
 *   SA_RESTART の処理関数なら、-EINTR になった読み書き・wait4 をやり直す。
 * ================================================================== */

#define SIGKILL 9
#define SIGSTOP 19
#define SIGCHLD 17
#define SA_SIGINFO   0x00000004UL
#define SA_ONSTACK   0x08000000UL
#define SA_RESTART   0x10000000UL
#define SA_NODEFER   0x40000000UL
#define SA_RESETHAND 0x80000000UL
#define SIGBIT(n)    (1ULL << ((n) - 1))
#define UNBLOCKABLE  (SIGBIT(SIGKILL) | SIGBIT(SIGSTOP))
#define SIGTRAMP_VA  UVA_SIGTRAMP           /* uspace.c が rt_sigreturn のコードを置くページ（<mmu_arch.h>） */

volatile unsigned long rv_linux_signals_sent, rv_linux_signals_handled;   /* テスト用 */

enum { DEF_TERM, DEF_IGN };

static int default_action(int sig)
{
    switch (sig) {
    case 17: case 23: case 28:          /* SIGCHLD SIGURG SIGWINCH */
    case 18:                            /* SIGCONT */
    case 19: case 20: case 21: case 22: /* 停止（まだ）: 無視 */
        return DEF_IGN;
    default:
        return DEF_TERM;
    }
}

static int ignored(const struct lproc *lp, int sig)
{
    if (sig == SIGKILL) {
        return 0;
    }
    if (lp->act[sig].handler == SIG_IGN) {
        return 1;
    }
    return lp->act[sig].handler == SIG_DFL && default_action(sig) == DEF_IGN;
}

/* 割込み処理の中からも呼べる（表の書き換えは保留のビットだけ、起こすのは tk_wup_tsk） */
static void post_signal(struct lproc *lp, int sig)
{
    if (lp == NULL || lp->state != LP_RUN || sig < 1 || sig > 64 || ignored(lp, sig)) {
        return;
    }
    __atomic_fetch_or(&lp->sig_pending, SIGBIT(sig), __ATOMIC_SEQ_CST);
    rv_linux_signals_sent++;
    if (lp->runtid > 0) {
        tk_wup_tsk(lp->runtid);         /* 眠っていれば起きて -EINTR、走っていなければ起床要求が残るだけ */
    }
}

static int sig_deliverable(const struct lproc *lp)
{
    return lp != NULL && (lp->sig_pending & ~(lp->sig_blocked & ~UNBLOCKABLE)) != 0;
}

/* 表を見るだけ（なければ登録しない） */
static struct lproc *find_proc(struct rv_uproc *u)
{
    int i;
    if (u == NULL) {
        return NULL;
    }
    for (i = 0; i < LPROC_MAX; i++) {
        if (procs[i].state == LP_RUN && procs[i].u == u) {
            return &procs[i];
        }
    }
    return NULL;
}

/* 眠るシステムコールから: 実行中のプロセスに届けるシグナルがあれば 1（-EINTR で戻る） */
int rv_linux_sig_interrupt(void)
{
    struct rv_uproc *u = rv_uproc_current();
    return rv_uproc_is_linux(u) && sig_deliverable(find_proc(u));
}

/* プロセスグループ pgid の全員へ（端末の Ctrl-C から: 割込み処理の中） */
void rv_linux_signal_pgrp(int pgid, int sig)
{
    int i;
    for (i = 0; i < LPROC_MAX; i++) {
        if (procs[i].state == LP_RUN && procs[i].pgid == pgid) {
            post_signal(&procs[i], sig);
        }
    }
}

/* 実行中のプロセス自身へ（EPIPE の SIGPIPE など） */
void rv_linux_signal_self(int sig)
{
    post_signal(find_proc(rv_uproc_current()), sig);
}

long rv_linux_kill(struct rv_uproc *u, long pid, long sig)
{
    struct lproc *me = current_proc(u);
    int i, hit = 0;

    if (sig < 0 || sig > 64) {
        return -EINVAL;
    }
    if (me == NULL) {
        return -ESRCH;
    }
    for (i = 0; i < LPROC_MAX; i++) {
        struct lproc *t = &procs[i];
        int match;
        if (t->state == LP_FREE) {
            continue;
        }
        if (pid > 0) {
            match = (t->pid == pid);
        } else if (pid == 0) {
            match = (t->pgid == me->pgid);
        } else if (pid == -1) {
            match = (t != me && t->ppid != 0);
        } else {
            match = (t->pgid == -pid);
        }
        if (!match) {
            continue;
        }
        hit = 1;
        if (sig != 0) {
            post_signal(t, (int)sig);   /* ゾンビは何もしない */
        }
    }
    return hit ? 0 : -ESRCH;
}

long rv_linux_sigaction(struct rv_uproc *u, long sig, uintptr_t uact, uintptr_t uoact, long size)
{
    struct lproc *me = current_proc(u);
    struct lsigact na;

    if (me == NULL || sig < 1 || sig > 64 || size != 8) {
        return -EINVAL;
    }
    if (uoact != 0 && rv_user_copy_out(uoact, &me->act[sig], sizeof(struct lsigact)) != 0) {
        return -EFAULT;
    }
    if (uact != 0) {
        if (rv_user_copy_in(&na, uact, sizeof(na)) != 0) {
            return -EFAULT;
        }
        if (sig == SIGKILL || sig == SIGSTOP) {
            return -EINVAL;
        }
        me->act[sig] = na;
        if (ignored(me, (int)sig)) {    /* 無視にしたら、保留も捨てる */
            __atomic_fetch_and(&me->sig_pending, ~SIGBIT(sig), __ATOMIC_SEQ_CST);
        }
    }
    return 0;
}

long rv_linux_sigprocmask(struct rv_uproc *u, long how, uintptr_t uset, uintptr_t uoset, long size)
{
    struct lproc *me = current_proc(u);
    uint64_t set, old;

    if (me == NULL || size != 8) {
        return -EINVAL;
    }
    old = me->sig_blocked;
    if (uset != 0) {
        if (rv_user_copy_in(&set, uset, sizeof(set)) != 0) {
            return -EFAULT;
        }
        if (how == 0) {
            me->sig_blocked |= set;             /* SIG_BLOCK */
        } else if (how == 1) {
            me->sig_blocked &= ~set;            /* SIG_UNBLOCK */
        } else if (how == 2) {
            me->sig_blocked = set;              /* SIG_SETMASK */
        } else {
            return -EINVAL;
        }
        me->sig_blocked &= ~UNBLOCKABLE;
    }
    if (uoset != 0 && rv_user_copy_out(uoset, &old, sizeof(old)) != 0) {
        return -EFAULT;
    }
    return 0;
}

long rv_linux_sigpending(struct rv_uproc *u, uintptr_t uset)
{
    struct lproc *me = current_proc(u);
    uint64_t set = me ? (me->sig_pending & me->sig_blocked) : 0;
    return rv_user_copy_out(uset, &set, sizeof(set)) == 0 ? 0 : -EFAULT;
}

/* マスクを入れ替えて、届けるシグナルが来るまで眠る。必ず -EINTR */
long rv_linux_sigsuspend(struct rv_uproc *u, uintptr_t umask, long size)
{
    struct lproc *me = current_proc(u);
    uint64_t m;

    if (me == NULL || size != 8) {
        return -EINVAL;
    }
    if (rv_user_copy_in(&m, umask, sizeof(m)) != 0) {
        return -EFAULT;
    }
    me->sig_saved = me->sig_blocked;
    me->sig_restore = 1;
    me->sig_blocked = m & ~UNBLOCKABLE;
    while (!sig_deliverable(me)) {
        tk_slp_tsk(TMO_FEVR);
    }
    return -EINTR;
}

/* ---- プロセスグループ・セッション ---- */

long rv_linux_setpgid(struct rv_uproc *u, long pid, long pgid)
{
    struct lproc *me = current_proc(u);
    struct lproc *t;

    if (me == NULL || pgid < 0) {
        return -EINVAL;
    }
    t = (pid == 0) ? me : by_pid((int)pid);
    if (t == NULL || (t != me && t->ppid != me->pid)) {
        return -ESRCH;
    }
    t->pgid = (pgid == 0) ? t->pid : (int)pgid;
    return 0;
}

long rv_linux_getpgid(struct rv_uproc *u, long pid)
{
    struct lproc *me = current_proc(u);
    struct lproc *t = (pid == 0) ? me : by_pid((int)pid);
    return t ? t->pgid : -ESRCH;
}

long rv_linux_getsid(struct rv_uproc *u, long pid)
{
    struct lproc *me = current_proc(u);
    struct lproc *t = (pid == 0) ? me : by_pid((int)pid);
    return t ? t->sid : -ESRCH;
}

long rv_linux_setsid(struct rv_uproc *u)
{
    struct lproc *me = current_proc(u);
    if (me == NULL) {
        return -EPERM;
    }
    if (me->pgid == me->pid) {
        return -EPERM;              /* すでにグループの長 */
    }
    me->sid = me->pgid = me->pid;
    me->ctty = -2;                  /* 新しいセッションは制御端末を持たない */
    return me->pid;
}

int rv_linux_my_pgid(struct rv_uproc *u)
{
    struct lproc *me = current_proc(u);
    return me ? me->pgid : 0;
}

/* ---- システムコールのやり直し（SA_RESTART）のための記録 ---- */

void rv_linux_sys_enter(struct rv_uproc *u, const uint64_t *f)
{
    struct lproc *me = find_proc(u);
    if (me != NULL) {
        me->sys_orig_a0 = f[TF_A0];
        me->sys_restartable = 0;
    }
}

void rv_linux_sys_leave(struct rv_uproc *u, long nr, long ret)
{
    struct lproc *me = find_proc(u);
    if (me != NULL && ret == -EINTR) {
        /* read readv write writev wait4 ioctl はやり直せる（nanosleep・ppoll・sigsuspend は不可） */
        me->sys_restartable = (nr == 63 || nr == 65 || nr == 64 || nr == 66 || nr == 260 || nr == 29);
    }
}

/* ---- 届ける ---- */

#if defined(__aarch64__)
/* Linux の AArch64 の形（uc_mcontext = sigcontext は 176 バイト目。__reserved に fpsimd_context） */
struct lnx_sigframe {
    uint8_t  info[128];
    uint64_t uc_flags;
    uint64_t uc_link;
    uint64_t ss_sp;
    uint32_t ss_flags, ss_pad;
    uint64_t ss_size;
    uint64_t uc_sigmask;
    uint8_t  unused[120];
    uint64_t fault_address;         /* sigcontext */
    uint64_t regs[31];              /* x0..x30 */
    uint64_t sp, pc, pstate;
    struct {                        /* fpsimd_context（magic 0x46508001、528 バイト） */
        uint32_t magic, size;
        uint32_t fpsr, fpcr;
        unsigned __int128 vregs[32];
    } fpsimd __attribute__((aligned(16)));
    uint32_t end_magic, end_size;   /* 終わりの印（0, 0） */
    uint64_t pad;
};

static void sf_save_regs(struct lnx_sigframe *sf, const uint64_t *f)
{
    int r;
    uint64_t v;
    for (r = 0; r < 31; r++) {
        sf->regs[r] = f[r];
    }
    sf->sp = f[TF_SP];
    sf->pc = f[TF_PC];
    sf->pstate = f[TF_STATUS];
    sf->fault_address = f[TF_TVAL];
    sf->fpsimd.magic = 0x46508001u;
    sf->fpsimd.size = 528;
    asm volatile(
        "stp q0, q1, [%0, #0]\n stp q2, q3, [%0, #32]\n stp q4, q5, [%0, #64]\n stp q6, q7, [%0, #96]\n"
        "stp q8, q9, [%0, #128]\n stp q10, q11, [%0, #160]\n stp q12, q13, [%0, #192]\n stp q14, q15, [%0, #224]\n"
        "stp q16, q17, [%0, #256]\n stp q18, q19, [%0, #288]\n stp q20, q21, [%0, #320]\n stp q22, q23, [%0, #352]\n"
        "stp q24, q25, [%0, #384]\n stp q26, q27, [%0, #416]\n stp q28, q29, [%0, #448]\n stp q30, q31, [%0, #480]\n"
        :: "r"(sf->fpsimd.vregs) : "memory");
    asm volatile("mrs %0, fpsr" : "=r"(v)); sf->fpsimd.fpsr = (uint32_t)v;
    asm volatile("mrs %0, fpcr" : "=r"(v)); sf->fpsimd.fpcr = (uint32_t)v;
}

static void sf_load_regs(const struct lnx_sigframe *sf, uint64_t *f)
{
    int r;
    for (r = 0; r < 31; r++) {
        f[r] = sf->regs[r];
    }
    f[TF_SP] = sf->sp;
    f[TF_PC] = sf->pc;
    /* pstate はユーザーの NZCV だけ戻す（EL0 のまま。割込みの禁止などは変えさせない） */
    f[TF_STATUS] = (f[TF_STATUS] & ~0xF0000000ull) | (sf->pstate & 0xF0000000ull);
    if (sf->fpsimd.magic == 0x46508001u) {
        asm volatile(
            "ldp q0, q1, [%0, #0]\n ldp q2, q3, [%0, #32]\n ldp q4, q5, [%0, #64]\n ldp q6, q7, [%0, #96]\n"
            "ldp q8, q9, [%0, #128]\n ldp q10, q11, [%0, #160]\n ldp q12, q13, [%0, #192]\n ldp q14, q15, [%0, #224]\n"
            "ldp q16, q17, [%0, #256]\n ldp q18, q19, [%0, #288]\n ldp q20, q21, [%0, #320]\n ldp q22, q23, [%0, #352]\n"
            "ldp q24, q25, [%0, #384]\n ldp q26, q27, [%0, #416]\n ldp q28, q29, [%0, #448]\n ldp q30, q31, [%0, #480]\n"
            :: "r"(sf->fpsimd.vregs) : "memory");
        asm volatile("msr fpsr, %0" :: "r"((uint64_t)sf->fpsimd.fpsr));
        asm volatile("msr fpcr, %0" :: "r"((uint64_t)sf->fpsimd.fpcr));
    }
}

#else
/* Linux の RISC-V の形（ucontext の uc_mcontext は 176 バイト目、16 バイト境界） */
struct lnx_sigframe {
    uint8_t  info[128];             /* siginfo: si_signo, si_errno, si_code, ... */
    uint64_t uc_flags;
    uint64_t uc_link;
    uint64_t ss_sp;
    uint32_t ss_flags, ss_pad;
    uint64_t ss_size;
    uint64_t uc_sigmask;
    uint8_t  unused[120];
    uint64_t pad16;                 /* 16 バイト境界へ */
    uint64_t regs[32];              /* pc, x1..x31 */
    uint64_t fpregs[64];            /* f0..f31（q 形の大きさ 528 バイトのうち先頭を使う） */
    uint32_t fcsr;
    uint32_t fpres[3];
};

static void save_fp(uint64_t *fr, uint32_t *fcsr)
{
    asm volatile(
        "fsd f0, 0(%0)\n fsd f1, 8(%0)\n fsd f2, 16(%0)\n fsd f3, 24(%0)\n"
        "fsd f4, 32(%0)\n fsd f5, 40(%0)\n fsd f6, 48(%0)\n fsd f7, 56(%0)\n"
        "fsd f8, 64(%0)\n fsd f9, 72(%0)\n fsd f10, 80(%0)\n fsd f11, 88(%0)\n"
        "fsd f12, 96(%0)\n fsd f13, 104(%0)\n fsd f14, 112(%0)\n fsd f15, 120(%0)\n"
        "fsd f16, 128(%0)\n fsd f17, 136(%0)\n fsd f18, 144(%0)\n fsd f19, 152(%0)\n"
        "fsd f20, 160(%0)\n fsd f21, 168(%0)\n fsd f22, 176(%0)\n fsd f23, 184(%0)\n"
        "fsd f24, 192(%0)\n fsd f25, 200(%0)\n fsd f26, 208(%0)\n fsd f27, 216(%0)\n"
        "fsd f28, 224(%0)\n fsd f29, 232(%0)\n fsd f30, 240(%0)\n fsd f31, 248(%0)\n"
        :: "r"(fr) : "memory");
    asm volatile("frcsr %0" : "=r"(*fcsr));
}

static void load_fp(const uint64_t *fr, uint32_t fcsr)
{
    asm volatile(
        "fld f0, 0(%0)\n fld f1, 8(%0)\n fld f2, 16(%0)\n fld f3, 24(%0)\n"
        "fld f4, 32(%0)\n fld f5, 40(%0)\n fld f6, 48(%0)\n fld f7, 56(%0)\n"
        "fld f8, 64(%0)\n fld f9, 72(%0)\n fld f10, 80(%0)\n fld f11, 88(%0)\n"
        "fld f12, 96(%0)\n fld f13, 104(%0)\n fld f14, 112(%0)\n fld f15, 120(%0)\n"
        "fld f16, 128(%0)\n fld f17, 136(%0)\n fld f18, 144(%0)\n fld f19, 152(%0)\n"
        "fld f20, 160(%0)\n fld f21, 168(%0)\n fld f22, 176(%0)\n fld f23, 184(%0)\n"
        "fld f24, 192(%0)\n fld f25, 200(%0)\n fld f26, 208(%0)\n fld f27, 216(%0)\n"
        "fld f28, 224(%0)\n fld f29, 232(%0)\n fld f30, 240(%0)\n fld f31, 248(%0)\n"
        :: "r"(fr) : "memory");
    asm volatile("fscsr %0" :: "r"(fcsr));
}

static void sf_save_regs(struct lnx_sigframe *sf, const uint64_t *f)
{
    int r;
    sf->regs[0] = f[TF_PC];
    for (r = 1; r < 32; r++) {
        sf->regs[r] = f[r - 1];
    }
    save_fp(sf->fpregs, &sf->fcsr);
}

static void sf_load_regs(const struct lnx_sigframe *sf, uint64_t *f)
{
    int r;
    f[TF_PC] = sf->regs[0];
    for (r = 1; r < 32; r++) {
        f[r - 1] = sf->regs[r];
    }
    load_fp(sf->fpregs, sf->fcsr);
}

#endif

/*
 * exception.S から: U-mode へ戻る直前（割込み禁止、f = このタスクのカーネルスタック上のフレーム）。
 * 戻り値: 0 = そのまま戻る / 2 = プロセスを終わらせる（f[9] に終了コード）
 */
int rv_signal_user_return(uint64_t *f)
{
    struct rv_uproc *u = rv_uproc_current();
    struct lproc *me;
    uint64_t ready;
    int sig;

    if (!rv_uproc_is_linux(u) || (me = find_proc(u)) == NULL) {
        return 0;
    }
    for (;;) {
        struct lsigact *a;
        struct lnx_sigframe sf;
        uintptr_t sp;

        ready = me->sig_pending & ~(me->sig_blocked & ~UNBLOCKABLE);
        if (ready == 0) {
            me->sys_restartable = 0;
            return 0;
        }
        for (sig = 1; !(ready & SIGBIT(sig)); sig++) {   /* 番号の小さい順（libgcc を使わない） */
        }
        if (ready & SIGBIT(SIGKILL)) {
            sig = SIGKILL;
        }
        __atomic_fetch_and(&me->sig_pending, ~SIGBIT(sig), __ATOMIC_SEQ_CST);
        a = &me->act[sig];
        if (ignored(me, sig)) {
            continue;
        }
        if (a->handler == SIG_DFL) {
            me->killsig = sig;          /* 既定の動作: 終了 */
            f[TF_A0] = (uint64_t)-1;
            return 2;
        }

        /* 処理関数へ。やり直せるシステムコールなら、ecall からやり直す形で保存する */
        if (me->sys_restartable && (a->flags & SA_RESTART)) {
            f[TF_A0] = me->sys_orig_a0;
            f[TF_PC] -= 4;
        }
        me->sys_restartable = 0;

        memset(&sf, 0, sizeof(sf));
        ((int32_t *)sf.info)[0] = sig;          /* si_signo */
        sf.ss_flags = 2;                        /* SS_DISABLE */
        sf.uc_sigmask = me->sig_restore ? me->sig_saved : me->sig_blocked;
        sf_save_regs(&sf, f);
        sp = (f[TF_SP] - sizeof(sf)) & ~(uintptr_t)15;
        if (rv_user_copy_out(sp, &sf, sizeof(sf)) != 0) {
            me->killsig = 11;                   /* スタックに積めない: SIGSEGV で終わる */
            f[TF_A0] = (uint64_t)-1;
            return 2;
        }
        me->sig_restore = 0;
        me->sig_blocked |= a->mask;
        if (!(a->flags & SA_NODEFER)) {
            me->sig_blocked |= SIGBIT(sig);
        }
        me->sig_blocked &= ~UNBLOCKABLE;
        f[TF_PC] = a->handler;
        f[TF_RA] = SIGTRAMP_VA;                     /* ra: 戻ると rt_sigreturn */
        f[TF_SP] = sp;
        f[TF_A0] = (uint64_t)sig;                   /* a0 */
        f[TF_A0 + 1] = sp;                             /* a1 = &siginfo */
        f[TF_A0 + 2] = sp + __builtin_offsetof(struct lnx_sigframe, uc_flags);   /* a2 = &ucontext */
        if (a->flags & SA_RESETHAND) {
            a->handler = SIG_DFL;
            a->flags = 0;
        }
        rv_linux_signals_handled++;
        return 0;
    }
}

/* rt_sigreturn: sp の rt_sigframe からレジスタとマスクを戻す */
long rv_linux_sigreturn(struct rv_uproc *u, uint64_t *f)
{
    struct lproc *me = current_proc(u);
    struct lnx_sigframe sf;

    if (me == NULL || rv_user_copy_in(&sf, f[TF_SP], sizeof(sf)) != 0) {
        if (me != NULL) {
            me->killsig = 11;
        }
        f[TF_A0] = (uint64_t)-1;
        return 1;                               /* 壊れたフレーム: 終わらせる */
    }
    sf_load_regs(&sf, f);
    me->sig_blocked = sf.uc_sigmask & ~UNBLOCKABLE;
    return 0;
}

/* ---- タイマ（setitimer の ITIMER_REAL・alarm）: 時間がたつと SIGALRM ---- */

static uint64_t now_us(void)
{
    uint64_t t;
    t = RV_RDTIME();
    return t / 10;                      /* QEMU virt の time は 10MHz */
}

/* タイマの割込みごと（10ms、割込み処理の中）: 時間になったプロセスへ SIGALRM */
void rv_linux_timer_tick(void)
{
    uint64_t now = now_us();
    int i;
    for (i = 0; i < LPROC_MAX; i++) {
        struct lproc *lp = &procs[i];
        if (lp->state == LP_RUN && lp->alarm_at != 0 && now >= lp->alarm_at) {
            lp->alarm_at = lp->alarm_iv ? now + lp->alarm_iv : 0;
            post_signal(lp, 14);        /* SIGALRM */
        }
    }
}

struct lnx_itimerval { int64_t iv_sec, iv_usec, v_sec, v_usec; };

long rv_linux_setitimer(struct rv_uproc *u, long which, uintptr_t unew, uintptr_t uold)
{
    struct lproc *me = current_proc(u);
    struct lnx_itimerval nv, ov;
    uint64_t now = now_us();

    if (me == NULL) {
        return -EINVAL;
    }
    if (which != 0) {
        return -EINVAL;                 /* ITIMER_VIRTUAL・PROF はまだ */
    }
    memset(&ov, 0, sizeof(ov));
    if (me->alarm_at) {
        uint64_t left = me->alarm_at > now ? me->alarm_at - now : 1;
        ov.v_sec = (int64_t)(left / 1000000);
        ov.v_usec = (int64_t)(left % 1000000);
        ov.iv_sec = (int64_t)(me->alarm_iv / 1000000);
        ov.iv_usec = (int64_t)(me->alarm_iv % 1000000);
    }
    if (unew != 0) {
        if (rv_user_copy_in(&nv, unew, sizeof(nv)) != 0) {
            return -EFAULT;
        }
        me->alarm_iv = (uint64_t)nv.iv_sec * 1000000 + (uint64_t)nv.iv_usec;
        if (nv.v_sec == 0 && nv.v_usec == 0) {
            me->alarm_at = 0;
        } else {
            me->alarm_at = now + (uint64_t)nv.v_sec * 1000000 + (uint64_t)nv.v_usec;
        }
    }
    if (uold != 0 && rv_user_copy_out(uold, &ov, sizeof(ov)) != 0) {
        return -EFAULT;
    }
    return 0;
}

long rv_linux_getitimer(struct rv_uproc *u, long which, uintptr_t ucur)
{
    return rv_linux_setitimer(u, which, 0, ucur);
}

/* ---- 制御端末（/dev/tty が指すもの） ---- */

int rv_linux_get_ctty(struct rv_uproc *u)
{
    struct lproc *me = current_proc(u);
    return me ? me->ctty : -1;
}

/* 疑似端末 n を制御端末にする。force でなければ、セッションの長で制御端末がないときだけ。
 * 端末の前面のグループがまだなければ、このプロセスのグループにする（Ctrl-C の送り先） */
void rv_linux_set_ctty(struct rv_uproc *u, int n, int force)
{
    extern void rv_pty_claim(int n, int pgid);
    struct lproc *me = current_proc(u);
    if (me != NULL && (force || (me->sid == me->pid && me->ctty == -2))) {
        me->ctty = n;
        rv_pty_claim(n, me->pgid);
    }
}


/* ---- 利用者（uid・gid）と、機器を動かしてよいか（安全の決まり 6・7） ---- */
#define EPERM_  1
#define EACCES_ 13
#define EINVAL_ 22
#define EFAULT_ 14

static int in_groups(const struct lproc *lp, int g)
{
    int k;
    if (lp->egid == g) {
        return 1;
    }
    for (k = 0; k < lp->ngroups; k++) {
        if (lp->groups[k] == g) {
            return 1;
        }
    }
    return 0;
}

/* uid・gid のシステムコール（143〜159・174〜177）。扱ったら 1 */
int rv_linux_cred_syscall(struct rv_uproc *u, long nr, const uint64_t *a, long *ret)
{
    struct lproc *lp;
    long r = 0;
    int root;
    if (!((nr >= 143 && nr <= 152) || nr == 158 || nr == 159 || (nr >= 174 && nr <= 177))) {
        return 0;                           /* ほかのシステムコールでは表を引かない */
    }
    lp = find_proc(u);
    if (lp == NULL) {
        *ret = 0;                           /* 表にないプロセス: root として扱う */
        return 1;
    }
    root = lp->euid == 0;
    switch (nr) {
    case 174: r = lp->uid; break;           /* getuid */
    case 175: r = lp->euid; break;          /* geteuid */
    case 176: r = lp->gid; break;           /* getgid */
    case 177: r = lp->egid; break;          /* getegid */
    case 146:                               /* setuid */
        if (root) {
            lp->uid = lp->euid = lp->suid = (int)a[0];
        } else if ((int)a[0] == lp->uid || (int)a[0] == lp->suid) {
            lp->euid = (int)a[0];
        } else {
            r = -EPERM_;
        }
        break;
    case 144:                               /* setgid */
        if (root) {
            lp->gid = lp->egid = lp->sgid = (int)a[0];
        } else if ((int)a[0] == lp->gid || (int)a[0] == lp->sgid) {
            lp->egid = (int)a[0];
        } else {
            r = -EPERM_;
        }
        break;
    case 145:                               /* setreuid */
    case 143: {                             /* setregid */
        int *rid = nr == 145 ? &lp->uid : &lp->gid, *eid = nr == 145 ? &lp->euid : &lp->egid;
        int *sid = nr == 145 ? &lp->suid : &lp->sgid;
        int nrid = (int)a[0], neid = (int)a[1];
        if (!root && ((nrid != -1 && nrid != *rid && nrid != *eid) || (neid != -1 && neid != *rid && neid != *eid && neid != *sid))) {
            r = -EPERM_;
            break;
        }
        if (nrid != -1) *rid = nrid;
        if (neid != -1) *eid = neid;
        if (nrid != -1 || neid != -1) *sid = *eid;
        break;
    }
    case 147:                               /* setresuid */
    case 149: {                             /* setresgid */
        int *ids[3];
        int k;
        ids[0] = nr == 147 ? &lp->uid : &lp->gid;
        ids[1] = nr == 147 ? &lp->euid : &lp->egid;
        ids[2] = nr == 147 ? &lp->suid : &lp->sgid;
        for (k = 0; k < 3 && !root; k++) {
            int v = (int)a[k];
            if (v != -1 && v != *ids[0] && v != *ids[1] && v != *ids[2]) {
                r = -EPERM_;
            }
        }
        if (r == 0) {
            for (k = 0; k < 3; k++) {
                if ((int)a[k] != -1) *ids[k] = (int)a[k];
            }
        }
        break;
    }
    case 148:                               /* getresuid */
    case 150: {                             /* getresgid */
        uint32_t v[3];
        int k;
        v[0] = (uint32_t)(nr == 148 ? lp->uid : lp->gid);
        v[1] = (uint32_t)(nr == 148 ? lp->euid : lp->egid);
        v[2] = (uint32_t)(nr == 148 ? lp->suid : lp->sgid);
        for (k = 0; k < 3 && r == 0; k++) {
            if (rv_user_copy_out((uintptr_t)a[k], &v[k], 4) != 0) r = -EFAULT_;
        }
        break;
    }
    case 151: case 152:                     /* setfsuid・setfsgid: 前の値を返すだけ */
        r = nr == 151 ? lp->euid : lp->egid;
        break;
    case 158: {                             /* getgroups */
        long n = (long)a[0];
        if (n == 0) {
            r = lp->ngroups;
        } else if (n < lp->ngroups) {
            r = -EINVAL_;
        } else {
            uint32_t g[8];
            int k;
            for (k = 0; k < lp->ngroups; k++) g[k] = (uint32_t)lp->groups[k];
            r = (lp->ngroups == 0 || rv_user_copy_out((uintptr_t)a[1], g, (size_t)lp->ngroups * 4) == 0) ? lp->ngroups : -EFAULT_;
        }
        break;
    }
    case 159: {                             /* setgroups */
        long n = (long)a[0];
        uint32_t g[8];
        int k;
        if (!root) {
            r = -EPERM_;
        } else if (n < 0 || n > 8) {
            r = -EINVAL_;
        } else if (n > 0 && rv_user_copy_in(g, (uintptr_t)a[1], (size_t)n * 4) != 0) {
            r = -EFAULT_;
        } else {
            for (k = 0; k < n; k++) lp->groups[k] = (int)g[k];
            lp->ngroups = (int)n;
        }
        break;
    }
    default:
        return 0;
    }
    *ret = r;
    return 1;
}

/* ネットワークのソケット（AF_INET・AF_INET6）を開いた: このプロセスと、これから作る子は機器を動かせない */
void rv_linux_mark_net(struct rv_uproc *u)
{
    struct lproc *lp = find_proc(u);
    if (lp != NULL) {
        lp->net = 1;
    }
}

/* いまのプロセスが機器（/dev/io）を動かしてよいか。0 = よい、-EPERM = ネットワークから、-EACCES = 権限がない。
 * io_gid: 動かしてよいグループ（root はいつもよい）。net_ok: ネットワークからも許すか（/etc/io.conf） */
int rv_linux_may_control(int io_gid, int net_ok)
{
    struct rv_uproc *u = rv_uproc_current();
    struct lproc *lp;
    if (u == NULL) {
        return 0;                           /* カーネルの中から（テストなど） */
    }
    lp = find_proc(u);
    if (lp == NULL) {
        return 0;
    }
    if (lp->net && !net_ok) {
        return -EPERM_;
    }
    if (lp->euid != 0 && !in_groups(lp, io_gid)) {
        return -EACCES_;
    }
    return 0;
}

/* 記録のため: いまのプロセスの番号・名前・uid（なければ 0・"kernel"） */
int rv_linux_who(int *pid, int *uid, char *comm, int max)
{
    struct rv_uproc *u = rv_uproc_current();
    struct lproc *lp = u ? find_proc(u) : NULL;
    int k;
    const char *c = lp ? (lp->comm[0] ? lp->comm : "?") : "kernel";
    *pid = lp ? lp->pid : 0;
    *uid = lp ? lp->euid : 0;
    for (k = 0; k < max - 1 && c[k]; k++) {
        comm[k] = c[k];
    }
    comm[k] = 0;
    return lp ? lp->net : 0;
}
