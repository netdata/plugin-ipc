#ifdef _WIN32

#ifndef TEST_WIN_RAW_CLIENT_HELPERS_H
#define TEST_WIN_RAW_CLIENT_HELPERS_H

#include "netipc/netipc_named_pipe.h"
#include "netipc/netipc_protocol.h"
#include "netipc/netipc_service.h"
#include "netipc/netipc_win_shm.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint32_t test_win_next_power_of_2_u32(uint32_t n)
{
    if (n < 16)
        return 16;

    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    return n + 1;
}

static void test_win_note_request_capacity(nipc_client_ctx_t *ctx,
                                           uint32_t payload_len)
{
    uint32_t grown = test_win_next_power_of_2_u32(payload_len);
    if (grown > ctx->transport_config.max_request_payload_bytes)
        ctx->transport_config.max_request_payload_bytes = grown;
}

static void test_win_note_response_capacity(nipc_client_ctx_t *ctx,
                                            uint32_t payload_len)
{
    uint32_t grown = test_win_next_power_of_2_u32(payload_len);
    if (grown > ctx->transport_config.max_response_payload_bytes)
        ctx->transport_config.max_response_payload_bytes = grown;
}

static void test_win_client_disconnect(nipc_client_ctx_t *ctx)
{
    if (ctx->shm) {
        nipc_win_shm_close(ctx->shm);
        free(ctx->shm);
        ctx->shm = NULL;
    }

    if (ctx->session_valid) {
        nipc_np_close_session(&ctx->session);
        ctx->session_valid = false;
    }
}

static void test_win_mark_broken(nipc_client_ctx_t *ctx)
{
    test_win_client_disconnect(ctx);
    ctx->state = NIPC_CLIENT_BROKEN;
}

static nipc_error_t test_win_transport_send(nipc_client_ctx_t *ctx,
                                            nipc_header_t *hdr,
                                            const void *payload,
                                            size_t payload_len)
{
    if (payload_len > UINT32_MAX)
        return NIPC_ERR_OVERFLOW;

    if (ctx->shm) {
        if (payload_len > ctx->session.max_request_payload_bytes) {
            test_win_note_request_capacity(ctx, (uint32_t)payload_len);
            return NIPC_ERR_OVERFLOW;
        }

        size_t msg_len = NIPC_HEADER_LEN + payload_len;
        uint8_t *msg = ctx->send_buf;
        if (!msg || msg_len > ctx->send_buf_size)
            return NIPC_ERR_OVERFLOW;

        hdr->magic = NIPC_MAGIC_MSG;
        hdr->version = NIPC_VERSION;
        hdr->header_len = NIPC_HEADER_LEN;
        hdr->payload_len = (uint32_t)payload_len;

        nipc_header_encode(hdr, msg, NIPC_HEADER_LEN);
        if (payload_len > 0)
            memcpy(msg + NIPC_HEADER_LEN, payload, payload_len);

        nipc_win_shm_error_t serr = nipc_win_shm_send(ctx->shm, msg, msg_len);
        if (serr == NIPC_WIN_SHM_ERR_MSG_TOO_LARGE) {
            test_win_note_request_capacity(ctx, (uint32_t)payload_len);
            return NIPC_ERR_OVERFLOW;
        }

        return (serr == NIPC_WIN_SHM_OK) ? NIPC_OK : NIPC_ERR_NOT_READY;
    }

    nipc_np_error_t uerr = nipc_np_send(&ctx->session, hdr, payload, payload_len);
    if (uerr == NIPC_NP_ERR_LIMIT_EXCEEDED) {
        test_win_note_request_capacity(ctx, (uint32_t)payload_len);
        return NIPC_ERR_OVERFLOW;
    }

    return (uerr == NIPC_NP_OK) ? NIPC_OK : NIPC_ERR_NOT_READY;
}

