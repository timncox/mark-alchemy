/*
 * pthread.h stub for the vendored Mark engine on bare-metal Daisy.
 *
 * mark_core.c uses threads for two Move-only features: session save/load
 * (WAV files under a directory) and hosting external Schwung FX modules
 * (dlopen). Neither exists on the Alchemy Lab. Rather than fork the engine,
 * this directory shadows the four POSIX headers it includes (-Isrc/stubs is
 * searched before the toolchain's own), and host_stubs.c provides the
 * functions so that every thread creation FAILS cleanly: the engine's own
 * error paths then leave sessions and external FX inert.
 *
 * newlib already defines pthread_t, pthread_attr_t and struct sched_param
 * (sys/_pthreadtypes.h, pulled in by sys/types.h) even on this target; what
 * its <pthread.h> withholds without _POSIX_THREADS is the function
 * declarations, so those are all this header adds.
 *
 * Safe for the rest of the build: the Daisy toolchain's thread model is
 * "single" (arm-none-eabi-gcc -v), so nothing in libstdc++ or the SDK pulls
 * the real <pthread.h>.
 */
#ifndef MARK_ALCHEMY_STUB_PTHREAD_H
#define MARK_ALCHEMY_STUB_PTHREAD_H

#include <sys/types.h>
#include <sched.h>

#ifdef __cplusplus
extern "C" {
#endif

int       pthread_create(pthread_t* t, const pthread_attr_t* a,
                         void* (*fn)(void*), void* arg);
int       pthread_join(pthread_t t, void** ret);
pthread_t pthread_self(void);
int       pthread_setschedparam(pthread_t t, int policy,
                                const struct sched_param* p);

#ifdef __cplusplus
}
#endif

#endif /* MARK_ALCHEMY_STUB_PTHREAD_H */
