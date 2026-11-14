#pragma once
/* rsx_ui_shop.h -- inventory / store pages of the port overlay (rsx_ui_overlay.c), drawn in the title's own pause-menu
 * look. The runtime only draws, navigates and reads keys; the TITLE owns items, money and what "use" / "buy" do.
 *
 *   page 1 = inventory (opened with GF_INV_KEY, default I), page 2 = store (opened with GF_SHOP_KEY, default B, only
 *   while the title's prompt() says a store is here).
 *
 * Threading: everything here is called on the RENDER thread -- only read title state; activate() should record a
 * request that the title's main-thread tick applies. */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct uiov_shop_row {
    char name[40];        /* item name                                           */
    char right[24];       /* inventory: "x 18"; store: "$15"                     */
    char sub[24];         /* store: "owned 2" (small, dim); may be empty         */
    char desc[160];       /* shown in the strip at the bottom for the selection  */
    int  enabled;         /* 0 = drawn dim (cannot use / cannot afford)          */
} uiov_shop_row;

typedef struct uiov_shop_api {
    const char* (*title)(int page);                       /* page heading                                  */
    int  (*count)(int page);                              /* rows                                          */
    int  (*row)(int page, int i, uiov_shop_row* out);     /* 0 = skip                                      */
    void (*activate)(int page, int i);                    /* use / buy (record a request)                  */
    int  (*status)(int page, char* text, size_t cap);     /* right side of the heading line (cash)         */
    int  (*prompt)(char* text, size_t cap);               /* closed: store here? text after the key, 0 none */
    int  (*toast)(char* text, size_t cap);                /* short message to show now (0 = none)          */
    int  (*can_open)(void);                               /* 0 = not now (no player, cutscene ...)         */
    int  (*request)(void);                                /* polled: 1 / 2 = open that page now, -1 = close, 0 = nothing */
    int  (*selected)(int page, int i);                    /* v1112A optional, every tick while open: the highlighted row;
                                                             returns a row to move the highlight to, or -1 */
    int  (*back)(int page);                               /* v1112A optional: Esc / B handled by the title (1) or closes (0) */
    void (*adjust)(int page, int i, int dir);             /* v1116B optional: Left / Right on a row (dir -1 / +1), e.g. a colour */
} uiov_shop_api;

extern const uiov_shop_api* volatile g_uiov_shop;   /* rsx_ui_overlay.c; NULL = no pages */

#ifdef __cplusplus
}
#endif
