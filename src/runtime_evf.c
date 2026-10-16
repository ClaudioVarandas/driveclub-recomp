/* sceKernel*EventFlag: a port of shadPS4's event_flag.cpp (GPL-2.0-or-later) to C and pthreads.
 *
 * Semantics follow the console, not a convenient approximation:
 *  - the handle is a pointer to the flag and is written through the first argument;
 *  - patterns are 64-bit;
 *  - waits block on a condition variable and are woken by Set in queue order;
 *  - Clear keeps only the bits that are set in its argument (new = old & pattern);
 *  - a timeout is a pointer to microseconds and is zeroed when the wait times out.
 * Everything is guarded by the flag's own mutex, so no caller needs external locking. */
#include "runtime.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SCE_ERR(errno_) ((int32_t)(0x80020000u | (unsigned)(errno_)))
enum { E_PERM = 1, E_SRCH = 3, E_BUSY = 16, E_ACCES = 13, E_INVAL = 22, E_TIMEDOUT = 60,
       E_NAMETOOLONG = 63, E_CANCELED = 85 };

#define EVF_ATTR_PRIO 0x02
#define EVF_ATTR_MULTI 0x20
#define EVF_WAIT_AND 0x01
#define EVF_WAIT_OR 0x02
#define EVF_CLEAR_ALL 0x10
#define EVF_CLEAR_BITS 0x20

typedef struct Waiter {
    pthread_cond_t cv;
    uint64_t bits, result;
    int wait_or, clear_mode; /* clear_mode: 0 none, 1 all, 2 bits */
    int ready, canceled, deleted;
    struct Waiter *next;
} Waiter;

typedef struct EventFlag {
    pthread_mutex_t lock;
    pthread_cond_t destroy_cv;
    Waiter *head, *tail;
    uint64_t bits;
    int multi, deleted;
    char name[32];
} EventFlag;

static int condition_met(const EventFlag *e, const Waiter *w) {
    return w->wait_or ? (e->bits & w->bits) != 0 : (e->bits & w->bits) == w->bits;
}
static void apply_clear(EventFlag *e, const Waiter *w) {
    if (w->clear_mode == 1) e->bits = 0;
    else if (w->clear_mode == 2) e->bits &= ~w->bits;
}
static void queue_add(EventFlag *e, Waiter *w) {
    w->next = NULL;
    if (e->tail) e->tail->next = w; else e->head = w;
    e->tail = w;
}
static void queue_remove(EventFlag *e, Waiter *w) {
    Waiter *prev = NULL;
    for (Waiter *it = e->head; it; prev = it, it = it->next) {
        if (it != w) continue;
        if (prev) prev->next = it->next; else e->head = it->next;
        if (e->tail == it) e->tail = prev;
        break;
    }
    if (e->deleted && !e->head) pthread_cond_signal(&e->destroy_cv);
}

static ABI int32_t evf_create(EventFlag **out, const char *name, uint32_t attr, uint64_t initial,
                              const void *option) {
    if (!out || !name) return SCE_ERR(E_INVAL);
    if (option || attr > (EVF_ATTR_MULTI | EVF_ATTR_PRIO)) return SCE_ERR(E_INVAL);
    if (strlen(name) >= sizeof(((EventFlag *)0)->name)) return SCE_ERR(E_NAMETOOLONG);
    EventFlag *e = calloc(1, sizeof(*e));
    if (!e) return SCE_ERR(12);
    pthread_mutex_init(&e->lock, NULL);
    pthread_cond_init(&e->destroy_cv, NULL);
    e->bits = initial;
    e->multi = (attr & 0xf0) == EVF_ATTR_MULTI;
    snprintf(e->name, sizeof(e->name), "%s", name);
    *out = e;
    return 0;
}
static ABI int32_t evf_delete(EventFlag *e) {
    if (!e) return SCE_ERR(E_SRCH);
    pthread_mutex_lock(&e->lock);
    e->deleted = 1;
    for (Waiter *w = e->head; w; w = w->next) { w->deleted = 1; pthread_cond_signal(&w->cv); }
    while (e->head) pthread_cond_wait(&e->destroy_cv, &e->lock);
    pthread_mutex_unlock(&e->lock);
    pthread_mutex_destroy(&e->lock);
    pthread_cond_destroy(&e->destroy_cv);
    free(e);
    return 0;
}
static ABI int32_t evf_set(EventFlag *e, uint64_t pattern) {
    if (!e) return SCE_ERR(E_SRCH);
    pthread_mutex_lock(&e->lock);
    e->bits |= pattern;
    for (Waiter *w = e->head; w; w = w->next) {
        if (w->ready || !condition_met(e, w)) continue;
        w->result = e->bits;
        apply_clear(e, w);
        w->ready = 1;
        pthread_cond_signal(&w->cv);
    }
    pthread_mutex_unlock(&e->lock);
    return 0;
}
static ABI int32_t evf_clear(EventFlag *e, uint64_t pattern) {
    if (!e) return SCE_ERR(E_SRCH);
    pthread_mutex_lock(&e->lock);
    e->bits &= pattern;
    pthread_mutex_unlock(&e->lock);
    return 0;
}
static ABI int32_t evf_cancel(EventFlag *e, uint64_t pattern, int32_t *num_waiters) {
    if (!e) return SCE_ERR(E_SRCH);
    pthread_mutex_lock(&e->lock);
    int count = 0;
    for (Waiter *w = e->head; w; w = w->next) ++count;
    if (num_waiters) *num_waiters = count;
    e->bits = pattern;
    for (Waiter *w = e->head; w; w = w->next) { w->result = pattern; w->canceled = 1; pthread_cond_signal(&w->cv); }
    pthread_mutex_unlock(&e->lock);
    return 0;
}

