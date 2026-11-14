/* gf_coop_world.h -- v1060 world sync (peds + traffic), host authoritative. Shared contract of 3 modules:
 *   gf_coop_worldre.c  (agent W1)  population RE: enumerate live peds/cars, suppress ambient spawning on the guest
 *   gf_coop_world.c    (agent W2)  network: host snapshots -> guest, net ids, spawn/despawn events, glue tick
 *   gf_coop_proxy.c    (agent W3)  guest proxies: spawn by class, drive transform/locomotion, release
 * All functions run on the main PPU thread (gf_coop_glue_tick) unless noted. Everything guarded with __try.
 * Env: GF_COOP_WORLD=0 turns the whole feature off (default ON when GF_COOP=host|join). */
#ifndef GF_COOP_WORLD_H
#define GF_COOP_WORLD_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { GF_WK_PED = 1, GF_WK_CAR = 2 };
enum { GF_COOP_WORLD_BLOB_CHANNEL = 2 };      /* gf_coop_send_blob channel for snapshots (pose uses 1) */
enum { GF_COOP_EV_WORLD_SPAWN = 0x0120, GF_COOP_EV_WORLD_DESPAWN = 0x0121 };   /* reliable events */

typedef struct {
    uint32_t ent;          /* guest-memory entity address (local only, never sent) */
    uint32_t class_hash;   /* managed-factory class hash (func_003388AC) */
    uint8_t  kind;         /* GF_WK_* */
    uint8_t  guid[16];     /* template / attrib GUID if known (zeros otherwise) */
    float    m[12];        /* world RwMatrix rows right/up/at/pos (Y up, metres) */
    float    vel[3];       /* world velocity m/s (0 if unknown) */
    float    avel[3];      /* v1063H world angular velocity rad/s (0 if unknown) */
    uint32_t gen;          /* v1063H factory spawn serial (identity: address + gen, never address + class alone) */
} gf_world_ent;

/* ---------------------------------------------------------------- v1063H protocol extensions (agent H)
 * Every hook below has a weak default (MSVC /alternatename in gf_coop_world.c), so a module only defines what it needs. */
