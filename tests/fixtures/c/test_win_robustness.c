/*
 * test_win_robustness.c - Robustness reproduction tests (SOW-0040), Windows.
 *
 * Each test asserts the correct behavior. Every blocking step runs on a
 * helper thread waited with a deadline, so a regression fails instead of
 * hanging. Bounds are deliberately lenient so they do not dictate a fix.
 * Tests wait on server events and peer signals, never on sleeps.
 *
 * Tests:
 *   K2  SHM session released after client crash
 *   K3  silent client does not block other clients
 *   K3  silent client does not block shutdown
 *   K3  silent client's worker is freed at the server handshake deadline
 *   K4  client bounded when server never sends HELLO_ACK
 *   K4  a call's reconnect is bounded by the call deadline
 *   K4  abort ends a refresh waiting for HELLO_ACK
 *   N2  shutdown not blocked by a client that stopped reading
 *   N3  stop before run is honored
 *   N5  SHM client notices a crashed server before its timeout
 *   N5  SHM attach stops when the server closes the session
 *
 * The binary re-executes itself with --helper-shm-client / --helper-shm-server
 * to simulate a peer process that is killed with TerminateProcess.
 */

#include "netipc/netipc_named_pipe.h"
#include "netipc/netipc_protocol.h"
#include "netipc/netipc_service.h"
#include "netipc/netipc_win_shm.h"
#include "test_sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <windows.h>

#define TEST_RUN_DIR      "C:\\Temp\\nipc_win_robustness"
#define AUTH_TOKEN        0xDEADBEEFCAFEBABEull
#define RESPONSE_BUF_SIZE 65536

static int g_pass = 0;
static int g_fail = 0;

static void check(const char *name, int cond)
{
    printf("  %s: %s\n", cond ? "PASS" : "FAIL", name);
    fflush(stdout);
    if (cond)
        g_pass++;
    else
        g_fail++;
}

static ULONGLONG now_ms(void)
{
    return GetTickCount64();
}

/* Unique service names keep runs independent of leftover pipe instances. */
static const char *svc_name(const char *prefix)
{
    static char names[16][96];
    static int next;
    char *out = names[next++ % 16];
    snprintf(out, 96, "%s_%lu", prefix, (unsigned long)GetCurrentProcessId());
    return out;
}

/* ------------------------------------------------------------------ */
/*  Server                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    size_t raw_len; /* if non-zero, reply with raw_len opaque bytes */
    BOOL   block;   /* report entry on stdout, then block until killed */
    int    handled; /* __atomic: raised once a raw reply is built */
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
        Sleep(INFINITE);
    }

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
    BOOL shm;
    uint32_t max_payload;
    handler_opts_t opts;
    nipc_managed_server_t server;
    test_server_events_t events;
    HANDLE thread;
} robust_server_t;

static nipc_np_server_config_t robust_server_config(const robust_server_t *rs)
{
    nipc_np_server_config_t scfg = {
        .supported_profiles         = NIPC_PROFILE_BASELINE |
                                      (rs->shm ? NIPC_WIN_SHM_PROFILE_HYBRID : 0),
        .preferred_profiles         = rs->shm ? NIPC_WIN_SHM_PROFILE_HYBRID : 0,
        .max_request_payload_bytes  = 4096,
        .max_request_batch_items    = 1,
        .max_response_payload_bytes = rs->max_payload ? rs->max_payload : RESPONSE_BUF_SIZE,
        .max_response_batch_items   = 1,
        .auth_token                 = AUTH_TOKEN,
    };
    return scfg;
}

static DWORD WINAPI robust_server_fn(LPVOID arg)
{
    robust_server_t *rs = (robust_server_t *)arg;
    nipc_server_run(&rs->server);
    nipc_server_destroy(&rs->server);
    return 0;
}

