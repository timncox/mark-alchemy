/*
 * dirent.h stub -- see pthread.h in this directory. newlib's sys/dirent.h
 * on this target is an #error. opendir() always returns NULL, so the
 * engine's FX-catalog scan finds nothing and sessions never list.
 */
#ifndef MARK_ALCHEMY_STUB_DIRENT_H
#define MARK_ALCHEMY_STUB_DIRENT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mark_stub_dir DIR;

struct dirent
{
    char d_name[256];
};

DIR*           opendir(const char* path);
struct dirent* readdir(DIR* d);
int            closedir(DIR* d);

#ifdef __cplusplus
}
#endif

#endif /* MARK_ALCHEMY_STUB_DIRENT_H */
