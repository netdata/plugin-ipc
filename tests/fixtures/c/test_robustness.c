/*
 * test_robustness.c - Robustness reproduction tests (SOW-0040), POSIX.
 *
 * Each test asserts the correct behavior. Every blocking step runs on a
 * helper thread joined with a deadline, so a regression fails instead of
 * hanging. Bounds are deliberately lenient so they do not dictate a fix.
 *
 * Tests:
 *   K1  SHM sessions released after clean close (guard: C is correct)
 *   K1  SHM sessions released after client crash (guard: C is correct)
 *   K3  silent client does not block other clients
 *   K3  silent client does not block shutdown
 *   K4  client bounded when server never accepts
 *   K4  client bounded when server never sends HELLO_ACK
 *   N1  call honors a timeout longer than 100 ms (guard: C is correct)
 *   N2  shutdown not blocked by a client that stopped reading
 *   N3  stop before run is honored
 *   N5  SHM client notices a crashed server before its timeout
 *   N11 default packet size carries a message larger than SO_SNDBUF
 *       (plus an explicit-packet-size control)
 *
 * The binary re-executes itself with --helper-shm-client / --helper-shm-server
 * to simulate a peer process that is killed with SIGKILL mid-session.
 */

#define _GNU_SOURCE
#include "netipc/netipc_service.h"
#include "netipc/netipc_protocol.h"
#include "netipc/netipc_uds.h"
#include "netipc/netipc_shm.h"

#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
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

static int g_pass = 0;
static int g_fail = 0;

/* Tests that reproduce still-open bugs run only when
 * NETIPC_REPRO_KNOWN_BUGS=1; each fix removes the gate from its tests. */
static int skip_known_bug(const char *id)
{
    const char *v = getenv("NETIPC_REPRO_KNOWN_BUGS");
    if (v && strcmp(v, "1") == 0)
        return 0;
    printf("  SKIP: reproduces open bug %s; set NETIPC_REPRO_KNOWN_BUGS=1 to run\n", id);
    return 1;
}

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

/* Join with a deadline. Returns true if the thread finished in time. */
static bool join_within(pthread_t tid, unsigned limit_ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += limit_ms / 1000u;
    ts.tv_nsec += (long)(limit_ms % 1000u) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    return pthread_timedjoin_np(tid, NULL, &ts) == 0;
}

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

static int wait_shm_files_gone(const char *service, unsigned limit_ms)
{
    uint64_t deadline = now_ms() + limit_ms;
    int n;
    while ((n = count_shm_files(service)) > 0 && now_ms() < deadline)
        sleep_ms(50);
    return n;
}

/* ------------------------------------------------------------------ */
/*  Server                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned delay_ms;   /* handler delay */
    size_t   raw_len;    /* if non-zero, reply with raw_len opaque bytes */
} handler_opts_t;

