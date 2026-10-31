/*
 * test_sync.h - deterministic synchronization for the C test fixtures.
 *
 * Tests wait on events, never on sleeps: a thread raises a flag or a server
 * reports an event, and the waiter blocks on a condition variable until it
 * happens. TEST_WAIT_LIMIT_MS only bounds waits that expire when the code
 * under test is broken; reaching it fails the test.
 */

#ifndef NETIPC_TEST_SYNC_H
#define NETIPC_TEST_SYNC_H

#include "netipc/netipc_service.h"

#include <stdbool.h>
#include <stdint.h>

#if defined(_WIN32) || defined(__MSYS__)
#include <windows.h>
#ifdef __MSYS__
#include <pthread.h>
#include <stdlib.h>
#endif
#else
#include <pthread.h>
#include <time.h>
#endif

#define TEST_WAIT_LIMIT_MS 10000u

/* Server waits for tests: timeout paths run in milliseconds while staying
 * well above scheduling noise. */
#define TEST_SERVER_POLL_MS 10u
#define TEST_SERVER_STOP_GRACE_MS 200u
#define TEST_SERVER_HANDSHAKE_MS 500u
#define TEST_CLIENT_SHM_ATTACH_TIMEOUT_MS 300u

/* One process-wide lock and condition variable back every flag and event
 * counter: tests are small, and a single broadcast keeps them simple. */
#if defined(_WIN32) || defined(__MSYS__)
static SRWLOCK g_test_sync_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_test_sync_cond = CONDITION_VARIABLE_INIT;

static inline void test_sync_lock(void) { AcquireSRWLockExclusive(&g_test_sync_lock); }
static inline void test_sync_unlock(void) { ReleaseSRWLockExclusive(&g_test_sync_lock); }
static inline void test_sync_broadcast(void) { WakeAllConditionVariable(&g_test_sync_cond); }

/* Wait for a broadcast until deadline_ms (GetTickCount64 clock). Returns
 * false once the deadline passed. */
static inline bool test_sync_wait_until(uint64_t deadline_ms)
{
    uint64_t now = GetTickCount64();
    if (now >= deadline_ms)
        return false;
    SleepConditionVariableSRW(&g_test_sync_cond, &g_test_sync_lock,
                              (DWORD)(deadline_ms - now), 0);
    return true;
}

static inline uint64_t test_sync_deadline(void)
{
    return GetTickCount64() + TEST_WAIT_LIMIT_MS;
}
#else
static pthread_mutex_t g_test_sync_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_test_sync_cond = PTHREAD_COND_INITIALIZER;

static inline void test_sync_lock(void) { pthread_mutex_lock(&g_test_sync_lock); }
static inline void test_sync_unlock(void) { pthread_mutex_unlock(&g_test_sync_lock); }
static inline void test_sync_broadcast(void) { pthread_cond_broadcast(&g_test_sync_cond); }

static inline uint64_t test_sync_realtime_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static inline bool test_sync_wait_until(uint64_t deadline_ms)
{
    if (test_sync_realtime_ms() >= deadline_ms)
        return false;
    struct timespec ts = {
        .tv_sec = (time_t)(deadline_ms / 1000u),
        .tv_nsec = (long)(deadline_ms % 1000u) * 1000000L,
    };
    pthread_cond_timedwait(&g_test_sync_cond, &g_test_sync_lock, &ts);
    return true;
}

static inline uint64_t test_sync_deadline(void)
{
    return test_sync_realtime_ms() + TEST_WAIT_LIMIT_MS;
}
#endif

#if defined(_WIN32) || defined(__MSYS__)
#ifdef __MSYS__
/* The MSYS runtime does not support threads started with CreateThread: under
 * load such a thread may never run and the process dies silently. Start test
 * threads with pthread_create and hand back an event that is signaled when
 * the thread function returns, so callers keep using WaitForSingleObject and
 * CloseHandle. */
typedef struct {
    LPTHREAD_START_ROUTINE fn;
    void *arg;
    HANDLE done;
} test_thread_start_t;

static void *test_thread_trampoline(void *p)
{
    test_thread_start_t start = *(test_thread_start_t *)p;
    free(p);
    start.fn(start.arg);
    SetEvent(start.done);
    CloseHandle(start.done);
    return NULL;
}

