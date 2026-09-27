/* Exercise the public SHM wait path on native and cross-compiled Linux ABIs. */
#define _GNU_SOURCE
#include "netipc/netipc_shm.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef NIPC_TEST_REQUIRE_TIME64_32
_Static_assert(sizeof(void *) == 4, "test must exercise a 32-bit ABI");
_Static_assert(sizeof(time_t) == 8, "test must exercise time64 libc");
#endif

static uint64_t clock_ns(clockid_t clock)
{
    struct timespec ts;
    if (clock_gettime(clock, &ts) != 0)
        abort();
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int idle_wait(nipc_shm_ctx_t *receiver, uint32_t timeout_ms)
{
    char buf[32];
    size_t len = 0;
    uint64_t cpu = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
    uint64_t start = clock_ns(CLOCK_MONOTONIC);
    nipc_shm_error_t err = nipc_shm_receive(receiver, buf, sizeof(buf), &len, timeout_ms);
    uint64_t elapsed = clock_ns(CLOCK_MONOTONIC) - start;
    cpu = clock_ns(CLOCK_PROCESS_CPUTIME_ID) - cpu;
    printf("idle %u ms: elapsed %.3f ms, CPU %.3f ms, result %d\n",
           timeout_ms, elapsed / 1e6, cpu / 1e6, err);
    /* Loose upper/CPU bounds tolerate slow CI and emulation; lower bound catches
     * immediate expiry and loss of the subsecond part of a longer timeout. */
    return err == NIPC_SHM_ERR_TIMEOUT &&
           elapsed >= (uint64_t)timeout_ms * 1000000ULL &&
           elapsed < (uint64_t)(timeout_ms + 5000) * 1000000ULL &&
           cpu < elapsed / 2;
}

static int wake_wait(nipc_shm_ctx_t *receiver, nipc_shm_ctx_t *sender,
                     uint32_t timeout_ms)
{
    const char message[] = "delayed peer message";
    pid_t pid = fork();
    if (pid < 0)
        return 0;
    if (pid == 0) {
        usleep(50000);
        _exit(nipc_shm_send(sender, message, sizeof(message)) == NIPC_SHM_OK ? 0 : 1);
    }
    char buf[64];
    size_t len = 0;
    nipc_shm_error_t err = nipc_shm_receive(receiver, buf, sizeof(buf), &len, timeout_ms);
    int status = 0;
    int waited = waitpid(pid, &status, 0) == pid;
    printf("wake %u ms: result %d, length %zu\n", timeout_ms, err, len);
    return waited && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
           err == NIPC_SHM_OK && len == sizeof(message) &&
           memcmp(buf, message, sizeof(message)) == 0;
}

int main(void)
{
    /* Also bounds an accidentally infinite wait when run directly. */
    alarm(20);
    printf("ABI: pointer=%zu time_t=%zu timespec=%zu nsec_offset=%zu\n",
           sizeof(void *), sizeof(time_t), sizeof(struct timespec),
           offsetof(struct timespec, tv_nsec));
    char dir[] = "/tmp/nipc_timeout_XXXXXX";
    if (!mkdtemp(dir))
        return 1;
    nipc_shm_ctx_t server, client;
    if (nipc_shm_server_create(dir, "timeout", 1, 1024, 1024, &server) != NIPC_SHM_OK) {
        rmdir(dir);
        return 1;
    }
    if (nipc_shm_client_attach(dir, "timeout", 1, &client) != NIPC_SHM_OK) {
        nipc_shm_destroy(&server);
        rmdir(dir);
        return 1;
    }
    server.spin_tries = client.spin_tries = 0;
    int ok = idle_wait(&server, 100);
    ok &= idle_wait(&client, 100);
    ok &= idle_wait(&server, 1100);
    ok &= wake_wait(&server, &client, 1000);
    ok &= wake_wait(&server, &client, 0);
    ok &= wake_wait(&server, &client, UINT32_MAX);
    nipc_shm_close(&client);
    nipc_shm_destroy(&server);
    rmdir(dir);
    puts(ok ? "PASS: SHM timeout ABI" : "FAIL: SHM timeout ABI");
    return ok ? 0 : 1;
}
