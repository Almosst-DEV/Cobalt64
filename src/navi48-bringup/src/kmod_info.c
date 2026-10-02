//
//  kmod_info.c — hand-written kmod glue for Navi48Bringup (see RDNA4FB for the
//  rationale). Link order: <objects> -lkmodc++ kmod_info.o -lkmod.
//
#include <mach/mach_types.h>
#include <libkern/OSKextLib.h>

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);

static kern_return_t n48_kmod_start(kmod_info_t *ki, void *data) { (void)ki; (void)data; return KERN_SUCCESS; }
static kern_return_t n48_kmod_stop (kmod_info_t *ki, void *data) { (void)ki; (void)data; return KERN_SUCCESS; }

KMOD_EXPLICIT_DECL(com.navi48.bringup, "0.0.15", _start, _stop)

__private_extern__ kmod_start_func_t *_realmain = n48_kmod_start;
__private_extern__ kmod_stop_func_t  *_antimain = n48_kmod_stop;
