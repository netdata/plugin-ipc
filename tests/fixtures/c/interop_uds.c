/*
 * interop_uds.c - Simple server/client for cross-language UDS interop tests.
 *
 * Usage:
 *   interop_uds server <run_dir> <service_name>
 *     Listens, accepts 1 client, echoes 1 message, exits.
 *
 *   interop_uds client <run_dir> <service_name>
 *     Connects, sends 1 message, verifies echo, exits 0 on success.
 */

#include "netipc/netipc_uds.h"
#include "netipc/netipc_protocol.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define AUTH_TOKEN 0xDEADBEEFCAFEBABEull

static nipc_uds_server_config_t server_config(void)
{
    return (nipc_uds_server_config_t){
        .supported_profiles        = NIPC_PROFILE_BASELINE,
        .max_request_payload_bytes = 65536,
        .max_request_batch_items   = 16,
        .max_response_payload_bytes = 65536,
        .max_response_batch_items  = 16,
        .auth_token                = AUTH_TOKEN,
        .backlog                   = 4,
    };
}

static nipc_uds_client_config_t client_config(void)
{
    return (nipc_uds_client_config_t){
        .supported_profiles        = NIPC_PROFILE_BASELINE,
        .max_request_payload_bytes = 65536,
        .max_request_batch_items   = 16,
        .max_response_payload_bytes = 65536,
        .max_response_batch_items  = 16,
        .auth_token                = AUTH_TOKEN,
    };
}

static int run_server(const char *run_dir, const char *service)
{
    nipc_uds_server_config_t cfg = server_config();
    nipc_uds_listener_t listener;

    nipc_uds_error_t err = nipc_uds_listen(run_dir, service, &cfg, &listener);
    if (err != NIPC_UDS_OK) {
        fprintf(stderr, "server: listen failed: %d\n", err);
        return 1;
    }

    /* Signal readiness to parent via stdout */
    printf("READY\n");
    fflush(stdout);

    nipc_uds_session_t session;
    err = nipc_uds_accept(&listener, 0, &session);
    if (err != NIPC_UDS_OK) {
        fprintf(stderr, "server: accept failed: %d\n", err);
        nipc_uds_close_listener(&listener);
        return 1;
    }

    /* Receive one message */
    uint8_t buf[65536 + 64];
    nipc_header_t hdr;
    const void *payload;
    size_t payload_len;

    err = nipc_uds_receive(&session, buf, sizeof(buf), &hdr, &payload, &payload_len);
    if (err != NIPC_UDS_OK) {
        fprintf(stderr, "server: receive failed: %d\n", err);
        nipc_uds_close_session(&session);
        nipc_uds_close_listener(&listener);
        return 1;
    }

    /* Echo as response */
    nipc_header_t resp = hdr;
    resp.kind = NIPC_KIND_RESPONSE;
    resp.transport_status = NIPC_STATUS_OK;

    err = nipc_uds_send(&session, &resp, payload, payload_len);
    if (err != NIPC_UDS_OK) {
        fprintf(stderr, "server: send failed: %d\n", err);
        nipc_uds_close_session(&session);
        nipc_uds_close_listener(&listener);
        return 1;
    }

    nipc_uds_close_session(&session);
    nipc_uds_close_listener(&listener);
    return 0;
}

