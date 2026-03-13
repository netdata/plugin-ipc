#include <netipc/netipc_schema.h>

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int write_bytes_file(const char *path, const uint8_t *data, size_t data_len) {
    FILE *fp = fopen(path, "wb");
    size_t written;
    int rc;

    if (!fp) {
        return -1;
    }

    written = fwrite(data, 1, data_len, fp);
    rc = fclose(fp);
    if (written != data_len || rc != 0) {
        errno = EIO;
        return -1;
    }

    return 0;
}

static int read_file_alloc(const char *path, uint8_t **data, size_t *data_len) {
    FILE *fp = fopen(path, "rb");
    long file_len;
    uint8_t *buf;
    size_t nread;
    int rc;

    if (!fp || !data || !data_len) {
        errno = EINVAL;
        return -1;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        rc = fclose(fp);
        (void)rc;
        return -1;
    }

    file_len = ftell(fp);
    if (file_len < 0) {
        rc = fclose(fp);
        (void)rc;
        return -1;
    }

    if (fseek(fp, 0, SEEK_SET) != 0) {
        rc = fclose(fp);
        (void)rc;
        return -1;
    }

    buf = malloc((size_t)file_len);
    if (!buf) {
        rc = fclose(fp);
        (void)rc;
        errno = ENOMEM;
        return -1;
    }

    nread = fread(buf, 1, (size_t)file_len, fp);
    rc = fclose(fp);
    if (nread != (size_t)file_len || rc != 0) {
        free(buf);
        errno = EIO;
        return -1;
    }

    *data = buf;
    *data_len = (size_t)file_len;
    return 0;
}

static int write_frame_file(const char *path, const uint8_t frame[NETIPC_FRAME_SIZE]) {
    return write_bytes_file(path, frame, NETIPC_FRAME_SIZE);
}

static int read_frame_file(const char *path, uint8_t frame[NETIPC_FRAME_SIZE]) {
    uint8_t *data = NULL;
    size_t data_len = 0;

    if (read_file_alloc(path, &data, &data_len) != 0) {
        return -1;
    }

    if (data_len != NETIPC_FRAME_SIZE) {
        free(data);
        errno = EIO;
        return -1;
    }

    memcpy(frame, data, NETIPC_FRAME_SIZE);
    free(data);
    return 0;
}

static uint64_t parse_u64(const char *s) {
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno != 0 || !end || *end != '\0') {
        fprintf(stderr, "invalid u64: %s\n", s);
        exit(2);
    }
    return (uint64_t)v;
}

static int32_t parse_i32(const char *s) {
    errno = 0;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (errno != 0 || !end || *end != '\0' || v < INT32_MIN || v > INT32_MAX) {
        fprintf(stderr, "invalid i32: %s\n", s);
        exit(2);
    }
    return (int32_t)v;
}

static int validate_cgroups_request_header(const uint8_t *message,
                                           size_t message_len,
                                           struct netipc_msg_header *header) {
    if (netipc_decode_msg_header(message, message_len, header) != 0) {
        return -1;
    }

    if (header->kind != NETIPC_MSG_KIND_REQUEST ||
        header->code != NETIPC_METHOD_CGROUPS_SNAPSHOT ||
        header->transport_status != NETIPC_TRANSPORT_STATUS_OK ||
        header->flags != 0u ||
        header->item_count != 1u ||
        header->payload_len != NETIPC_CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN) {
        errno = EPROTO;
        return -1;
    }

    if (netipc_msg_total_size(header) != message_len) {
        errno = EPROTO;
        return -1;
    }

    return 0;
}

static int validate_cgroups_response_header(const uint8_t *message,
                                            size_t message_len,
                                            struct netipc_msg_header *header) {
    if (netipc_decode_msg_header(message, message_len, header) != 0) {
        return -1;
    }

    if (header->kind != NETIPC_MSG_KIND_RESPONSE ||
        header->code != NETIPC_METHOD_CGROUPS_SNAPSHOT ||
        header->transport_status != NETIPC_TRANSPORT_STATUS_OK ||
        header->flags != NETIPC_MSG_FLAG_BATCH) {
        errno = EPROTO;
        return -1;
    }

    if (netipc_msg_total_size(header) != message_len) {
        errno = EPROTO;
        return -1;
    }

    return 0;
}

