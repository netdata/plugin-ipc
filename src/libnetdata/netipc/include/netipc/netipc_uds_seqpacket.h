#ifndef NETIPC_UDS_SEQPACKET_H
#define NETIPC_UDS_SEQPACKET_H

#include <stdint.h>

#include <netipc/netipc_schema.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NETIPC_PROFILE_UDS_SEQPACKET (1u << 0)
#define NETIPC_PROFILE_SHM_HYBRID (1u << 1)
#define NETIPC_PROFILE_SHM_FUTEX (1u << 2)

#define NETIPC_SEQPACKET_DEFAULT_PROFILES NETIPC_PROFILE_UDS_SEQPACKET

typedef struct netipc_uds_seqpacket_server netipc_uds_seqpacket_server_t;
typedef struct netipc_uds_seqpacket_client netipc_uds_seqpacket_client_t;
typedef struct netipc_uds_seqpacket_listener netipc_uds_seqpacket_listener_t;
typedef struct netipc_uds_seqpacket_session netipc_uds_seqpacket_session_t;

struct netipc_uds_seqpacket_config {
    const char *run_dir;
    const char *service_name;
    uint32_t file_mode;
    uint32_t supported_profiles;
    uint32_t preferred_profiles;
    uint32_t max_request_payload_bytes;
    uint32_t max_request_batch_items;
    uint32_t max_response_payload_bytes;
    uint32_t max_response_batch_items;
    uint64_t auth_token;
};

int netipc_uds_seqpacket_server_create(const struct netipc_uds_seqpacket_config *config,
                                       netipc_uds_seqpacket_server_t **out_server);
int netipc_uds_seqpacket_server_accept(netipc_uds_seqpacket_server_t *server, uint32_t timeout_ms);
int netipc_uds_seqpacket_server_receive_message(netipc_uds_seqpacket_server_t *server,
                                                uint8_t *message,
                                                size_t message_capacity,
                                                size_t *out_message_len,
                                                uint32_t timeout_ms);
int netipc_uds_seqpacket_server_send_message(netipc_uds_seqpacket_server_t *server,
                                             const uint8_t *message,
                                             size_t message_len,
                                             uint32_t timeout_ms);
int netipc_uds_seqpacket_server_receive_frame(netipc_uds_seqpacket_server_t *server,
                                              uint8_t frame[NETIPC_FRAME_SIZE],
                                              uint32_t timeout_ms);
int netipc_uds_seqpacket_server_send_frame(netipc_uds_seqpacket_server_t *server,
                                           const uint8_t frame[NETIPC_FRAME_SIZE],
                                           uint32_t timeout_ms);
int netipc_uds_seqpacket_server_receive_increment(netipc_uds_seqpacket_server_t *server,
                                                  uint64_t *request_id,
                                                  struct netipc_increment_request *request,
                                                  uint32_t timeout_ms);
int netipc_uds_seqpacket_server_send_increment(netipc_uds_seqpacket_server_t *server,
                                               uint64_t request_id,
                                               const struct netipc_increment_response *response,
                                               uint32_t timeout_ms);
uint32_t netipc_uds_seqpacket_server_negotiated_profile(const netipc_uds_seqpacket_server_t *server);
void netipc_uds_seqpacket_server_destroy(netipc_uds_seqpacket_server_t *server);

int netipc_uds_seqpacket_listener_create(const struct netipc_uds_seqpacket_config *config,
                                         netipc_uds_seqpacket_listener_t **out_listener);
int netipc_uds_seqpacket_listener_accept(netipc_uds_seqpacket_listener_t *listener,
                                         netipc_uds_seqpacket_session_t **out_session,
                                         uint32_t timeout_ms);
int netipc_uds_seqpacket_listener_fd(const netipc_uds_seqpacket_listener_t *listener);
void netipc_uds_seqpacket_listener_destroy(netipc_uds_seqpacket_listener_t *listener);

int netipc_uds_seqpacket_session_receive_message(netipc_uds_seqpacket_session_t *session,
                                                 uint8_t *message,
                                                 size_t message_capacity,
                                                 size_t *out_message_len,
                                                 uint32_t timeout_ms);