static int run_client(const char *run_dir, const char *service)
{
    nipc_uds_client_config_t cfg = client_config();
    nipc_uds_session_t session;

    nipc_uds_error_t err = nipc_uds_connect(run_dir, service, &cfg, &session);
    if (err != NIPC_UDS_OK) {
        fprintf(stderr, "client: connect failed: %d\n", err);
        return 1;
    }

    /* Build a payload with known pattern */
    uint8_t payload[256];
    for (int i = 0; i < (int)sizeof(payload); i++)
        payload[i] = (uint8_t)(i & 0xFF);

    nipc_header_t hdr = {
        .kind       = NIPC_KIND_REQUEST,
        .code       = NIPC_METHOD_INCREMENT,
        .flags      = 0,
        .item_count = 1,
        .message_id = 12345,
    };

    err = nipc_uds_send(&session, &hdr, payload, sizeof(payload));
    if (err != NIPC_UDS_OK) {
        fprintf(stderr, "client: send failed: %d\n", err);
        nipc_uds_close_session(&session);
        return 1;
    }

    /* Receive response */
    uint8_t rbuf[65536 + 64];
    nipc_header_t rhdr;
    const void *rpayload;
    size_t rpayload_len;

    err = nipc_uds_receive(&session, rbuf, sizeof(rbuf), &rhdr, &rpayload, &rpayload_len);
    if (err != NIPC_UDS_OK) {
        fprintf(stderr, "client: receive failed: %d\n", err);
        nipc_uds_close_session(&session);
        return 1;
    }

    /* Verify */
    int ok = 1;
    if (rhdr.kind != NIPC_KIND_RESPONSE) {
        fprintf(stderr, "client: expected RESPONSE, got %u\n", rhdr.kind);
        ok = 0;
    }
    if (rhdr.message_id != 12345) {
        fprintf(stderr, "client: expected message_id 12345, got %lu\n",
                (unsigned long)rhdr.message_id);
        ok = 0;
    }
    if (rpayload_len != sizeof(payload)) {
        fprintf(stderr, "client: payload length mismatch: %zu vs %zu\n",
                rpayload_len, sizeof(payload));
        ok = 0;
    }
    if (rpayload_len == sizeof(payload) && memcmp(rpayload, payload, sizeof(payload)) != 0) {
        fprintf(stderr, "client: payload data mismatch\n");
        ok = 0;
    }

    nipc_uds_close_session(&session);

    if (ok)
        printf("PASS\n");
    else
        printf("FAIL\n");

    return ok ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/*  Pipeline server: echoes <count> messages on one session            */
/* ------------------------------------------------------------------ */

static int run_pipeline_server(const char *run_dir, const char *service, int count)
{
    nipc_uds_server_config_t cfg = server_config();
    nipc_uds_listener_t listener;

    nipc_uds_error_t err = nipc_uds_listen(run_dir, service, &cfg, &listener);
    if (err != NIPC_UDS_OK) {
        fprintf(stderr, "server: listen failed: %d\n", err);
        return 1;
    }

    printf("READY\n");
    fflush(stdout);

    nipc_uds_session_t session;
    err = nipc_uds_accept(&listener, 0, &session);
    if (err != NIPC_UDS_OK) {
        fprintf(stderr, "server: accept failed: %d\n", err);
        nipc_uds_close_listener(&listener);
        return 1;
    }

    for (int i = 0; i < count; i++) {
        uint8_t buf[65536 + 64];
        nipc_header_t hdr;
        const void *payload;
        size_t payload_len;

        err = nipc_uds_receive(&session, buf, sizeof(buf), &hdr, &payload, &payload_len);
        if (err != NIPC_UDS_OK) {
            fprintf(stderr, "server: receive[%d] failed: %d\n", i, err);
            break;
        }

        nipc_header_t resp = hdr;
        resp.kind = NIPC_KIND_RESPONSE;
        resp.transport_status = NIPC_STATUS_OK;

        err = nipc_uds_send(&session, &resp, payload, payload_len);
        if (err != NIPC_UDS_OK) {
            fprintf(stderr, "server: send[%d] failed: %d\n", i, err);
            break;
        }
    }

    nipc_uds_close_session(&session);
    nipc_uds_close_listener(&listener);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Pipeline client: sends <count> requests, then reads <count>        */
/* ------------------------------------------------------------------ */

static int run_pipeline_client(const char *run_dir, const char *service, int count)
{
    nipc_uds_client_config_t cfg = client_config();
    nipc_uds_session_t session;

    nipc_uds_error_t err = nipc_uds_connect(run_dir, service, &cfg, &session);
    if (err != NIPC_UDS_OK) {
        fprintf(stderr, "client: connect failed: %d\n", err);
        return 1;
    }

    /* Send all requests before reading any response */
    for (int i = 0; i < count; i++) {
        uint8_t payload[8];
        uint64_t val = (uint64_t)(i + 1);
        memcpy(payload, &val, 8);

        nipc_header_t hdr = {
            .kind       = NIPC_KIND_REQUEST,
            .code       = NIPC_METHOD_INCREMENT,
            .flags      = 0,
            .item_count = 1,
            .message_id = val,
        };

        err = nipc_uds_send(&session, &hdr, payload, sizeof(payload));
        if (err != NIPC_UDS_OK) {
            fprintf(stderr, "client: send[%d] failed: %d\n", i, err);
            nipc_uds_close_session(&session);
            return 1;
        }
    }

    /* Read all responses and verify */
    int ok = 1;
    for (int i = 0; i < count; i++) {
        uint8_t rbuf[65536 + 64];
        nipc_header_t rhdr;
        const void *rpayload;
        size_t rpayload_len;

        err = nipc_uds_receive(&session, rbuf, sizeof(rbuf), &rhdr, &rpayload, &rpayload_len);
        if (err != NIPC_UDS_OK) {
            fprintf(stderr, "client: receive[%d] failed: %d\n", i, err);
            ok = 0;
            break;
        }

        uint64_t expected = (uint64_t)(i + 1);
        if (rhdr.kind != NIPC_KIND_RESPONSE) {
            fprintf(stderr, "client: [%d] expected RESPONSE, got %u\n", i, rhdr.kind);
            ok = 0;
        }
        if (rhdr.message_id != expected) {
            fprintf(stderr, "client: [%d] message_id %lu, want %lu\n",
                    i, (unsigned long)rhdr.message_id, (unsigned long)expected);
            ok = 0;
        }
        if (rpayload_len != 8) {
            fprintf(stderr, "client: [%d] payload len %zu, want 8\n", i, rpayload_len);
            ok = 0;
        } else {
            uint64_t val;
            memcpy(&val, rpayload, 8);
            if (val != expected) {
                fprintf(stderr, "client: [%d] payload %lu, want %lu\n",
                        i, (unsigned long)val, (unsigned long)expected);
                ok = 0;
            }
        }
    }

    nipc_uds_close_session(&session);

    if (ok)
        printf("PASS\n");
    else
        printf("FAIL\n");

    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);

    if (argc < 4) {
        fprintf(stderr,
            "Usage:\n"
            "  %s server   <run_dir> <service_name>\n"
            "  %s client   <run_dir> <service_name>\n"
            "  %s pipeline <run_dir> <service_name> <count>\n",
            argv[0], argv[0], argv[0]);
        return 1;
    }

    const char *mode = argv[1];
    const char *run_dir = argv[2];
    const char *service = argv[3];

    /* Ensure run_dir exists */
    mkdir(run_dir, 0700);

    if (strcmp(mode, "server") == 0) {
        return run_server(run_dir, service);
    } else if (strcmp(mode, "client") == 0) {
        return run_client(run_dir, service);
    } else if (strcmp(mode, "pipeline") == 0) {
        if (argc < 5) {
            fprintf(stderr, "pipeline mode requires <count> argument\n");
            return 1;
        }
        int count = atoi(argv[4]);
        if (count <= 0) {
            fprintf(stderr, "count must be > 0\n");
            return 1;
        }
        /* If run as "pipeline" with server/client distinction via environment
         * or by convention: the interop script starts server first, then client.
         * We need both. Pipeline server uses count, pipeline client uses count.
         * Use a sub-mode: pipeline-server or pipeline-client detected by env or
         * we use the existing run_test pattern: server prints READY, client prints PASS.
         *
         * For simplicity, the interop test starts:
         *   <binary> pipeline-server <run_dir> <service> <count>
         *   <binary> pipeline-client <run_dir> <service> <count>
         */
        fprintf(stderr, "Use pipeline-server or pipeline-client\n");
        return 1;
    } else if (strcmp(mode, "pipeline-server") == 0) {
        if (argc < 5) {
            fprintf(stderr, "pipeline-server requires <count>\n");
            return 1;
        }
        return run_pipeline_server(run_dir, service, atoi(argv[4]));
    } else if (strcmp(mode, "pipeline-client") == 0) {
        if (argc < 5) {
            fprintf(stderr, "pipeline-client requires <count>\n");
            return 1;
        }
        return run_pipeline_client(run_dir, service, atoi(argv[4]));
    } else {
        fprintf(stderr, "Unknown mode: %s\n", mode);
        return 1;
    }
}
