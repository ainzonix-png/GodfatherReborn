#ifndef INFAMOUS_COOP_PROTOCOL_H
#define INFAMOUS_COOP_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include "ps3emu/coop_net.h"

#ifdef __cplusplus
extern "C" {
#endif

/* v2: adds BLOB messages (opaque renderer payloads such as the live skinned
 * Cole pose).  Both peers must run the same protocol version. */
#define INFAMOUS_COOP_PROTOCOL_VERSION 2u
#define INFAMOUS_COOP_PACKET_MAX_SIZE  1280u
#define INFAMOUS_COOP_HEADER_SIZE      20u
#define INFAMOUS_COOP_BLOB_MAX_SIZE    (INFAMOUS_COOP_PACKET_MAX_SIZE - INFAMOUS_COOP_HEADER_SIZE)

typedef enum infamous_coop_message_type {
    INFAMOUS_COOP_MESSAGE_HELLO = 1,
    INFAMOUS_COOP_MESSAGE_HEARTBEAT = 2,
    INFAMOUS_COOP_MESSAGE_STATE = 3,
    INFAMOUS_COOP_MESSAGE_BLOB = 4
} infamous_coop_message_type;

/* Deliberately contains only POD fields that the first gameplay hook needs. */
typedef struct infamous_coop_decoded_packet {
    uint16_t type;
    uint32_t sequence;
    uint32_t session;
    ps3emu_coop_player_state state;
    const uint8_t *blob;      /* BLOB only: points into the decoded buffer */
    uint16_t blob_size;
} infamous_coop_decoded_packet;

/* Internal wire helpers are public only so the codec can be unit-tested. */
size_t infamous_coop_protocol_encode(uint8_t *output, size_t capacity,
                                    uint16_t type, uint32_t sequence,
                                    uint32_t session,
                                    const ps3emu_coop_player_state *state);
size_t infamous_coop_protocol_encode_blob(uint8_t *output, size_t capacity,
                                         uint32_t sequence, uint32_t session,
                                         const void *blob, size_t blob_size);
int infamous_coop_protocol_decode(const uint8_t *data, size_t size,
                                  infamous_coop_decoded_packet *packet);

#ifdef __cplusplus
}
#endif

#endif
