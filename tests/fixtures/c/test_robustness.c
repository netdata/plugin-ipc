/*
 * test_robustness.c - Robustness reproduction tests (SOW-0040), POSIX.
 *
 * Each test asserts the correct behavior. Every blocking step runs on a
 * helper thread joined with a deadline, so a regression fails instead of
 * hanging. Bounds are deliberately lenient so they do not dictate a fix.
 * Tests wait on server events and peer signals, never on sleeps.
 *
 * Tests:
 *   K1  SHM sessions released after clean close (guard: C is correct)
 *   K1  SHM sessions released after client crash (guard: C is correct)
 *   K3  silent client does not block other clients
 *   K3  silent client does not block shutdown
 *   K3  silent client's worker is freed at the server handshake deadline
 *   K4  client bounded when server never accepts
 *   K4  client bounded when server never sends HELLO_ACK
 *   K4  a call's reconnect is bounded by the call deadline
 *   K4  abort ends a refresh waiting for HELLO_ACK
 *   N1  call honors a timeout longer than 100 ms (guard: C is correct)
 *   N2  shutdown not blocked by a client that stopped reading
 *   N3  stop before run is honored
 *   N5  SHM client notices a crashed server before its timeout
 *   N5  baseline client reports a crashed server as disconnected
 *   N5  SHM attach stops when the server closes the session
 *   CLOEXEC  every NetIPC socket and pipe is close-on-exec
 *   N11 default packet size carries a message larger than SO_SNDBUF
 *       (plus an explicit-packet-size control)
 *
 * The binary re-executes itself with --helper-shm-client / --helper-shm-server /
 * --helper-uds-server to simulate a peer process that is killed with SIGKILL mid-session.
 */

#define _GNU_SOURCE
#include "netipc/netipc_service.h"
#include "netipc/netipc_protocol.h"
#include "netipc/netipc_uds.h"
#include "netipc/netipc_shm.h"
#include "test_sync.h"
#include "uds_test_socket.h"

#include <dirent.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#if defined(__FreeBSD__)
#include <pthread_np.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TEST_RUN_DIR      "/tmp/nipc_robustness_test"
#define AUTH_TOKEN        0xDEADBEEFCAFEBABEull
#define RESPONSE_BUF_SIZE 65536

/* Absolute path of this binary, for re-executing it in helper mode. */
static char g_self_path[PATH_MAX];

static int g_pass = 0;
static int g_fail = 0;

static void check(const char *name, int cond)
{
    if (cond) {
        printf("  PASS: %s\n", name);
        g_pass++;
    } else {
        printf("  FAIL: %s\n", name);
        g_fail++;
    }
    fflush(stdout);
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void sleep_ms(unsigned ms)
{
    usleep(ms * 1000u);
}

static void cleanup_all(const char *service)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.sock", TEST_RUN_DIR, service);
    unlink(path);
    nipc_shm_cleanup_stale(TEST_RUN_DIR, service);
}

/* Join with a deadline, portably (macOS has no pthread_timedjoin_np): a
 * helper thread joins the target and signals. Returns true if the thread
 * finished in time; otherwise the join stays pending and join_rest()
 * completes it. */
typedef struct {
    pthread_t target;
    pthread_t helper;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool done;
} timed_join_t;

#define MAX_PENDING_JOINS 8
static timed_join_t *g_pending_joins[MAX_PENDING_JOINS];

static void *timed_join_helper(void *arg)
{
    timed_join_t *j = arg;
    pthread_join(j->target, NULL);
    pthread_mutex_lock(&j->lock);
    j->done = true;
    pthread_cond_signal(&j->cond);
    pthread_mutex_unlock(&j->lock);
    return NULL;
}

static void timed_join_free(timed_join_t *j)
{
    pthread_join(j->helper, NULL);
    pthread_cond_destroy(&j->cond);
    pthread_mutex_destroy(&j->lock);
    free(j);
}

static bool join_within(pthread_t tid, unsigned limit_ms)
{
    timed_join_t *j = calloc(1, sizeof(*j));
    if (!j)
        abort();
    j->target = tid;
    pthread_mutex_init(&j->lock, NULL);
    pthread_cond_init(&j->cond, NULL);
    pthread_create(&j->helper, NULL, timed_join_helper, j);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += limit_ms / 1000u;
    ts.tv_nsec += (long)(limit_ms % 1000u) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&j->lock);
    while (!j->done) {
        if (pthread_cond_timedwait(&j->cond, &j->lock, &ts) != 0)
            break;
    }
    bool done = j->done;
    pthread_mutex_unlock(&j->lock);

    if (done) {
        timed_join_free(j);
        return true;
    }
    for (int i = 0; i < MAX_PENDING_JOINS; i++) {
        if (!g_pending_joins[i]) {
            g_pending_joins[i] = j;
            return false;
        }
    }
    abort();
}

/* Finish the join of a thread whose join_within() timed out. */
static void join_rest(pthread_t tid)
{
    for (int i = 0; i < MAX_PENDING_JOINS; i++) {
        timed_join_t *j = g_pending_joins[i];
        if (j && pthread_equal(j->target, tid)) {
            g_pending_joins[i] = NULL;
            timed_join_free(j);
            return;
        }
    }
    abort();
}

