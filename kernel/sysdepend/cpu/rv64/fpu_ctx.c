/*
 * fpu_ctx.c — タスクの切替での浮動小数点・ベクトルのレジスタの保存と復元
 *
 *   RISC-V の xSTATUS.FS / VS は「レジスタの状態」を表す（0 = Off、1 = Initial、2 = Clean、3 = Dirty）。
 *   ハードウェアは、レジスタを書き換えると Dirty にする。
 *
 *   出ていくタスク（rv_fpu_switch_out）: Dirty なら、そのタスクの保存場所へ退避して Clean にする。
 *     Clean のままなら、レジスタの中身は前に戻した（退避した）ものと同じなので何もしない。
 *   入ってくるタスク（rv_fpu_switch_in）: 保存済みの中身があれば戻して Clean にする。
 *     一度も書き換えていないタスクには、丸めのモードなどの設定（fcsr）だけ 0 にする。
 *
 *   ベクトルのレジスタの大きさ（vlenb）は CPU によって違う（QEMU は 16 バイト、K3 は 32 バイトの見込み）。
 *   起動時に VS を立てて読み返し、V 拡張があれば vlenb を読む。保存場所は 1 タスク最大 64 バイト × 32 本。
 *
 *   ディスパッチャ（cpu_support_tk.S の dispatch_entry と切替のループ、exception.S の割込み出口での切替）
 *   から、割込み禁止のまま呼ばれる。
 */
#include <stdint.h>
#include "rv_priv.h"

#define TASK_SLOTS      64
#define VLENB_MAX       64              /* VLEN 512 ビットまで */

#define XSTATUS_FS_MASK (3UL << 13)
#define XSTATUS_FS_CLEAN (2UL << 13)
#define XSTATUS_FS_DIRTY (3UL << 13)
#define XSTATUS_VS_MASK (3UL << 9)
#define XSTATUS_VS_CLEAN (2UL << 9)
#define XSTATUS_VS_DIRTY (3UL << 9)

struct fp_area {
    uint64_t f[32];
    uint64_t fcsr;
};
struct v_area {
    uint64_t vl, vtype, vstart, vcsr;
    uint8_t  v[32 * VLENB_MAX] __attribute__((aligned(16)));
};

static struct fp_area fp_save[TASK_SLOTS];
static struct v_area  v_save[TASK_SLOTS];
static uint8_t fp_valid[TASK_SLOTS];
static uint8_t v_valid[TASK_SLOTS];

static int      has_vector;
static unsigned vlenb;
volatile unsigned long rv_fpu_saves, rv_fpu_restores, rv_vec_saves, rv_vec_restores;  /* テスト用 */

struct task_control_block;
extern struct task_control_block *tcb_table;
extern unsigned long rv_tcb_size(void);

static int slot_of(void *tcb)
{
    long i;
    if (tcb == 0 || tcb_table == 0) {
        return -1;
    }
    i = (long)(((uintptr_t)tcb - (uintptr_t)tcb_table) / rv_tcb_size());
    return (i >= 0 && i < TASK_SLOTS) ? (int)i : -1;
}

static inline unsigned long read_status(void)
{
    unsigned long s;
    asm volatile("csrr %0, " RV_STR(CSR_XSTATUS) : "=r"(s));
    return s;
}

/* 起動時に 1 回: V 拡張の有無と vlenb を調べる（devinit.c が FS/VS を立てたあと） */
void rv_fpu_init(void)
{
    if (read_status() & XSTATUS_VS_MASK) {
        unsigned long b;
        asm volatile(".option push\n.option arch, +v\ncsrr %0, vlenb\n.option pop" : "=r"(b));
        if (b > 0 && b <= VLENB_MAX) {
            has_vector = 1;
            vlenb = (unsigned)b;
        }
    }
}

int rv_fpu_vlenb(void) { return has_vector ? (int)vlenb : 0; }

static void fp_store(struct fp_area *a)
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
        :: "r"(a->f) : "memory");
    asm volatile("frcsr %0" : "=r"(a->fcsr));
}

static void fp_load(const struct fp_area *a)
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
        :: "r"(a->f) : "memory");
    asm volatile("fscsr %0" :: "r"(a->fcsr));
}