static nipc_error_t test_win_transport_receive(nipc_client_ctx_t *ctx,
                                               void *buf,
                                               size_t buf_size,
                                               nipc_header_t *hdr_out,
                                               const void **payload_out,
                                               size_t *payload_len_out)
{
    if (ctx->shm) {
        size_t msg_len = 0;
        nipc_win_shm_error_t serr = nipc_win_shm_receive(
            ctx->shm, buf, buf_size, &msg_len, 30000);
        if (serr != NIPC_WIN_SHM_OK)
            return NIPC_ERR_TRUNCATED;

        if (msg_len < NIPC_HEADER_LEN)
            return NIPC_ERR_TRUNCATED;

        nipc_error_t perr = nipc_header_decode(buf, msg_len, hdr_out);
        if (perr != NIPC_OK)
            return perr;

        *payload_out = (const uint8_t *)buf + NIPC_HEADER_LEN;
        *payload_len_out = msg_len - NIPC_HEADER_LEN;
        return NIPC_OK;
    }

    nipc_np_error_t uerr = nipc_np_receive(&ctx->session, buf, buf_size,
                                           hdr_out, payload_out,
                                           payload_len_out);
    return (uerr == NIPC_NP_OK) ? NIPC_OK : NIPC_ERR_TRUNCATED;
}

static nipc_error_t test_win_do_raw_call(nipc_client_ctx_t *ctx,
                                         uint16_t method_code,
                                         uint16_t flags,
                                         uint32_t item_count,
                                         const void *request_payload,
                                         size_t request_len,
                                         nipc_header_t *resp_hdr_out,
                                         const void **response_payload_out,
                                         size_t *response_len_out)
{
    nipc_header_t req_hdr = {0};
    req_hdr.kind = NIPC_KIND_REQUEST;
    req_hdr.code = method_code;
    req_hdr.flags = flags;
    req_hdr.item_count = item_count;
    req_hdr.message_id = (uint64_t)(ctx->call_count + 1);
    req_hdr.transport_status = NIPC_STATUS_OK;

    nipc_error_t err = test_win_transport_send(ctx, &req_hdr,
                                               request_payload, request_len);
    if (err != NIPC_OK)
        return err;

    err = test_win_transport_receive(ctx, ctx->response_buf, ctx->response_buf_size,
                                     resp_hdr_out, response_payload_out,
                                     response_len_out);
    if (err != NIPC_OK)
        return err;

    if (resp_hdr_out->kind != NIPC_KIND_RESPONSE)
        return NIPC_ERR_BAD_KIND;
    if (resp_hdr_out->code != method_code)
        return NIPC_ERR_BAD_LAYOUT;
    if (resp_hdr_out->message_id != req_hdr.message_id)
        return NIPC_ERR_BAD_LAYOUT;

    switch (resp_hdr_out->transport_status) {
    case NIPC_STATUS_OK:
        return NIPC_OK;

    case NIPC_STATUS_LIMIT_EXCEEDED:
        if (ctx->session.max_response_payload_bytes > 0) {
            uint32_t learned = ctx->session.max_response_payload_bytes;
            if (learned < UINT32_MAX / 2u)
                learned *= 2u;
            else
                learned = UINT32_MAX;
            test_win_note_response_capacity(ctx, learned);
        }
        return NIPC_ERR_OVERFLOW;

    case NIPC_STATUS_UNSUPPORTED:
        return NIPC_ERR_BAD_LAYOUT;

    case NIPC_STATUS_BAD_ENVELOPE:
    case NIPC_STATUS_INTERNAL_ERROR:
    default:
        return NIPC_ERR_BAD_LAYOUT;
    }
}

typedef nipc_error_t (*test_win_attempt_fn)(nipc_client_ctx_t *ctx, void *state);