static int parse_mode(uint32_t mode, int *wait_or, int *clear_mode) {
    switch (mode & 0xf) {
    case EVF_WAIT_AND: *wait_or = 0; break;
    case EVF_WAIT_OR: *wait_or = 1; break;
    default: return 0;
    }
    switch (mode & 0xf0) {
    case 0x00: *clear_mode = 0; break;
    case EVF_CLEAR_ALL: *clear_mode = 1; break;
    case EVF_CLEAR_BITS: *clear_mode = 2; break;
    default: return 0;
    }
    return 1;
}

static int evf_wait_common(EventFlag *e, uint64_t pattern, uint32_t mode, uint64_t *result,
                           uint32_t *timeout_us, int poll) {
    if (!e) return SCE_ERR(E_SRCH);
    if (!pattern) return SCE_ERR(E_INVAL);
    Waiter w;
    memset(&w, 0, sizeof(w));
    if (!parse_mode(mode, &w.wait_or, &w.clear_mode)) return SCE_ERR(E_INVAL);
    w.bits = pattern;
    pthread_mutex_lock(&e->lock);
    if (e->deleted) { pthread_mutex_unlock(&e->lock); return SCE_ERR(E_ACCES); }
    if (!e->multi && e->head) { pthread_mutex_unlock(&e->lock); return SCE_ERR(E_PERM); }
    if (condition_met(e, &w)) {
        if (result) *result = e->bits;
        apply_clear(e, &w);
        pthread_mutex_unlock(&e->lock);
        return 0;
    }
    if (poll) { if (result) *result = e->bits; pthread_mutex_unlock(&e->lock); return SCE_ERR(E_BUSY); }
    pthread_cond_init(&w.cv, NULL);
    queue_add(e, &w);
    int timed_out = 0;
    if (!timeout_us) {
        while (!(w.ready || w.canceled || w.deleted)) pthread_cond_wait(&w.cv, &e->lock);
    } else {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        uint64_t us = *timeout_us;
        deadline.tv_sec += (time_t)(us / 1000000u);
        deadline.tv_nsec += (long)(us % 1000000u) * 1000;
        if (deadline.tv_nsec >= 1000000000L) { deadline.tv_nsec -= 1000000000L; ++deadline.tv_sec; }
        while (!(w.ready || w.canceled || w.deleted)) {
            if (pthread_cond_timedwait(&w.cv, &e->lock, &deadline) != 0 &&
                !(w.ready || w.canceled || w.deleted)) { timed_out = 1; break; }
        }
    }
    queue_remove(e, &w);
    int32_t status = 0;
    if (timed_out) {
        if (result) *result = e->bits;
        *timeout_us = 0;
        status = SCE_ERR(E_TIMEDOUT);
    } else {
        if (result) *result = w.result;
        if (w.canceled) status = SCE_ERR(E_CANCELED);
        else if (w.deleted) status = SCE_ERR(E_ACCES);
    }
    pthread_mutex_unlock(&e->lock);
    pthread_cond_destroy(&w.cv);
    return status;
}
static ABI int32_t evf_wait(EventFlag *e, uint64_t pattern, uint32_t mode, uint64_t *result,
                            uint32_t *timeout_us) {
    runtime_diag_wait_begin("evflag", (uint64_t)pattern, __builtin_return_address(0));
    const int32_t status = evf_wait_common(e, pattern, mode, result, timeout_us, 0);
    runtime_diag_wait_end();
    return status;
}
static ABI int32_t evf_poll(EventFlag *e, uint64_t pattern, uint32_t mode, uint64_t *result) {
    return evf_wait_common(e, pattern, mode, result, NULL, 1);
}
static ABI int32_t evf_open_close(void) { return 0; } /* named flags are not shared across processes */

uintptr_t runtime_evf_resolve(const char *name) {
    static const RuntimeExport table[] = {
        {"sceKernelCreateEventFlag", evf_create}, {"sceKernelDeleteEventFlag", evf_delete},
        {"sceKernelSetEventFlag", evf_set}, {"sceKernelClearEventFlag", evf_clear},
        {"sceKernelCancelEventFlag", evf_cancel}, {"sceKernelWaitEventFlag", evf_wait},
        {"sceKernelPollEventFlag", evf_poll}, {"sceKernelOpenEventFlag", evf_open_close},
        {"sceKernelCloseEventFlag", evf_open_close},
    };
    return RUNTIME_LOOKUP(table, name);
}