static void v_store(struct v_area *a)
{
    uint8_t *p = a->v;
    asm volatile(
        ".option push\n.option arch, +v\n"
        "csrr %0, vl\n csrr %1, vtype\n csrr %2, vstart\n csrr %3, vcsr\n"
        ".option pop"
        : "=r"(a->vl), "=r"(a->vtype), "=r"(a->vstart), "=r"(a->vcsr));
    asm volatile(
        ".option push\n.option arch, +v\n"
        "vs8r.v v0, (%0)\n add %0, %0, %1\n"
        "vs8r.v v8, (%0)\n add %0, %0, %1\n"
        "vs8r.v v16, (%0)\n add %0, %0, %1\n"
        "vs8r.v v24, (%0)\n"
        ".option pop"
        : "+r"(p) : "r"((unsigned long)vlenb * 8) : "memory");
}

static void v_load(const struct v_area *a)
{
    const uint8_t *p = a->v;
    asm volatile(
        ".option push\n.option arch, +v\n"
        "vl8re8.v v0, (%0)\n add %0, %0, %1\n"
        "vl8re8.v v8, (%0)\n add %0, %0, %1\n"
        "vl8re8.v v16, (%0)\n add %0, %0, %1\n"
        "vl8re8.v v24, (%0)\n"
        ".option pop"
        : "+r"(p) : "r"((unsigned long)vlenb * 8) : "memory");
    asm volatile(
        ".option push\n.option arch, +v\n"
        "vsetvl x0, %0, %1\n csrw vstart, %2\n csrw vcsr, %3\n"
        ".option pop"
        :: "r"(a->vl), "r"(a->vtype), "r"(a->vstart), "r"(a->vcsr));
}

void rv_fpu_switch_out(void *tcb)
{
    int i = slot_of(tcb);
    unsigned long st = read_status();
    unsigned long clear = 0, set = 0;

    if (i < 0) {
        return;
    }
    if ((st & XSTATUS_FS_MASK) == XSTATUS_FS_DIRTY) {
        fp_store(&fp_save[i]);
        fp_valid[i] = 1;
        rv_fpu_saves++;
        clear |= XSTATUS_FS_MASK;
        set |= XSTATUS_FS_CLEAN;
    }
    if (has_vector && (st & XSTATUS_VS_MASK) == XSTATUS_VS_DIRTY) {
        v_store(&v_save[i]);
        v_valid[i] = 1;
        rv_vec_saves++;
        clear |= XSTATUS_VS_MASK;
        set |= XSTATUS_VS_CLEAN;
    }
    if (clear) {
        asm volatile("csrc " RV_STR(CSR_XSTATUS) ", %0" :: "r"(clear));
        asm volatile("csrs " RV_STR(CSR_XSTATUS) ", %0" :: "r"(set));
    }
}

void rv_fpu_switch_in(void *tcb)
{
    int i = slot_of(tcb);
    unsigned long clear = 0, set = 0;

    if (i < 0) {
        return;
    }
    if (fp_valid[i]) {
        fp_load(&fp_save[i]);
        rv_fpu_restores++;
        clear |= XSTATUS_FS_MASK;
        set |= XSTATUS_FS_CLEAN;
    } else {
        unsigned long fcsr;
        asm volatile("frcsr %0" : "=r"(fcsr));
        if (fcsr != 0) {
            asm volatile("fscsr zero"); /* 丸めのモードなどは既定値に（0 なら触らず Clean のまま） */
        }
    }
    if (has_vector && v_valid[i]) {
        v_load(&v_save[i]);
        rv_vec_restores++;
        clear |= XSTATUS_VS_MASK;
        set |= XSTATUS_VS_CLEAN;
    }
    if (clear) {
        asm volatile("csrc " RV_STR(CSR_XSTATUS) ", %0" :: "r"(clear));
        asm volatile("csrs " RV_STR(CSR_XSTATUS) ", %0" :: "r"(set));
    }
}

/* タスクが新しく始まるとき（前のタスクの中身を引き継がない） */
void rv_fpu_task_reset(void *tcb)
{
    int i = slot_of(tcb);
    if (i >= 0) {
        fp_valid[i] = 0;
        v_valid[i] = 0;
    }
}

/*
 * fork: 実行中のタスクの浮動小数点・ベクトルのレジスタを、子のタスクの保存場所へ写す。
 * 子が最初に動くとき、ディスパッチャがそれを戻す。
 */
void rv_fpu_copy_current_to(void *tcb)
{
    int i = slot_of(tcb);
    if (i < 0) {
        return;
    }
    fp_store(&fp_save[i]);
    fp_valid[i] = 1;
    if (has_vector) {
        v_store(&v_save[i]);
        v_valid[i] = 1;
    }
}