static BOOL start_server(robust_server_t *rs)
{
    nipc_np_server_config_t scfg = robust_server_config(rs);
    nipc_error_t err = nipc_server_init(&rs->server, TEST_RUN_DIR, rs->service, &scfg,
                                        rs->workers, NIPC_METHOD_CGROUPS_SNAPSHOT,
                                        robust_handler, &rs->opts);
    if (err != NIPC_OK) {
        fprintf(stderr, "server init failed: %d\n", err);
        return FALSE;
    }
    /* init creates the first pipe instance: clients may connect now. */
    test_server_use_test_timings(&rs->server, &rs->events);
    rs->thread = test_create_thread(NULL, 0, robust_server_fn, rs, 0, NULL);
    return rs->thread != NULL;
}

/* Stop and report whether run+destroy finished within limit_ms. */
static BOOL stop_server_within(robust_server_t *rs, DWORD limit_ms)
{
    nipc_server_stop(&rs->server);
    return WaitForSingleObject(rs->thread, limit_ms) == WAIT_OBJECT_0;
}

/* ------------------------------------------------------------------ */
/*  Clients                                                            */
/* ------------------------------------------------------------------ */

static nipc_client_config_t client_config(BOOL shm, uint32_t timeout_ms)
{
    nipc_client_config_t ccfg = {
        .supported_profiles         = NIPC_PROFILE_BASELINE | (shm ? NIPC_WIN_SHM_PROFILE_HYBRID : 0),
        .preferred_profiles         = shm ? NIPC_WIN_SHM_PROFILE_HYBRID : 0,
        .max_request_batch_items    = 1,
        .max_response_payload_bytes = RESPONSE_BUF_SIZE,
        .auth_token                 = AUTH_TOKEN,
        .call_timeout_ms            = timeout_ms,
    };
    return ccfg;
}

/* Connect and complete one snapshot call, in one attempt. */
static BOOL served_once(const char *service, BOOL shm)
{
    nipc_client_config_t ccfg = client_config(shm, 1000);
    nipc_client_ctx_t client;
    nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
    nipc_client_refresh(&client);
    BOOL ok = FALSE;
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
    BOOL result;
} served_job_t;

static DWORD WINAPI served_job_fn(LPVOID arg)
{
    served_job_t *job = (served_job_t *)arg;
    job->result = served_once(job->service, FALSE);
    return 0;
}

/* Open the pipe without sending HELLO. While every instance is busy the
 * server is creating the next one; WaitNamedPipeW blocks until it exists. */
static HANDLE open_pipe_silently(const char *service)
{
    wchar_t name[NIPC_NP_MAX_PIPE_NAME];
    nipc_np_build_pipe_name(name, NIPC_NP_MAX_PIPE_NAME, TEST_RUN_DIR, service);
    HANDLE h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY &&
        WaitNamedPipeW(name, TEST_WAIT_LIMIT_MS))
        h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    return h;
}

/* ------------------------------------------------------------------ */
/*  Helper processes                                                   */
/* ------------------------------------------------------------------ */

static int helper_shm_client(const char *service)
{
    nipc_client_config_t ccfg = client_config(TRUE, 2000);
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
    Sleep(INFINITE);
    return 0;
}

/* SHM server whose handler stalls, so a client call is in flight when the
 * parent kills this process. */
static int helper_shm_server(const char *service)
{
    static robust_server_t rs;
    rs = (robust_server_t){.service = service, .workers = 2, .shm = TRUE,
                           .opts = {.block = TRUE}};
    if (!start_server(&rs)) {
        printf("HELPER_FAILED\n");
        return 1;
    }
    printf("HELPER_READY\n");
    fflush(stdout);
    Sleep(INFINITE);
    return 0;
}

typedef struct {
    HANDLE rd;
    const char *marker;
    BOOL found;
} line_job_t;

static DWORD WINAPI line_job_fn(LPVOID arg)
{
    line_job_t *job = (line_job_t *)arg;
    char buf[256];
    DWORD used = 0;
    while (used < sizeof(buf) - 1) {
        DWORD n = 0;
        /* One byte at a time, so a later marker stays in the pipe. */
        if (!ReadFile(job->rd, buf + used, 1, &n, NULL) || n == 0)
            return 0;
        used += n;
        buf[used] = '\0';
        if (strstr(buf, job->marker)) {
            job->found = TRUE;
            return 0;
        }
        if (buf[used - 1] == '\n')
            used = 0;
    }
    return 0;
}

