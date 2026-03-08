#ifndef NETIPC_SCHEMA_H
#define NETIPC_SCHEMA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NETIPC_FRAME_MAGIC 0x4e495043u
#define NETIPC_FRAME_VERSION 1u
#define NETIPC_FRAME_SIZE 64u

enum netipc_frame_kind {
    NETIPC_FRAME_KIND_REQUEST = 1,
    NETIPC_FRAME_KIND_RESPONSE = 2
};

enum netipc_method {
    NETIPC_METHOD_INCREMENT = 1
};

enum netipc_status {
    NETIPC_STATUS_OK = 0,
    NETIPC_STATUS_BAD_REQUEST = 1,
    NETIPC_STATUS_INTERNAL_ERROR = 2
};

struct netipc_increment_request {
    uint64_t value;
};

struct netipc_increment_response {
    int32_t status;
    uint64_t value;
};

int netipc_encode_increment_request(uint8_t frame[NETIPC_FRAME_SIZE],
                                    uint64_t request_id,
                                    const struct netipc_increment_request *req);

int netipc_decode_increment_request(const uint8_t frame[NETIPC_FRAME_SIZE],
                                    uint64_t *request_id,
                                    struct netipc_increment_request *req);

int netipc_encode_increment_response(uint8_t frame[NETIPC_FRAME_SIZE],
                                     uint64_t request_id,
                                     const struct netipc_increment_response *resp);

int netipc_decode_increment_response(const uint8_t frame[NETIPC_FRAME_SIZE],
                                     uint64_t *request_id,
                                     struct netipc_increment_response *resp);

#ifdef __cplusplus
}
#endif

#endif