#if defined(__linux__)
static int count_shm_files(const char *service)
{
    DIR *d = opendir(TEST_RUN_DIR);
    if (!d)
        return 0;
    size_t plen = strlen(service);
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len > plen + 7 && strncmp(e->d_name, service, plen) == 0 &&
            e->d_name[plen] == '-' && strcmp(e->d_name + len - 7, ".ipcshm") == 0)
            n++;
    }
    closedir(d);
    return n;
}
#endif

/* ------------------------------------------------------------------ */
/*  Server                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned delay_ms;   /* handler delay */
    size_t   raw_len;    /* if non-zero, reply with raw_len opaque bytes */
    bool     block;      /* report entry on stdout, then block until killed */
    int      handled;    /* __atomic: raised once a raw reply is built */
} handler_opts_t;

static nipc_error_t robust_handler(void *user, const nipc_header_t *request_hdr,
                                   const uint8_t *request_payload, size_t request_len,
                                   uint8_t *response_buf, size_t response_buf_size,
                                   size_t *response_len_out)
{
    handler_opts_t *opts = (handler_opts_t *)user;
    (void)request_hdr;

    if (opts && opts->block) {
        printf("HELPER_HANDLER_ENTERED\n");
        fflush(stdout);
        for (;;)
            pause();
    }

    /* N1 stimulus: a handler slower than the 100 ms abort-poll slice. */
    if (opts && opts->delay_ms)
        sleep_ms(opts->delay_ms);

    if (opts && opts->raw_len) {
        if (opts->raw_len > response_buf_size)
            return NIPC_ERR_OVERFLOW;
        memset(response_buf, 0, opts->raw_len);
        *response_len_out = opts->raw_len;
        test_flag_set(&opts->handled);
        return NIPC_OK;
    }

    nipc_cgroups_req_t req;
    nipc_error_t err = nipc_cgroups_req_decode(request_payload, request_len, &req);
    if (err != NIPC_OK)
        return err;

    nipc_cgroups_builder_t builder;
    nipc_cgroups_builder_init(&builder, response_buf, response_buf_size, 1, 1, 42);
    err = nipc_cgroups_builder_add(&builder, 1001, 0, 1, "rob-cg", 6, "/sys/fs/cgroup/rob", 18);
    if (err != NIPC_OK)
        return err;
    *response_len_out = nipc_cgroups_builder_finish(&builder);
    return (*response_len_out > 0) ? NIPC_OK : NIPC_ERR_OVERFLOW;
}

typedef struct {
    const char *service;
    int workers;
    bool shm;
    uint32_t max_payload;
    uint32_t packet_size;
    handler_opts_t opts;
    nipc_managed_server_t server;
    test_server_events_t events;
    pthread_t tid;
} robust_server_t;

static nipc_uds_server_config_t robust_server_config(const robust_server_t *rs)
{
    nipc_uds_server_config_t scfg = {
        .supported_profiles         = NIPC_PROFILE_BASELINE |
                                      (rs->shm ? NIPC_PROFILE_SHM_HYBRID : 0),
        .preferred_profiles         = rs->shm ? NIPC_PROFILE_SHM_HYBRID : 0,
        .max_request_payload_bytes  = rs->max_payload ? rs->max_payload : 4096,
        .max_request_batch_items    = 1,
        .max_response_payload_bytes = rs->max_payload ? rs->max_payload : RESPONSE_BUF_SIZE,
        .max_response_batch_items   = 1,
        .auth_token                 = AUTH_TOKEN,
        .packet_size                = rs->packet_size,
        .backlog                    = 16,
    };
    return scfg;
}

static void *robust_server_fn(void *arg)
{
    robust_server_t *rs = (robust_server_t *)arg;
    nipc_server_run(&rs->server);
    nipc_server_destroy(&rs->server);
    return NULL;
}

static bool start_server(robust_server_t *rs)
{
    cleanup_all(rs->service);
    nipc_uds_server_config_t scfg = robust_server_config(rs);
    nipc_error_t err = nipc_server_init(&rs->server, TEST_RUN_DIR, rs->service, &scfg,
                                        rs->workers, NIPC_METHOD_CGROUPS_SNAPSHOT,
                                        robust_handler, &rs->opts);
    if (err != NIPC_OK) {
        fprintf(stderr, "server init failed: %d\n", err);
        return false;
    }
    /* init binds the listener: clients may connect once it returns. */
    test_server_use_test_timings(&rs->server, &rs->events);
    pthread_create(&rs->tid, NULL, robust_server_fn, rs);
    return true;
}

/* Stop and report whether run+destroy finished within limit_ms. */
static bool stop_server_within(robust_server_t *rs, unsigned limit_ms)
{
    nipc_server_stop(&rs->server);
    return join_within(rs->tid, limit_ms);
}

/* ------------------------------------------------------------------ */
/*  Clients                                                            */
/* ------------------------------------------------------------------ */

static nipc_client_config_t client_config(bool shm)
{
    nipc_client_config_t ccfg = {
        .supported_profiles         = NIPC_PROFILE_BASELINE | (shm ? NIPC_PROFILE_SHM_HYBRID : 0),
        .preferred_profiles         = shm ? NIPC_PROFILE_SHM_HYBRID : 0,
        .max_request_batch_items    = 1,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .auth_token                 = AUTH_TOKEN,
        .call_timeout_ms            = 1000,
    };
    return ccfg;
}

