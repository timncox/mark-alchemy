/*
 * dlfcn.h stub -- see pthread.h in this directory. The toolchain has no
 * dlfcn.h at all. dlopen() always fails, so the engine never hosts an
 * external FX module here; its built-in track FX are unaffected.
 */
#ifndef MARK_ALCHEMY_STUB_DLFCN_H
#define MARK_ALCHEMY_STUB_DLFCN_H

#define RTLD_NOW   2
#define RTLD_LOCAL 0

#ifdef __cplusplus
extern "C" {
#endif

void*       dlopen(const char* path, int flags);
void*       dlsym(void* handle, const char* name);
int         dlclose(void* handle);
const char* dlerror(void);

#ifdef __cplusplus
}
#endif

#endif /* MARK_ALCHEMY_STUB_DLFCN_H */
