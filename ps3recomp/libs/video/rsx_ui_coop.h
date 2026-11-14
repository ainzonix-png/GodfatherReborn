/* rsx_ui_coop.h -- v1119M (Godfather): the CO-OP tab of the pause-menu page (rsx_ui_overlay.c).
 * The title (godfather-recomp gf_coop.c) fills g_uiov_coop at startup; NULL = no co-op tab.
 * All calls are made on the RENDER thread and must not block: host / join / leave only queue a request that the
 * title's network thread carries out. */
#pragma once
#include <stddef.h>

enum { UIOV_COOP_OFF = 0, UIOV_COOP_HOSTING = 1, UIOV_COOP_CONNECTING = 2, UIOV_COOP_CONNECTED = 3, UIOV_COOP_TEST = 4, UIOV_COOP_BUSY = 5 };

typedef struct uiov_coop_api {
    /* state (UIOV_COOP_*) + up to three short status lines (upper-cased by the page) */
    int (*status)(char* l1, size_t n1, char* l2, size_t n2, char* l3, size_t n3);
    int (*host)(void);                    /* 1 = request queued */
    int (*join)(const char* address);     /* "1.2.3.4", "1.2.3.4:27960" or a host name */
    void (*leave)(void);
    const char* (*name)(void);            /* our player name */
    void (*set_name)(const char* name);   /* only while offline */
    const char* (*address)(void);         /* last join address (remembered in settings.ini as COOP_ADDR) */
    void (*set_address)(const char* a);
} uiov_coop_api;

extern const uiov_coop_api* volatile g_uiov_coop;
