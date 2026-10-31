/*
 * test_win_robustness.c - Robustness reproduction tests (SOW-0040), Windows.
 *
 * Each test asserts the correct behavior. Every blocking step runs on a
 * helper thread waited with a deadline, so a regression fails instead of
 * hanging. Bounds are deliberately lenient so they do not dictate a fix.
 *
 * Tests:
 *   K2  SHM session released after client crash
 *   K3  silent client does not block other clients
 *   K3  silent client does not block shutdown
 *   K3  silent client's worker is freed at the server handshake deadline
 *   K4  client bounded when server never sends HELLO_ACK
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
    DWORD  delay_ms;
    size_t raw_len; /* if non-zero, reply with raw_len opaque bytes */
} handler_opts_t;

static nipc_error_t robust_handler(void *user, const nipc_header_t *request_hdr,
                                   const uint8_t *request_payload, size_t request_len,
                                   uint8_t *response_buf, size_t response_buf_size,
                                   size_t *response_len_out)
{
    const handler_opts_t *opts = (const handler_opts_t *)user;
    (void)request_hdr;

    if (opts && opts->delay_ms)
        Sleep(opts->delay_ms);

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
    BOOL shm;
    uint32_t max_payload;
    handler_opts_t opts;
    nipc_managed_server_t server;
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
    rs->thread = CreateThread(NULL, 0, robust_server_fn, rs, 0, NULL);
    Sleep(100);
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

static BOOL served_within(const char *service, BOOL shm, DWORD limit_ms)
{
    ULONGLONG deadline = now_ms() + limit_ms;
    nipc_client_config_t ccfg = client_config(shm, 1000);
    while (now_ms() < deadline) {
        nipc_client_ctx_t client;
        nipc_client_init(&client, TEST_RUN_DIR, service, &ccfg);
        nipc_client_refresh(&client);
        if (nipc_client_ready(&client)) {
            nipc_cgroups_resp_view_t view;
            nipc_error_t err = nipc_client_call_cgroups_snapshot_timeout(&client, &view, 1000);
            BOOL ok = (err == NIPC_OK && view.item_count == 1);
            nipc_client_close(&client);
            if (ok)
                return TRUE;
        } else {
            nipc_client_close(&client);
        }
        Sleep(50);
    }
    return FALSE;
}

typedef struct {
    const char *service;
    DWORD limit_ms;
    BOOL result;
} served_job_t;

static DWORD WINAPI served_job_fn(LPVOID arg)
{
    served_job_t *job = (served_job_t *)arg;
    job->result = served_within(job->service, FALSE, job->limit_ms);
    return 0;
}

static HANDLE open_pipe_silently(const char *service)
{
    wchar_t name[NIPC_NP_MAX_PIPE_NAME];
    nipc_np_build_pipe_name(name, NIPC_NP_MAX_PIPE_NAME, TEST_RUN_DIR, service);
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 100 && h == INVALID_HANDLE_VALUE; i++) {
        h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE)
            Sleep(20);
    }
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
    for (int i = 0; i < 200 && !nipc_client_ready(&client); i++) {
        nipc_client_refresh(&client);
        Sleep(10);
    }
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
    robust_server_t rs = {.service = service, .workers = 2, .shm = TRUE,
                          .opts = {.delay_ms = 60000}};
    if (!start_server(&rs)) {
        printf("HELPER_FAILED\n");
        return 1;
    }
    printf("HELPER_READY\n");
    fflush(stdout);
    Sleep(INFINITE);
    return 0;
}

