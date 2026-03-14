/*
 * interop_shm.c - Simple server/client for cross-language SHM interop tests.
 *
 * Usage:
 *   interop_shm server <run_dir> <service_name>
 *     Creates SHM region, receives 1 message, echoes it, exits.
 *
 *   interop_shm client <run_dir> <service_name>
 *     Attaches to SHM, sends 1 message, verifies echo, exits 0 on success.
 */

#include "netipc/netipc_shm.h"
#include "netipc/netipc_protocol.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int run_server(const char *run_dir, const char *service)
{
    nipc_shm_ctx_t shm;
    nipc_shm_error_t err = nipc_shm_server_create(
        run_dir, service, 65536, 65536, &shm);
    if (err != NIPC_SHM_OK) {
        fprintf(stderr, "server: shm create failed: %d\n", err);
        return 1;
    }

    /* Signal readiness */
    printf("READY\n");
    fflush(stdout);

    /* Receive one message */
    const void *msg;
    size_t msg_len;
    err = nipc_shm_receive(&shm, &msg, &msg_len, 10000);
    if (err != NIPC_SHM_OK) {
        fprintf(stderr, "server: receive failed: %d\n", err);
        nipc_shm_destroy(&shm);
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
                       (const uint8_t *)msg + NIPC_HEADER_LEN,
                       payload_len);

            err = nipc_shm_send(&shm, resp_buf, resp_len);
            free(resp_buf);
            if (err != NIPC_SHM_OK) {
                fprintf(stderr, "server: send failed: %d\n", err);
                nipc_shm_destroy(&shm);
                return 1;
            }
        }
    }

    nipc_shm_destroy(&shm);
    return 0;
}

static int run_client(const char *run_dir, const char *service)
{
    nipc_shm_ctx_t shm;

    /* Retry attach -- server may not be fully ready yet. */
    nipc_shm_error_t err = NIPC_SHM_ERR_NOT_READY;
    for (int i = 0; i < 500; i++) {
        err = nipc_shm_client_attach(run_dir, service, &shm);
        if (err == NIPC_SHM_OK)
            break;
        if (err == NIPC_SHM_ERR_NOT_READY ||
            err == NIPC_SHM_ERR_OPEN ||
            err == NIPC_SHM_ERR_BAD_MAGIC)
            usleep(10000);
        else
            break;
    }
    if (err != NIPC_SHM_OK) {
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
    uint8_t msg_buf[NIPC_HEADER_LEN + sizeof(payload)];
    nipc_header_encode(&hdr, msg_buf, NIPC_HEADER_LEN);
    memcpy(msg_buf + NIPC_HEADER_LEN, payload, sizeof(payload));

    err = nipc_shm_send(&shm, msg_buf, sizeof(msg_buf));
    if (err != NIPC_SHM_OK) {
        fprintf(stderr, "client: send failed: %d\n", err);
        nipc_shm_close(&shm);
        return 1;
    }

    /* Receive response */
    const void *resp;
    size_t resp_len;
    err = nipc_shm_receive(&shm, &resp, &resp_len, 10000);
    if (err != NIPC_SHM_OK) {
        fprintf(stderr, "client: receive failed: %d\n", err);
        nipc_shm_close(&shm);
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
            fprintf(stderr, "client: expected message_id 12345, got %lu\n",
                    (unsigned long)rhdr.message_id);
            ok = 0;
        }
        size_t rpayload_len = resp_len - NIPC_HEADER_LEN;
        if (rpayload_len != sizeof(payload)) {
            fprintf(stderr, "client: payload length mismatch: %zu vs %zu\n",
                    rpayload_len, sizeof(payload));
            ok = 0;
        }
        if (rpayload_len == sizeof(payload) &&
            memcmp((const uint8_t *)resp + NIPC_HEADER_LEN, payload, sizeof(payload)) != 0) {
            fprintf(stderr, "client: payload data mismatch\n");
            ok = 0;
        }
    }

    nipc_shm_close(&shm);

    if (ok)
        printf("PASS\n");
    else
        printf("FAIL\n");

    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);

    if (argc != 4) {
        fprintf(stderr, "Usage: %s <server|client> <run_dir> <service_name>\n", argv[0]);
        return 1;
    }

    const char *mode = argv[1];
    const char *run_dir = argv[2];
    const char *service = argv[3];

    /* Ensure run_dir exists */
    mkdir(run_dir, 0700);

    if (strcmp(mode, "server") == 0)
        return run_server(run_dir, service);
    else if (strcmp(mode, "client") == 0)
        return run_client(run_dir, service);
    else {
        fprintf(stderr, "Unknown mode: %s\n", mode);
        return 1;
    }
}
