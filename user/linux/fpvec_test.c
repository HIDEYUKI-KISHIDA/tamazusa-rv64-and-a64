/* fpvec_test.c — 浮動小数点・ベクトルのレジスタがタスクの切替で保たれるかを確かめる（M19）
 *
 *   main(argc, argv): argv[1] = 種（数字）、argv[2] = "vec" ならベクトルも確かめる
 *   20 回: 浮動小数点のレジスタ 32 本（とベクトルのレジスタ 32 本）に種から作った値を入れ、
 *          そのまま nanosleep で眠り（その間に別のプログラムが同じレジスタを使う）、
 *          起きてから読み出して比べる。すべて一致すれば 0、違えば 1 で終わる。
 *   値の出し入れと眠りは 1 つの asm にまとめ、コンパイラがレジスタを触らないようにする。
 *   ベクトルは要素 64 ビット・1 本 128 ビット（vlenb = 16）として 2 要素ずつ使う。
 *   AArch64 では浮動小数点が d0〜d31、ベクトルが NEON の q0〜q31（d は q の下 64 ビット）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

struct ts { long sec; long nsec; };

static uint64_t fsrc[32], fdst[32];
static uint64_t vsrc[64], vdst[64];

#if defined(__aarch64__)
/* AArch64: 浮動小数点は d0〜d31（q0〜q31 の下 64 ビット）、ベクトルは q0〜q31（NEON、128 ビット） */
static long round_fp(const struct ts *t)
{
    register long x0 asm("x0") = (long)t;
    register long x1 asm("x1") = 0;
    register long x8 asm("x8") = 101;   /* nanosleep */
    asm volatile(
        "ldr d0, [%[fs], #0]\n"
        "ldr d1, [%[fs], #8]\n"
        "ldr d2, [%[fs], #16]\n"
        "ldr d3, [%[fs], #24]\n"
        "ldr d4, [%[fs], #32]\n"
        "ldr d5, [%[fs], #40]\n"
        "ldr d6, [%[fs], #48]\n"
        "ldr d7, [%[fs], #56]\n"
        "ldr d8, [%[fs], #64]\n"
        "ldr d9, [%[fs], #72]\n"
        "ldr d10, [%[fs], #80]\n"
        "ldr d11, [%[fs], #88]\n"
        "ldr d12, [%[fs], #96]\n"
        "ldr d13, [%[fs], #104]\n"
        "ldr d14, [%[fs], #112]\n"
        "ldr d15, [%[fs], #120]\n"
        "ldr d16, [%[fs], #128]\n"
        "ldr d17, [%[fs], #136]\n"
        "ldr d18, [%[fs], #144]\n"
        "ldr d19, [%[fs], #152]\n"
        "ldr d20, [%[fs], #160]\n"
        "ldr d21, [%[fs], #168]\n"
        "ldr d22, [%[fs], #176]\n"
        "ldr d23, [%[fs], #184]\n"
        "ldr d24, [%[fs], #192]\n"
        "ldr d25, [%[fs], #200]\n"
        "ldr d26, [%[fs], #208]\n"
        "ldr d27, [%[fs], #216]\n"
        "ldr d28, [%[fs], #224]\n"
        "ldr d29, [%[fs], #232]\n"
        "ldr d30, [%[fs], #240]\n"
        "ldr d31, [%[fs], #248]\n"
        "svc #0\n"
        "str d0, [%[fd], #0]\n"
        "str d1, [%[fd], #8]\n"
        "str d2, [%[fd], #16]\n"
        "str d3, [%[fd], #24]\n"
        "str d4, [%[fd], #32]\n"
        "str d5, [%[fd], #40]\n"
        "str d6, [%[fd], #48]\n"
        "str d7, [%[fd], #56]\n"
        "str d8, [%[fd], #64]\n"
        "str d9, [%[fd], #72]\n"
        "str d10, [%[fd], #80]\n"
        "str d11, [%[fd], #88]\n"
        "str d12, [%[fd], #96]\n"
        "str d13, [%[fd], #104]\n"
        "str d14, [%[fd], #112]\n"
        "str d15, [%[fd], #120]\n"
        "str d16, [%[fd], #128]\n"
        "str d17, [%[fd], #136]\n"
        "str d18, [%[fd], #144]\n"
        "str d19, [%[fd], #152]\n"
        "str d20, [%[fd], #160]\n"
        "str d21, [%[fd], #168]\n"
        "str d22, [%[fd], #176]\n"
        "str d23, [%[fd], #184]\n"
        "str d24, [%[fd], #192]\n"
        "str d25, [%[fd], #200]\n"
        "str d26, [%[fd], #208]\n"
        "str d27, [%[fd], #216]\n"
        "str d28, [%[fd], #224]\n"
        "str d29, [%[fd], #232]\n"
        "str d30, [%[fd], #240]\n"
        "str d31, [%[fd], #248]\n"
        : "+r"(x0) : [fs] "r"(fsrc), [fd] "r"(fdst), "r"(x1), "r"(x8)
        : "memory", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31");
    return x0;
}

/* q0〜q31 の 128 ビットすべて。d（下 64 ビット）は fsrc と同じ値になるよう main が用意する */
static long round_vec(const struct ts *t)
{
    register long x0 asm("x0") = (long)t;
    register long x1 asm("x1") = 0;
    register long x8 asm("x8") = 101;
    asm volatile(
        "ldr q0, [%[vs], #0]\n"
        "ldr q1, [%[vs], #16]\n"
        "ldr q2, [%[vs], #32]\n"
        "ldr q3, [%[vs], #48]\n"
        "ldr q4, [%[vs], #64]\n"
        "ldr q5, [%[vs], #80]\n"
        "ldr q6, [%[vs], #96]\n"
        "ldr q7, [%[vs], #112]\n"
        "ldr q8, [%[vs], #128]\n"
        "ldr q9, [%[vs], #144]\n"
        "ldr q10, [%[vs], #160]\n"
        "ldr q11, [%[vs], #176]\n"
        "ldr q12, [%[vs], #192]\n"
        "ldr q13, [%[vs], #208]\n"
        "ldr q14, [%[vs], #224]\n"
        "ldr q15, [%[vs], #240]\n"
        "ldr q16, [%[vs], #256]\n"
        "ldr q17, [%[vs], #272]\n"
        "ldr q18, [%[vs], #288]\n"
        "ldr q19, [%[vs], #304]\n"
        "ldr q20, [%[vs], #320]\n"
        "ldr q21, [%[vs], #336]\n"
        "ldr q22, [%[vs], #352]\n"
        "ldr q23, [%[vs], #368]\n"
        "ldr q24, [%[vs], #384]\n"
        "ldr q25, [%[vs], #400]\n"
        "ldr q26, [%[vs], #416]\n"
        "ldr q27, [%[vs], #432]\n"
        "ldr q28, [%[vs], #448]\n"
        "ldr q29, [%[vs], #464]\n"
        "ldr q30, [%[vs], #480]\n"
        "ldr q31, [%[vs], #496]\n"
        "svc #0\n"
        "str q0, [%[vd], #0]\n"
        "str q1, [%[vd], #16]\n"
        "str q2, [%[vd], #32]\n"
        "str q3, [%[vd], #48]\n"
        "str q4, [%[vd], #64]\n"
        "str q5, [%[vd], #80]\n"
        "str q6, [%[vd], #96]\n"
        "str q7, [%[vd], #112]\n"
        "str q8, [%[vd], #128]\n"
        "str q9, [%[vd], #144]\n"
        "str q10, [%[vd], #160]\n"
        "str q11, [%[vd], #176]\n"
        "str q12, [%[vd], #192]\n"
        "str q13, [%[vd], #208]\n"
        "str q14, [%[vd], #224]\n"
        "str q15, [%[vd], #240]\n"
        "str q16, [%[vd], #256]\n"
        "str q17, [%[vd], #272]\n"
        "str q18, [%[vd], #288]\n"
        "str q19, [%[vd], #304]\n"
        "str q20, [%[vd], #320]\n"
        "str q21, [%[vd], #336]\n"
        "str q22, [%[vd], #352]\n"
        "str q23, [%[vd], #368]\n"
        "str q24, [%[vd], #384]\n"
        "str q25, [%[vd], #400]\n"
        "str q26, [%[vd], #416]\n"
        "str q27, [%[vd], #432]\n"
        "str q28, [%[vd], #448]\n"
        "str q29, [%[vd], #464]\n"
        "str q30, [%[vd], #480]\n"
        "str q31, [%[vd], #496]\n"
        "str d0, [%[fd], #0]\n"
        "str d1, [%[fd], #8]\n"
        "str d2, [%[fd], #16]\n"
        "str d3, [%[fd], #24]\n"
        "str d4, [%[fd], #32]\n"
        "str d5, [%[fd], #40]\n"
        "str d6, [%[fd], #48]\n"
        "str d7, [%[fd], #56]\n"
        "str d8, [%[fd], #64]\n"
        "str d9, [%[fd], #72]\n"
        "str d10, [%[fd], #80]\n"
        "str d11, [%[fd], #88]\n"
        "str d12, [%[fd], #96]\n"
        "str d13, [%[fd], #104]\n"
        "str d14, [%[fd], #112]\n"
        "str d15, [%[fd], #120]\n"
        "str d16, [%[fd], #128]\n"
        "str d17, [%[fd], #136]\n"
        "str d18, [%[fd], #144]\n"
        "str d19, [%[fd], #152]\n"
        "str d20, [%[fd], #160]\n"
        "str d21, [%[fd], #168]\n"
        "str d22, [%[fd], #176]\n"
        "str d23, [%[fd], #184]\n"
        "str d24, [%[fd], #192]\n"
        "str d25, [%[fd], #200]\n"
        "str d26, [%[fd], #208]\n"
        "str d27, [%[fd], #216]\n"
        "str d28, [%[fd], #224]\n"
        "str d29, [%[fd], #232]\n"
        "str d30, [%[fd], #240]\n"
        "str d31, [%[fd], #248]\n"
        : "+r"(x0) : [vs] "r"(vsrc), [vd] "r"(vdst), [fd] "r"(fdst), "r"(x1), "r"(x8)
        : "memory", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31");
    return x0;
}
#else
static long round_fp(const struct ts *t)
{
    register long a0 asm("a0") = (long)t;
    register long a1 asm("a1") = 0;
    register long a7 asm("a7") = 101;   /* nanosleep */
    asm volatile(
        "fld f0, 0(%[fs])\n"
        "fld f1, 8(%[fs])\n"
        "fld f2, 16(%[fs])\n"
        "fld f3, 24(%[fs])\n"
        "fld f4, 32(%[fs])\n"
        "fld f5, 40(%[fs])\n"
        "fld f6, 48(%[fs])\n"
        "fld f7, 56(%[fs])\n"
        "fld f8, 64(%[fs])\n"
        "fld f9, 72(%[fs])\n"
        "fld f10, 80(%[fs])\n"
        "fld f11, 88(%[fs])\n"
        "fld f12, 96(%[fs])\n"
        "fld f13, 104(%[fs])\n"
        "fld f14, 112(%[fs])\n"
        "fld f15, 120(%[fs])\n"
        "fld f16, 128(%[fs])\n"
        "fld f17, 136(%[fs])\n"
        "fld f18, 144(%[fs])\n"
        "fld f19, 152(%[fs])\n"
        "fld f20, 160(%[fs])\n"
        "fld f21, 168(%[fs])\n"
        "fld f22, 176(%[fs])\n"
        "fld f23, 184(%[fs])\n"
        "fld f24, 192(%[fs])\n"
        "fld f25, 200(%[fs])\n"
        "fld f26, 208(%[fs])\n"
        "fld f27, 216(%[fs])\n"
        "fld f28, 224(%[fs])\n"
        "fld f29, 232(%[fs])\n"
        "fld f30, 240(%[fs])\n"
        "fld f31, 248(%[fs])\n"
        "ecall\n"
        "fsd f0, 0(%[fd])\n"
        "fsd f1, 8(%[fd])\n"
        "fsd f2, 16(%[fd])\n"
        "fsd f3, 24(%[fd])\n"
        "fsd f4, 32(%[fd])\n"
        "fsd f5, 40(%[fd])\n"
        "fsd f6, 48(%[fd])\n"
        "fsd f7, 56(%[fd])\n"
        "fsd f8, 64(%[fd])\n"
        "fsd f9, 72(%[fd])\n"
        "fsd f10, 80(%[fd])\n"
        "fsd f11, 88(%[fd])\n"
        "fsd f12, 96(%[fd])\n"
        "fsd f13, 104(%[fd])\n"
        "fsd f14, 112(%[fd])\n"
        "fsd f15, 120(%[fd])\n"
        "fsd f16, 128(%[fd])\n"
        "fsd f17, 136(%[fd])\n"
        "fsd f18, 144(%[fd])\n"
        "fsd f19, 152(%[fd])\n"
        "fsd f20, 160(%[fd])\n"
        "fsd f21, 168(%[fd])\n"
        "fsd f22, 176(%[fd])\n"
        "fsd f23, 184(%[fd])\n"
        "fsd f24, 192(%[fd])\n"
        "fsd f25, 200(%[fd])\n"
        "fsd f26, 208(%[fd])\n"
        "fsd f27, 216(%[fd])\n"
        "fsd f28, 224(%[fd])\n"
        "fsd f29, 232(%[fd])\n"
        "fsd f30, 240(%[fd])\n"
        "fsd f31, 248(%[fd])\n"
        : "+r"(a0) : [fs] "r"(fsrc), [fd] "r"(fdst), "r"(a1), "r"(a7)
        : "memory", "f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12", "f13", "f14", "f15", "f16", "f17", "f18", "f19", "f20", "f21", "f22", "f23", "f24", "f25", "f26", "f27", "f28", "f29", "f30", "f31");
    return a0;
}

static long round_vec(const struct ts *t)
{
    register long a0 asm("a0") = (long)t;
    register long a1 asm("a1") = 0;
    register long a7 asm("a7") = 101;
    uint64_t *vs_p = vsrc, *vd_p = vdst;
    unsigned long vl;
    asm volatile(
        ".option push\n.option arch, +v\n"
        "vsetivli %[vl], 2, e64, m1, ta, ma\n"
        "vle64.v v0, (%[vs])\n addi %[vs], %[vs], 16\n"
        "vle64.v v1, (%[vs])\n addi %[vs], %[vs], 16\n"
        "vle64.v v2, (%[vs])\n addi %[vs], %[vs], 16\n"
        "vle64.v v3, (%[vs])\n addi %[vs], %[vs], 16\n"
        "vle64.v v4, (%[vs])\n addi %[vs], %[vs], 16\n"
        "vle64.v v5, (%[vs])\n addi %[vs], %[vs], 16\n"
        "vle64.v v6, (%[vs])\n addi %[vs], %[vs], 16\n"
        "vle64.v v7, (%[vs])\n addi %[vs], %[vs], 16\n"
        "vle64.v v8, (%[vs])\n addi %[vs], %[vs], 16\n"
        "vle64.v v9, (%[vs])\n addi %[vs], %[vs], 16\n"
        "vle64.v v10, (%[vs])\n addi %[vs], %[vs], 16\n"
        "vle64.v v11, (%[vs])\n addi %[vs], %[vs], 16\n"
        "vle64.v v12, (%[vs])\n addi %[vs], %[vs], 16\n"
        "vle64.v v13, (%[vs])\n addi %[vs], %[vs], 16\n"
        "vle64.v v14, (%[vs])\n addi %[vs], %[vs], 16\n"
        "vle64.v v15, (%[vs])\n addi %[vs], %[vs], 16\n"
        "vle64.v v16, (%[vs])\n addi %[vs], %[vs], 16\n"
        "vle64.v v17, (%[vs])\n addi %[vs], %[vs], 16\n"
        "vle64.v v18, (%[vs])\n addi %[vs], %[vs], 16\n"
        "vle64.v v19, (%[vs])\n addi %[vs], %[vs], 16\n"
        "vle64.v v20, (%[vs])\n addi %[vs], %[vs], 16\n"
        "vle64.v v21, (%[vs])\n addi %[vs], %[vs], 16\n"
        "vle64.v v22, (%[vs])\n addi %[vs], %[vs], 16\n"
        "vle64.v v23, (%[vs])\n addi %[vs], %[vs], 16\n"
        "vle64.v v24, (%[vs])\n addi %[vs], %[vs], 16\n"
        "vle64.v v25, (%[vs])\n addi %[vs], %[vs], 16\n"
        "vle64.v v26, (%[vs])\n addi %[vs], %[vs], 16\n"
        "vle64.v v27, (%[vs])\n addi %[vs], %[vs], 16\n"
        "vle64.v v28, (%[vs])\n addi %[vs], %[vs], 16\n"
        "vle64.v v29, (%[vs])\n addi %[vs], %[vs], 16\n"
        "vle64.v v30, (%[vs])\n addi %[vs], %[vs], 16\n"
        "vle64.v v31, (%[vs])\n addi %[vs], %[vs], 16\n"
        "fld f0, 0(%[fs])\n"
        "fld f1, 8(%[fs])\n"
        "fld f2, 16(%[fs])\n"
        "fld f3, 24(%[fs])\n"
        "fld f4, 32(%[fs])\n"
        "fld f5, 40(%[fs])\n"
        "fld f6, 48(%[fs])\n"
        "fld f7, 56(%[fs])\n"
        "fld f8, 64(%[fs])\n"
        "fld f9, 72(%[fs])\n"
        "fld f10, 80(%[fs])\n"
        "fld f11, 88(%[fs])\n"
        "fld f12, 96(%[fs])\n"
        "fld f13, 104(%[fs])\n"
        "fld f14, 112(%[fs])\n"
        "fld f15, 120(%[fs])\n"
        "fld f16, 128(%[fs])\n"
        "fld f17, 136(%[fs])\n"
        "fld f18, 144(%[fs])\n"
        "fld f19, 152(%[fs])\n"
        "fld f20, 160(%[fs])\n"
        "fld f21, 168(%[fs])\n"
        "fld f22, 176(%[fs])\n"
        "fld f23, 184(%[fs])\n"
        "fld f24, 192(%[fs])\n"
        "fld f25, 200(%[fs])\n"
        "fld f26, 208(%[fs])\n"
        "fld f27, 216(%[fs])\n"
        "fld f28, 224(%[fs])\n"
        "fld f29, 232(%[fs])\n"
        "fld f30, 240(%[fs])\n"
        "fld f31, 248(%[fs])\n"
        "ecall\n"
        "fsd f0, 0(%[fd])\n"
        "fsd f1, 8(%[fd])\n"
        "fsd f2, 16(%[fd])\n"
        "fsd f3, 24(%[fd])\n"
        "fsd f4, 32(%[fd])\n"
        "fsd f5, 40(%[fd])\n"
        "fsd f6, 48(%[fd])\n"
        "fsd f7, 56(%[fd])\n"
        "fsd f8, 64(%[fd])\n"
        "fsd f9, 72(%[fd])\n"
        "fsd f10, 80(%[fd])\n"
        "fsd f11, 88(%[fd])\n"
        "fsd f12, 96(%[fd])\n"
        "fsd f13, 104(%[fd])\n"
        "fsd f14, 112(%[fd])\n"
        "fsd f15, 120(%[fd])\n"
        "fsd f16, 128(%[fd])\n"
        "fsd f17, 136(%[fd])\n"
        "fsd f18, 144(%[fd])\n"
        "fsd f19, 152(%[fd])\n"
        "fsd f20, 160(%[fd])\n"
        "fsd f21, 168(%[fd])\n"
        "fsd f22, 176(%[fd])\n"
        "fsd f23, 184(%[fd])\n"
        "fsd f24, 192(%[fd])\n"
        "fsd f25, 200(%[fd])\n"
        "fsd f26, 208(%[fd])\n"
        "fsd f27, 216(%[fd])\n"
        "fsd f28, 224(%[fd])\n"
        "fsd f29, 232(%[fd])\n"
        "fsd f30, 240(%[fd])\n"
        "fsd f31, 248(%[fd])\n"
        "vse64.v v0, (%[vd])\n addi %[vd], %[vd], 16\n"
        "vse64.v v1, (%[vd])\n addi %[vd], %[vd], 16\n"
        "vse64.v v2, (%[vd])\n addi %[vd], %[vd], 16\n"
        "vse64.v v3, (%[vd])\n addi %[vd], %[vd], 16\n"
        "vse64.v v4, (%[vd])\n addi %[vd], %[vd], 16\n"
        "vse64.v v5, (%[vd])\n addi %[vd], %[vd], 16\n"
        "vse64.v v6, (%[vd])\n addi %[vd], %[vd], 16\n"
        "vse64.v v7, (%[vd])\n addi %[vd], %[vd], 16\n"
        "vse64.v v8, (%[vd])\n addi %[vd], %[vd], 16\n"
        "vse64.v v9, (%[vd])\n addi %[vd], %[vd], 16\n"
        "vse64.v v10, (%[vd])\n addi %[vd], %[vd], 16\n"
        "vse64.v v11, (%[vd])\n addi %[vd], %[vd], 16\n"
        "vse64.v v12, (%[vd])\n addi %[vd], %[vd], 16\n"
        "vse64.v v13, (%[vd])\n addi %[vd], %[vd], 16\n"
        "vse64.v v14, (%[vd])\n addi %[vd], %[vd], 16\n"
        "vse64.v v15, (%[vd])\n addi %[vd], %[vd], 16\n"
        "vse64.v v16, (%[vd])\n addi %[vd], %[vd], 16\n"
        "vse64.v v17, (%[vd])\n addi %[vd], %[vd], 16\n"
        "vse64.v v18, (%[vd])\n addi %[vd], %[vd], 16\n"
        "vse64.v v19, (%[vd])\n addi %[vd], %[vd], 16\n"
        "vse64.v v20, (%[vd])\n addi %[vd], %[vd], 16\n"
        "vse64.v v21, (%[vd])\n addi %[vd], %[vd], 16\n"
        "vse64.v v22, (%[vd])\n addi %[vd], %[vd], 16\n"
        "vse64.v v23, (%[vd])\n addi %[vd], %[vd], 16\n"
        "vse64.v v24, (%[vd])\n addi %[vd], %[vd], 16\n"
        "vse64.v v25, (%[vd])\n addi %[vd], %[vd], 16\n"
        "vse64.v v26, (%[vd])\n addi %[vd], %[vd], 16\n"
        "vse64.v v27, (%[vd])\n addi %[vd], %[vd], 16\n"
        "vse64.v v28, (%[vd])\n addi %[vd], %[vd], 16\n"
        "vse64.v v29, (%[vd])\n addi %[vd], %[vd], 16\n"
        "vse64.v v30, (%[vd])\n addi %[vd], %[vd], 16\n"
        "vse64.v v31, (%[vd])\n addi %[vd], %[vd], 16\n"
        ".option pop\n"
        : "+r"(a0), [vs] "+r"(vs_p), [vd] "+r"(vd_p), [vl] "=&r"(vl)
        : [fs] "r"(fsrc), [fd] "r"(fdst), "r"(a1), "r"(a7)
        : "memory", "f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12", "f13", "f14", "f15", "f16", "f17", "f18", "f19", "f20", "f21", "f22", "f23", "f24", "f25", "f26", "f27", "f28", "f29", "f30", "f31", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31");
    return a0;
}

#endif

int main(int argc, char **argv)
{
    long seed = (argc > 1) ? atol(argv[1]) : 1;
    int vec = (argc > 2) && strcmp(argv[2], "vec") == 0;
    struct ts t = { 0, 20 * 1000 * 1000 };    /* 20ms */
    int round, i;

    for (round = 0; round < 20; round++) {
        for (i = 0; i < 32; i++) {
            fsrc[i] = (uint64_t)seed * 0x100000001ULL + (uint64_t)round * 1000 + (uint64_t)i;
            fdst[i] = 0;
        }
        for (i = 0; i < 64; i++) {
            vsrc[i] = ((uint64_t)seed << 40) + (uint64_t)round * 1000 + (uint64_t)i;
            vdst[i] = 0;
        }
#if defined(__aarch64__)
        if (vec) {
            for (i = 0; i < 32; i++) {
                fsrc[i] = vsrc[2 * i];  /* d レジスタは q レジスタの下 64 ビット */
            }
        }
#endif
        if (vec) {
            round_vec(&t);
        } else {
            round_fp(&t);
        }
        if (memcmp(fsrc, fdst, sizeof(fsrc)) != 0) {
            printf("fpvec %ld: FP registers changed in round %d\n", seed, round);
            return 1;
        }
        if (vec && memcmp(vsrc, vdst, sizeof(vsrc)) != 0) {
            printf("fpvec %ld: vector registers changed in round %d\n", seed, round);
            return 1;
        }
    }
    printf("fpvec %ld: 20 rounds, registers kept (%s)\n", seed, vec ? "FP + vector" : "FP");
    return 0;
}
