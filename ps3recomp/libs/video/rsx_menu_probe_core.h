/* v1107V menu pointer: finds the game's vertical text menu on a frame (highlighted item = red text, the others = white
 * text, all left-aligned, even pitch). Shared by rsx_d3d12_backend.c (readback) and the offline test (mp_test.c).
 * Coordinates: 1280x720 layout. MP_GX x MP_GY grid of 4x2 cells, one sample each. */
#define MP_GX 320
#define MP_GY 360
#define MP_MAXI 24
#define MP_MAXV 12   /* v1107Z4: value words / volume-bar segments per row (was 4) */
typedef struct { int n, sel, pitch, x0, x1; short y0[MP_MAXI], y1[MP_MAXI], ix0[MP_MAXI], ix1[MP_MAXI];
                 /* v1107X sliders (character creator): per item the track under its label (-1 = none), knob centre x */
                 int nsl, kw; short ty[MP_MAXI], tx0[MP_MAXI], tx1[MP_MAXI], kx[MP_MAXI];
                 /* v1107Z boxed lists (OPTIONS: the highlighted row has a box around it, chosen values are red): box = 1,
                  * per row up to MP_MAXV value words (x range), vsel = the red (chosen) one */
                 int box; signed char nv[MP_MAXI], vsel[MP_MAXI]; short vx0[MP_MAXI][MP_MAXV], vx1[MP_MAXI][MP_MAXV]; } mp_menu;
typedef struct { int y0, y1, x0, x1, n; } mp_band;
static unsigned char s_mp_red[MP_GY][MP_GX], s_mp_wht[MP_GY][MP_GX];
static int mp_isred(int r, int g, int b) { return r >= 70 && r * 2 >= g * 5 && r * 2 >= b * 5; }
static int mp_iswht(int r, int g, int b) { int mn = r < g ? (r < b ? r : b) : (g < b ? g : b), mx = r > g ? (r > b ? r : b) : (g > b ? g : b); return mn >= 105 && mx - mn <= 30; }
/* v1107X slider classes (creator FACE/HEAD dump s21/048: track 34 / 51 grey, knob 68 grey or 142,22,22 red, arrows likewise,
 * panel black). trk = dark neutral grey, gry = mid neutral grey (unselected knob / arrows). */
static unsigned char s_mp_trk[MP_GY][MP_GX], s_mp_gry[MP_GY][MP_GX], s_mp_lit[MP_GY][MP_GX], s_mp_rdim[MP_GY][MP_GX];   /* rdim: v1107Z4 dark red (chosen value in the boxed row, ~66,10,10) */   /* lit: v1107Z, any non-dark */
static int mp_neutral(int r, int g, int b, int lo, int hi) { int mn = r < g ? (r < b ? r : b) : (g < b ? g : b), mx = r > g ? (r > b ? r : b) : (g > b ? g : b); return mn >= lo && mx <= hi && mx - mn <= 12; }
static void mp_classify(int gy, int gx, int r, int g, int b)
{
    s_mp_red[gy][gx] = (unsigned char)mp_isred(r, g, b);
    s_mp_wht[gy][gx] = (unsigned char)mp_iswht(r, g, b);
    s_mp_trk[gy][gx] = (unsigned char)mp_neutral(r, g, b, 22, 60);
    s_mp_gry[gy][gx] = (unsigned char)mp_neutral(r, g, b, 61, 104);
    s_mp_lit[gy][gx] = (unsigned char)((r > g ? (r > b ? r : b) : (g > b ? g : b)) >= 45);
    s_mp_rdim[gy][gx] = (unsigned char)(r >= 45 && r >= 4 * g && r >= 4 * b);
}
/* v1107Z boxed list (OPTIONS screen, dumps m7shots/018-024): the highlighted row sits in a thin rectangle (red or grey,
 * >= 600 px wide, 24..90 px tall, dark 6 px above / below its top and bottom lines); the row labels are left-aligned text
 * at one x; each row's values (Normal / Inverted, On / Off ...) are words further right, the chosen one red.
 * Returns the item count (0 = no box list). */
