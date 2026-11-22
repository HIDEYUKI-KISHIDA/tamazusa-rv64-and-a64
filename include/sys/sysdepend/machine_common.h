/*
 * machine_common.h (overlay) — add virt RV64 to upstream router.
 */

#ifndef __SYS_MACHINE_COMMON_H__
#define __SYS_MACHINE_COMMON_H__

#include_next <sys/sysdepend/machine_common.h>

#ifdef _TEF_VIRT_RV64_
#include <sys/sysdepend/tef_virt_rv64/machine_depend.h>
#endif
#if defined(_TEF_VIRT_A64_) || TEF_VIRT_A64
#include <sys/sysdepend/tef_virt_a64/machine_depend.h>
#endif

#endif
