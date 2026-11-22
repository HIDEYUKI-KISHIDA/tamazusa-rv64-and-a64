#ifndef _CPU_INSN_
#define _CPU_INSN_
/* AArch64: 割込みの禁止・許可（DAIF の I）とタスク独立部 */

#include <tk/typedef.h>
#include <sys/sysinfo.h>

#define VECNO_DEFAULT   EIT_DEFAULT

typedef struct monhdr {
    FP default_hdr;
} MONHDR;

IMPORT MONHDR SaveMonHdr;

Inline void define_inthdr(INT vecno, FP inthdr)
{
    (void)vecno;
    (void)inthdr;
}

Inline BOOL isTaskIndependent(void)
{
    return (SCInfo.taskindp > 0) ? TRUE : FALSE;
}

Inline void EnterTaskIndependent(void)
{
    SCInfo.taskindp++;
}

Inline void LeaveTaskIndependent(void)
{
    SCInfo.taskindp--;
}

#define A64_DAIF_I  0x80u

Inline UINT disint(void)
{
    UINT daif;
    asm volatile("mrs %0, daif" : "=r"(daif));
    asm volatile("msr daifset, #2" ::: "memory");
    return daif;
}

Inline void enaint(UINT daif_saved)
{
    /* 割込み許可（I）だけを元に戻す */
    if (daif_saved & A64_DAIF_I) {
        asm volatile("msr daifset, #2" ::: "memory");
    } else {
        asm volatile("msr daifclr, #2" ::: "memory");
    }
}

Inline BOOL isDI(UINT daif)
{
    return ((daif & A64_DAIF_I) != 0) ? TRUE : FALSE;
}

#endif