enum { GF_COOP_EV_WORLD_SPAWN2 = 0x0122, GF_COOP_EV_WORLD_CLAIM = 0x0124 };
enum { GF_WORLD_SNAP_EXT_MAX = 16, GF_WORLD_SPAWN_EXT_MAX = 64 };   /* v1064I spawn ext 32 -> 64 B (record u8 extlen, SPAWN2 events <= 1000 B) */
typedef struct {
    float m[12];               /* interpolated world rows (what gf_proxy_drive gets next) */
    float vel[3];              /* interpolated linear velocity m/s */
    float avel[3];             /* interpolated angular velocity rad/s (world) */
    const uint8_t* ext_a;      /* ext bytes of the older bracketing sample (NULL if none) */
    const uint8_t* ext_b;      /* ext bytes of the newer bracketing sample (== ext_a when not interpolating) */
    int   ext_len;             /* bytes in ext_a / ext_b (0..GF_WORLD_SNAP_EXT_MAX) */
    float ext_f;               /* 0..1 blend between ext_a and ext_b */
    uint32_t age_ms;           /* how old the newest sample is (playback clock) */
} gf_world_drive_info;
/* host side, per kind: P defines the _ped hooks, V the _car hooks (weak defaults in gf_coop_world.c: 0 bytes / no-op) */
int  gf_world_snap_ext_ped(uint32_t ent, uint8_t kind, uint8_t* out, int max);   /* per snapshot, <= GF_WORLD_SNAP_EXT_MAX */
int  gf_world_snap_ext_car(uint32_t ent, uint8_t kind, uint8_t* out, int max);
int  gf_world_spawn_ext_ped(uint32_t ent, uint8_t kind, uint8_t* out, int max);  /* per spawn, <= GF_WORLD_SPAWN_EXT_MAX */
int  gf_world_spawn_ext_car(uint32_t ent, uint8_t kind, uint8_t* out, int max);
int  gf_world_snap_ext(uint32_t ent, uint8_t kind, uint8_t* out, int max);       /* H dispatcher by kind (do not define) */
int  gf_world_spawn_ext(uint32_t ent, uint8_t kind, uint8_t* out, int max);      /* H dispatcher by kind (do not define) */
void gf_world_owned_apply(uint32_t host_ent, uint8_t kind, const gf_world_drive_info* di);   /* host: guest-claimed entity state (V) */
/* guest side, per kind (P: _ped, V: _car; default no-op) */
void gf_proxy_spawn_ext_ped(uint16_t netid, const uint8_t* ext, int len);      /* before the first gf_proxy_spawn of netid */
void gf_proxy_spawn_ext_car(uint16_t netid, const uint8_t* ext, int len);
void gf_proxy_drive_info_ped(uint16_t netid, const gf_world_drive_info* di);   /* every frame, right before gf_proxy_drive */
void gf_proxy_drive_info_car(uint16_t netid, const gf_world_drive_info* di);
int  gf_proxy_adopt(uint16_t netid, uint8_t kind, uint32_t cls, const uint8_t guid[16], uint32_t ent);   /* P: bind native ent, 1 = ok, 0 = no, -1 = n/a */
/* provided by H */
void     gf_coop_world_claim(uint16_t netid, int on);   /* guest: the local player now drives proxy netid (1) / left it (0) */
uint32_t gf_world_native_match(uint32_t cls, const uint8_t guid[16], const float pos[3], float maxd);   /* guest native, non-ambient */
void     gf_world_set_host_pos(const float pos[3], int valid);   /* guest: host player position for spatial suppression */
uint32_t gf_coop_world_ent_of(uint16_t netid);        /* host: entity of a synced net id (0 = none) */
int      gf_coop_world_netid_of(uint32_t ent);          /* host: net id of a synced entity (0 = none) */
double   gf_coop_world_play_ms(void);                   /* v1066C guest: world playback time on the sender (host) clock, ms; -1 = none yet.
                                                         * gf_coop_wpose.c can play poses at this time to stay in step with the root. */
/* v1068R zone spawn authority (agent R): both peers are source AND sink of world entities when this returns 1 (GF_COOP_WORLD_GUEST_AUTH).
 * Net ids: host-owned 1..0x7FFF, joiner-owned 0x8000..0xFFFF. gf_coop_world_host_peds / _ent_of / _netid_of return the LOCAL peer's
 * own sources (on the joiner: its owned natives); proxies of the partner's entities are found with gf_proxy_any_ent_of / gf_proxy_netid_of. */
int      gf_coop_world_guest_auth(void);
int      gf_coop_world_src_has(uint32_t ent);           /* 1 = the local peer sources this native */
int      gf_coop_world_host_peds(uint32_t* ents, uint16_t* ids, float (*pos)[3], int max);

/* W1 */
int  gf_world_enum(gf_world_ent* out, int max);          /* all live peds + cars (not players, not the co-op puppet) */
void gf_world_suppress_ambient(int on);                   /* guest: stop ambient ped/traffic spawning + remove existing */
int  gf_world_is_proxy(uint32_t ent);                     /* implemented by W3, used by W1 to skip proxies */

/* W3 */
uint32_t gf_proxy_spawn(uint16_t netid, uint8_t kind, uint32_t class_hash, const uint8_t guid[16], const float m[12]);
void     gf_proxy_drive(uint16_t netid, const float m[12], const float vel[3]);   /* target; proxy smooths */
void     gf_proxy_release(uint16_t netid);
void     gf_proxy_release_all(void);
void     gf_proxy_tick(void);

/* W2 */
void gf_coop_world_init(void);
void gf_coop_world_tick(void);   /* called from gf_coop_glue_tick */

#ifdef __cplusplus
}
#endif
#endif