static int encode_cgroups_request_message(uint64_t message_id, const char *out_path) {
    uint8_t payload[NETIPC_CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN];
    uint8_t message[NETIPC_MSG_HEADER_LEN + NETIPC_CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN];
    struct netipc_msg_header header;
    struct netipc_cgroups_snapshot_request request = {.flags = 0u};

    if (netipc_encode_cgroups_snapshot_request_payload(payload, sizeof(payload), &request) != 0) {
        return -1;
    }

    header.magic = NETIPC_MSG_MAGIC;
    header.version = NETIPC_MSG_VERSION;
    header.header_len = NETIPC_MSG_HEADER_LEN;
    header.kind = NETIPC_MSG_KIND_REQUEST;
    header.flags = 0u;
    header.code = NETIPC_METHOD_CGROUPS_SNAPSHOT;
    header.transport_status = NETIPC_TRANSPORT_STATUS_OK;
    header.payload_len = NETIPC_CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN;
    header.item_count = 1u;
    header.message_id = message_id;

    if (netipc_encode_msg_header(message, sizeof(message), &header) != 0) {
        return -1;
    }

    memcpy(message + NETIPC_MSG_HEADER_LEN, payload, sizeof(payload));
    return write_bytes_file(out_path, message, sizeof(message));
}

static int write_fixed_cgroups_response(uint64_t message_id, const char *out_path) {
    uint8_t payload[1024];
    uint8_t *message = NULL;
    size_t payload_len = 0;
    size_t message_len = 0;
    struct netipc_msg_header header;
    struct netipc_cgroups_snapshot_builder builder;
    struct netipc_cgroups_snapshot_item item;
    int rc = -1;

    if (netipc_cgroups_snapshot_builder_init(&builder, payload, sizeof(payload), 42u, 1u, 3u, 2u) != 0) {
        return -1;
    }

    item.hash = 123u;
    item.options = 0x2u;
    item.enabled = 1u;
    item.name = "system.slice-nginx";
    item.name_len = (uint32_t)strlen(item.name);
    item.path = "/sys/fs/cgroup/system.slice/nginx.service/cgroup.procs";
    item.path_len = (uint32_t)strlen(item.path);
    if (netipc_cgroups_snapshot_builder_add_item(&builder, &item) != 0) {
        return -1;
    }

    item.hash = 456u;
    item.options = 0x4u;
    item.enabled = 0u;
    item.name = "docker-1234";
    item.name_len = (uint32_t)strlen(item.name);
    item.path = "";
    item.path_len = 0u;
    if (netipc_cgroups_snapshot_builder_add_item(&builder, &item) != 0) {
        return -1;
    }

    if (netipc_cgroups_snapshot_builder_finish(&builder, &payload_len) != 0) {
        return -1;
    }

    message_len = NETIPC_MSG_HEADER_LEN + payload_len;
    message = malloc(message_len);
    if (!message) {
        errno = ENOMEM;
        return -1;
    }

    header.magic = NETIPC_MSG_MAGIC;
    header.version = NETIPC_MSG_VERSION;
    header.header_len = NETIPC_MSG_HEADER_LEN;
    header.kind = NETIPC_MSG_KIND_RESPONSE;
    header.flags = NETIPC_MSG_FLAG_BATCH;
    header.code = NETIPC_METHOD_CGROUPS_SNAPSHOT;
    header.transport_status = NETIPC_TRANSPORT_STATUS_OK;
    header.payload_len = (uint32_t)payload_len;
    header.item_count = 2u;
    header.message_id = message_id;

    if (netipc_encode_msg_header(message, message_len, &header) != 0) {
        goto cleanup;
    }

    memcpy(message + NETIPC_MSG_HEADER_LEN, payload, payload_len);
    rc = write_bytes_file(out_path, message, message_len);

cleanup:
    free(message);
    return rc;
}

