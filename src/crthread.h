#ifndef CRTHREAD_H
#define CRTHREAD_H

// The mutex Carrottus's own code locks, written once so that every module and
// backend that has to serialize something does it the same way. Private:
// nothing here is in a public header, and Carrottus still starts no thread of
// its own. A cr_mutex is never copied: it lives in an object that is only
// ever reached through a pointer.
//
// On Windows it is an SRWLOCK, which needs no destroy and can't fail to be
// made. Elsewhere it is a pthread mutex, whose init can fail: a create call
// that makes one fails with CR_RESULT_GENERAL_FAILURE when it does.
//
// Not recursive. Nothing that holds one calls back into the caller.

#include <stdbool.h>

#if CARROTTUS_PLATFORM_WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef struct cr_mutex {
	SRWLOCK lock;
} cr_mutex;

static inline bool cr_mutex_init(cr_mutex *mutex) {
	InitializeSRWLock(&mutex->lock);
	return true;
}

static inline void cr_mutex_destroy(cr_mutex *mutex) { (void)mutex; }

static inline void cr_mutex_lock(cr_mutex *mutex) { AcquireSRWLockExclusive(&mutex->lock); }

static inline void cr_mutex_unlock(cr_mutex *mutex) { ReleaseSRWLockExclusive(&mutex->lock); }

#else
#include <pthread.h>

typedef struct cr_mutex {
	pthread_mutex_t lock;
} cr_mutex;

static inline bool cr_mutex_init(cr_mutex *mutex) { return pthread_mutex_init(&mutex->lock, NULL) == 0; }

static inline void cr_mutex_destroy(cr_mutex *mutex) { pthread_mutex_destroy(&mutex->lock); }

static inline void cr_mutex_lock(cr_mutex *mutex) { pthread_mutex_lock(&mutex->lock); }

static inline void cr_mutex_unlock(cr_mutex *mutex) { pthread_mutex_unlock(&mutex->lock); }

#endif

#endif // CRTHREAD_H
