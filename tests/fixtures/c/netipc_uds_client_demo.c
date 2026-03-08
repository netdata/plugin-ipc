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
    if (argc < 4 || argc > 8) {
        fprintf(stderr,
                "usage: %s <run_dir> <service> <start_value> [iterations] [supported_profiles] [preferred_profiles] [auth_token]\n",
                argv[0]);
        return 2;
    }

    uint64_t value = parse_u64(argv[3]);
    uint64_t iterations = 1;
    if (argc >= 5) {
        iterations = parse_u64(argv[4]);
    }

    uint32_t supported_profiles = NETIPC_PROFILE_UDS_SEQPACKET;
    uint32_t preferred_profiles = NETIPC_PROFILE_UDS_SEQPACKET;
    uint64_t auth_token = 0u;

    if (argc >= 6) {
        supported_profiles = parse_u32(argv[5]);
    }
    if (argc >= 7) {
        preferred_profiles = parse_u32(argv[6]);
    }
    if (argc >= 8) {
        auth_token = parse_u64(argv[7]);
    }

    struct netipc_uds_seqpacket_config cfg = {
        .run_dir = argv[1],
        .service_name = argv[2],
        .file_mode = 0600,
        .supported_profiles = supported_profiles,
        .preferred_profiles = preferred_profiles,
        .auth_token = auth_token,
    };

    netipc_uds_seqpacket_client_t *client = NULL;
    if (netipc_uds_seqpacket_client_create(&cfg, &client, 10000) != 0) {
        perror("netipc_uds_seqpacket_client_create");
        return 1;
    }

    printf("negotiated_profile=%u\n", netipc_uds_seqpacket_client_negotiated_profile(client));

    for (uint64_t i = 0; i < iterations; ++i) {
        struct netipc_increment_request req = {.value = value};
        struct netipc_increment_response resp;

        if (netipc_uds_seqpacket_client_call_increment(client, &req, &resp, 10000) != 0) {
            perror("netipc_uds_seqpacket_client_call_increment");
            netipc_uds_seqpacket_client_destroy(client);
            return 1;
        }

        if (resp.status != NETIPC_STATUS_OK) {
            fprintf(stderr, "server returned status=%d\n", resp.status);
            netipc_uds_seqpacket_client_destroy(client);
            return 1;
        }

        if (resp.value != value + 1u) {
            fprintf(stderr,
                    "unexpected response value: got=%" PRIu64 " expected=%" PRIu64 "\n",
                    resp.value,
                    value + 1u);
            netipc_uds_seqpacket_client_destroy(client);
            return 1;
        }

        printf("request=%" PRIu64 " response=%" PRIu64 "\n", value, resp.value);
        value = resp.value;
    }

    netipc_uds_seqpacket_client_destroy(client);
    return 0;
}
