/*
 * sched.h stub -- see pthread.h in this directory. newlib's sys/sched.h
 * defines struct sched_param on every target; SCHED_OTHER only appears with
 * _POSIX_PRIORITY_SCHEDULING, which bare metal does not set.
 */
#ifndef MARK_ALCHEMY_STUB_SCHED_H
#define MARK_ALCHEMY_STUB_SCHED_H

#include <sys/sched.h>

#ifndef SCHED_OTHER
#define SCHED_OTHER 0
#endif

#endif /* MARK_ALCHEMY_STUB_SCHED_H */