/* Wait until the helper's stdout carries marker. A blocked reader thread
 * ends once the caller kills the helper, which breaks the pipe. */
static BOOL helper_wait_line(HANDLE rd, const char *marker)
{
    static line_job_t job;
    job = (line_job_t){.rd = rd, .marker = marker};
    HANDLE t = test_create_thread(NULL, 0, line_job_fn, &job, 0, NULL);
    if (!t)
        return FALSE;
    BOOL done = WaitForSingleObject(t, TEST_WAIT_LIMIT_MS) == WAIT_OBJECT_0;
    CloseHandle(t);
    return done && job.found;
}

/* Spawn this binary in helper mode and wait for HELPER_READY. When out_rd
 * is given it receives the helper's stdout for later markers. */
static HANDLE spawn_helper(const char *mode, const char *service, HANDLE *out_rd)
{
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0))
        return NULL;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    char cmd[2 * MAX_PATH];
    snprintf(cmd, sizeof(cmd), "\"%s\" %s %s", exe, mode, service);

    STARTUPINFOA si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        return NULL;
    }
    CloseHandle(pi.hThread);

    if (!helper_wait_line(rd, "HELPER_READY")) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(rd);
        return NULL;
    }
    if (out_rd)
        *out_rd = rd;
    else
        CloseHandle(rd);
    return pi.hProcess;
}

static void kill_helper(HANDLE process)
{
    TerminateProcess(process, 9);
    WaitForSingleObject(process, 5000);
    CloseHandle(process);
}

/* ------------------------------------------------------------------ */
/*  K2: SHM session released after client crash                        */
/* ------------------------------------------------------------------ */

static void test_shm_session_released_after_client_crash(void)
{
    printf("K2: SHM session released after client crash\n");
    robust_server_t rs = {.service = svc_name("wrob_shm_crash"), .workers = 2, .shm = TRUE};
    if (!start_server(&rs)) {
        check("server started", 0);
        return;
    }

    BOOL spawned = TRUE;
    for (int i = 0; i < 2; i++) {
        HANDLE p = spawn_helper("--helper-shm-client", rs.service, NULL);
        if (!p) {
            spawned = FALSE;
            break;
        }
        kill_helper(p);
    }
    check("crash helpers held SHM sessions", spawned);
    check("both workers freed their crashed clients' slots",
          spawned && test_server_wait_session_exits(&rs.events, 2));
    check("new client served after 2 crashed SHM clients on 2 workers",
          served_once(rs.service, TRUE));
    stop_server_within(&rs, 5000);
}

/* ------------------------------------------------------------------ */
/*  K3: silent client                                                  */
/* ------------------------------------------------------------------ */

static void test_silent_client_does_not_block_other_clients(void)
{
    printf("K3: silent client does not block other clients\n");
    robust_server_t rs = {.service = svc_name("wrob_silent"), .workers = 4};
    if (!start_server(&rs)) {
        check("server started", 0);
        return;
    }

    HANDLE silent = open_pipe_silently(rs.service);
    check("silent client opened the pipe", silent != INVALID_HANDLE_VALUE);
    check("a worker waits for the silent client's HELLO",
          test_server_wait_handshakes(&rs.events, 1));

    served_job_t job = {.service = rs.service};
    HANDLE t = test_create_thread(NULL, 0, served_job_fn, &job, 0, NULL);
    BOOL finished = WaitForSingleObject(t, 5000) == WAIT_OBJECT_0;

    if (silent != INVALID_HANDLE_VALUE)
        CloseHandle(silent);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    check("legitimate client served within 5s while a silent client is connected",
          finished && job.result);
    stop_server_within(&rs, 5000);
}

