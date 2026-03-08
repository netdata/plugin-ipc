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

struct netipc_uds_seqpacket_config {
    const char *run_dir;
    const char *service_name;
    uint32_t file_mode;
    uint32_t supported_profiles;
    uint32_t preferred_profiles;
    uint64_t auth_token;
};

int netipc_uds_seqpacket_server_create(const struct netipc_uds_seqpacket_config *config,
                                       netipc_uds_seqpacket_server_t **out_server);
int netipc_uds_seqpacket_server_accept(netipc_uds_seqpacket_server_t *server, uint32_t timeout_ms);
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

int netipc_uds_seqpacket_client_create(const struct netipc_uds_seqpacket_config *config,
                                       netipc_uds_seqpacket_client_t **out_client,
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
