// hand-written kmod glue (same scheme as src/navi48-bringup/src/kmod_info.c and tools/native/e3auxkc). Link order: objs -lkmodc++ kmod_info.o -lkmod.
// _start/_stop do nothing: the kext's classes are registered by their static OSMetaClass constructors and everything else runs from probe/start.
#include <mach/mach_types.h>
#include <libkern/OSKextLib.h>
extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);
static kern_return_t accel_start(kmod_info_t *ki, void *data) { (void)ki; (void)data; return KERN_SUCCESS; }
static kern_return_t accel_stop (kmod_info_t *ki, void *data) { (void)ki; (void)data; return KERN_SUCCESS; }
KMOD_EXPLICIT_DECL(com.navi48.accelprobe, "0.0.3", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = accel_start;
__private_extern__ kmod_stop_func_t  *_antimain = accel_stop;
