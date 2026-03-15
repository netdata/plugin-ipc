/*
 * interop_win_shm.c - Server/client for cross-language Windows SHM interop.
 *
 * Usage:
 *   interop_win_shm server <run_dir> <service_name>
 *     Creates SHM region, receives 1 message, echoes it, exits.
 *
 *   interop_win_shm client <run_dir> <service_name>
 *     Attaches to SHM, sends 1 message, verifies echo, exits 0 on success.
 */

#ifdef _WIN32

#include "netipc/netipc_win_shm.h"
#include "netipc/netipc_named_pipe.h"
#include "netipc/netipc_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define AUTH_TOKEN 0xDEADBEEFCAFEBABEull
#define PROFILE   NIPC_WIN_SHM_PROFILE_HYBRID

static int run_server(const char *run_dir, const char *service)
{
    nipc_win_shm_ctx_t ctx;
    nipc_win_shm_error_t err = nipc_win_shm_server_create(
        run_dir, service, AUTH_TOKEN, PROFILE, 65536, 65536, &ctx);
    if (err != NIPC_WIN_SHM_OK) {
        fprintf(stderr, "server: shm create failed: %d\n", err);
        return 1;
    }

    /* Signal readiness */
    printf("READY\n");
    fflush(stdout);

    /* Receive one message */
    uint8_t msg[65536];
    size_t msg_len;
    err = nipc_win_shm_receive(&ctx, msg, sizeof(msg), &msg_len, 10000);
    if (err != NIPC_WIN_SHM_OK) {
        fprintf(stderr, "server: receive failed: %d\n", err);
        nipc_win_shm_destroy(&ctx);
        return 1;
    }

    /* Echo as response: decode header, flip kind, send back */
    if (msg_len >= NIPC_HEADER_LEN) {
        nipc_header_t hdr;
        nipc_header_decode(msg, msg_len, &hdr);
        hdr.kind = NIPC_KIND_RESPONSE;
        hdr.transport_status = NIPC_STATUS_OK;

        size_t payload_len = msg_len - NIPC_HEADER_LEN;
        size_t resp_len = NIPC_HEADER_LEN + payload_len;
        uint8_t *resp_buf = malloc(resp_len);
        if (resp_buf) {
            nipc_header_encode(&hdr, resp_buf, NIPC_HEADER_LEN);
            if (payload_len > 0)
                memcpy(resp_buf + NIPC_HEADER_LEN,
                       msg + NIPC_HEADER_LEN,
                       payload_len);

            err = nipc_win_shm_send(&ctx, resp_buf, resp_len);
            free(resp_buf);
            if (err != NIPC_WIN_SHM_OK) {
                fprintf(stderr, "server: send failed: %d\n", err);
                nipc_win_shm_destroy(&ctx);
                return 1;
            }
        }
    }

    nipc_win_shm_destroy(&ctx);
    return 0;
}

static int run_client(const char *run_dir, const char *service)
{
    nipc_win_shm_ctx_t ctx;

    /* Retry attach -- server may not be fully ready yet. */
    nipc_win_shm_error_t err = NIPC_WIN_SHM_ERR_OPEN_MAPPING;
    for (int i = 0; i < 500; i++) {
        err = nipc_win_shm_client_attach(run_dir, service, AUTH_TOKEN,
                                          PROFILE, &ctx);
        if (err == NIPC_WIN_SHM_OK)
            break;
        if (err == NIPC_WIN_SHM_ERR_OPEN_MAPPING ||
            err == NIPC_WIN_SHM_ERR_BAD_MAGIC)
            Sleep(10);
        else
            break;
    }
    if (err != NIPC_WIN_SHM_OK) {
        fprintf(stderr, "client: attach failed: %d\n", err);
        return 1;
    }

    /* Build a payload with known pattern */
    uint8_t payload[256];
    for (int i = 0; i < (int)sizeof(payload); i++)
        payload[i] = (uint8_t)(i & 0xFF);

    nipc_header_t hdr = {
        .magic       = NIPC_MAGIC_MSG,
        .version     = NIPC_VERSION,
        .header_len  = NIPC_HEADER_LEN,
        .kind        = NIPC_KIND_REQUEST,
        .code        = NIPC_METHOD_INCREMENT,
        .flags       = 0,
        .item_count  = 1,
        .message_id  = 12345,
        .payload_len = sizeof(payload),
        .transport_status = NIPC_STATUS_OK,
    };

    /* Build complete message */
    uint8_t msg_buf[NIPC_HEADER_LEN + 256];
    nipc_header_encode(&hdr, msg_buf, NIPC_HEADER_LEN);
    memcpy(msg_buf + NIPC_HEADER_LEN, payload, sizeof(payload));

    err = nipc_win_shm_send(&ctx, msg_buf, sizeof(msg_buf));
    if (err != NIPC_WIN_SHM_OK) {
        fprintf(stderr, "client: send failed: %d\n", err);
        nipc_win_shm_close(&ctx);
        return 1;
    }

    /* Receive response */
    uint8_t resp[65536];
    size_t resp_len;
    err = nipc_win_shm_receive(&ctx, resp, sizeof(resp), &resp_len, 10000);
    if (err != NIPC_WIN_SHM_OK) {
        fprintf(stderr, "client: receive failed: %d\n", err);
        nipc_win_shm_close(&ctx);
        return 1;
    }

    /* Verify */
    int ok = 1;
    if (resp_len < NIPC_HEADER_LEN) {
        fprintf(stderr, "client: response too short\n");
        ok = 0;
    } else {
        nipc_header_t rhdr;
        nipc_header_decode(resp, resp_len, &rhdr);

        if (rhdr.kind != NIPC_KIND_RESPONSE) {
            fprintf(stderr, "client: expected RESPONSE, got %u\n", rhdr.kind);
            ok = 0;
        }
        if (rhdr.message_id != 12345) {
            fprintf(stderr, "client: expected message_id 12345, got %llu\n",
                    (unsigned long long)rhdr.message_id);
            ok = 0;
        }
        size_t rpayload_len = resp_len - NIPC_HEADER_LEN;
        if (rpayload_len != sizeof(payload)) {
            fprintf(stderr, "client: payload length mismatch: %zu vs %zu\n",
                    rpayload_len, sizeof(payload));
            ok = 0;
        }
        if (rpayload_len == sizeof(payload) &&
            memcmp(resp + NIPC_HEADER_LEN, payload, sizeof(payload)) != 0) {
            fprintf(stderr, "client: payload data mismatch\n");
            ok = 0;
        }
    }

    nipc_win_shm_close(&ctx);

    if (ok)
        printf("PASS\n");
    else
        printf("FAIL\n");

    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <server|client> <run_dir> <service_name>\n", argv[0]);
        return 1;
    }

    const char *mode = argv[1];
    const char *run_dir = argv[2];
    const char *service = argv[3];

    if (strcmp(mode, "server") == 0)
        return run_server(run_dir, service);
    else if (strcmp(mode, "client") == 0)
        return run_client(run_dir, service);
    else {
        fprintf(stderr, "Unknown mode: %s\n", mode);
        return 1;
    }
}

#else /* !_WIN32 */

#include <stdio.h>
int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("Windows SHM interop not supported on this platform\n");
    return 1;
}

#endif /* _WIN32 */