static void test_silent_client_does_not_block_shutdown(void)
{
    printf("K3: silent client does not block shutdown\n");
    robust_server_t rs = {.service = svc_name("wrob_silent_stop"), .workers = 4};
    if (!start_server(&rs)) {
        check("server started", 0);
        return;
    }

    HANDLE silent = open_pipe_silently(rs.service);
    check("a worker waits for the silent client's HELLO",
          test_server_wait_handshakes(&rs.events, 1));
    BOOL stopped = stop_server_within(&rs, 3000);

    if (silent != INVALID_HANDLE_VALUE)
        CloseHandle(silent);
    if (!stopped)
        WaitForSingleObject(rs.thread, INFINITE);
    check("server stop + destroy finish within 3s while a silent client is connected", stopped);
}

static void test_silent_client_released_at_handshake_deadline(void)
{
    printf("K3: silent client released at the handshake deadline\n");
    robust_server_t rs = {.service = svc_name("wrob_silent_deadline"), .workers = 1};
    if (!start_server(&rs)) {
        check("server started", 0);
        return;
    }

    HANDLE silent = open_pipe_silently(rs.service);
    check("silent client opened the pipe", silent != INVALID_HANDLE_VALUE);
    ULONGLONG start = now_ms();
    BOOL released = test_server_wait_handshakes(&rs.events, 1) &&
                    test_server_wait_session_exits(&rs.events, 1);
    ULONGLONG elapsed = now_ms() - start;
    check("the silent client's worker exits", released);
    check("the worker waited for the handshake deadline",
          elapsed >= TEST_SERVER_HANDSHAKE_MS);
    check("the only worker serves a new client once the silent one misses the deadline",
          served_once(rs.service, FALSE));

    if (silent != INVALID_HANDLE_VALUE)
        CloseHandle(silent);
    stop_server_within(&rs, 5000);
}

/* ------------------------------------------------------------------ */
/*  K4: client bounded when the server never sends HELLO_ACK           */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *service;
} bounded_job_t;

static DWORD WINAPI bounded_client_fn(LPVOID arg)
{
    bounded_job_t *job = (bounded_job_t *)arg;
    nipc_client_config_t ccfg = client_config(FALSE, 1000);
    nipc_client_ctx_t client;
    nipc_client_init(&client, TEST_RUN_DIR, job->service, &ccfg);
    nipc_client_refresh(&client);
    nipc_cgroups_resp_view_t view;
    (void)nipc_client_call_cgroups_snapshot_timeout(&client, &view, 1000);
    nipc_client_close(&client);
    return 0;
}

static void test_client_bounded_when_server_never_sends_hello_ack(void)
{
    printf("K4: client bounded when server never sends HELLO_ACK\n");
    const char *svc = svc_name("wrob_nohelloack");
    wchar_t name[NIPC_NP_MAX_PIPE_NAME];
    nipc_np_build_pipe_name(name, NIPC_NP_MAX_PIPE_NAME, TEST_RUN_DIR, svc);
    HANDLE pipe = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                   PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                   4, 65536, 65536, 0, NULL);
    check("fake server pipe created", pipe != INVALID_HANDLE_VALUE);

    bounded_job_t job = {.service = svc};
    HANDLE t = test_create_thread(NULL, 0, bounded_client_fn, &job, 0, NULL);
    BOOL finished = WaitForSingleObject(t, 6000) == WAIT_OBJECT_0;

    if (pipe != INVALID_HANDLE_VALUE)
        CloseHandle(pipe);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    check("refresh + call with 1s timeout return within 6s", finished);
}

/* A pipe that accepts clients but never answers HELLO. */
static HANDLE fake_server_pipe(const char *service)
{
    wchar_t name[NIPC_NP_MAX_PIPE_NAME];
    nipc_np_build_pipe_name(name, NIPC_NP_MAX_PIPE_NAME, TEST_RUN_DIR, service);
    return CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                            4, 65536, 65536, 0, NULL);
}

typedef struct {
    nipc_client_ctx_t *client;
    nipc_error_t err;
    ULONGLONG elapsed_ms;
} call_job_t;

