#ifndef NETIPC_SHM_HYBRID_H
#define NETIPC_SHM_HYBRID_H

#include <stdint.h>

#include <netipc/netipc_schema.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NETIPC_SHM_DEFAULT_SPIN_TRIES 128u

typedef struct netipc_shm_server netipc_shm_server_t;
typedef struct netipc_shm_client netipc_shm_client_t;

struct netipc_shm_config {
    const char *run_dir;
    const char *service_name;
    uint32_t spin_tries;
    uint32_t file_mode;
    uint32_t max_request_message_bytes;
    uint32_t max_response_message_bytes;
};

int netipc_shm_server_create(const struct netipc_shm_config *config, netipc_shm_server_t **out_server);
int netipc_shm_server_receive_message(netipc_shm_server_t *server,
                                      uint8_t *message,
                                      size_t message_capacity,
                                      size_t *out_message_len,
                                      uint32_t timeout_ms);
int netipc_shm_server_send_message(netipc_shm_server_t *server,
                                   const uint8_t *message,
                                   size_t message_len);
int netipc_shm_server_receive_frame(netipc_shm_server_t *server,
                                    uint8_t frame[NETIPC_FRAME_SIZE],
                                    uint32_t timeout_ms);
int netipc_shm_server_send_frame(netipc_shm_server_t *server,
                                 const uint8_t frame[NETIPC_FRAME_SIZE]);
int netipc_shm_server_receive_increment(netipc_shm_server_t *server,
                                        uint64_t *request_id,
                                        struct netipc_increment_request *request,
                                        uint32_t timeout_ms);
int netipc_shm_server_send_increment(netipc_shm_server_t *server,
                                     uint64_t request_id,
                                     const struct netipc_increment_response *response);
void netipc_shm_server_destroy(netipc_shm_server_t *server);

int netipc_shm_client_create(const struct netipc_shm_config *config, netipc_shm_client_t **out_client);
int netipc_shm_client_receive_message(netipc_shm_client_t *client,
                                      uint8_t *response_message,
                                      size_t response_capacity,
                                      size_t *out_response_len,
                                      uint32_t timeout_ms);
int netipc_shm_client_send_message(netipc_shm_client_t *client,
                                   const uint8_t *request_message,
                                   size_t request_message_len,
                                   uint32_t timeout_ms);
int netipc_shm_client_receive_frame(netipc_shm_client_t *client,
                                    uint8_t response_frame[NETIPC_FRAME_SIZE],
                                    uint32_t timeout_ms);
int netipc_shm_client_send_frame(netipc_shm_client_t *client,
                                 const uint8_t request_frame[NETIPC_FRAME_SIZE],
                                 uint32_t timeout_ms);
int netipc_shm_client_receive_increment(netipc_shm_client_t *client,
                                        uint64_t *request_id,
                                        struct netipc_increment_response *response,
                                        uint32_t timeout_ms);
int netipc_shm_client_send_increment(netipc_shm_client_t *client,
                                     uint64_t request_id,
                                     const struct netipc_increment_request *request,
                                     uint32_t timeout_ms);
int netipc_shm_client_call_message(netipc_shm_client_t *client,
                                   const uint8_t *request_message,
                                   size_t request_message_len,
                                   uint8_t *response_message,
                                   size_t response_capacity,
                                   size_t *out_response_len,
                                   uint32_t timeout_ms);
int netipc_shm_client_call_frame(netipc_shm_client_t *client,
                                 const uint8_t request_frame[NETIPC_FRAME_SIZE],
                                 uint8_t response_frame[NETIPC_FRAME_SIZE],
                                 uint32_t timeout_ms);
int netipc_shm_client_call_increment(netipc_shm_client_t *client,
                                     const struct netipc_increment_request *request,
                                     struct netipc_increment_response *response,
                                     uint32_t timeout_ms);
void netipc_shm_client_destroy(netipc_shm_client_t *client);

#ifdef __cplusplus
}
#endif

#endif