int netipc_uds_seqpacket_session_send_message(netipc_uds_seqpacket_session_t *session,
                                              const uint8_t *message,
                                              size_t message_len,
                                              uint32_t timeout_ms);
int netipc_uds_seqpacket_session_receive_frame(netipc_uds_seqpacket_session_t *session,
                                               uint8_t frame[NETIPC_FRAME_SIZE],
                                               uint32_t timeout_ms);
int netipc_uds_seqpacket_session_send_frame(netipc_uds_seqpacket_session_t *session,
                                            const uint8_t frame[NETIPC_FRAME_SIZE],
                                            uint32_t timeout_ms);
int netipc_uds_seqpacket_session_receive_increment(netipc_uds_seqpacket_session_t *session,
                                                   uint64_t *request_id,
                                                   struct netipc_increment_request *request,
                                                   uint32_t timeout_ms);
int netipc_uds_seqpacket_session_send_increment(netipc_uds_seqpacket_session_t *session,
                                                uint64_t request_id,
                                                const struct netipc_increment_response *response,
                                                uint32_t timeout_ms);
uint32_t netipc_uds_seqpacket_session_negotiated_profile(const netipc_uds_seqpacket_session_t *session);
int netipc_uds_seqpacket_session_fd(const netipc_uds_seqpacket_session_t *session);
void netipc_uds_seqpacket_session_destroy(netipc_uds_seqpacket_session_t *session);

int netipc_uds_seqpacket_client_create(const struct netipc_uds_seqpacket_config *config,
                                       netipc_uds_seqpacket_client_t **out_client,
                                       uint32_t timeout_ms);
int netipc_uds_seqpacket_client_receive_message(netipc_uds_seqpacket_client_t *client,
                                                uint8_t *response_message,
                                                size_t response_capacity,
                                                size_t *out_response_len,
                                                uint32_t timeout_ms);
int netipc_uds_seqpacket_client_send_message(netipc_uds_seqpacket_client_t *client,
                                             const uint8_t *request_message,
                                             size_t request_message_len,
                                             uint32_t timeout_ms);
int netipc_uds_seqpacket_client_receive_frame(netipc_uds_seqpacket_client_t *client,
                                              uint8_t response_frame[NETIPC_FRAME_SIZE],
                                              uint32_t timeout_ms);
int netipc_uds_seqpacket_client_send_frame(netipc_uds_seqpacket_client_t *client,
                                           const uint8_t request_frame[NETIPC_FRAME_SIZE],
                                           uint32_t timeout_ms);
int netipc_uds_seqpacket_client_receive_increment(netipc_uds_seqpacket_client_t *client,
                                                  uint64_t *request_id,
                                                  struct netipc_increment_response *response,
                                                  uint32_t timeout_ms);
int netipc_uds_seqpacket_client_send_increment(netipc_uds_seqpacket_client_t *client,
                                               uint64_t request_id,
                                               const struct netipc_increment_request *request,
                                               uint32_t timeout_ms);
int netipc_uds_seqpacket_client_call_message(netipc_uds_seqpacket_client_t *client,
                                             const uint8_t *request_message,
                                             size_t request_message_len,
                                             uint8_t *response_message,
                                             size_t response_capacity,
                                             size_t *out_response_len,
                                             uint32_t timeout_ms);
int netipc_uds_seqpacket_client_call_frame(netipc_uds_seqpacket_client_t *client,
                                           const uint8_t request_frame[NETIPC_FRAME_SIZE],
                                           uint8_t response_frame[NETIPC_FRAME_SIZE],
                                           uint32_t timeout_ms);
int netipc_uds_seqpacket_client_call_increment(netipc_uds_seqpacket_client_t *client,
                                               const struct netipc_increment_request *request,
                                               struct netipc_increment_response *response,
                                               uint32_t timeout_ms);
uint32_t netipc_uds_seqpacket_client_negotiated_profile(const netipc_uds_seqpacket_client_t *client);
void netipc_uds_seqpacket_client_destroy(netipc_uds_seqpacket_client_t *client);

#ifdef __cplusplus
}
#endif

#endif