static inline HANDLE test_create_thread(LPSECURITY_ATTRIBUTES attrs, SIZE_T stack,
                                        LPTHREAD_START_ROUTINE fn, void *arg,
                                        DWORD flags, LPDWORD id)
{
    (void)attrs;
    (void)stack;
    (void)flags;
    if (id)
        *id = 0;
    HANDLE done = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!done)
        return NULL;
    test_thread_start_t *start = malloc(sizeof(*start));
    /* The thread signals and closes its own copy, so the caller may close
     * the returned handle at any time. */
    if (!start || !DuplicateHandle(GetCurrentProcess(), done, GetCurrentProcess(),
                                   &start->done, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        free(start);
        CloseHandle(done);
        return NULL;
    }
    start->fn = fn;
    start->arg = arg;
    pthread_t thread;
    if (pthread_create(&thread, NULL, test_thread_trampoline, start) != 0) {
        CloseHandle(start->done);
        free(start);
        CloseHandle(done);
        return NULL;
    }
    pthread_detach(thread);
    return done;
}
#else
#define test_create_thread CreateThread
#endif
#endif

/* Raise a flag that another thread waits for. */
static inline void test_flag_set(int *flag)
{
    test_sync_lock();
    __atomic_store_n(flag, 1, __ATOMIC_RELEASE);
    test_sync_broadcast();
    test_sync_unlock();
}

/* Wait until *a or (when given) *b is raised. Returns false on the limit. */
static inline bool test_flag_wait_any(int *a, int *b)
{
    uint64_t deadline = test_sync_deadline();
    bool ok = true;
    test_sync_lock();
    while (!__atomic_load_n(a, __ATOMIC_ACQUIRE) &&
           !(b && __atomic_load_n(b, __ATOMIC_ACQUIRE))) {
        if (!test_sync_wait_until(deadline)) {
            ok = false;
            break;
        }
    }
    test_sync_unlock();
    return ok;
}

static inline bool test_flag_wait(int *flag)
{
    return test_flag_wait_any(flag, NULL);
}

/* Count an occurrence another thread waits for. */
static inline void test_counter_add(int *counter)
{
    test_sync_lock();
    __atomic_add_fetch(counter, 1, __ATOMIC_ACQ_REL);
    test_sync_broadcast();
    test_sync_unlock();
}

/* Wait until *counter reaches n. Returns false on the limit. */
static inline bool test_counter_wait(int *counter, int n)
{
    uint64_t deadline = test_sync_deadline();
    bool ok = true;
    test_sync_lock();
    while (__atomic_load_n(counter, __ATOMIC_ACQUIRE) < n) {
        if (!test_sync_wait_until(deadline)) {
            ok = false;
            break;
        }
    }
    test_sync_unlock();
    return ok;
}

#ifdef NIPC_INTERNAL_TESTING
/* Counts managed-server events; waits consume them like a channel. */
typedef struct {
    int handshakes_started;
    int handshakes_consumed;
    int sessions_exited;
    int exits_consumed;
} test_server_events_t;

static inline void test_server_event_hook(void *user, int event)
{
    test_server_events_t *ev = (test_server_events_t *)user;
    test_sync_lock();
    if (event == NIPC_SERVER_TEST_EVENT_HANDSHAKE_STARTED)
        ev->handshakes_started++;
    else if (event == NIPC_SERVER_TEST_EVENT_SESSION_EXITED)
        ev->sessions_exited++;
    test_sync_broadcast();
    test_sync_unlock();
}

static inline bool test_server_events_wait(int *count, int *consumed, int n)
{
    uint64_t deadline = test_sync_deadline();
    bool ok = true;
    test_sync_lock();
    while (*count - *consumed < n) {
        if (!test_sync_wait_until(deadline)) {
            ok = false;
            break;
        }
    }
    if (ok)
        *consumed += n;
    test_sync_unlock();
    return ok;
}

/* Wait until n session workers exited and freed their slots. */
static inline bool test_server_wait_session_exits(test_server_events_t *ev, int n)
{
    return test_server_events_wait(&ev->sessions_exited, &ev->exits_consumed, n);
}

/* Wait until n workers are waiting for HELLO. */
static inline bool test_server_wait_handshakes(test_server_events_t *ev, int n)
{
    return test_server_events_wait(&ev->handshakes_started,
                                   &ev->handshakes_consumed, n);
}

/* Switch an initialized, not yet running server to test timings; report its
 * events to ev when given. */
static inline void test_server_use_test_timings(nipc_managed_server_t *server,
                                                test_server_events_t *ev)
{
    nipc_server_set_timings_for_tests(server, TEST_SERVER_POLL_MS,
                                      TEST_SERVER_STOP_GRACE_MS,
                                      TEST_SERVER_HANDSHAKE_MS);
    if (ev)
        nipc_server_set_event_hook_for_tests(server, test_server_event_hook, ev);
}
#endif /* NIPC_INTERNAL_TESTING */

#endif /* NETIPC_TEST_SYNC_H */