static DWORD WINAPI snapshot_call_fn(LPVOID arg)
{
    call_job_t *job = (call_job_t *)arg;
    nipc_cgroups_resp_view_t view;
    ULONGLONG start = GetTickCount64();
    job->err = nipc_client_call_cgroups_snapshot_timeout(job->client, &view, 1000);
    job->elapsed_ms = GetTickCount64() - start;
    return 0;
}

static void test_call_reconnect_bounded_by_call_deadline(void)
{
    printf("K4: a call's reconnect is bounded by the call deadline\n");
    robust_server_t rs = {.service = svc_name("wrob_reconnect_noack"), .workers = 1};
    if (!start_server(&rs)) {
        check("server started", 0);
        return;
    }
    nipc_client_config_t ccfg = client_config(FALSE, 1000);
    nipc_client_ctx_t client;
    nipc_client_init(&client, TEST_RUN_DIR, rs.service, &ccfg);
    nipc_client_refresh(&client);
    check("client ready", nipc_client_ready(&client));
    check("server stops", stop_server_within(&rs, 5000));

    /* The call fails on the dead session and reconnects to a server that
     * accepts but never sends HELLO_ACK. */
    HANDLE pipe = fake_server_pipe(rs.service);
    check("fake server pipe created", pipe != INVALID_HANDLE_VALUE);

    call_job_t job = {.client = &client};
    HANDLE t = test_create_thread(NULL, 0, snapshot_call_fn, &job, 0, NULL);
    BOOL finished = WaitForSingleObject(t, 6000) == WAIT_OBJECT_0;

    if (pipe != INVALID_HANDLE_VALUE)
        CloseHandle(pipe);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    printf("    call took %llums\n", (unsigned long long)job.elapsed_ms);
    check("call with 1s timeout returns within 6s", finished);
    check("call reports the timeout", job.err == NIPC_ERR_TIMEOUT);
    nipc_client_close(&client);
}

static DWORD WINAPI refresh_fn(LPVOID arg)
{
    nipc_client_refresh((nipc_client_ctx_t *)arg);
    return 0;
}

static void test_abort_ends_refresh_waiting_for_hello_ack(void)
{
    printf("K4: abort ends a refresh waiting for HELLO_ACK\n");
    const char *svc = svc_name("wrob_abort_refresh");
    HANDLE pipe = fake_server_pipe(svc);
    check("fake server pipe created", pipe != INVALID_HANDLE_VALUE);

    /* Only an abort can end the wait early. */
    nipc_client_config_t ccfg = client_config(FALSE, 10000);
    nipc_client_ctx_t client;
    nipc_client_init(&client, TEST_RUN_DIR, svc, &ccfg);

    HANDLE t = test_create_thread(NULL, 0, refresh_fn, &client, 0, NULL);
    BOOL connected = ConnectNamedPipe(pipe, NULL) || GetLastError() == ERROR_PIPE_CONNECTED;
    check("client connected to the fake server", connected);
    nipc_client_abort(&client);
    BOOL finished = WaitForSingleObject(t, 2000) == WAIT_OBJECT_0;

    if (pipe != INVALID_HANDLE_VALUE)
        CloseHandle(pipe);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    check("refresh returns within 2s of the abort", finished);
    check("client is not ready", !nipc_client_ready(&client));
    nipc_client_close(&client);
}

/* ------------------------------------------------------------------ */
/*  N2: shutdown with a worker blocked writing to a non-reading client */
/* ------------------------------------------------------------------ */