static nipc_error_t test_win_call_with_retry(nipc_client_ctx_t *ctx,
                                             test_win_attempt_fn attempt,
                                             void *state)
{
    if (ctx->state != NIPC_CLIENT_READY) {
        ctx->error_count++;
        return NIPC_ERR_NOT_READY;
    }

    for (;;) {
        uint32_t prev_req = ctx->session.max_request_payload_bytes;
        uint32_t prev_resp = ctx->session.max_response_payload_bytes;
        uint32_t prev_cfg_req = ctx->transport_config.max_request_payload_bytes;
        uint32_t prev_cfg_resp = ctx->transport_config.max_response_payload_bytes;

        nipc_error_t err = attempt(ctx, state);
        if (err == NIPC_OK) {
            ctx->call_count++;
            return NIPC_OK;
        }

        test_win_mark_broken(ctx);
        (void)nipc_client_refresh(ctx);
        if (ctx->state != NIPC_CLIENT_READY) {
            ctx->error_count++;
            return err;
        }

        if (err != NIPC_ERR_OVERFLOW) {
            err = attempt(ctx, state);
            if (err == NIPC_OK) {
                ctx->call_count++;
                return NIPC_OK;
            }

            test_win_mark_broken(ctx);
            ctx->error_count++;
            return err;
        }

        if (ctx->session.max_request_payload_bytes <= prev_req &&
            ctx->session.max_response_payload_bytes <= prev_resp &&
            ctx->transport_config.max_request_payload_bytes <= prev_cfg_req &&
            ctx->transport_config.max_response_payload_bytes <= prev_cfg_resp) {
            test_win_mark_broken(ctx);
            ctx->error_count++;
            return err;
        }
    }
}

typedef struct {
    uint64_t request;
    uint64_t *response_out;
} test_win_increment_state_t;

static nipc_error_t test_win_increment_attempt(nipc_client_ctx_t *ctx, void *opaque)
{
    test_win_increment_state_t *state = (test_win_increment_state_t *)opaque;
    uint8_t req_buf[NIPC_INCREMENT_PAYLOAD_SIZE];
    size_t req_len = nipc_increment_encode(state->request, req_buf, sizeof(req_buf));
    const void *payload = NULL;
    size_t payload_len = 0;
    nipc_header_t resp_hdr = {0};

    if (req_len == 0)
        return NIPC_ERR_OVERFLOW;

    nipc_error_t err = test_win_do_raw_call(ctx, NIPC_METHOD_INCREMENT, 0, 1,
                                            req_buf, req_len, &resp_hdr,
                                            &payload, &payload_len);
    if (err != NIPC_OK)
        return err;

    return nipc_increment_decode(payload, payload_len, state->response_out);
}

static nipc_error_t test_win_client_call_increment_raw(nipc_client_ctx_t *ctx,
                                                       uint64_t request,
                                                       uint64_t *response_out)
{
    test_win_increment_state_t state = {
        .request = request,
        .response_out = response_out,
    };
    return test_win_call_with_retry(ctx, test_win_increment_attempt, &state);
}

typedef struct {
    const char *request_str;
    uint32_t request_len;
    nipc_string_reverse_view_t *view_out;
} test_win_string_state_t;

static nipc_error_t test_win_string_attempt(nipc_client_ctx_t *ctx, void *opaque)
{
    test_win_string_state_t *state = (test_win_string_state_t *)opaque;
    size_t req_cap = NIPC_STRING_REVERSE_HDR_SIZE + (size_t)state->request_len + 1u;
    uint8_t *req_buf = malloc(req_cap);
    const void *payload = NULL;
    size_t payload_len = 0;
    nipc_header_t resp_hdr = {0};

    if (!req_buf)
        return NIPC_ERR_OVERFLOW;

    size_t req_len = nipc_string_reverse_encode(
        state->request_str, state->request_len, req_buf, req_cap);
    if (req_len == 0) {
        free(req_buf);
        return NIPC_ERR_OVERFLOW;
    }

    nipc_error_t err = test_win_do_raw_call(ctx, NIPC_METHOD_STRING_REVERSE, 0, 1,
                                            req_buf, req_len, &resp_hdr,
                                            &payload, &payload_len);
    free(req_buf);
    if (err != NIPC_OK)
        return err;

    return nipc_string_reverse_decode(payload, payload_len, state->view_out);
}

