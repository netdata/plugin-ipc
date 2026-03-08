#include <netipc/netipc_schema.h>

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int write_frame_file(const char *path, const uint8_t frame[NETIPC_FRAME_SIZE]) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return -1;
    }

    size_t written = fwrite(frame, 1, NETIPC_FRAME_SIZE, fp);
    int rc = fclose(fp);
    if (written != NETIPC_FRAME_SIZE || rc != 0) {
        errno = EIO;
        return -1;
    }

    return 0;
}

static int read_frame_file(const char *path, uint8_t frame[NETIPC_FRAME_SIZE]) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return -1;
    }

    size_t n = fread(frame, 1, NETIPC_FRAME_SIZE, fp);
    int rc = fclose(fp);
    if (n != NETIPC_FRAME_SIZE || rc != 0) {
        errno = EIO;
        return -1;
    }

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

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage:\n"
            "  %s encode-req <request_id> <value> <out_file>\n"
            "  %s decode-req <in_file>\n"
            "  %s encode-resp <request_id> <status> <value> <out_file>\n"
            "  %s decode-resp <in_file>\n"
            "  %s serve-once <req_file> <resp_file>\n",
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

    usage(argv[0]);
    return 2;
}
