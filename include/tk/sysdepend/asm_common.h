#ifndef __TK_ASM_COMMON_H__
#define __TK_ASM_COMMON_H__

#include_next <tk/sysdepend/asm_common.h>

#if TEF_VIRT_RV64
#include <tk/sysdepend/tef_virt_rv64/asm_depend.h>
#endif
#if defined(_TEF_VIRT_A64_) || TEF_VIRT_A64
#include <tk/sysdepend/tef_virt_a64/asm_depend.h>
#endif

#endif
