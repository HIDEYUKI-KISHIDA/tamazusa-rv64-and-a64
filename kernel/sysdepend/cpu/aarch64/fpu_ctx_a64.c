/*
 * fpu_ctx_a64.c — タスクの切替での浮動小数点・SIMD のレジスタ（q0〜q31、FPSR、FPCR）の保存と復元
 *
 *   RISC-V 版（cpu/rv64/fpu_ctx.c）と同じ呼ばれ方をする（ディスパッチャから、割込み禁止のまま）。
 *     rv_fpu_switch_out(tcb) … 出ていくタスクのレジスタを、そのタスクの保存場所へ
 *     rv_fpu_switch_in(tcb)  … 入ってくるタスクの保存済みの中身を戻す
 *   RISC-V には「書き換えたか」（FS = Dirty）の印があるが、AArch64 にはない。
 *   カーネルは浮動小数点のレジスタを使わない（-mgeneral-regs-only）ので、レジスタの中身は
 *   いつも「最後に動いたタスクのもの」。そこで、持ち主（owner）を覚えておき、
 *   別のタスクが入ってくるときだけ、持ち主の分を退避してから入れ替える（遅延の入れ替え）。
 *   SVE はまだ使わない（CPACR の ZEN は 0 のまま）。
 */
#include <stdint.h>

#define TASK_SLOTS      64

struct fp_area {
    unsigned __int128 q[32];
    uint64_t fpsr, fpcr;
};

static struct fp_area fp_save[TASK_SLOTS] __attribute__((aligned(16)));
static uint8_t fp_valid[TASK_SLOTS];
static int fp_owner = -1;               /* いまのレジスタの中身の持ち主（スロット番号）。-1 は誰でもない */

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

static void fp_store(struct fp_area *a)
{
    uint64_t v;
    asm volatile(
        "stp q0, q1, [%0, #0]\n stp q2, q3, [%0, #32]\n stp q4, q5, [%0, #64]\n stp q6, q7, [%0, #96]\n"
        "stp q8, q9, [%0, #128]\n stp q10, q11, [%0, #160]\n stp q12, q13, [%0, #192]\n stp q14, q15, [%0, #224]\n"
        "stp q16, q17, [%0, #256]\n stp q18, q19, [%0, #288]\n stp q20, q21, [%0, #320]\n stp q22, q23, [%0, #352]\n"
        "stp q24, q25, [%0, #384]\n stp q26, q27, [%0, #416]\n stp q28, q29, [%0, #448]\n stp q30, q31, [%0, #480]\n"
        :: "r"(a->q) : "memory");
    asm volatile("mrs %0, fpsr" : "=r"(v)); a->fpsr = v;
    asm volatile("mrs %0, fpcr" : "=r"(v)); a->fpcr = v;
}

static void fp_load(const struct fp_area *a)
{
    asm volatile(
        "ldp q0, q1, [%0, #0]\n ldp q2, q3, [%0, #32]\n ldp q4, q5, [%0, #64]\n ldp q6, q7, [%0, #96]\n"
        "ldp q8, q9, [%0, #128]\n ldp q10, q11, [%0, #160]\n ldp q12, q13, [%0, #192]\n ldp q14, q15, [%0, #224]\n"
        "ldp q16, q17, [%0, #256]\n ldp q18, q19, [%0, #288]\n ldp q20, q21, [%0, #320]\n ldp q22, q23, [%0, #352]\n"
        "ldp q24, q25, [%0, #384]\n ldp q26, q27, [%0, #416]\n ldp q28, q29, [%0, #448]\n ldp q30, q31, [%0, #480]\n"
        :: "r"(a->q) : "memory");
    asm volatile("msr fpsr, %0" :: "r"(a->fpsr));
    asm volatile("msr fpcr, %0" :: "r"(a->fpcr));
}

static void fp_clear(void)
{
    asm volatile(
        "movi v0.2d, #0\n mov v1.16b, v0.16b\n mov v2.16b, v0.16b\n mov v3.16b, v0.16b\n"
        "mov v4.16b, v0.16b\n mov v5.16b, v0.16b\n mov v6.16b, v0.16b\n mov v7.16b, v0.16b\n"
        "mov v8.16b, v0.16b\n mov v9.16b, v0.16b\n mov v10.16b, v0.16b\n mov v11.16b, v0.16b\n"
        "mov v12.16b, v0.16b\n mov v13.16b, v0.16b\n mov v14.16b, v0.16b\n mov v15.16b, v0.16b\n"
        "mov v16.16b, v0.16b\n mov v17.16b, v0.16b\n mov v18.16b, v0.16b\n mov v19.16b, v0.16b\n"
        "mov v20.16b, v0.16b\n mov v21.16b, v0.16b\n mov v22.16b, v0.16b\n mov v23.16b, v0.16b\n"
        "mov v24.16b, v0.16b\n mov v25.16b, v0.16b\n mov v26.16b, v0.16b\n mov v27.16b, v0.16b\n"
        "mov v28.16b, v0.16b\n mov v29.16b, v0.16b\n mov v30.16b, v0.16b\n mov v31.16b, v0.16b\n"
        "msr fpsr, xzr\n msr fpcr, xzr\n" ::: "memory");
}

void rv_fpu_init(void)
{
    fp_owner = -1;
}

/* NEON のレジスタは 128 ビット（M19 の試験は vlenb = 16 のときにベクトルの部分も確かめる）。
 * 浮動小数点とベクトルは同じレジスタ（q0〜q31）なので、いっしょに退避・復元する */
int rv_fpu_vlenb(void) { return 16; }

/* 出ていくとき: 何もしない（中身は持ち主のものとしてレジスタに残しておく） */
void rv_fpu_switch_out(void *tcb)
{
    (void)tcb;
}

/* 入ってくるとき: 持ち主が別なら、持ち主の分を退避してから、このタスクの分を戻す */
void rv_fpu_switch_in(void *tcb)
{
    int i = slot_of(tcb);

    if (i < 0 || i == fp_owner) {
        return;
    }
    if (fp_owner >= 0) {
        fp_store(&fp_save[fp_owner]);
        fp_valid[fp_owner] = 1;
        rv_fpu_saves++;
        rv_vec_saves++;
    }
    if (fp_valid[i]) {
        fp_load(&fp_save[i]);
        rv_fpu_restores++;
        rv_vec_restores++;
    } else {
        fp_clear();                     /* 前のタスクの中身を見せない */
    }
    fp_owner = i;
}

/* タスクが新しく始まるとき（前のタスクの中身を引き継がない） */
void rv_fpu_task_reset(void *tcb)
{
    int i = slot_of(tcb);
    if (i >= 0) {
        fp_valid[i] = 0;
        if (fp_owner == i) {
            fp_owner = -1;              /* レジスタに残っている前のタスクの中身は捨てる */
        }
    }
}

/* fork: 実行中のタスクのレジスタを、子のタスクの保存場所へ写す（子が最初に動くときに戻る） */
void rv_fpu_copy_current_to(void *tcb)
{
    int i = slot_of(tcb);
    if (i < 0) {
        return;
    }
    fp_store(&fp_save[i]);
    fp_valid[i] = 1;
}
