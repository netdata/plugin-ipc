#ifndef TEST_LOCK_CLEANUP_H
#define TEST_LOCK_CLEANUP_H

/* Test-only cleanup of endpoint lock files. Lock files stay after a server
 * closes, by design (docs/level1-posix-uds.md, "Endpoint lock"). Test
 * service names are unique per run, so without this every run would leave
 * new locks behind: the test process removes the locks it named at exit. */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t g_test_locks_mu = PTHREAD_MUTEX_INITIALIZER;
static char **g_test_locks = NULL;
static size_t g_test_locks_len = 0;
static size_t g_test_locks_cap = 0;
static pid_t g_test_locks_owner = 0;

static void test_locks_remove(void)
{
    /* A forked child inherits the handler; only the process that named the
     * services removes their locks. */
    if (getpid() != g_test_locks_owner)
        return;
    for (size_t i = 0; i < g_test_locks_len; i++) {
        unlink(g_test_locks[i]);
        free(g_test_locks[i]);
    }
    free(g_test_locks);
    g_test_locks = NULL;
    g_test_locks_len = 0;
}

/* Records {dir}/{service}.lock for removal at process exit. */
static void test_locks_track(const char *dir, const char *service)
{
    pthread_mutex_lock(&g_test_locks_mu);
    if (g_test_locks_owner == 0) {
        g_test_locks_owner = getpid();
        atexit(test_locks_remove);
    }
    if (g_test_locks_len == g_test_locks_cap) {
        size_t cap = g_test_locks_cap ? g_test_locks_cap * 2 : 64;
        char **grown = realloc(g_test_locks, cap * sizeof(*grown));
        if (!grown) {
            pthread_mutex_unlock(&g_test_locks_mu);
            return;
        }
        g_test_locks = grown;
        g_test_locks_cap = cap;
    }
    size_t len = strlen(dir) + strlen(service) + sizeof("/.lock");
    char *path = malloc(len);
    if (path) {
        snprintf(path, len, "%s/%s.lock", dir, service);
        g_test_locks[g_test_locks_len++] = path;
    }
    pthread_mutex_unlock(&g_test_locks_mu);
}

#endif
