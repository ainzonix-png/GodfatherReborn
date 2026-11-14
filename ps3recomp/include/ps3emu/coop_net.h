#ifndef PS3EMU_COOP_NET_H
#define PS3EMU_COOP_NET_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ps3emu_coop_mode {
    PS3EMU_COOP_OFF = 0,
    PS3EMU_COOP_HOST = 1,
    PS3EMU_COOP_CLIENT = 2
} ps3emu_coop_mode;

typedef struct ps3emu_coop_player_state {
    uint32_t game_tick;
    float position[3];
    float heading;
    float velocity[3];
    uint32_t animation;
    uint32_t buttons;
} ps3emu_coop_player_state;

typedef struct ps3emu_coop_remote_state {
    ps3emu_coop_player_state player;
    uint32_t sequence;
    uint64_t received_at_ms;
} ps3emu_coop_remote_state;

typedef struct ps3emu_coop_status {
    int enabled;
    ps3emu_coop_mode mode;
    int connected;
    uint64_t peer_age_ms;
    uint32_t received_sequence;
    uint32_t rejected_packets;
} ps3emu_coop_status;

/*
 * Opt-in configuration (read once by ps3emu_coop_init):
 *   INFAMOUS_COOP_MODE=host|client  (unset/off keeps networking disabled)
 *   INFAMOUS_COOP_ADDRESS=127.0.0.1 (client destination; IPv4 or hostname)
 *   INFAMOUS_COOP_PORT=29715         (UDP port, 1..65535)
 *   INFAMOUS_COOP_TIMEOUT_MS=3000    (peer timeout, minimum 250)
 *
 * The API is single-threaded. Call tick from the game loop with a monotonic
 * millisecond clock. Socket polling is nonblocking and never stalls a frame.
 */
int ps3emu_coop_init(void);
void ps3emu_coop_shutdown(void);
void ps3emu_coop_tick(uint64_t now_ms);
int ps3emu_coop_send_local_state(const ps3emu_coop_player_state *state);
int ps3emu_coop_get_remote_state(ps3emu_coop_remote_state *state);
void ps3emu_coop_get_status(ps3emu_coop_status *status);
const char *ps3emu_coop_last_error(void);

/* Opaque payload channel (protocol v2).  At most PS3EMU_COOP_BLOB_MAX bytes
 * per datagram; delivery is unreliable and unordered packets are dropped like
 * every other message.  The handler runs inside ps3emu_coop_tick on the
 * calling thread and must copy whatever it keeps. */
#define PS3EMU_COOP_BLOB_MAX 1260u
typedef void (*ps3emu_coop_blob_handler)(const uint8_t *data, uint32_t size,
                                         uint64_t now_ms, void *user);
void ps3emu_coop_set_blob_handler(ps3emu_coop_blob_handler handler, void *user);
int ps3emu_coop_send_blob(const void *data, uint32_t size);
/* Bytes sent/received through the blob channel since start (diagnostics). */
void ps3emu_coop_blob_counters(uint64_t *sent_bytes, uint64_t *received_bytes);

#ifdef __cplusplus
}
#endif

#endif