static nipc_error_t robust_handler(void *user, const nipc_header_t *request_hdr,
                                   const uint8_t *request_payload, size_t request_len,
                                   uint8_t *response_buf, size_t response_buf_size,
                                   size_t *response_len_out)
{
    const handler_opts_t *opts = (const handler_opts_t *)user;
    (void)request_hdr;

    if (opts && opts->delay_ms)
        sleep_ms(opts->delay_ms);

    if (opts && opts->raw_len) {
        if (opts->raw_len > response_buf_size)
            return NIPC_ERR_OVERFLOW;
        memset(response_buf, 0, opts->raw_len);
        *response_len_out = opts->raw_len;
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
    pthread_t tid;
    int ready; /* __atomic */
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
    pthread_create(&rs->tid, NULL, robust_server_fn, rs);
    sleep_ms(50);
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

/* Connect and complete one snapshot call, retrying until limit_ms. */
static bool served_within(const char *service, bool shm, unsigned limit_ms)
{
    uint64_t deadline = now_ms() + limit_ms;
    nipc_client_config_t ccfg = client_config(shm);
    while (now_ms() < deadline) {
        nipc_client_ctx_t client;
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        nipc_client_refresh(&client);
        if (nipc_client_ready(&client)) {
            nipc_cgroups_resp_view_t view;
            nipc_error_t err = nipc_client_call_cgroups_snapshot_timeout(&client, &view, 1000);
            bool ok = (err == NIPC_OK && view.item_count == 1);
            nipc_client_close(&client);
            if (ok)
                return true;
        } else {
            nipc_client_close(&client);
        }
        sleep_ms(50);
    }
    return false;
}

typedef struct {
    const char *service;
    bool shm;
    unsigned limit_ms;
    bool result;
} served_job_t;

static void *served_job_fn(void *arg)
{
    served_job_t *job = (served_job_t *)arg;
    job->result = served_within(job->service, job->shm, job->limit_ms);
    return NULL;
}

static int raw_socket_connect(const char *service)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.sock", TEST_RUN_DIR, service);
    int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
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
    int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
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

static int helper_shm_client(const char *service)
{
    nipc_client_config_t ccfg = client_config(true);
    nipc_client_ctx_t client;
    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
    for (int i = 0; i < 200 && !nipc_client_ready(&client); i++) {
        nipc_client_refresh(&client);
        sleep_ms(10);
    }
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

/* SHM server whose handler stalls, so a client call is in flight when the
 * parent kills this process. */
static int helper_shm_server(const char *service)
{
    static robust_server_t rs;
    rs = (robust_server_t){.service = service, .workers = 2, .shm = true,
                           .opts = {.delay_ms = 60000}};
    if (!start_server(&rs)) {
        printf("HELPER_FAILED\n");
        return 1;
    }
    printf("HELPER_READY\n");
    fflush(stdout);
    for (;;)
        pause();
}

/* Spawn this binary in helper mode and wait for HELPER_READY. */
static pid_t spawn_helper(const char *mode, const char *service)
{
    int pipefd[2];
    if (pipe(pipefd) < 0)
        return -1;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execl("/proc/self/exe", "test_robustness", mode, service, (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    if (pid < 0) {
        close(pipefd[0]);
        return -1;
    }

    char buf[256];
    size_t used = 0;
    bool ready = false;
    uint64_t deadline = now_ms() + 10000;
    while (!ready && now_ms() < deadline && used < sizeof(buf) - 1) {
        struct pollfd pfd = {.fd = pipefd[0], .events = POLLIN};
        if (poll(&pfd, 1, 100) <= 0)
            continue;
        ssize_t n = read(pipefd[0], buf + used, sizeof(buf) - 1 - used);
        if (n <= 0)
            break;
        used += (size_t)n;
        buf[used] = '\0';
        ready = strstr(buf, "HELPER_READY") != NULL;
    }
    close(pipefd[0]);
    if (!ready) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }
    return pid;
}

/* ------------------------------------------------------------------ */
/*  K1: SHM sessions released after the client leaves                  */
/* ------------------------------------------------------------------ */

static void test_shm_sessions_released_after_clean_close(void)
{
    printf("K1: SHM sessions released after clean close\n");
    robust_server_t rs = {.service = "rob_shm_clean", .workers = 2, .shm = true};
    if (!start_server(&rs)) {
        check("server started", 0);
        return;
    }

    int served = 0;
    for (int i = 0; i < 5; i++)
        served += served_within(rs.service, true, 3000) ? 1 : 0;
    int leftover = wait_shm_files_gone(rs.service, 2000);

    check("5 sequential SHM clients served by 2 workers", served == 5);
    check("no SHM regions left behind", leftover == 0);
    stop_server_within(&rs, 5000);
    cleanup_all(rs.service);
}

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
        pid_t pid = spawn_helper("--helper-shm-client", rs.service);
        if (pid < 0) {
            spawned = false;
            break;
        }
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }
    check("crash helpers held SHM sessions", spawned);
    check("new client served after 2 crashed SHM clients on 2 workers",
          served_within(rs.service, true, 3000));
    check("no SHM regions left behind", wait_shm_files_gone(rs.service, 2000) == 0);
    stop_server_within(&rs, 5000);
    cleanup_all(rs.service);
}

/* ------------------------------------------------------------------ */
/*  K3: silent client                                                  */
/* ------------------------------------------------------------------ */

static void test_silent_client_does_not_block_other_clients(void)
{
    printf("K3: silent client does not block other clients\n");
    if (skip_known_bug("K3"))
        return;
    robust_server_t rs = {.service = "rob_silent", .workers = 4};
    if (!start_server(&rs)) {
        check("server started", 0);
        return;
    }

    int silent = raw_socket_connect(rs.service);
    check("silent client connected", silent >= 0);
    sleep_ms(100);

    served_job_t job = {.service = rs.service, .limit_ms = 5000};
    pthread_t tid;
    pthread_create(&tid, NULL, served_job_fn, &job);
    bool finished = join_within(tid, 5000);

    if (silent >= 0)
        close(silent);
    if (!finished)
        pthread_join(tid, NULL);
    check("legitimate client served within 5s while a silent client is connected",
          finished && job.result);
    stop_server_within(&rs, 5000);
    cleanup_all(rs.service);
}

static void test_silent_client_does_not_block_shutdown(void)
{
    printf("K3: silent client does not block shutdown\n");
    if (skip_known_bug("K3"))
        return;
    robust_server_t rs = {.service = "rob_silent_stop", .workers = 4};
    if (!start_server(&rs)) {
        check("server started", 0);
        return;
    }

    int silent = raw_socket_connect(rs.service);
    sleep_ms(100);
    bool stopped = stop_server_within(&rs, 3000);

    if (silent >= 0)
        close(silent);
    if (!stopped)
        pthread_join(rs.tid, NULL);
    check("server stop + destroy finish within 3s while a silent client is connected",
          stopped);
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
    if (skip_known_bug("K4"))
        return;
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
        pthread_join(tid, NULL);
    check("refresh + call with 1s timeout return within 6s", finished);
    cleanup_all(svc);
}

typedef struct {
    int listener;
    unsigned hold_ms;
} hold_job_t;

static void *accept_and_hold_fn(void *arg)
{
    hold_job_t *job = (hold_job_t *)arg;
    int fd = accept(job->listener, NULL, NULL);
    sleep_ms(job->hold_ms);
    if (fd >= 0)
        close(fd);
    close(job->listener);
    return NULL;
}

static void test_client_bounded_when_server_never_sends_hello_ack(void)
{
    printf("K4: client bounded when server never sends HELLO_ACK\n");
    if (skip_known_bug("K4"))
        return;
    const char *svc = "rob_nohelloack";
    hold_job_t hold = {.listener = raw_listener(svc), .hold_ms = 8000};
    check("raw listener bound", hold.listener >= 0);
    pthread_t holder;
    pthread_create(&holder, NULL, accept_and_hold_fn, &hold);

    bounded_job_t job = {.service = svc};
    pthread_t tid;
    pthread_create(&tid, NULL, bounded_client_fn, &job);
    bool finished = join_within(tid, 6000);

    pthread_join(holder, NULL);
    if (!finished)
        pthread_join(tid, NULL);
    check("refresh + call with 1s timeout return within 6s", finished);
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
    if (skip_known_bug("N2"))
        return;
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
    /* Never read the response; give the worker time to block in send. */
    sleep_ms(500);

    bool stopped = stop_server_within(&rs, 3000);
    if (connected)
        nipc_uds_close_session(&session);
    if (!stopped)
        pthread_join(rs.tid, NULL);
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
    if (skip_known_bug("N3"))
        return;
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
        pthread_join(tid, NULL);
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
    nipc_error_t err;
    uint64_t elapsed_ms;
    int calling; /* __atomic */
} dead_server_job_t;

static void *dead_server_client_fn(void *arg)
{
    dead_server_job_t *job = (dead_server_job_t *)arg;
    nipc_client_config_t ccfg = client_config(true);
    ccfg.call_timeout_ms = 10000;
    nipc_client_ctx_t client;
    nipc_client_init(&client, TEST_RUN_DIR, job->service, &ccfg);
    for (int i = 0; i < 200 && !nipc_client_ready(&client); i++) {
        nipc_client_refresh(&client);
        sleep_ms(10);
    }
    nipc_cgroups_resp_view_t view;
    __atomic_store_n(&job->calling, 1, __ATOMIC_RELEASE);
    uint64_t start = now_ms();
    job->err = nipc_client_call_cgroups_snapshot_timeout(&client, &view, 10000);
    job->elapsed_ms = now_ms() - start;
    nipc_client_close(&client);
    return NULL;
}

static void test_shm_client_notices_crashed_server(void)
{
    printf("N5: SHM client notices a crashed server before its timeout\n");
    if (skip_known_bug("N5"))
        return;
    const char *svc = "rob_dead_server";
    cleanup_all(svc);
    pid_t server = spawn_helper("--helper-shm-server", svc);
    check("server helper started", server > 0);
    if (server <= 0)
        return;

    dead_server_job_t job = {.service = svc};
    pthread_t tid;
    pthread_create(&tid, NULL, dead_server_client_fn, &job);
    for (int i = 0; i < 500 && !__atomic_load_n(&job.calling, __ATOMIC_ACQUIRE); i++)
        sleep_ms(10);
    sleep_ms(500); /* request is now parked in the stalled handler */
    kill(server, SIGKILL);
    waitpid(server, NULL, 0);

    bool finished = join_within(tid, 15000);
    if (!finished)
        pthread_join(tid, NULL);
    printf("    call returned err=%d after %llums\n", job.err,
           (unsigned long long)job.elapsed_ms);
    check("call fails within 3s of the server crash (timeout is 10s)",
          finished && job.err != NIPC_OK && job.elapsed_ms < 3500);
    cleanup_all(svc);
}

/* ------------------------------------------------------------------ */
/*  N11: message larger than SO_SNDBUF with the default packet size    */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *service;
    uint32_t packet_size;
    size_t expected_len;
    bool received_ok;
    int ready; /* __atomic */
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
        __atomic_store_n(&job->ready, -1, __ATOMIC_RELEASE);
        return NULL;
    }
    __atomic_store_n(&job->ready, 1, __ATOMIC_RELEASE);

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
    for (int i = 0; i < 400 && __atomic_load_n(&job.ready, __ATOMIC_ACQUIRE) == 0; i++)
        sleep_ms(5);

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
    if (!skip_known_bug("N11"))
        check("512 KiB message delivered with auto-detected packet size",
              large_message_round_trip("rob_bigmsg_default", 0));
    check("control: 512 KiB message delivered with 64 KiB packet size",
          large_message_round_trip("rob_bigmsg_explicit", 65536));
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--helper-shm-client") == 0)
        return helper_shm_client(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--helper-shm-server") == 0)
        return helper_shm_server(argv[2]);

    signal(SIGPIPE, SIG_IGN);
    mkdir(TEST_RUN_DIR, 0700);
    setvbuf(stdout, NULL, _IOLBF, 0);

    test_shm_sessions_released_after_clean_close();
    test_shm_sessions_released_after_client_crash();
    test_silent_client_does_not_block_other_clients();
    test_silent_client_does_not_block_shutdown();
    test_client_bounded_when_server_never_accepts();
    test_client_bounded_when_server_never_sends_hello_ack();
    test_call_honors_timeout_longer_than_100ms();
    test_shutdown_not_blocked_by_non_reading_client();
    test_stop_before_run_is_honored();
    test_shm_client_notices_crashed_server();
    test_default_packet_size_carries_large_message();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
