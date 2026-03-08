#include <netipc/netipc_uds_seqpacket.h>

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t parse_u64(const char *s) {
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (!end || *end != '\0') {
        fprintf(stderr, "invalid integer: %s\n", s);
        exit(2);
    }
    return (uint64_t)v;
}

static uint32_t parse_u32(const char *s) {
    uint64_t v = parse_u64(s);
    if (v > UINT32_MAX) {
        fprintf(stderr, "value out of range for u32: %s\n", s);
        exit(2);
    }
    return (uint32_t)v;
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 7) {
        fprintf(stderr,
                "usage: %s <run_dir> <service> [iterations] [supported_profiles] [preferred_profiles] [auth_token]\n",
                argv[0]);
        return 2;
    }

    uint64_t iterations = 1;
    if (argc >= 4) {
        iterations = parse_u64(argv[3]);
    }

    uint32_t supported_profiles = NETIPC_PROFILE_UDS_SEQPACKET;
    uint32_t preferred_profiles = NETIPC_PROFILE_UDS_SEQPACKET;
    uint64_t auth_token = 0u;

    if (argc >= 5) {
        supported_profiles = parse_u32(argv[4]);
    }
    if (argc >= 6) {
        preferred_profiles = parse_u32(argv[5]);
    }
    if (argc >= 7) {
        auth_token = parse_u64(argv[6]);
    }

    struct netipc_uds_seqpacket_config cfg = {
        .run_dir = argv[1],
        .service_name = argv[2],
        .file_mode = 0600,
        .supported_profiles = supported_profiles,
        .preferred_profiles = preferred_profiles,
        .auth_token = auth_token,
    };

    netipc_uds_seqpacket_server_t *server = NULL;
    if (netipc_uds_seqpacket_server_create(&cfg, &server) != 0) {
        perror("netipc_uds_seqpacket_server_create");
        return 1;
    }

    if (netipc_uds_seqpacket_server_accept(server, 10000) != 0) {
        perror("netipc_uds_seqpacket_server_accept");
        netipc_uds_seqpacket_server_destroy(server);
        return 1;
    }

    printf("negotiated_profile=%u\n", netipc_uds_seqpacket_server_negotiated_profile(server));

    for (uint64_t i = 0; i < iterations; ++i) {
        uint64_t request_id = 0;
        struct netipc_increment_request req;

        if (netipc_uds_seqpacket_server_receive_increment(server, &request_id, &req, 10000) != 0) {
            perror("netipc_uds_seqpacket_server_receive_increment");
            netipc_uds_seqpacket_server_destroy(server);
            return 1;
        }

        struct netipc_increment_response resp = {
            .status = NETIPC_STATUS_OK,
            .value = req.value + 1u,
        };

        if (netipc_uds_seqpacket_server_send_increment(server, request_id, &resp, 10000) != 0) {
            perror("netipc_uds_seqpacket_server_send_increment");
            netipc_uds_seqpacket_server_destroy(server);
            return 1;
        }

        printf("served request_id=%" PRIu64 " value=%" PRIu64 " response=%" PRIu64 "\n",
               request_id,
               req.value,
               resp.value);
    }

    netipc_uds_seqpacket_server_destroy(server);
    return 0;
}