/* Spawn this binary in helper mode and wait for HELPER_READY. */
static HANDLE spawn_helper(const char *mode, const char *service)
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

    char buf[256];
    DWORD used = 0;
    BOOL ready = FALSE;
    ULONGLONG deadline = now_ms() + 10000;
    while (!ready && now_ms() < deadline && used < sizeof(buf) - 1) {
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL))
            break;
        if (avail == 0) {
            Sleep(20);
            continue;
        }
        DWORD n = 0;
        if (!ReadFile(rd, buf + used, sizeof(buf) - 1 - used, &n, NULL) || n == 0)
            break;
        used += n;
        buf[used] = '\0';
        ready = strstr(buf, "HELPER_READY") != NULL;
    }
    CloseHandle(rd);
    if (!ready) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        return NULL;
    }
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
        HANDLE p = spawn_helper("--helper-shm-client", rs.service);
        if (!p) {
            spawned = FALSE;
            break;
        }
        kill_helper(p);
    }
    check("crash helpers held SHM sessions", spawned);
    check("new client served after 2 crashed SHM clients on 2 workers",
          served_within(rs.service, TRUE, 8000));
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
    Sleep(100);

    served_job_t job = {.service = rs.service, .limit_ms = 5000};
    HANDLE t = CreateThread(NULL, 0, served_job_fn, &job, 0, NULL);
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
    Sleep(100);
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
    BOOL served = served_within(rs.service, FALSE, NIPC_SERVER_HANDSHAKE_TIMEOUT_MS + 4000);
    check("the only worker serves a new client once the silent one misses the deadline",
          served);

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
    HANDLE t = CreateThread(NULL, 0, bounded_client_fn, &job, 0, NULL);
    BOOL finished = WaitForSingleObject(t, 6000) == WAIT_OBJECT_0;

    if (pipe != INVALID_HANDLE_VALUE)
        CloseHandle(pipe);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    check("refresh + call with 1s timeout return within 6s", finished);
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
    /* Never read the response; give the worker time to block in WriteFile. */
    Sleep(500);

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
    HANDLE t = CreateThread(NULL, 0, run_only_fn, &rs.server, 0, NULL);
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
    volatile LONG calling;
} dead_server_job_t;

static DWORD WINAPI dead_server_client_fn(LPVOID arg)
{
    dead_server_job_t *job = (dead_server_job_t *)arg;
    nipc_client_config_t ccfg = client_config(TRUE, 10000);
    nipc_client_ctx_t client;
    nipc_client_init(&client, TEST_RUN_DIR, job->service, &ccfg);
    for (int i = 0; i < 200 && !nipc_client_ready(&client); i++) {
        nipc_client_refresh(&client);
        Sleep(10);
    }
    nipc_cgroups_resp_view_t view;
    InterlockedExchange(&job->calling, 1);
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
    HANDLE server = spawn_helper("--helper-shm-server", svc);
    check("server helper started", server != NULL);
    if (!server)
        return;

    dead_server_job_t job = {.service = svc};
    HANDLE t = CreateThread(NULL, 0, dead_server_client_fn, &job, 0, NULL);
    for (int i = 0; i < 500 && !InterlockedCompareExchange(&job.calling, 0, 0); i++)
        Sleep(10);
    Sleep(500); /* request is now parked in the stalled handler */
    kill_helper(server);

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
    for (int i = 0; i < 200 && !nipc_client_ready(&holder); i++) {
        nipc_client_refresh(&holder);
        Sleep(10);
    }
    check("first client holds the only worker", nipc_client_ready(&holder));

    nipc_client_ctx_t refused;
    nipc_client_init(&refused, TEST_RUN_DIR, rs.service, &ccfg);
    ULONGLONG start = now_ms();
    nipc_client_refresh(&refused);
    ULONGLONG elapsed = now_ms() - start;
    printf("    refused refresh took %llums\n", (unsigned long long)elapsed);
    check("refused connect returns within 1s (attach retry budget is 5s)", elapsed < 1000);

    nipc_client_close(&holder);
    BOOL ready = FALSE;
    for (int i = 0; i < 300 && !ready; i++) {
        nipc_client_refresh(&refused);
        ready = nipc_client_ready(&refused);
        if (!ready)
            Sleep(10);
    }
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