static void test_shutdown_not_blocked_by_non_reading_client(void)
{
    printf("N2: shutdown not blocked by a client that stopped reading\n");
    const uint32_t big = 8u * 1024u * 1024u;
    robust_server_t rs = {.service = svc_name("wrob_noread"), .workers = 2,
                          .max_payload = big, .opts = {.raw_len = 4u * 1024u * 1024u}};
    if (!start_server(&rs)) {
        check("server started", 0);
        return;
    }

    nipc_np_client_config_t ccfg = {
        .supported_profiles         = NIPC_PROFILE_BASELINE,
        .max_request_payload_bytes  = 4096,
        .max_request_batch_items    = 1,
        .max_response_payload_bytes = big,
        .max_response_batch_items   = 1,
        .auth_token                 = AUTH_TOKEN,
    };
    nipc_np_session_t session;
    BOOL connected = nipc_np_connect(TEST_RUN_DIR, rs.service, &ccfg, &session) == NIPC_NP_OK;
    check("raw client connected", connected);
    if (connected) {
        uint8_t req[64];
        nipc_cgroups_req_t r = {.layout_version = 1};
        size_t req_len = nipc_cgroups_req_encode(&r, req, sizeof(req));
        nipc_header_t hdr = {.kind = NIPC_KIND_REQUEST, .code = NIPC_METHOD_CGROUPS_SNAPSHOT,
                             .item_count = 1, .message_id = 1};
        check("request sent", nipc_np_send(&session, &hdr, req, req_len) == NIPC_NP_OK);
    }
    /* Never read the response. Once the handler built it, the worker can
     * only be writing, which blocks. */
    check("handler built the 4 MiB response", test_flag_wait(&rs.opts.handled));

    BOOL stopped = stop_server_within(&rs, 3000);
    if (connected)
        nipc_np_close_session(&session);
    if (!stopped)
        WaitForSingleObject(rs.thread, INFINITE);
    check("server stop + destroy finish within 3s while a worker is blocked writing", stopped);
}

/* ------------------------------------------------------------------ */
/*  N3: stop before run                                                */
/* ------------------------------------------------------------------ */

static DWORD WINAPI run_only_fn(LPVOID arg)
{
    nipc_server_run((nipc_managed_server_t *)arg);
    return 0;
}

static void test_stop_before_run_is_honored(void)
{
    printf("N3: stop before run is honored\n");
    robust_server_t rs = {.service = svc_name("wrob_stop_first"), .workers = 2};
    nipc_np_server_config_t scfg = robust_server_config(&rs);
    if (nipc_server_init(&rs.server, TEST_RUN_DIR, rs.service, &scfg, rs.workers,
                         NIPC_METHOD_CGROUPS_SNAPSHOT, robust_handler, &rs.opts) != NIPC_OK) {
        check("server init", 0);
        return;
    }

    nipc_server_stop(&rs.server);
    HANDLE t = test_create_thread(NULL, 0, run_only_fn, &rs.server, 0, NULL);
    BOOL returned = WaitForSingleObject(t, 2000) == WAIT_OBJECT_0;
    if (!returned) {
        nipc_server_stop(&rs.server);
        WaitForSingleObject(t, INFINITE);
    }
    CloseHandle(t);
    nipc_server_destroy(&rs.server);
    check("run after stop returns promptly", returned);
}

/* ------------------------------------------------------------------ */
/*  N5: SHM client notices a crashed server                            */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *service;
    nipc_error_t err;
    ULONGLONG elapsed_ms;
    int calling; /* __atomic */
} dead_server_job_t;

static DWORD WINAPI dead_server_client_fn(LPVOID arg)
{
    dead_server_job_t *job = (dead_server_job_t *)arg;
    nipc_client_config_t ccfg = client_config(TRUE, 10000);
    nipc_client_ctx_t client;
    nipc_client_init(&client, TEST_RUN_DIR, job->service, &ccfg);
    nipc_client_refresh(&client);
    nipc_cgroups_resp_view_t view;
    test_flag_set(&job->calling);
    ULONGLONG start = now_ms();
    job->err = nipc_client_call_cgroups_snapshot_timeout(&client, &view, 10000);
    job->elapsed_ms = now_ms() - start;
    nipc_client_close(&client);
    return 0;
}

