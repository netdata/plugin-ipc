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
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "usage: %s <run_dir> <service> [iterations]\n", argv[0]);
        return 2;
    }

    uint64_t iterations = 1;
    if (argc == 4) {
        iterations = parse_u64(argv[3]);
    }

    struct netipc_shm_config cfg = {
        .run_dir = argv[1],
        .service_name = argv[2],
        .spin_tries = NETIPC_SHM_DEFAULT_SPIN_TRIES,
        .file_mode = 0600,
    };

    netipc_shm_server_t *server = NULL;
    if (netipc_shm_server_create(&cfg, &server) != 0) {
        perror("netipc_shm_server_create");
        return 1;
    }

    for (uint64_t i = 0; i < iterations; ++i) {
        uint64_t request_id = 0;
        struct netipc_increment_request req;

        if (netipc_shm_server_receive_increment(server, &request_id, &req, 10000) != 0) {
            perror("netipc_shm_server_receive_increment");
            netipc_shm_server_destroy(server);
            return 1;
        }

        struct netipc_increment_response resp = {
            .status = NETIPC_STATUS_OK,
            .value = req.value + 1u,
        };

        if (netipc_shm_server_send_increment(server, request_id, &resp) != 0) {
            perror("netipc_shm_server_send_increment");
            netipc_shm_server_destroy(server);
            return 1;
        }

        printf("served request_id=%" PRIu64 " value=%" PRIu64 " response=%" PRIu64 "\n",
               request_id,
               req.value,
               resp.value);
    }

    netipc_shm_server_destroy(server);
    return 0;
}