static nipc_error_t test_win_client_call_string_reverse_raw(
    nipc_client_ctx_t *ctx,
    const char *request_str,
    uint32_t request_len,
    nipc_string_reverse_view_t *view_out)
{
    test_win_string_state_t state = {
        .request_str = request_str,
        .request_len = request_len,
        .view_out = view_out,
    };
    return test_win_call_with_retry(ctx, test_win_string_attempt, &state);
}

typedef struct {
    const uint64_t *requests;
    uint32_t item_count;
    uint64_t *responses;
} test_win_increment_batch_state_t;

static nipc_error_t test_win_increment_batch_attempt(nipc_client_ctx_t *ctx, void *opaque)
{
    test_win_increment_batch_state_t *state =
        (test_win_increment_batch_state_t *)opaque;
    uint32_t item_count = state->item_count;

    if (item_count == 0)
        return NIPC_OK;

    if (item_count == 1) {
        test_win_increment_state_t single = {
            .request = state->requests[0],
            .response_out = &state->responses[0],
        };
        return test_win_increment_attempt(ctx, &single);
    }

    size_t dir_cap = (size_t)((item_count == 0) ? 1u : item_count) * 32u;
    uint8_t *req_buf = malloc(dir_cap);
    const void *payload = NULL;
    size_t payload_len = 0;
    nipc_header_t resp_hdr = {0};

    if (!req_buf)
        return NIPC_ERR_OVERFLOW;

    nipc_batch_builder_t builder;
    nipc_batch_builder_init(&builder, req_buf, dir_cap, item_count);
    for (uint32_t i = 0; i < item_count; i++) {
        uint8_t item_buf[NIPC_INCREMENT_PAYLOAD_SIZE];
        size_t item_len = nipc_increment_encode(state->requests[i],
                                                item_buf, sizeof(item_buf));
        if (item_len == 0) {
            free(req_buf);
            return NIPC_ERR_OVERFLOW;
        }
        nipc_error_t berr = nipc_batch_builder_add(&builder, item_buf, item_len);
        if (berr != NIPC_OK) {
            free(req_buf);
            return berr;
        }
    }

    uint32_t built_items = 0;
    size_t req_len = nipc_batch_builder_finish(&builder, &built_items);
    nipc_error_t err = test_win_do_raw_call(ctx, NIPC_METHOD_INCREMENT,
                                            NIPC_FLAG_BATCH, built_items,
                                            req_buf, req_len, &resp_hdr,
                                            &payload, &payload_len);
    free(req_buf);
    if (err != NIPC_OK)
        return err;

    if (built_items == 1 && !(resp_hdr.flags & NIPC_FLAG_BATCH)) {
        return nipc_increment_decode(payload, payload_len, &state->responses[0]);
    }

    if (!(resp_hdr.flags & NIPC_FLAG_BATCH) || resp_hdr.item_count != built_items)
        return NIPC_ERR_BAD_LAYOUT;

    for (uint32_t i = 0; i < built_items; i++) {
        const void *item_ptr = NULL;
        uint32_t item_len = 0;
        err = nipc_batch_item_get(payload, payload_len, built_items, i,
                                  &item_ptr, &item_len);
        if (err != NIPC_OK)
            return err;
        err = nipc_increment_decode(item_ptr, item_len, &state->responses[i]);
        if (err != NIPC_OK)
            return err;
    }

    return NIPC_OK;
}

static nipc_error_t test_win_client_call_increment_batch_raw(
    nipc_client_ctx_t *ctx,
    const uint64_t *requests,
    uint32_t item_count,
    uint64_t *responses)
{
    test_win_increment_batch_state_t state = {
        .requests = requests,
        .item_count = item_count,
        .responses = responses,
    };
    return test_win_call_with_retry(ctx, test_win_increment_batch_attempt, &state);
}

#endif /* TEST_WIN_RAW_CLIENT_HELPERS_H */

#endif /* _WIN32 */
