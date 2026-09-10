/*
 * The POSIX surface the vendored Mark engine links against, on a board with
 * no OS: threads, dynamic loading, directories, and the two file-system
 * calls newlib's nosys layer does not cover.
 *
 * Every entry point FAILS the way its real counterpart fails when the
 * resource is unavailable, so the engine's own error handling does the rest:
 *   - pthread_create -> EAGAIN: no session worker, no FX loader thread;
 *     mark_set_param("save_session") drops the job and reports io_error.
 *   - dlopen -> NULL: no external FX module is ever hosted.
 *   - opendir -> NULL: the FX catalog is empty, sessions list as absent.
 *   - mkdir / rmdir -> -1: a session directory can never be made.
 *   - usleep -> returns at once: only the FX loader's wait loops call it,
 *     and that thread never starts.
 * fopen/stat/unlink go through newlib to nosys.specs and fail with -1 too.
 *
 * See stubs/pthread.h for why this is done with stubs rather than an
 * engine edit.
 */
#include <errno.h>
#include <stddef.h>
#include <sys/types.h>
#include <unistd.h>

#include "stubs/pthread.h"
#include "stubs/dlfcn.h"
#include "stubs/dirent.h"

int pthread_create(pthread_t* t, const pthread_attr_t* a, void* (*fn)(void*), void* arg)
{
    (void)t; (void)a; (void)fn; (void)arg;
    return EAGAIN;
}

int pthread_join(pthread_t t, void** ret)
{
    (void)t;
    if (ret) *ret = NULL;
    return 0;
}

pthread_t pthread_self(void) { return 0; }

int pthread_setschedparam(pthread_t t, int policy, const struct sched_param* p)
{
    (void)t; (void)policy; (void)p;
    return 0;
}

void*       dlopen(const char* path, int flags) { (void)path; (void)flags; return NULL; }
void*       dlsym(void* handle, const char* name) { (void)handle; (void)name; return NULL; }
int         dlclose(void* handle) { (void)handle; return 0; }
const char* dlerror(void) { return "dynamic loading is not available on this board"; }

DIR*           opendir(const char* path) { (void)path; errno = ENOENT; return NULL; }
struct dirent* readdir(DIR* d) { (void)d; return NULL; }
int            closedir(DIR* d) { (void)d; return 0; }

int mkdir(const char* path, mode_t mode) { (void)path; (void)mode; errno = ENOSYS; return -1; }
int rmdir(const char* path) { (void)path; errno = ENOSYS; return -1; }

int usleep(useconds_t us) { (void)us; return 0; }