/* Connect and complete one snapshot call, in one attempt. */
static bool served_once(const char *service, bool shm)
{
    nipc_client_config_t ccfg = client_config(shm);
    nipc_client_ctx_t client;
    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
    nipc_client_refresh(&client);
    bool ok = false;
    if (nipc_client_ready(&client)) {
        nipc_cgroups_resp_view_t view;
        nipc_error_t err = nipc_client_call_cgroups_snapshot_timeout(&client, &view, 1000);
        ok = (err == NIPC_OK && view.item_count == 1);
    }
    nipc_client_close(&client);
    return ok;
}

typedef struct {
    const char *service;
    bool shm;
    bool result;
} served_job_t;

static void *served_job_fn(void *arg)
{
    served_job_t *job = (served_job_t *)arg;
    job->result = served_once(job->service, job->shm);
    return NULL;
}

static int raw_socket_connect(const char *service)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.sock", TEST_RUN_DIR, service);
    int fd = socket(AF_UNIX, TEST_UDS_SOCK_TYPE, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int raw_listener(const char *service)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.sock", TEST_RUN_DIR, service);
    unlink(path);
    int fd = socket(AF_UNIX, TEST_UDS_SOCK_TYPE, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 4) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ------------------------------------------------------------------ */
/*  Crash helper (child process)                                       */
/* ------------------------------------------------------------------ */

#if defined(__linux__)
static int helper_shm_client(const char *service)
{
    nipc_client_config_t ccfg = client_config(true);
    nipc_client_ctx_t client;
    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
    nipc_client_refresh(&client);
    nipc_cgroups_resp_view_t view;
    if (nipc_client_call_cgroups_snapshot_timeout(&client, &view, 2000) != NIPC_OK) {
        printf("HELPER_FAILED\n");
        return 1;
    }
    printf("HELPER_READY\n");
    fflush(stdout);
    for (;;)
        pause();
}
#endif

/* Server whose handler stalls, so a client call is in flight when the
 * parent kills this process. */
static int helper_stalled_server(const char *service, bool shm)
{
    static robust_server_t rs;
    rs = (robust_server_t){.service = service, .workers = 2, .shm = shm,
                           .opts = {.block = true}};
    if (!start_server(&rs)) {
        printf("HELPER_FAILED\n");
        return 1;
    }
    printf("HELPER_READY\n");
    fflush(stdout);
    for (;;)
        pause();
}

/* Wait until the helper's stdout carries marker. Returns false on EOF or
 * after TEST_WAIT_LIMIT_MS. */
static bool helper_wait_line(int fd, const char *marker)
{
    char buf[256];
    size_t used = 0;
    uint64_t deadline = now_ms() + TEST_WAIT_LIMIT_MS;
    while (used < sizeof(buf) - 1) {
        uint64_t now = now_ms();
        if (now >= deadline)
            return false;
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        if (poll(&pfd, 1, (int)(deadline - now)) <= 0)
            continue;
        /* One byte at a time, so a later marker stays in the pipe. */
        ssize_t n = read(fd, buf + used, 1);
        if (n <= 0)
            return false;
        used += (size_t)n;
        buf[used] = '\0';
        if (strstr(buf, marker) != NULL)
            return true;
        if (buf[used - 1] == '\n')
            used = 0;
    }
    return false;
}

/* Spawn this binary in helper mode and wait for HELPER_READY. When out_fd
 * is given it receives the helper's stdout for later markers. */
static pid_t spawn_helper(const char *mode, const char *service, int *out_fd)
{
    int pipefd[2];
    if (pipe(pipefd) < 0)
        return -1;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execl(g_self_path, "test_robustness", mode, service, (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    if (pid < 0) {
        close(pipefd[0]);
        return -1;
    }

    if (!helper_wait_line(pipefd[0], "HELPER_READY")) {
        close(pipefd[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }
    if (out_fd)
        *out_fd = pipefd[0];
    else
        close(pipefd[0]);
    return pid;
}

/* ------------------------------------------------------------------ */
/*  K1: SHM sessions released after the client leaves                  */
/* ------------------------------------------------------------------ */

#if defined(__linux__)
static void test_shm_sessions_released_after_clean_close(void)
{
    printf("K1: SHM sessions released after clean close\n");
    robust_server_t rs = {.service = "rob_shm_clean", .workers = 2, .shm = true};
    if (!start_server(&rs)) {
        check("server started", 0);
        return;
    }

    /* Each worker must exit and remove its region once its client closes;
     * a leaked session fails the session-exit wait. */
    int served = 0, exited = 0;
    for (int i = 0; i < 5; i++) {
        served += served_once(rs.service, true) ? 1 : 0;
        exited += test_server_wait_session_exits(&rs.events, 1) ? 1 : 0;
    }
    int leftover = count_shm_files(rs.service);

    check("5 sequential SHM clients served by 2 workers", served == 5);
    check("every SHM session exited after its client closed", exited == 5);
    check("no SHM regions left behind", leftover == 0);
    stop_server_within(&rs, 5000);
    cleanup_all(rs.service);
}
#endif

#if defined(__linux__)
static void test_shm_sessions_released_after_client_crash(void)
{
    printf("K1: SHM sessions released after client crash\n");
    robust_server_t rs = {.service = "rob_shm_crash", .workers = 2, .shm = true};
    if (!start_server(&rs)) {
        check("server started", 0);
        return;
    }

    bool spawned = true;
    for (int i = 0; i < 2; i++) {
        pid_t pid = spawn_helper("--helper-shm-client", rs.service, NULL);
        if (pid < 0) {
            spawned = false;
            break;
        }
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }
    check("crash helpers held SHM sessions", spawned);
    check("both workers freed their crashed clients' slots",
          spawned && test_server_wait_session_exits(&rs.events, 2));
    check("new client served after 2 crashed SHM clients on 2 workers",
          served_once(rs.service, true));
    check("new client's session exited",
          test_server_wait_session_exits(&rs.events, 1));
    check("no SHM regions left behind", count_shm_files(rs.service) == 0);
    stop_server_within(&rs, 5000);
    cleanup_all(rs.service);
}
#endif

/* ------------------------------------------------------------------ */
/*  K3: silent client                                                  */
/* ------------------------------------------------------------------ */

static void test_silent_client_does_not_block_other_clients(void)
{
    printf("K3: silent client does not block other clients\n");
    robust_server_t rs = {.service = "rob_silent", .workers = 4};
    if (!start_server(&rs)) {
        check("server started", 0);
        return;
    }

    int silent = raw_socket_connect(rs.service);
    check("silent client connected", silent >= 0);
    check("a worker waits for the silent client's HELLO",
          test_server_wait_handshakes(&rs.events, 1));

    served_job_t job = {.service = rs.service};
    pthread_t tid;
    pthread_create(&tid, NULL, served_job_fn, &job);
    bool finished = join_within(tid, 5000);

    if (silent >= 0)
        close(silent);
    if (!finished)
        join_rest(tid);
    check("legitimate client served within 5s while a silent client is connected",
          finished && job.result);
    stop_server_within(&rs, 5000);
    cleanup_all(rs.service);
}

static void test_silent_client_does_not_block_shutdown(void)
{
    printf("K3: silent client does not block shutdown\n");
    robust_server_t rs = {.service = "rob_silent_stop", .workers = 4};
    if (!start_server(&rs)) {
        check("server started", 0);
        return;
    }

    int silent = raw_socket_connect(rs.service);
    check("a worker waits for the silent client's HELLO",
          test_server_wait_handshakes(&rs.events, 1));
    bool stopped = stop_server_within(&rs, 3000);

    if (silent >= 0)
        close(silent);
    if (!stopped)
        join_rest(rs.tid);
    check("server stop + destroy finish within 3s while a silent client is connected",
          stopped);
    cleanup_all(rs.service);
}

static void test_silent_client_released_at_handshake_deadline(void)
{
    printf("K3: silent client released at the handshake deadline\n");
    robust_server_t rs = {.service = "rob_silent_deadline", .workers = 1};
    if (!start_server(&rs)) {
        check("server started", 0);
        return;
    }

    int silent = raw_socket_connect(rs.service);
    check("silent client connected", silent >= 0);
    uint64_t start = now_ms();
    bool released = test_server_wait_handshakes(&rs.events, 1) &&
                    test_server_wait_session_exits(&rs.events, 1);
    uint64_t elapsed = now_ms() - start;
    check("the silent client's worker exits", released);
    check("the worker waited for the handshake deadline",
          elapsed >= TEST_SERVER_HANDSHAKE_MS);
    check("the only worker serves a new client once the silent one misses the deadline",
          served_once(rs.service, false));

    if (silent >= 0) {
        char ch;
        ssize_t n = recv(silent, &ch, 1, MSG_DONTWAIT);
        check("server closed the silent connection at the deadline", n == 0);
        close(silent);
    }
    stop_server_within(&rs, 5000);
    cleanup_all(rs.service);
}

/* ------------------------------------------------------------------ */
/*  K4: client bounded when the server never completes HELLO           */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *service;
} bounded_job_t;

static void *bounded_client_fn(void *arg)
{
    bounded_job_t *job = (bounded_job_t *)arg;
    nipc_client_config_t ccfg = client_config(false);
    nipc_client_ctx_t client;
    nipc_client_init(&client, TEST_RUN_DIR, job->service, &ccfg);
    nipc_client_refresh(&client);
    nipc_cgroups_resp_view_t view;
    (void)nipc_client_call_cgroups_snapshot_timeout(&client, &view, 1000);
    nipc_client_close(&client);
    return NULL;
}

static void test_client_bounded_when_server_never_accepts(void)
{
    printf("K4: client bounded when server never accepts\n");
    const char *svc = "rob_noaccept";
    int listener = raw_listener(svc);
    check("raw listener bound", listener >= 0);

    bounded_job_t job = {.service = svc};
    pthread_t tid;
    pthread_create(&tid, NULL, bounded_client_fn, &job);
    bool finished = join_within(tid, 6000);

    if (listener >= 0)
        close(listener);
    if (!finished)
        join_rest(tid);
    check("refresh + call with 1s timeout return within 6s", finished);
    cleanup_all(svc);
}

typedef struct {
    int listener;
    int accepted; /* __atomic */
    int release;  /* __atomic */
} hold_job_t;

static void *accept_and_hold_fn(void *arg)
{
    hold_job_t *job = (hold_job_t *)arg;
    int fd = accept(job->listener, NULL, NULL);
    test_flag_set(&job->accepted);
    /* Hold the session open without replying until the test releases it. */
    (void)test_flag_wait(&job->release);
    if (fd >= 0)
        close(fd);
    close(job->listener);
    return NULL;
}

static void test_client_bounded_when_server_never_sends_hello_ack(void)
{
    printf("K4: client bounded when server never sends HELLO_ACK\n");
    const char *svc = "rob_nohelloack";
    hold_job_t hold = {.listener = raw_listener(svc)};
    check("raw listener bound", hold.listener >= 0);
    pthread_t holder;
    pthread_create(&holder, NULL, accept_and_hold_fn, &hold);

    bounded_job_t job = {.service = svc};
    pthread_t tid;
    pthread_create(&tid, NULL, bounded_client_fn, &job);
    bool finished = join_within(tid, 6000);

    test_flag_set(&hold.release);
    pthread_join(holder, NULL);
    if (!finished)
        join_rest(tid);
    check("refresh + call with 1s timeout return within 6s", finished);
    cleanup_all(svc);
}

typedef struct {
    nipc_client_ctx_t *client;
    nipc_error_t err;
    uint64_t elapsed_ms;
} call_job_t;

static void *snapshot_call_fn(void *arg)
{
    call_job_t *job = (call_job_t *)arg;
    nipc_cgroups_resp_view_t view;
    uint64_t start = now_ms();
    job->err = nipc_client_call_cgroups_snapshot_timeout(job->client, &view, 1000);
    job->elapsed_ms = now_ms() - start;
    return NULL;
}

static void test_call_reconnect_bounded_by_call_deadline(void)
{
    printf("K4: a call's reconnect is bounded by the call deadline\n");
    robust_server_t rs = {.service = "rob_reconnect_noack", .workers = 1};
    if (!start_server(&rs)) {
        check("server started", 0);
        return;
    }
    nipc_client_config_t ccfg = client_config(false);
    nipc_client_ctx_t client;
    nipc_client_init(&client, TEST_RUN_DIR, rs.service, &ccfg);
    nipc_client_refresh(&client);
    check("client ready", nipc_client_ready(&client));
    check("server stops", stop_server_within(&rs, 5000));

    /* The call fails on the dead session and reconnects to a server that
     * accepts but never sends HELLO_ACK. */
    hold_job_t hold = {.listener = raw_listener(rs.service)};
    check("raw listener bound", hold.listener >= 0);
    pthread_t holder;
    pthread_create(&holder, NULL, accept_and_hold_fn, &hold);

    call_job_t job = {.client = &client};
    pthread_t tid;
    pthread_create(&tid, NULL, snapshot_call_fn, &job);
    bool finished = join_within(tid, 6000);

    test_flag_set(&hold.release);
    pthread_join(holder, NULL);
    if (!finished)
        join_rest(tid);
    printf("    call took %llums\n", (unsigned long long)job.elapsed_ms);
    check("call with 1s timeout returns within 6s", finished);
    check("call reports the timeout", job.err == NIPC_ERR_TIMEOUT);
    nipc_client_close(&client);
    cleanup_all(rs.service);
}

typedef struct {
    nipc_client_ctx_t *client;
} refresh_job_t;

static void *refresh_fn(void *arg)
{
    refresh_job_t *job = (refresh_job_t *)arg;
    nipc_client_refresh(job->client);
    return NULL;
}

static void test_abort_ends_refresh_waiting_for_hello_ack(void)
{
    printf("K4: abort ends a refresh waiting for HELLO_ACK\n");
    const char *svc = "rob_abort_refresh";
    hold_job_t hold = {.listener = raw_listener(svc)};
    check("raw listener bound", hold.listener >= 0);
    pthread_t holder;
    pthread_create(&holder, NULL, accept_and_hold_fn, &hold);

    nipc_client_config_t ccfg = client_config(false);
    ccfg.call_timeout_ms = 10000; /* only an abort can end the wait early */
    nipc_client_ctx_t client;
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);

    refresh_job_t job = {.client = &client};
    pthread_t tid;
    pthread_create(&tid, NULL, refresh_fn, &job);
    check("server accepted the connection", test_flag_wait(&hold.accepted));
    nipc_client_abort(&client);
    bool finished = join_within(tid, 2000);

    test_flag_set(&hold.release);
    pthread_join(holder, NULL);
    if (!finished)
        join_rest(tid);
    check("refresh returns within 2s of the abort", finished);
    check("client is not ready", !nipc_client_ready(&client));
    nipc_client_close(&client);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  N1 guard: timeout longer than the 100 ms abort-poll slice          */
/* ------------------------------------------------------------------ */

static void test_call_honors_timeout_longer_than_100ms(void)
{
    printf("N1: call honors a timeout longer than 100 ms\n");
    robust_server_t rs = {.service = "rob_slow_handler", .workers = 2,
                          .opts = {.delay_ms = 300}};
    if (!start_server(&rs)) {
        check("server started", 0);
        return;
    }

    nipc_client_config_t ccfg = client_config(false);
    nipc_client_ctx_t client;
    nipc_client_init(&client, TEST_RUN_DIR, rs.service, &ccfg);
    nipc_client_refresh(&client);
    nipc_cgroups_resp_view_t view;
    nipc_error_t err = nipc_client_call_cgroups_snapshot_timeout(&client, &view, 2000);
    nipc_client_close(&client);

    check("300ms handler succeeds with a 2000ms timeout", err == NIPC_OK);
    stop_server_within(&rs, 5000);
    cleanup_all(rs.service);
}

/* ------------------------------------------------------------------ */
/*  N2: shutdown with a worker blocked sending to a non-reading client */
/* ------------------------------------------------------------------ */

static void test_shutdown_not_blocked_by_non_reading_client(void)
{
    printf("N2: shutdown not blocked by a client that stopped reading\n");
    const uint32_t big = 8u * 1024u * 1024u;
    robust_server_t rs = {.service = "rob_noread", .workers = 2,
                          .max_payload = big, .packet_size = 65536,
                          .opts = {.raw_len = 4u * 1024u * 1024u}};
    if (!start_server(&rs)) {
        check("server started", 0);
        return;
    }

    nipc_uds_client_config_t ccfg = {
        .supported_profiles         = NIPC_PROFILE_BASELINE,
        .max_request_payload_bytes  = 4096,
        .max_request_batch_items    = 1,
        .max_response_payload_bytes = big,
        .max_response_batch_items   = 1,
        .auth_token                 = AUTH_TOKEN,
        .packet_size                = 65536,
    };
    nipc_uds_session_t session;
    bool connected = nipc_uds_connect(TEST_RUN_DIR, rs.service, &ccfg, &session) == NIPC_UDS_OK;
    check("raw client connected", connected);
    if (connected) {
        uint8_t req[64];
        size_t req_len = nipc_cgroups_req_encode(&(nipc_cgroups_req_t){.layout_version = 1},
                                                 req, sizeof(req));
        nipc_header_t hdr = {.kind = NIPC_KIND_REQUEST, .code = NIPC_METHOD_CGROUPS_SNAPSHOT,
                             .item_count = 1, .message_id = 1};
        check("request sent", nipc_uds_send(&session, &hdr, req, req_len) == NIPC_UDS_OK);
    }
    /* Never read the response. Once the handler built it, the worker can
     * only be sending, which blocks. */
    check("handler built the 4 MiB response", test_flag_wait(&rs.opts.handled));

    bool stopped = stop_server_within(&rs, 3000);
    if (connected)
        nipc_uds_close_session(&session);
    if (!stopped)
        join_rest(rs.tid);
    check("server stop + destroy finish within 3s while a worker is blocked sending", stopped);
    cleanup_all(rs.service);
}

/* ------------------------------------------------------------------ */
/*  N3: stop before run                                                */
/* ------------------------------------------------------------------ */

static void *run_only_fn(void *arg)
{
    nipc_server_run((nipc_managed_server_t *)arg);
    return NULL;
}

static void test_stop_before_run_is_honored(void)
{
    printf("N3: stop before run is honored\n");
    robust_server_t rs = {.service = "rob_stop_first", .workers = 2};
    cleanup_all(rs.service);
    nipc_uds_server_config_t scfg = robust_server_config(&rs);
    if (nipc_server_init(&rs.server, TEST_RUN_DIR, rs.service, &scfg, rs.workers,
                         NIPC_METHOD_CGROUPS_SNAPSHOT, robust_handler, &rs.opts) != NIPC_OK) {
        check("server init", 0);
        return;
    }

    nipc_server_stop(&rs.server);
    pthread_t tid;
    pthread_create(&tid, NULL, run_only_fn, &rs.server);
    bool returned = join_within(tid, 2000);
    if (!returned) {
        nipc_server_stop(&rs.server);
        join_rest(tid);
    }
    nipc_server_destroy(&rs.server);
    check("run after stop returns promptly", returned);
    cleanup_all(rs.service);
}

/* ------------------------------------------------------------------ */
/*  N5: SHM client notices a crashed server                            */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *service;
    bool shm;
    nipc_error_t err;
    uint64_t elapsed_ms;
    int calling; /* __atomic */
} dead_server_job_t;

static void *dead_server_client_fn(void *arg)
{
    dead_server_job_t *job = (dead_server_job_t *)arg;
    nipc_client_config_t ccfg = client_config(job->shm);
    ccfg.call_timeout_ms = 10000;
    nipc_client_ctx_t client;
    nipc_client_init(&client, TEST_RUN_DIR, job->service, &ccfg);
    nipc_client_refresh(&client);
    nipc_cgroups_resp_view_t view;
    test_flag_set(&job->calling);
    uint64_t start = now_ms();
    job->err = nipc_client_call_cgroups_snapshot_timeout(&client, &view, 10000);
    job->elapsed_ms = now_ms() - start;
    nipc_client_close(&client);
    return NULL;
}

static void client_notices_crashed_server(bool shm)
{
    const char *svc = shm ? "rob_dead_server" : "rob_dead_uds_server";
    cleanup_all(svc);
    int helper_out = -1;
    pid_t server = spawn_helper(shm ? "--helper-shm-server" : "--helper-uds-server", svc,
                                &helper_out);
    check("server helper started", server > 0);
    if (server <= 0)
        return;

    dead_server_job_t job = {.service = svc, .shm = shm};
    pthread_t tid;
    pthread_create(&tid, NULL, dead_server_client_fn, &job);
    check("client connected", test_flag_wait(&job.calling));
    check("request parked in the stalled handler",
          helper_wait_line(helper_out, "HELPER_HANDLER_ENTERED"));
    close(helper_out);
    kill(server, SIGKILL);
    waitpid(server, NULL, 0);

    bool finished = join_within(tid, 15000);
    if (!finished)
        join_rest(tid);
    printf("    call returned err=%d after %llums\n", job.err,
           (unsigned long long)job.elapsed_ms);
    check("call fails within 3s of the server crash (timeout is 10s)",
          finished && job.err != NIPC_OK && job.elapsed_ms < 3500);
    check("call reports the server as disconnected",
          finished && job.err == NIPC_ERR_DISCONNECTED);
    cleanup_all(svc);
}

#if defined(__linux__)
static void test_shm_client_notices_crashed_server(void)
{
    printf("N5: SHM client notices a crashed server before its timeout\n");
    client_notices_crashed_server(true);
}
#endif

static void test_uds_client_reports_crashed_server_as_disconnected(void)
{
    printf("N5: baseline client reports a crashed server as disconnected\n");
    client_notices_crashed_server(false);
}

#if defined(__linux__)
/* A server at worker capacity completes the handshake, then closes the
 * session and removes the prepared SHM region. The client must not spend
 * its attach retry budget (5 s) on a region that will never appear, and
 * must keep SHM enabled for its next connect. */
static void test_shm_attach_stops_when_server_closes_session(void)
{
    printf("N5: SHM attach stops when the server closes the session\n");
    robust_server_t rs = {.service = "rob_attach_refused", .workers = 1, .shm = true};
    if (!start_server(&rs)) {
        check("server started", 0);
        return;
    }

    nipc_client_config_t ccfg = client_config(true);
    nipc_client_ctx_t holder;
    nipc_client_init(&holder, TEST_RUN_DIR, rs.service, &ccfg);
    nipc_client_refresh(&holder);
    check("first client holds the only worker", nipc_client_ready(&holder));

    nipc_client_ctx_t refused;
    nipc_client_init(&refused, TEST_RUN_DIR, rs.service, &ccfg);
    uint64_t start = now_ms();
    nipc_client_refresh(&refused);
    uint64_t elapsed = now_ms() - start;
    printf("    refused refresh took %llums\n", (unsigned long long)elapsed);
    check("refused connect returns within 1s (attach retry budget is 5s)", elapsed < 1000);

    nipc_client_close(&holder);
    check("holder's session exited", test_server_wait_session_exits(&rs.events, 1));
    nipc_client_refresh(&refused);
    bool ready = nipc_client_ready(&refused);
    check("refused client connects once the worker is free", ready);
    check("refused client still uses SHM", ready && refused.shm != NULL);
    nipc_client_close(&refused);
    check("server stops", stop_server_within(&rs, 5000));
}
#endif

/* Sockets and pipes must not leak into children a consumer forks and
 * execs: an inherited session socket keeps the peer from seeing EOF. */
static bool all_ipc_fds_cloexec(int *checked)
{
    long max_fd = sysconf(_SC_OPEN_MAX);
    if (max_fd < 0 || max_fd > 4096)
        max_fd = 4096;
    bool ok = true;
    for (int fd = 3; fd < (int)max_fd; fd++) {
        struct stat st;
        if (fstat(fd, &st) < 0)
            continue;
        if (!S_ISSOCK(st.st_mode) && !S_ISFIFO(st.st_mode))
            continue;
        (*checked)++;
        int flags = fcntl(fd, F_GETFD);
        if (flags < 0 || !(flags & FD_CLOEXEC)) {
            printf("    fd %d (%s) is not close-on-exec\n", fd,
                   S_ISSOCK(st.st_mode) ? "socket" : "pipe");
            ok = false;
        }
    }
    return ok;
}

static void test_ipc_fds_are_cloexec(void)
{
    printf("CLOEXEC: every NetIPC socket and pipe is close-on-exec\n");
    robust_server_t rs = {.service = "rob_cloexec", .workers = 2, .shm = true};
    if (!start_server(&rs)) {
        check("server started", 0);
        return;
    }
    nipc_client_config_t ccfg = client_config(true);
    nipc_client_ctx_t client;
    nipc_client_init(&client, TEST_RUN_DIR, rs.service, &ccfg);
    nipc_client_refresh(&client);
    check("client connected", nipc_client_ready(&client));
    /* A completed call proves the server holds its session socket. */
    nipc_cgroups_resp_view_t view;
    check("client call served",
          nipc_client_call_cgroups_snapshot_timeout(&client, &view, 1000) == NIPC_OK);

    /* Listener, accepted session, client session and client abort pipe. */
    int checked = 0;
    bool ok = all_ipc_fds_cloexec(&checked);
    printf("    checked %d socket/pipe fds\n", checked);
    check("listener, both session ends and the abort pipe are checked", checked >= 5);
    check("all of them are close-on-exec", ok);

    nipc_client_close(&client);
    check("server stops", stop_server_within(&rs, 5000));
}

/* ------------------------------------------------------------------ */
/*  N11: message larger than SO_SNDBUF with the default packet size    */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *service;
    uint32_t packet_size;
    size_t expected_len;
    bool received_ok;
    int ready;  /* __atomic */
    int failed; /* __atomic */
} big_listener_job_t;

static void *big_listener_fn(void *arg)
{
    big_listener_job_t *job = (big_listener_job_t *)arg;
    nipc_uds_server_config_t scfg = {
        .supported_profiles         = NIPC_PROFILE_BASELINE,
        .max_request_payload_bytes  = 1024u * 1024u,
        .max_request_batch_items    = 1,
        .max_response_payload_bytes = 4096,
        .max_response_batch_items   = 1,
        .auth_token                 = AUTH_TOKEN,
        .packet_size                = job->packet_size,
        .backlog                    = 4,
    };
    nipc_uds_listener_t listener;
    if (nipc_uds_listen(TEST_RUN_DIR, job->service, &scfg, &listener) != NIPC_UDS_OK) {
        test_flag_set(&job->failed);
        return NULL;
    }
    test_flag_set(&job->ready);

    struct pollfd pfd = {.fd = listener.fd, .events = POLLIN};
    if (poll(&pfd, 1, 5000) == 1) {
        nipc_uds_session_t session;
        if (nipc_uds_accept(&listener, 1, &session) == NIPC_UDS_OK) {
            size_t buf_size = job->expected_len + 4096;
            uint8_t *buf = malloc(buf_size);
            nipc_header_t hdr;
            const void *payload = NULL;
            size_t payload_len = 0;
            job->received_ok =
                buf && nipc_uds_receive_timeout(&session, buf, buf_size, &hdr, &payload,
                                                &payload_len, 5000, -1) == NIPC_UDS_OK &&
                payload_len == job->expected_len;
            free(buf);
            nipc_uds_close_session(&session);
        }
    }
    nipc_uds_close_listener(&listener);
    return NULL;
}

static bool large_message_round_trip(const char *service, uint32_t packet_size)
{
    const size_t len = 512u * 1024u;
    cleanup_all(service);
    big_listener_job_t job = {.service = service, .packet_size = packet_size,
                              .expected_len = len};
    pthread_t tid;
    pthread_create(&tid, NULL, big_listener_fn, &job);
    (void)test_flag_wait_any(&job.ready, &job.failed);

    nipc_uds_client_config_t ccfg = {
        .supported_profiles         = NIPC_PROFILE_BASELINE,
        .max_request_payload_bytes  = 1024u * 1024u,
        .max_request_batch_items    = 1,
        .max_response_payload_bytes = 4096,
        .max_response_batch_items   = 1,
        .auth_token                 = AUTH_TOKEN,
        .packet_size                = packet_size,
    };
    nipc_uds_session_t session;
    bool sent = false;
    if (nipc_uds_connect(TEST_RUN_DIR, service, &ccfg, &session) == NIPC_UDS_OK) {
        uint8_t *payload = calloc(1, len);
        nipc_header_t hdr = {.kind = NIPC_KIND_REQUEST, .code = NIPC_METHOD_CGROUPS_SNAPSHOT,
                             .item_count = 1, .message_id = 1};
        sent = payload && nipc_uds_send(&session, &hdr, payload, len) == NIPC_UDS_OK;
        free(payload);
        nipc_uds_close_session(&session);
    }
    pthread_join(tid, NULL);
    cleanup_all(service);
    return sent && job.received_ok;
}

static void test_default_packet_size_carries_large_message(void)
{
    printf("N11: default packet size carries a 512 KiB message\n");
    check("512 KiB message delivered with auto-detected packet size",
          large_message_round_trip("rob_bigmsg_default", 0));
    check("control: 512 KiB message delivered with 64 KiB packet size",
          large_message_round_trip("rob_bigmsg_explicit", 65536));
    check("512 KiB message delivered with an oversized 4 MiB packet size (clamped)",
          large_message_round_trip("rob_bigmsg_oversized", 4u * 1024u * 1024u));
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
#if defined(__linux__)
    if (argc == 3 && strcmp(argv[1], "--helper-shm-client") == 0)
        return helper_shm_client(argv[2]);
#endif
    if (argc == 3 && strcmp(argv[1], "--helper-shm-server") == 0)
        return helper_stalled_server(argv[2], true);
    if (argc == 3 && strcmp(argv[1], "--helper-uds-server") == 0)
        return helper_stalled_server(argv[2], false);

    if (!realpath(argv[0], g_self_path)) {
        perror("realpath");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    mkdir(TEST_RUN_DIR, 0700);
    setvbuf(stdout, NULL, _IOLBF, 0);

#if defined(__linux__)
    test_shm_sessions_released_after_clean_close();
    test_shm_sessions_released_after_client_crash();
#endif
    test_silent_client_does_not_block_other_clients();
    test_silent_client_does_not_block_shutdown();
    test_silent_client_released_at_handshake_deadline();
    test_client_bounded_when_server_never_accepts();
    test_client_bounded_when_server_never_sends_hello_ack();
    test_call_reconnect_bounded_by_call_deadline();
    test_abort_ends_refresh_waiting_for_hello_ack();
    test_call_honors_timeout_longer_than_100ms();
    test_shutdown_not_blocked_by_non_reading_client();
    test_stop_before_run_is_honored();
#if defined(__linux__)
    test_shm_client_notices_crashed_server();
#endif
    test_uds_client_reports_crashed_server_as_disconnected();
#if defined(__linux__)
    test_shm_attach_stops_when_server_closes_session();
#endif
    test_ipc_fds_are_cloexec();
    test_default_packet_size_carries_large_message();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