static int mp_lit_run(int y, int x0, int x1, int* b0, int* b1)
{
    int best = 0, s0 = -1, last = -9; *b0 = 0; *b1 = -1;
    for (int x = x0; x < x1; x++) {
        if (!s_mp_lit[y][x]) continue;
        if (x - last > 2) s0 = x;
        last = x;
        if (x - s0 + 1 > best) { best = x - s0 + 1; *b0 = s0; *b1 = x; }
    }
    return best;
}
static int mp_lit_frac(int y, int x0, int x1) { int c = 0; if (y < 0 || y >= MP_GY) return 0; for (int x = x0; x <= x1; x++) c += s_mp_lit[y][x]; return c * 100 / (x1 - x0 + 1); }
static int mp_boxlist(mp_menu* m)
{
    int ly[32], lx0[32], lx1[32], nl = 0;
    for (int y = 3; y < MP_GY - 3 && nl < 32; y++) {
        int b0, b1; if (mp_lit_run(y, 0, MP_GX, &b0, &b1) < 150) continue;
        if (nl && ly[nl - 1] >= y - 2 && abs(lx0[nl - 1] - b0) <= 3) continue;   /* same (thick) line */
        if (mp_lit_frac(y - 3, b0, b1) > 30 || mp_lit_frac(y + 3, b0, b1) > 30) continue;
        ly[nl] = y; lx0[nl] = b0; lx1[nl] = b1; nl++;
    }
    for (int a = 0; a < nl; a++) for (int b = a + 1; b < nl; b++) {
        const int gap = ly[b] - ly[a];
        if (gap < 12 || gap > 45 || abs(lx0[a] - lx0[b]) > 4 || abs(lx1[a] - lx1[b]) > 4) continue;
        int edge = 0; for (int y = ly[a] + 1; y < ly[b]; y++) edge += s_mp_lit[y][lx0[a]] || s_mp_lit[y][lx0[a] + 1] || s_mp_lit[y][lx0[a] - (lx0[a] > 0)];
        if (edge * 100 < (gap - 1) * 60) continue;
        /* label inside: leftmost text */
        const int ix0 = lx0[a] + 4, ix1 = lx1[a] - 4;
        int tx = 9999, ty0 = -1, ty1 = -1;
        for (int y = ly[a] + 2; y <= ly[b] - 2; y++) { int c = 0, mn = 9999; for (int x = ix0; x <= ix1; x++) if (s_mp_lit[y][x]) { c++; if (x < mn) mn = x; }
            if (c >= 2) { if (mn < tx) tx = mn; if (ty0 < 0) ty0 = y; ty1 = y; } }
        if (tx == 9999 || ty0 < 0) continue;
        const int h = ty1 - ty0 + 1; if (h < 3 || h > 22) continue;
        /* rows: text runs starting at tx (+-3 cells), similar height, anywhere on screen */
        mp_band it[MP_MAXI + 1]; int ni = 0, on = 0; mp_band cur = {0};
        for (int y = 0; y <= MP_GY; y++) {
            int c = 0, mn = 9999;
            if (y < MP_GY) for (int x = tx - 3; x < tx + 60 && x < MP_GX; x++) if (x >= 0 && s_mp_lit[y][x]) { c++; if (x < mn) mn = x; }
            if (c >= 3 && mn <= tx + 3) { if (!on) { on = 1; cur.y0 = cur.y1 = y; cur.x0 = mn; cur.n = c; } else { cur.y1 = y; if (mn < cur.x0) cur.x0 = mn; } }
            else if (on && (y >= MP_GY || y - cur.y1 > 1)) {
                on = 0; const int wh = cur.y1 - cur.y0 + 1;
                if (cur.x0 >= tx - 3 && wh * 10 >= h * 6 && wh * 10 <= h * 17 && ni < MP_MAXI) it[ni++] = cur;
            }
        }
        int ri = -1; for (int i = 0; i < ni; i++) if (it[i].y1 >= ty0 && it[i].y0 <= ty1) { ri = i; break; }
        if (ri < 0) continue;
        int lo = ri, hi = ri, pitch = 0;
        for (int d = 1; d >= -1; d -= 2) {
            int j = ri;
            while (j + d >= 0 && j + d < ni) {
                const int g = abs((it[j + d].y0 + it[j + d].y1) - (it[j].y0 + it[j].y1)) / 2;
                if (g > 5 * h) break;
                if (pitch && abs(g - pitch) * 100 > 35 * pitch) break;
                if (!pitch) pitch = g;
                j += d;
            }
            if (d > 0) hi = j; else lo = j;
        }
        const int cnt = hi - lo + 1;
        if (cnt < 2) continue;
        m->n = cnt; m->sel = ri - lo; m->pitch = pitch * 2; m->x0 = tx * 4; m->x1 = lx1[a] * 4 - 180; m->box = 1;
        for (int i = 0; i < cnt; i++) {
            const mp_band* bd = &it[lo + i];
            m->y0[i] = (short)(bd->y0 * 2); m->y1[i] = (short)(bd->y1 * 2 + 1); m->ix0[i] = (short)(bd->x0 * 4);
            /* label end: text run from the label start with gaps <= 8 cells */
            int last = bd->x0;
            for (int x = bd->x0; x < ix1 && x - last <= 8; x++) for (int y = bd->y0; y <= bd->y1; y++) if (s_mp_lit[y][x]) { last = x; break; }
            m->ix1[i] = (short)(last * 4 + 3);
            /* values: words / bar segments right of the label, split by gaps > 2 empty cells (v1107Z4: not by colour -- a
             * dim red "Off" was split in two); the chosen one = the most red (>= 30 % of its columns) */
            m->nv[i] = 0; m->vsel[i] = -1;
            { int vs = -1, vl = -9, rc = 0, best = 0;
              for (int x = last + 20; x <= ix1 + 3; x++) {
                  int lit = 0, red = 0;
                  if (x <= ix1) for (int y = bd->y0; y <= bd->y1; y++) { if (s_mp_lit[y][x]) lit = 1; if (s_mp_red[y][x] || s_mp_rdim[y][x]) red = 1; }
                  if (lit && vs >= 0 && x - vl <= 2) { vl = x; rc += red; continue; }
                  if (lit && vs < 0) { vs = x; vl = x; rc = red; continue; }
                  if (vs >= 0 && x - vl > 2) {   /* a gap of >= 2 empty cells ends the word / segment */
                      if (m->nv[i] < MP_MAXV) {
                          const int k = m->nv[i], wcols = vl - vs + 1, pct = rc * 100 / (wcols > 0 ? wcols : 1);
                          m->vx0[i][k] = (short)(vs * 4); m->vx1[i][k] = (short)(vl * 4 + 3);
                          if (pct >= 30 && pct > best) { best = pct; m->vsel[i] = (signed char)k; }
                          m->nv[i]++; }
                      vs = -1;
                      if (lit) { vs = x; vl = x; rc = red; } }
              } }
            m->ty[i] = m->tx0[i] = m->tx1[i] = m->kx[i] = -1;
        }
        m->nsl = 0; m->kw = 0;
        return cnt;
    }
    return 0;
}
/* v1107X: for each item, look below its label (up to the next label / 56 px) for a slider track: a row run of track /
 * knob cells (gaps <= 1 cell) >= 240 px wide starting near the label, on >= 2 consecutive rows. The knob = the longest
 * run of mid-grey or red cells inside it (24..100 px). Arrows sit outside the run (10 px black gap). */