static void test_shm_client_notices_crashed_server(void)
{
    printf("N5: SHM client notices a crashed server before its timeout\n");
    const char *svc = svc_name("wrob_dead_server");
    HANDLE helper_out = NULL;
    HANDLE server = spawn_helper("--helper-shm-server", svc, &helper_out);
    check("server helper started", server != NULL);
    if (!server)
        return;

    dead_server_job_t job = {.service = svc};
    HANDLE t = test_create_thread(NULL, 0, dead_server_client_fn, &job, 0, NULL);
    check("client connected", test_flag_wait(&job.calling));
    check("request parked in the stalled handler",
          helper_wait_line(helper_out, "HELPER_HANDLER_ENTERED"));
    kill_helper(server);
    CloseHandle(helper_out);

    BOOL finished = WaitForSingleObject(t, 15000) == WAIT_OBJECT_0;
    if (!finished)
        WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    printf("    call returned err=%d after %llums\n", job.err, job.elapsed_ms);
    check("call fails within 3s of the server crash (timeout is 10s)",
          finished && job.err != NIPC_OK && job.elapsed_ms < 3500);
    check("call reports the server as disconnected",
          finished && job.err == NIPC_ERR_DISCONNECTED);
}

/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/*  N5: SHM attach stops when the server closes the session            */
/* ------------------------------------------------------------------ */

/* A server at worker capacity completes the handshake, then closes the
 * session and destroys the prepared WinSHM objects. The client must not
 * spend its attach retry budget (5 s) on objects that will never appear,
 * and must keep WinSHM enabled for its next connect. */
static void test_shm_attach_stops_when_server_closes_session(void)
{
    printf("N5: SHM attach stops when the server closes the session\n");
    robust_server_t rs = {.service = svc_name("wrob_attach_refused"), .workers = 1, .shm = TRUE};
    if (!start_server(&rs)) {
        check("server started", 0);
        return;
    }

    nipc_client_config_t ccfg = client_config(TRUE, 1000);
    nipc_client_ctx_t holder;
    nipc_client_init(&holder, TEST_RUN_DIR, rs.service, &ccfg);
    nipc_client_refresh(&holder);
    check("first client holds the only worker", nipc_client_ready(&holder));

    nipc_client_ctx_t refused;
    nipc_client_init(&refused, TEST_RUN_DIR, rs.service, &ccfg);
    ULONGLONG start = now_ms();
    nipc_client_refresh(&refused);
    ULONGLONG elapsed = now_ms() - start;
    printf("    refused refresh took %llums\n", (unsigned long long)elapsed);
    check("refused connect returns within 1s (attach retry budget is 5s)", elapsed < 1000);

    nipc_client_close(&holder);
    check("holder's session exited", test_server_wait_session_exits(&rs.events, 1));
    nipc_client_refresh(&refused);
    BOOL ready = nipc_client_ready(&refused);
    check("refused client connects once the worker is free", ready);
    check("refused client still uses WinSHM", ready && refused.shm != NULL);
    nipc_client_close(&refused);
    check("server stops", stop_server_within(&rs, 5000));
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--helper-shm-client") == 0)
        return helper_shm_client(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--helper-shm-server") == 0)
        return helper_shm_server(argv[2]);

    CreateDirectoryA("C:\\Temp", NULL);
    CreateDirectoryA(TEST_RUN_DIR, NULL);
    setvbuf(stdout, NULL, _IONBF, 0);

    test_shm_session_released_after_client_crash();
    test_silent_client_does_not_block_other_clients();
    test_silent_client_does_not_block_shutdown();
    test_silent_client_released_at_handshake_deadline();
    test_client_bounded_when_server_never_sends_hello_ack();
    test_call_reconnect_bounded_by_call_deadline();
    test_abort_ends_refresh_waiting_for_hello_ack();
    test_shutdown_not_blocked_by_non_reading_client();
    test_stop_before_run_is_honored();
    test_shm_client_notices_crashed_server();
    test_shm_attach_stops_when_server_closes_session();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    fflush(stdout);
    /* Exit without joining any thread a failed test may have leaked.
     * _exit keeps the exit status visible to the MSYS runtime, which
     * ExitProcess bypasses. */
    _exit(g_fail ? 1 : 0);
}
