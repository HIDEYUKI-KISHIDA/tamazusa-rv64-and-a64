#ifndef _CPU_INSN_
#define _CPU_INSN_

#include <tk/typedef.h>
#include <sys/sysinfo.h>
#include "rv_priv.h"

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

Inline UINT disint(void)
{
    UINT st;
    asm volatile("csrr %0, " RV_STR(CSR_XSTATUS) : "=r"(st));
    asm volatile("csrc " RV_STR(CSR_XSTATUS) ", %0" :: "r"((unsigned long)XSTATUS_IE));
    return st;
}

Inline void enaint(UINT mstatus_saved)
{
    /* 割込み許可ビットだけを復元する（他のステータスビットは触らない） */
    if (mstatus_saved & XSTATUS_IE) {
        asm volatile("csrs " RV_STR(CSR_XSTATUS) ", %0" :: "r"((unsigned long)XSTATUS_IE) : "memory");
    } else {
        asm volatile("csrc " RV_STR(CSR_XSTATUS) ", %0" :: "r"((unsigned long)XSTATUS_IE) : "memory");
    }
}

Inline BOOL isDI(UINT mstatus)
{
    return ((mstatus & XSTATUS_IE) == 0) ? TRUE : FALSE;
}

#endif
