#ifndef NETIPC_NAMED_PIPE_H
#define NETIPC_NAMED_PIPE_H

#include <stdint.h>

#include <netipc/netipc_schema.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NETIPC_PROFILE_NAMED_PIPE (1u << 0)
#define NETIPC_PROFILE_SHM_HYBRID (1u << 1)
#define NETIPC_PROFILE_SHM_BUSYWAIT (1u << 2)
#define NETIPC_PROFILE_SHM_WAITADDR (1u << 3)
#define NETIPC_NAMED_PIPE_DEFAULT_PROFILES NETIPC_PROFILE_NAMED_PIPE
#define NETIPC_SHM_HYBRID_DEFAULT_SPIN_TRIES 1024u

typedef struct netipc_named_pipe_server netipc_named_pipe_server_t;
typedef struct netipc_named_pipe_client netipc_named_pipe_client_t;

struct netipc_named_pipe_config {
    const char *run_dir;
    const char *service_name;
    uint32_t supported_profiles;
    uint32_t preferred_profiles;
    uint32_t max_request_payload_bytes;
    uint32_t max_request_batch_items;
    uint32_t max_response_payload_bytes;
    uint32_t max_response_batch_items;
    uint64_t auth_token;
    uint32_t shm_spin_tries;
};

int netipc_named_pipe_server_create(const struct netipc_named_pipe_config *config,
                                    netipc_named_pipe_server_t **out_server);
int netipc_named_pipe_server_accept(netipc_named_pipe_server_t *server, uint32_t timeout_ms);
int netipc_named_pipe_server_receive_message(netipc_named_pipe_server_t *server,
                                             uint8_t *message,
                                             size_t message_capacity,
                                             size_t *out_message_len,
                                             uint32_t timeout_ms);
int netipc_named_pipe_server_send_message(netipc_named_pipe_server_t *server,
                                          const uint8_t *message,
                                          size_t message_len,
                                          uint32_t timeout_ms);
int netipc_named_pipe_server_receive_frame(netipc_named_pipe_server_t *server,
                                           uint8_t frame[NETIPC_FRAME_SIZE],
                                           uint32_t timeout_ms);
int netipc_named_pipe_server_send_frame(netipc_named_pipe_server_t *server,
                                        const uint8_t frame[NETIPC_FRAME_SIZE],
                                        uint32_t timeout_ms);
int netipc_named_pipe_server_receive_increment(netipc_named_pipe_server_t *server,
                                               uint64_t *request_id,
                                               struct netipc_increment_request *request,
                                               uint32_t timeout_ms);
int netipc_named_pipe_server_send_increment(netipc_named_pipe_server_t *server,
                                            uint64_t request_id,
                                            const struct netipc_increment_response *response,
                                            uint32_t timeout_ms);
uint32_t netipc_named_pipe_server_negotiated_profile(const netipc_named_pipe_server_t *server);
void netipc_named_pipe_server_destroy(netipc_named_pipe_server_t *server);

int netipc_named_pipe_client_create(const struct netipc_named_pipe_config *config,
                                    netipc_named_pipe_client_t **out_client,
                                    uint32_t timeout_ms);
int netipc_named_pipe_client_call_message(netipc_named_pipe_client_t *client,
                                          const uint8_t *request_message,
                                          size_t request_message_len,
                                          uint8_t *response_message,
                                          size_t response_capacity,
                                          size_t *out_response_len,
                                          uint32_t timeout_ms);
int netipc_named_pipe_client_call_frame(netipc_named_pipe_client_t *client,
                                        const uint8_t request_frame[NETIPC_FRAME_SIZE],
                                        uint8_t response_frame[NETIPC_FRAME_SIZE],
                                        uint32_t timeout_ms);
int netipc_named_pipe_client_call_increment(netipc_named_pipe_client_t *client,
                                            const struct netipc_increment_request *request,
                                            struct netipc_increment_response *response,
                                            uint32_t timeout_ms);
uint32_t netipc_named_pipe_client_negotiated_profile(const netipc_named_pipe_client_t *client);
void netipc_named_pipe_client_destroy(netipc_named_pipe_client_t *client);

#ifdef __cplusplus
}
#endif

#endif