static int decode_and_print_cgroups_request(const char *path) {
    uint8_t *message = NULL;
    size_t message_len = 0;
    struct netipc_msg_header header;
    struct netipc_cgroups_snapshot_request_view view;
    int rc = -1;

    if (read_file_alloc(path, &message, &message_len) != 0 ||
        validate_cgroups_request_header(message, message_len, &header) != 0 ||
        netipc_decode_cgroups_snapshot_request_view(message + NETIPC_MSG_HEADER_LEN,
                                                    header.payload_len,
                                                    &view) != 0) {
        goto cleanup;
    }

    printf("CGROUPS_REQ\t%" PRIu64 "\t%u\n", header.message_id, (unsigned)view.flags);
    rc = 0;

cleanup:
    free(message);
    return rc;
}

static int decode_and_print_cgroups_response(const char *path) {
    uint8_t *message = NULL;
    size_t message_len = 0;
    struct netipc_msg_header header;
    struct netipc_cgroups_snapshot_view view;
    struct netipc_cgroups_snapshot_item_view item_view;
    uint32_t i;
    int rc = -1;

    if (read_file_alloc(path, &message, &message_len) != 0 ||
        validate_cgroups_response_header(message, message_len, &header) != 0 ||
        netipc_decode_cgroups_snapshot_view(message + NETIPC_MSG_HEADER_LEN,
                                            header.payload_len,
                                            header.item_count,
                                            &view) != 0) {
        goto cleanup;
    }

    printf("CGROUPS_RESP\t%" PRIu64 "\t%" PRIu64 "\t%u\t%u\n",
           header.message_id,
           view.generation,
           (unsigned)view.systemd_enabled,
           (unsigned)view.item_count);

    for (i = 0u; i < view.item_count; ++i) {
        if (netipc_cgroups_snapshot_view_item_at(&view, i, &item_view) != 0) {
            goto cleanup;
        }

        printf("ITEM\t%u\t%u\t%u\t%u\t%.*s\t%.*s\n",
               (unsigned)i,
               item_view.hash,
               item_view.options,
               item_view.enabled,
               (int)item_view.name_view.len,
               item_view.name_view.ptr,
               (int)item_view.path_view.len,
               item_view.path_view.ptr);
    }

    rc = 0;

cleanup:
    free(message);
    return rc;
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage:\n"
            "  %s encode-req <request_id> <value> <out_file>\n"
            "  %s decode-req <in_file>\n"
            "  %s encode-resp <request_id> <status> <value> <out_file>\n"
            "  %s decode-resp <in_file>\n"
            "  %s serve-once <req_file> <resp_file>\n"
            "  %s encode-cgroups-req <message_id> <out_file>\n"
            "  %s decode-cgroups-req <in_file>\n"
            "  %s encode-cgroups-resp <message_id> <out_file>\n"
            "  %s decode-cgroups-resp <in_file>\n"
            "  %s serve-cgroups-once <req_file> <resp_file>\n",
            argv0,
            argv0,
            argv0,
            argv0,
            argv0,
            argv0,
            argv0,
            argv0,
            argv0,
            argv0);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    if (strcmp(argv[1], "encode-req") == 0) {
        if (argc != 5) {
            usage(argv[0]);
            return 2;
        }

        uint8_t frame[NETIPC_FRAME_SIZE];
        uint64_t request_id = parse_u64(argv[2]);
        struct netipc_increment_request req = {.value = parse_u64(argv[3])};

        if (netipc_encode_increment_request(frame, request_id, &req) != 0 ||
            write_frame_file(argv[4], frame) != 0) {
            perror("encode-req");
            return 1;
        }

        return 0;
    }

    if (strcmp(argv[1], "decode-req") == 0) {
        if (argc != 3) {
            usage(argv[0]);
            return 2;
        }

        uint8_t frame[NETIPC_FRAME_SIZE];
        uint64_t request_id = 0;
        struct netipc_increment_request req;

        if (read_frame_file(argv[2], frame) != 0 ||
            netipc_decode_increment_request(frame, &request_id, &req) != 0) {
            perror("decode-req");
            return 1;
        }

        printf("REQ %" PRIu64 " %" PRIu64 "\n", request_id, req.value);
        return 0;
    }

    if (strcmp(argv[1], "encode-resp") == 0) {
        if (argc != 6) {
            usage(argv[0]);
            return 2;
        }

        uint8_t frame[NETIPC_FRAME_SIZE];
        uint64_t request_id = parse_u64(argv[2]);
        struct netipc_increment_response resp = {
            .status = parse_i32(argv[3]),
            .value = parse_u64(argv[4]),
        };

        if (netipc_encode_increment_response(frame, request_id, &resp) != 0 ||
            write_frame_file(argv[5], frame) != 0) {
            perror("encode-resp");
            return 1;
        }

        return 0;
    }

    if (strcmp(argv[1], "decode-resp") == 0) {
        if (argc != 3) {
            usage(argv[0]);
            return 2;
        }

        uint8_t frame[NETIPC_FRAME_SIZE];
        uint64_t request_id = 0;
        struct netipc_increment_response resp;

        if (read_frame_file(argv[2], frame) != 0 ||
            netipc_decode_increment_response(frame, &request_id, &resp) != 0) {
            perror("decode-resp");
            return 1;
        }

        printf("RESP %" PRIu64 " %" PRId32 " %" PRIu64 "\n", request_id, resp.status, resp.value);
        return 0;
    }

    if (strcmp(argv[1], "serve-once") == 0) {
        if (argc != 4) {
            usage(argv[0]);
            return 2;
        }

        uint8_t frame[NETIPC_FRAME_SIZE];
        uint8_t out[NETIPC_FRAME_SIZE];
        uint64_t request_id = 0;
        struct netipc_increment_request req;
        struct netipc_increment_response resp;

        if (read_frame_file(argv[2], frame) != 0 ||
            netipc_decode_increment_request(frame, &request_id, &req) != 0) {
            perror("serve-once decode");
            return 1;
        }

        resp.status = NETIPC_STATUS_OK;
        resp.value = req.value + 1u;

        if (netipc_encode_increment_response(out, request_id, &resp) != 0 ||
            write_frame_file(argv[3], out) != 0) {
            perror("serve-once encode");
            return 1;
        }

        return 0;
    }

    if (strcmp(argv[1], "encode-cgroups-req") == 0) {
        if (argc != 4) {
            usage(argv[0]);
            return 2;
        }

        if (encode_cgroups_request_message(parse_u64(argv[2]), argv[3]) != 0) {
            perror("encode-cgroups-req");
            return 1;
        }

        return 0;
    }

    if (strcmp(argv[1], "decode-cgroups-req") == 0) {
        if (argc != 3) {
            usage(argv[0]);
            return 2;
        }

        if (decode_and_print_cgroups_request(argv[2]) != 0) {
            perror("decode-cgroups-req");
            return 1;
        }

        return 0;
    }

    if (strcmp(argv[1], "encode-cgroups-resp") == 0) {
        if (argc != 4) {
            usage(argv[0]);
            return 2;
        }

        if (write_fixed_cgroups_response(parse_u64(argv[2]), argv[3]) != 0) {
            perror("encode-cgroups-resp");
            return 1;
        }

        return 0;
    }

    if (strcmp(argv[1], "decode-cgroups-resp") == 0) {
        if (argc != 3) {
            usage(argv[0]);
            return 2;
        }

        if (decode_and_print_cgroups_response(argv[2]) != 0) {
            perror("decode-cgroups-resp");
            return 1;
        }

        return 0;
    }

    if (strcmp(argv[1], "serve-cgroups-once") == 0) {
        uint8_t *message = NULL;
        size_t message_len = 0;
        struct netipc_msg_header header;

        if (argc != 4) {
            usage(argv[0]);
            return 2;
        }

        if (read_file_alloc(argv[2], &message, &message_len) != 0 ||
            validate_cgroups_request_header(message, message_len, &header) != 0) {
            perror("serve-cgroups-once decode");
            free(message);
            return 1;
        }
        free(message);

        if (write_fixed_cgroups_response(header.message_id, argv[3]) != 0) {
            perror("serve-cgroups-once encode");
            return 1;
        }

        return 0;
    }

    usage(argv[0]);
    return 2;
}
