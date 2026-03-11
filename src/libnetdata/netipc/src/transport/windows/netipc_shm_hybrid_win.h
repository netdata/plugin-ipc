#ifndef NETIPC_SHM_HYBRID_WIN_H
#define NETIPC_SHM_HYBRID_WIN_H

#include <stdint.h>

#include <netipc/netipc_named_pipe.h>

typedef struct netipc_win_shm_server netipc_win_shm_server_t;
typedef struct netipc_win_shm_client netipc_win_shm_client_t;

int netipc_win_shm_server_create(const struct netipc_named_pipe_config *config,
                                 uint32_t profile,
                                 netipc_win_shm_server_t **out_server);
int netipc_win_shm_server_receive_frame(netipc_win_shm_server_t *server,
                                        uint8_t frame[NETIPC_FRAME_SIZE],
                                        uint32_t timeout_ms);
int netipc_win_shm_server_send_frame(netipc_win_shm_server_t *server,
                                     const uint8_t frame[NETIPC_FRAME_SIZE],
                                     uint32_t timeout_ms);
void netipc_win_shm_server_destroy(netipc_win_shm_server_t *server);

int netipc_win_shm_client_create(const struct netipc_named_pipe_config *config,
                                 uint32_t profile,
                                 netipc_win_shm_client_t **out_client,
                                 uint32_t timeout_ms);
int netipc_win_shm_client_call_frame(netipc_win_shm_client_t *client,
                                     const uint8_t request_frame[NETIPC_FRAME_SIZE],
                                     uint8_t response_frame[NETIPC_FRAME_SIZE],
                                     uint32_t timeout_ms);
void netipc_win_shm_client_destroy(netipc_win_shm_client_t *client);

#endif
