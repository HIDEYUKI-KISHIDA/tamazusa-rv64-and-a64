#ifndef __TK_SYSDEF_COMMON_H__
#define __TK_SYSDEF_COMMON_H__

#include_next <tk/sysdepend/sysdef_common.h>

#if TEF_VIRT_RV64
#include <tk/sysdepend/tef_virt_rv64/sysdef_depend.h>
#endif
#if defined(_TEF_VIRT_A64_) || TEF_VIRT_A64
#include <tk/sysdepend/tef_virt_a64/sysdef_depend.h>
#endif

#endif
