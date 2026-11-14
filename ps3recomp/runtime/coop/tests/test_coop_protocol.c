#include "../coop_protocol.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    uint8_t bytes[INFAMOUS_COOP_PACKET_MAX_SIZE];
    ps3emu_coop_player_state source = {0};
    infamous_coop_decoded_packet decoded;
    size_t size;

    source.game_tick = 0x12345678u;
    source.position[0] = 1.25f;
    source.position[1] = -2.5f;
    source.position[2] = 99.0f;
    source.heading = 0.75f;
    source.velocity[0] = -4.0f;
    source.animation = 17;
    source.buttons = 0xa55au;

    size = infamous_coop_protocol_encode(bytes, sizeof(bytes),
                                         INFAMOUS_COOP_MESSAGE_STATE,
                                         42, 0xabcdef01u, &source);
    assert(size > 0 && size <= INFAMOUS_COOP_PACKET_MAX_SIZE);
    assert(infamous_coop_protocol_decode(bytes, size, &decoded));
    assert(decoded.type == INFAMOUS_COOP_MESSAGE_STATE);
    assert(decoded.sequence == 42 && decoded.session == 0xabcdef01u);
    assert(memcmp(&decoded.state, &source, sizeof(source)) == 0);

    bytes[4] = 0;
    bytes[5] = INFAMOUS_COOP_PROTOCOL_VERSION + 1;
    assert(!infamous_coop_protocol_decode(bytes, size, &decoded));
    assert(!infamous_coop_protocol_decode(bytes, size - 1, &decoded));

    source.position[1] = NAN;
    size = infamous_coop_protocol_encode(bytes, sizeof(bytes),
                                         INFAMOUS_COOP_MESSAGE_STATE,
                                         43, 0xabcdef01u, &source);
    assert(!infamous_coop_protocol_decode(bytes, size, &decoded));
    source.position[1] = 0.0f;
    source.velocity[2] = INFINITY;
    size = infamous_coop_protocol_encode(bytes, sizeof(bytes),
                                         INFAMOUS_COOP_MESSAGE_STATE,
                                         44, 0xabcdef01u, &source);
    assert(!infamous_coop_protocol_decode(bytes, size, &decoded));

    {
        uint8_t blob[INFAMOUS_COOP_BLOB_MAX_SIZE];
        for (unsigned i = 0; i < sizeof(blob); ++i) blob[i] = (uint8_t)(i * 7u);
        size = infamous_coop_protocol_encode_blob(bytes, sizeof(bytes), 45, 0xabcdef01u, blob, sizeof(blob));
        assert(size == INFAMOUS_COOP_PACKET_MAX_SIZE);
        assert(infamous_coop_protocol_decode(bytes, size, &decoded));
        assert(decoded.type == INFAMOUS_COOP_MESSAGE_BLOB && decoded.blob_size == sizeof(blob));
        assert(memcmp(decoded.blob, blob, sizeof(blob)) == 0 && decoded.sequence == 45);
        assert(!infamous_coop_protocol_decode(bytes, size - 1, &decoded));
        assert(!infamous_coop_protocol_encode_blob(bytes, sizeof(bytes), 46, 1, blob, sizeof(blob) + 1));
    }

    puts("coop protocol tests passed");
    return 0;
}
