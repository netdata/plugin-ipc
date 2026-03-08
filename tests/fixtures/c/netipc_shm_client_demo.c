#include <netipc/netipc_shm_hybrid.h>

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

int main(int argc, char **argv) {
    if (argc < 4 || argc > 5) {
        fprintf(stderr, "usage: %s <run_dir> <service> <start_value> [iterations]\n", argv[0]);
        return 2;
    }

    uint64_t value = parse_u64(argv[3]);
    uint64_t iterations = 1;
    if (argc == 5) {
        iterations = parse_u64(argv[4]);
    }

    struct netipc_shm_config cfg = {
        .run_dir = argv[1],
        .service_name = argv[2],
        .spin_tries = NETIPC_SHM_DEFAULT_SPIN_TRIES,
        .file_mode = 0600,
    };

    netipc_shm_client_t *client = NULL;
    if (netipc_shm_client_create(&cfg, &client) != 0) {
        perror("netipc_shm_client_create");
        return 1;
    }

    for (uint64_t i = 0; i < iterations; ++i) {
        struct netipc_increment_request req = {.value = value};
        struct netipc_increment_response resp;

        if (netipc_shm_client_call_increment(client, &req, &resp, 10000) != 0) {
            perror("netipc_shm_client_call_increment");
            netipc_shm_client_destroy(client);
            return 1;
        }

        if (resp.status != NETIPC_STATUS_OK) {
            fprintf(stderr, "server returned status=%d\n", resp.status);
            netipc_shm_client_destroy(client);
            return 1;
        }

        if (resp.value != value + 1u) {
            fprintf(stderr,
                    "unexpected response value: got=%" PRIu64 " expected=%" PRIu64 "\n",
                    resp.value,
                    value + 1u);
            netipc_shm_client_destroy(client);
            return 1;
        }

        printf("request=%" PRIu64 " response=%" PRIu64 "\n", value, resp.value);
        value = resp.value;
    }

    netipc_shm_client_destroy(client);
    return 0;
}
