#include "coop_protocol.h"

#include <math.h>
#include <string.h>

#define COOP_MAGIC       0x49464350u /* "IFCP" */
#define COOP_HEADER_SIZE INFAMOUS_COOP_HEADER_SIZE
#define COOP_STATE_SIZE  40u

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void put_float(uint8_t *p, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    put_u32(p, bits);
}

static float get_float(const uint8_t *p)
{
    uint32_t bits = get_u32(p);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

size_t infamous_coop_protocol_encode(uint8_t *output, size_t capacity,
                                    uint16_t type, uint32_t sequence,
                                    uint32_t session,
                                    const ps3emu_coop_player_state *state)
{
    uint16_t payload_size;
    size_t offset;
    unsigned i;

    if (!output)
        return 0;
    if (type != INFAMOUS_COOP_MESSAGE_HELLO &&
        type != INFAMOUS_COOP_MESSAGE_HEARTBEAT &&
        type != INFAMOUS_COOP_MESSAGE_STATE)
        return 0;
    if (type == INFAMOUS_COOP_MESSAGE_STATE && !state)
        return 0;

    payload_size = type == INFAMOUS_COOP_MESSAGE_STATE ? COOP_STATE_SIZE : 0;
    if (capacity < COOP_HEADER_SIZE + payload_size)
        return 0;

    memset(output, 0, COOP_HEADER_SIZE + payload_size);
    put_u32(output + 0, COOP_MAGIC);
    put_u16(output + 4, INFAMOUS_COOP_PROTOCOL_VERSION);
    put_u16(output + 6, type);
    put_u16(output + 8, payload_size);
    put_u32(output + 12, sequence);
    put_u32(output + 16, session);

    if (!payload_size)
        return COOP_HEADER_SIZE;

    offset = COOP_HEADER_SIZE;
    put_u32(output + offset, state->game_tick);
    offset += 4;
    for (i = 0; i < 3; ++i, offset += 4)
        put_float(output + offset, state->position[i]);
    put_float(output + offset, state->heading);
    offset += 4;
    for (i = 0; i < 3; ++i, offset += 4)
        put_float(output + offset, state->velocity[i]);
    put_u32(output + offset, state->animation);
    offset += 4;
    put_u32(output + offset, state->buttons);
    return COOP_HEADER_SIZE + COOP_STATE_SIZE;
}

size_t infamous_coop_protocol_encode_blob(uint8_t *output, size_t capacity,
                                         uint32_t sequence, uint32_t session,
                                         const void *blob, size_t blob_size)
{
    if (!output || !blob || !blob_size || blob_size > INFAMOUS_COOP_BLOB_MAX_SIZE ||
        capacity < COOP_HEADER_SIZE + blob_size)
        return 0;
    memset(output, 0, COOP_HEADER_SIZE);
    put_u32(output + 0, COOP_MAGIC);
    put_u16(output + 4, INFAMOUS_COOP_PROTOCOL_VERSION);
    put_u16(output + 6, INFAMOUS_COOP_MESSAGE_BLOB);
    put_u16(output + 8, (uint16_t)blob_size);
    put_u32(output + 12, sequence);
    put_u32(output + 16, session);
    memcpy(output + COOP_HEADER_SIZE, blob, blob_size);
    return COOP_HEADER_SIZE + blob_size;
}

int infamous_coop_protocol_decode(const uint8_t *data, size_t size,
                                  infamous_coop_decoded_packet *packet)
{
    uint16_t payload_size;
    size_t offset;
    unsigned i;

    if (!data || !packet || size < COOP_HEADER_SIZE ||
        get_u32(data) != COOP_MAGIC ||
        get_u16(data + 4) != INFAMOUS_COOP_PROTOCOL_VERSION)
        return 0;

    packet->type = get_u16(data + 6);
    payload_size = get_u16(data + 8);
    packet->blob = NULL;
    packet->blob_size = 0;
    if (packet->type == INFAMOUS_COOP_MESSAGE_BLOB) {
        if (!payload_size || payload_size > INFAMOUS_COOP_BLOB_MAX_SIZE ||
            size != COOP_HEADER_SIZE + payload_size)
            return 0;
        packet->sequence = get_u32(data + 12);
        packet->session = get_u32(data + 16);
        memset(&packet->state, 0, sizeof(packet->state));
        packet->blob = data + COOP_HEADER_SIZE;
        packet->blob_size = payload_size;
        return 1;
    }
    if (packet->type != INFAMOUS_COOP_MESSAGE_HELLO &&
        packet->type != INFAMOUS_COOP_MESSAGE_HEARTBEAT &&
        packet->type != INFAMOUS_COOP_MESSAGE_STATE)
        return 0;
    if ((packet->type == INFAMOUS_COOP_MESSAGE_STATE && payload_size != COOP_STATE_SIZE) ||
        (packet->type != INFAMOUS_COOP_MESSAGE_STATE && payload_size != 0) ||
        size != COOP_HEADER_SIZE + payload_size)
        return 0;

    packet->sequence = get_u32(data + 12);
    packet->session = get_u32(data + 16);
    memset(&packet->state, 0, sizeof(packet->state));
    if (!payload_size)
        return 1;

    offset = COOP_HEADER_SIZE;
    packet->state.game_tick = get_u32(data + offset);
    offset += 4;
    for (i = 0; i < 3; ++i, offset += 4)
        packet->state.position[i] = get_float(data + offset);
    packet->state.heading = get_float(data + offset);
    offset += 4;
    for (i = 0; i < 3; ++i, offset += 4)
        packet->state.velocity[i] = get_float(data + offset);
    packet->state.animation = get_u32(data + offset);
    offset += 4;
    packet->state.buttons = get_u32(data + offset);
    if (!isfinite(packet->state.position[0]) ||
        !isfinite(packet->state.position[1]) ||
        !isfinite(packet->state.position[2]) ||
        !isfinite(packet->state.heading) ||
        !isfinite(packet->state.velocity[0]) ||
        !isfinite(packet->state.velocity[1]) ||
        !isfinite(packet->state.velocity[2]))
        return 0;
    return 1;
}