static void mp_sliders(mp_menu* m)
{
    m->nsl = 0; m->kw = 0;
    for (int i = 0; i < m->n; i++) {
        m->ty[i] = m->tx0[i] = m->tx1[i] = m->kx[i] = -1;
        const int r0 = m->y1[i] / 2 + 1;
        int r1 = r0 + 28; if (i + 1 < m->n && m->y0[i + 1] / 2 - 1 < r1) r1 = m->y0[i + 1] / 2 - 1; if (r1 > MP_GY - 1) r1 = MP_GY - 1;
        int cx0 = m->ix0[i] / 4 - 12, cx1 = m->ix0[i] / 4 + 170; if (cx0 < 0) cx0 = 0; if (cx1 > MP_GX) cx1 = MP_GX;
        int hits = 0, best0 = 0, best1 = -1, brow = -1;
        for (int y = r0; y <= r1; y++) {
            int b0 = 0, b1 = -1, s0 = -1, last = -9;
            for (int x = cx0; x < cx1; x++) {
                const int on = s_mp_trk[y][x] || s_mp_gry[y][x] || s_mp_red[y][x];
                if (!on) continue;
                if (x - last > 2) s0 = x;
                last = x;
                if (x - s0 > b1 - b0) { b0 = s0; b1 = x; }
            }
            if (b1 - b0 + 1 >= 60) {
                if (!hits) { best0 = b0; best1 = b1; brow = y; }
                if (++hits >= 2) break;
            } else if (hits) { hits = 0; brow = -1; }
        }
        if (hits < 2 || brow < 0) continue;
        /* knob on the middle row of the track */
        const int ky = brow + 1;
        int k0 = -1, k1 = -2, s0 = -1;
        for (int x = best0; x <= best1 + 1; x++) {
            const int on = x <= best1 && (s_mp_gry[ky][x] || s_mp_red[ky][x]);
            if (on) { if (s0 < 0) s0 = x; }
            else if (s0 >= 0) { if (x - s0 > k1 - k0 + 1) { k0 = s0; k1 = x - 1; } s0 = -1; }
        }
        m->ty[i] = (short)(brow * 2); m->tx0[i] = (short)(best0 * 4); m->tx1[i] = (short)(best1 * 4 + 3);
        if (k0 >= 0 && k1 - k0 + 1 >= 6 && k1 - k0 + 1 <= 25) {
            m->kx[i] = (short)((k0 + k1) * 2 + 2);
            if (i == m->sel || !m->kw) m->kw = (k1 - k0 + 1) * 4;
        }
        m->nsl++;
    }
}
/* white row runs inside x cells [cx0, cx1): rows with >= 3 white cells; a 1-row hole ends nothing, 2 rows end the run */
static int mp_wruns(int cx0, int cx1, mp_band* out, int maxo)
{
    int no = 0, on = 0; mp_band cur = {0};
    if (cx0 < 0) cx0 = 0; if (cx1 > MP_GX) cx1 = MP_GX;
    for (int y = 0; y < MP_GY; y++) {
        int cnt = 0, mn = 9999, mx = -1;
        for (int x = cx0; x < cx1; x++) if (s_mp_wht[y][x]) { cnt++; if (x < mn) mn = x; mx = x; }
        if (cnt >= 3) {
            if (!on) { on = 1; cur.y0 = cur.y1 = y; cur.x0 = mn; cur.x1 = mx; cur.n = cnt; }
            else { cur.y1 = y; if (mn < cur.x0) cur.x0 = mn; if (mx > cur.x1) cur.x1 = mx; cur.n += cnt; }
        } else if (on && y - cur.y1 > 1) { if (no < maxo) out[no++] = cur; on = 0; }
    }
    if (on && no < maxo) out[no++] = cur;
    return no;
}
/* red components: rows with red cells, split where a row's red cells are > 10 cells (40 px) apart */
static int mp_reds(mp_band* out, int maxo)
{
    int no = 0;
    mp_band open[16]; int nop = 0;
    for (int y = 0; y <= MP_GY; y++) {
        mp_band seg[16]; int ns = 0;
        if (y < MP_GY) {
            int x = 0;
            while (x < MP_GX && ns < 16) {
                while (x < MP_GX && !s_mp_red[y][x]) x++;
                if (x >= MP_GX) break;
                int s0 = x, last = x, cnt = 0;
                while (x < MP_GX && x - last <= 10) { if (s_mp_red[y][x]) { last = x; cnt++; } x++; }
                if (cnt >= 2) { seg[ns].y0 = seg[ns].y1 = y; seg[ns].x0 = s0; seg[ns].x1 = last; seg[ns].n = cnt; ns++; }
            }
        }
        int used[16] = {0};
        mp_band nopen[16]; int nn = 0;
        for (int i = 0; i < nop; i++) {
            int merged = 0;
            for (int k = 0; k < ns; k++) if (!used[k] && seg[k].x0 <= open[i].x1 + 2 && seg[k].x1 >= open[i].x0 - 2) {
                used[k] = 1; merged = 1;
                open[i].y1 = y; if (seg[k].x0 < open[i].x0) open[i].x0 = seg[k].x0; if (seg[k].x1 > open[i].x1) open[i].x1 = seg[k].x1; open[i].n += seg[k].n; }
            if (merged || y - open[i].y1 <= 1) { if (nn < 16) nopen[nn++] = open[i]; }
            else if (no < maxo) out[no++] = open[i];
        }
        for (int k = 0; k < ns; k++) if (!used[k] && nn < 16) nopen[nn++] = seg[k];
        memcpy(open, nopen, sizeof(mp_band) * (size_t)nn); nop = nn;
    }
    return no;
}
static int mp_analyze(mp_menu* m)
{
    m->box = 0;
    if (mp_boxlist(m) >= 2) return m->n;   /* v1107Z: a boxed list wins (its red value words are not highlights) */
    mp_band reds[64]; const int nr = mp_reds(reds, 64);
    int bestn = 0; m->n = 0; m->sel = -1;
    /* v1114P (user: "the pause menu is fucked up, I can't use the cursor"): the PAUSE menu's rows are CENTRED (MAP,
     * SKILLS AND UPGRADES, ... each centred on one x), so the left-aligned search only ever found the red row (1 item)
     * and the pointer never hit anything. Each red row is now tried both ways -- rows starting at its x (left-aligned,
     * as before) and rows whose centre is its centre (+-3 cells, >= 3 rows: the film archive's red "Movie 1/7" over a centred title is
     * not a menu) -- and the longer list wins (ties: left-aligned). */
    for (int r = 0; r < nr * 2; r++) {
        const int centred = r >= nr;
        const mp_band* rb = &reds[centred ? r - nr : r];
        const int h = rb->y1 - rb->y0 + 1;   /* rows of 2 px */
        if (rb->n < 10 || h < 3 || h > 22 || rb->x1 - rb->x0 < 4) continue;
        const int rc = (rb->x0 + rb->x1) / 2;
        mp_band ws[64]; const int nw = centred ? mp_wruns(rc - 110, rc + 110, ws, 64) : mp_wruns(rb->x0 - 8, rb->x0 + 105, ws, 64);
        mp_band it[MP_MAXI + 1]; int ni = 0, ri;
        it[ni++] = *rb;
        for (int i = 0; i < nw && ni < MP_MAXI; i++) {
            const mp_band* w = &ws[i]; const int wh = w->y1 - w->y0 + 1;
            if (centred) { const int wc = (w->x0 + w->x1) / 2; if (wc < rc - 3 || wc > rc + 3) continue; }
            else if (w->x0 < rb->x0 - 3 || w->x0 > rb->x0 + 12) continue;
            if (wh * 10 < h * 6 || wh * 10 > h * 17) continue;
            if (w->y1 >= rb->y0 && w->y0 <= rb->y1) continue;
            it[ni++] = *w;
        }
        for (int i = 1; i < ni; i++) for (int k = i; k > 0 && it[k].y0 < it[k - 1].y0; k--) { mp_band t = it[k]; it[k] = it[k - 1]; it[k - 1] = t; }
        for (ri = 0; ri < ni; ri++) if (it[ri].y0 == rb->y0 && it[ri].x0 == rb->x0) break;
        int lo = ri, hi = ri, pitch = 0;
        for (int d = 1; d >= -1; d -= 2) {
            int j = ri;
            while (j + d >= 0 && j + d < ni) {
                const int gap = abs((it[j + d].y0 + it[j + d].y1) - (it[j].y0 + it[j].y1)) / 2;
                if (gap > 5 * h) break;
                if (pitch && abs(gap - pitch) * 100 > 35 * pitch) break;
                if (!pitch) pitch = gap;
                j += d;
            }
            if (d > 0) hi = j; else lo = j;
        }
        const int cnt = hi - lo + 1;
        if (centred && cnt < 3) continue;
        if (cnt > bestn) {
            bestn = cnt; m->n = cnt; m->sel = ri - lo; m->pitch = pitch * 2; m->x0 = rb->x0 * 4; m->x1 = 0;
            for (int i = 0; i < cnt; i++) {
                const mp_band* b = &it[lo + i];
                m->y0[i] = (short)(b->y0 * 2); m->y1[i] = (short)(b->y1 * 2 + 1); m->ix0[i] = (short)(b->x0 * 4); m->ix1[i] = (short)(b->x1 * 4 + 3);
                if (m->ix1[i] > m->x1) m->x1 = m->ix1[i]; if (m->ix0[i] < m->x0) m->x0 = m->ix0[i];
            }
        }
    }
    mp_sliders(m);   /* v1107X */
    return m->n;
}
