/* ui_core.inc -- theme, dp scaling, input/gestures, text editing + on-screen keyboard, widgets.
 * Included by main.c only (not compiled on its own). */

/* ============================== theme ============================== */
static const Color C_BG = { 10, 10, 14, 255 }, C_SURF = { 21, 21, 28, 255 }, C_SURF2 = { 33, 33, 43, 255 }, C_SURF3 = { 48, 48, 62, 255 },
                   C_LINE = { 56, 56, 72, 255 }, C_TEXT = { 242, 242, 248, 255 }, C_DIM = { 146, 146, 166, 255 }, C_ACC = { 255, 154, 46, 255 },
                   C_ACC2 = { 76, 201, 240, 255 }, C_RED = { 240, 84, 84, 255 }, C_GREEN = { 110, 210, 130, 255 }, C_FIELD = { 14, 14, 20, 255 },
                   C_VIOLET = { 176, 124, 255, 255 }, C_INK = { 12, 12, 16, 255 };

static float DP = 1.0f;                 /* pixels per dp */
#define D(v) ((v) * DP)
static float T_TINY = 10, T_SMALL = 12, T_BODY = 14, T_TITLE = 17;
static void set_scale(float dp) {
    DP = dp < 0.75f ? 0.75f : dp > 4.5f ? 4.5f : dp;
    T_TINY = D(10.5f); T_SMALL = D(12); T_BODY = D(14); T_TITLE = D(17);
}

/* ============================== shared state ============================== */
enum { TOOL_NONE, TOOL_ADD, TOOL_TRANSFORM, TOOL_STYLE, TOOL_FX, TOOL_FUSION, TOOL_ANIM, TOOL_LAYERS, TOOL_CANVAS, TOOL_SCRIPT, TOOL_EXPORT, TOOL_COUNT };
enum { DLG_NONE, DLG_PROJECTS, DLG_SCRIPT, DLG_MEDIA };

static Project *P;
static Font F, FB, FM;
static int SW = 1280, SH = 720;
static bool portrait = true, touch_mode;
static Rectangle clip_r;
static bool blocked;
static unsigned long frame_no;
static int sel = -1, tool, dlg;
static float cur_t;
static bool playing, loop_play = true;
static char proj_dir[PATH_LEN];

#define LOGN 200
static struct { char s[200]; int lvl; } logs[LOGN];
static int log_n; static bool log_dirty;
static char status[200]; static int status_lvl; static double status_until;
static void set_status(int lvl, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(status, sizeof status, fmt, ap); va_end(ap);
    status_lvl = lvl; status_until = GetTime() + 4;
}
static void log_cb(int lvl, const char *msg) {
    const char *p = msg;
    do {
        const char *nl = strchr(p, '\n');
        int n = nl ? (int)(nl - p) : (int)strlen(p);
        int i = log_n % LOGN;
        snprintf(logs[i].s, sizeof logs[i].s, "%.*s", n, p);
        logs[i].lvl = lvl; log_n++;
        p = nl ? nl + 1 : p + n;
    } while (*p);
    log_dirty = true;
    if (lvl) set_status(1, "%s", msg);
}

static inline float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static inline bool in_rect(Rectangle r, Vector2 p) { return CheckCollisionPointRec(p, r); }
static Rectangle inset(Rectangle r, float d) { return (Rectangle){ r.x + d, r.y + d, r.width - 2 * d, r.height - 2 * d }; }
static Rectangle inset_xy(Rectangle r, float dx, float dy) { return (Rectangle){ r.x + dx, r.y + dy, r.width - 2 * dx, r.height - 2 * dy }; }
static Color alpha_c(Color c, float a) { c.a = (unsigned char)clampf(a * 255.0f, 0, 255); return c; }
static Color mix_c(Color a, Color b, float t) {
    return (Color){ (unsigned char)(a.r + (b.r - a.r) * t), (unsigned char)(a.g + (b.g - a.g) * t), (unsigned char)(a.b + (b.b - a.b) * t), 255 };
}

/* ---- test hooks: label registry used by the --uitest harness ---- */
static struct { char l[48]; Rectangle r; } dbg_cur[320], dbg_prev[320];
static int dbg_n, dbg_pn;
static void dbg_reg(const char *l, Rectangle r) {
    if (dbg_n < 320) { snprintf(dbg_cur[dbg_n].l, sizeof dbg_cur[0].l, "%s", l); dbg_cur[dbg_n++].r = r; }
}
static void dbg_swap(void) { memcpy(dbg_prev, dbg_cur, sizeof(dbg_cur[0]) * (size_t)dbg_n); dbg_pn = dbg_n; dbg_n = 0; }

/* ============================== fonts / text ============================== */
static unsigned char *asset_bytes(const char *rel, int *n) {
#ifdef PLATFORM_ANDROID
    return LoadFileData(rel, n);
#else
    char buf[PATH_LEN];
    const char *p = afx_find_asset(rel, buf, sizeof buf);
    return p ? LoadFileData(p, n) : NULL;
#endif
}
static int font_bake;
static void load_fonts(void) {
    static int cp[512];
    int nc = 0;
    for (int c = 32; c <= 383; c++) cp[nc++] = c;
    static const int extra[] = { 0x192, 0x2022, 0x2026, 0x2013, 0x2014, 0x2190, 0x2192, 0x2212 };
    for (unsigned i = 0; i < sizeof extra / sizeof *extra; i++) cp[nc++] = extra[i];
    int bake = (int)(D(18) + 0.5f);
    if (bake < 16) bake = 16;
    if (bake > 72) bake = 72;
    if (font_bake == bake) return;
    if (font_bake) { UnloadFont(F); UnloadFont(FB); UnloadFont(FM); }
    const char *names[3] = { "fonts/DejaVuSans.ttf", "fonts/DejaVuSans-Bold.ttf", "fonts/DejaVuSansMono.ttf" };
    Font *out[3] = { &F, &FB, &FM };
    for (int i = 0; i < 3; i++) {
        int n = 0; unsigned char *d = asset_bytes(names[i], &n);
        *out[i] = d ? LoadFontFromMemory(".ttf", d, n, bake, cp, nc) : GetFontDefault();
        if (d) { SetTextureFilter(out[i]->texture, TEXTURE_FILTER_BILINEAR); UnloadFileData(d); }
    }
    font_bake = bake;
}
static float tw(Font f, const char *s, float sz) { return MeasureTextEx(f, s, sz, 0).x; }
static void dtext(Font f, const char *s, float x, float y, float sz, Color c) { DrawTextEx(f, s, (Vector2){ roundf(x), roundf(y) }, sz, 0, c); }
/* text truncated with "..." to fit r; align 0 left, 1 center, 2 right; vertically centred */
static void dtext_fit(Font f, const char *s, Rectangle r, float sz, Color c, int align) {
    char b[300];
    snprintf(b, sizeof b, "%s", s);
    if (tw(f, b, sz) > r.width) {
        int n = (int)strlen(b);
        if (n > 250) { n = 250; b[n] = 0; }
        while (n > 1 && tw(f, b, sz) > r.width - sz * 1.1f) { n--; while (n > 0 && (b[n] & 0xC0) == 0x80) n--; b[n] = 0; }
        strcat(b, "...");
    }
    float w = tw(f, b, sz);
    float x = align == 0 ? r.x : align == 1 ? r.x + (r.width - w) / 2 : r.x + r.width - w;
    dtext(f, b, x, r.y + (r.height - sz) / 2, sz, c);
}
static void fmt_num(char *b, float v) {
    if (fabsf(v - roundf(v)) < 0.005f) snprintf(b, 32, "%d", (int)roundf(v));
    else if (fabsf(v) < 100) snprintf(b, 32, "%.2f", v);
    else snprintf(b, 32, "%.1f", v);
}
static void fmt_time(char *b, float t) {
    if (t < 0) t = 0;
    int cs = (int)floorf(t * 100 + 0.5f);
    snprintf(b, 24, "%d:%02d.%02d", cs / 6000, (cs / 100) % 60, cs % 100);
}

/* ============================== shapes + icons ============================== */
static void rrect(Rectangle r, float rad, Color c) {
    float m = fminf(r.width, r.height) / 2;
    if (m <= 0) return;
    DrawRectangleRounded(r, fminf(rad / m, 1.0f), 10, c);
}
static void rrect_line(Rectangle r, float rad, float th, Color c) {
    float m = fminf(r.width, r.height) / 2;
    if (m <= 0) return;
    DrawRectangleRoundedLinesEx(r, fminf(rad / m, 1.0f), 10, th, c);
}
static void ln(Vector2 a, Vector2 b, float th, Color c) { DrawLineEx(a, b, th, c); DrawCircleV(a, th / 2, c); DrawCircleV(b, th / 2, c); }
static void tri(Vector2 a, Vector2 b, Vector2 c, Color col) {
    float cr = (b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x);
    if (cr > 0) { Vector2 t = b; b = c; c = t; }
    DrawTriangle(a, b, c, col);
}

enum { IC_NONE, IC_BACK, IC_MENU, IC_PLUS, IC_CHECK, IC_CLOSE, IC_PLAY, IC_PAUSE, IC_PREV, IC_NEXT, IC_LOOP, IC_DIA, IC_DIA_FILL, IC_EYE, IC_EYE_OFF,
       IC_COPY, IC_TRASH, IC_UP, IC_DOWN, IC_RIGHT, IC_UNDO, IC_REDO, IC_TEXT, IC_SHAPE, IC_ASTEROID, IC_STARS, IC_SOLID, IC_SVG, IC_IMAGE,
       IC_ADJUST, IC_FX, IC_ANIM, IC_STYLE, IC_MOVE, IC_CANVAS, IC_SCRIPT, IC_EXPORT, IC_LAYERS, IC_BLEND, IC_FOLDER, IC_SPLIT, IC_NEW, IC_VIDEO, IC_FUSION };

/* icon drawn on a 24x24 design grid inside the square box r */
static void draw_icon(int ic, Rectangle r, Color c) {
    float u = fminf(r.width, r.height) / 24.0f;
    float ox = r.x + r.width / 2 - 12 * u, oy = r.y + r.height / 2 - 12 * u, th = fmaxf(1.5f, 2.0f * u);
#define PT(x, y) ((Vector2){ ox + (x) * u, oy + (y) * u })
#define LN(x1, y1, x2, y2) ln(PT(x1, y1), PT(x2, y2), th, c)
    switch (ic) {
    case IC_BACK: LN(15, 5, 8, 12); LN(8, 12, 15, 19); break;
    case IC_RIGHT: LN(9, 5, 16, 12); LN(16, 12, 9, 19); break;
    case IC_UP: LN(5, 15, 12, 8); LN(12, 8, 19, 15); break;
    case IC_DOWN: LN(5, 9, 12, 16); LN(12, 16, 19, 9); break;
    case IC_MENU: LN(4, 7, 20, 7); LN(4, 12, 20, 12); LN(4, 17, 20, 17); break;
    case IC_PLUS: LN(12, 4.5f, 12, 19.5f); LN(4.5f, 12, 19.5f, 12); break;
    case IC_CHECK: LN(4, 12.5f, 9.5f, 18); LN(9.5f, 18, 20, 6.5f); break;
    case IC_CLOSE: LN(6, 6, 18, 18); LN(18, 6, 6, 18); break;
    case IC_PLAY: tri(PT(7, 4), PT(7, 20), PT(20, 12), c); break;
    case IC_PAUSE: rrect((Rectangle){ ox + 6 * u, oy + 4 * u, 4 * u, 16 * u }, u, c); rrect((Rectangle){ ox + 14 * u, oy + 4 * u, 4 * u, 16 * u }, u, c); break;
    case IC_PREV: tri(PT(18, 5), PT(18, 19), PT(8, 12), c); DrawRectangleRec((Rectangle){ ox + 5 * u, oy + 5 * u, 2.4f * u, 14 * u }, c); break;
    case IC_NEXT: tri(PT(6, 5), PT(6, 19), PT(16, 12), c); DrawRectangleRec((Rectangle){ ox + 16.6f * u, oy + 5 * u, 2.4f * u, 14 * u }, c); break;
    case IC_LOOP:
        DrawRing(PT(12, 12), 6.2f * u, 6.2f * u + th, 40, 340, 18, c);
        tri(PT(17.5f, 2.5f), PT(17.5f, 9.5f), PT(11, 6), c); break;
    case IC_DIA: case IC_DIA_FILL: {
        Vector2 a = PT(12, 3.5f), b = PT(20.5f, 12), d = PT(12, 20.5f), e = PT(3.5f, 12);
        if (ic == IC_DIA_FILL) { tri(a, b, d, c); tri(a, d, e, c); }
        else { ln(a, b, th, c); ln(b, d, th, c); ln(d, e, th, c); ln(e, a, th, c); }
        break; }
    case IC_EYE: case IC_EYE_OFF: {
        Vector2 prev = PT(21, 12);
        for (int i = 1; i <= 28; i++) { float a = (float)i / 28 * 6.2832f; Vector2 q = PT(12 + 9 * cosf(a), 12 + 5.6f * sinf(a)); ln(prev, q, th, c); prev = q; }
        DrawCircleV(PT(12, 12), 2.7f * u, c);
        if (ic == IC_EYE_OFF) { ln(PT(4, 20), PT(20, 4), th * 1.6f, C_BG); LN(4, 20, 20, 4); }
        break; }
    case IC_COPY: rrect_line((Rectangle){ ox + 8 * u, oy + 8 * u, 12 * u, 12 * u }, 2.5f * u, th, c);
        ln(PT(16, 5), PT(7, 5), th, c); ln(PT(7, 5), PT(4.5f, 7.5f), th, c); ln(PT(4.5f, 7.5f), PT(4.5f, 16), th, c); break;
    case IC_TRASH: LN(4.5f, 6.5f, 19.5f, 6.5f); LN(9, 6.5f, 9, 4); LN(9, 4, 15, 4); LN(15, 4, 15, 6.5f);
        LN(6.5f, 6.5f, 7.6f, 19.5f); LN(7.6f, 19.5f, 16.4f, 19.5f); LN(16.4f, 19.5f, 17.5f, 6.5f); LN(10.2f, 10, 10.4f, 16); LN(13.8f, 10, 13.6f, 16); break;
    case IC_UNDO: case IC_REDO: {
        float s = ic == IC_REDO ? -1.0f : 1.0f, cx = ic == IC_REDO ? 24.0f : 0.0f;
        #define MX(x) (cx + s * (x))
        Vector2 prev = PT(MX(5), 13);
        for (int i = 1; i <= 16; i++) { float a = 3.1416f + (float)i / 16 * 3.6f; Vector2 q = PT(MX(12 + 7 * cosf(a)), 14 + 7 * sinf(a)); ln(prev, q, th, c); prev = q; }
        ln(PT(MX(5), 14), PT(MX(5), 8), th, c); ln(PT(MX(5), 14), PT(MX(11), 14), th, c);
        #undef MX
        break; }
    case IC_TEXT: LN(5.5f, 6, 18.5f, 6); LN(12, 6, 12, 19); LN(5.5f, 6, 5.5f, 8.5f); LN(18.5f, 6, 18.5f, 8.5f); LN(9, 19, 15, 19); break;
    case IC_SHAPE: DrawRing(PT(9.5f, 9.5f), 5.2f * u, 5.2f * u + th, 0, 360, 28, c); rrect_line((Rectangle){ ox + 11 * u, oy + 11 * u, 9 * u, 9 * u }, 1.8f * u, th, c); break;
    case IC_ASTEROID: DrawCircleV(PT(14.5f, 14.5f), 6.2f * u, c); DrawCircleV(PT(13, 13), 1.3f * u, alpha_c(C_INK, .45f)); DrawCircleV(PT(16.5f, 16), 1.0f * u, alpha_c(C_INK, .45f));
        LN(10, 9.5f, 4, 4); LN(8, 13.5f, 3, 11); LN(9.5f, 8, 11.5f, 2.5f); break;
    case IC_STARS: {
        Vector2 ctr = PT(12, 12), pts[8];
        for (int i = 0; i < 8; i++) { float a = (float)i / 8 * 6.2832f - 1.5708f, rr = (i & 1) ? 3.2f : 10.0f; pts[i] = PT(12 + rr * cosf(a), 12 + rr * sinf(a)); }
        for (int i = 0; i < 8; i++) tri(ctr, pts[i], pts[(i + 1) % 8], c);
        break; }
    case IC_SOLID: rrect((Rectangle){ ox + 4 * u, oy + 4 * u, 16 * u, 16 * u }, 3 * u, c); break;
    case IC_SVG: {
        Vector2 prev = PT(5, 18.5f);
        for (int i = 1; i <= 18; i++) { float t = (float)i / 18, a = (1 - t) * (1 - t), b = 2 * (1 - t) * t, d = t * t; Vector2 q = PT(a * 5 + b * 4 + d * 19, a * 18.5f + b * 4 + d * 6); ln(prev, q, th, c); prev = q; }
        DrawRectangleRec((Rectangle){ ox + 2.5f * u, oy + 16 * u, 5 * u, 5 * u }, c); DrawRectangleRec((Rectangle){ ox + 16.5f * u, oy + 3.5f * u, 5 * u, 5 * u }, c); break; }
    case IC_IMAGE: rrect_line((Rectangle){ ox + 3 * u, oy + 4.5f * u, 18 * u, 15 * u }, 2.5f * u, th, c);
        LN(5, 17, 10, 11.5f); LN(10, 11.5f, 14, 15.5f); LN(14, 15.5f, 16.5f, 13); LN(16.5f, 13, 20, 16.5f); DrawCircleV(PT(16, 9), 1.6f * u, c); break;
    case IC_ADJUST: LN(4, 7, 20, 7); LN(4, 12, 20, 12); LN(4, 17, 20, 17);
        DrawCircleV(PT(8, 7), 2.6f * u, c); DrawCircleV(PT(16, 12), 2.6f * u, c); DrawCircleV(PT(10, 17), 2.6f * u, c); break;
    case IC_FX: LN(4.5f, 19.5f, 14, 10); {
        Vector2 ctr = PT(17.5f, 6.5f), pts[8];
        for (int i = 0; i < 8; i++) { float a = (float)i / 8 * 6.2832f - 1.5708f, rr = (i & 1) ? 1.5f : 5.0f; pts[i] = PT(17.5f + rr * cosf(a), 6.5f + rr * sinf(a)); }
        for (int i = 0; i < 8; i++) tri(ctr, pts[i], pts[(i + 1) % 8], c); }
        DrawCircleV(PT(7, 6), 1.1f * u, c); DrawCircleV(PT(19.5f, 17), 1.1f * u, c); break;
    case IC_ANIM: ln(PT(8.5f, 12), PT(15.5f, 12), th, c);
        { Vector2 a = PT(5, 7.5f), b = PT(8.5f, 12), d = PT(5, 16.5f), e = PT(1.5f, 12); ln(a, b, th, c); ln(b, d, th, c); ln(d, e, th, c); ln(e, a, th, c); }
        { Vector2 a = PT(19, 7), b = PT(23, 12), d = PT(19, 17), e = PT(15, 12); tri(a, b, d, c); tri(a, d, e, c); } break;
    case IC_STYLE: DrawRing(PT(12, 12), 8.5f * u, 8.5f * u + th, 0, 360, 36, c);
        DrawCircleV(PT(8.5f, 10), 1.5f * u, c); DrawCircleV(PT(12, 7.5f), 1.5f * u, c); DrawCircleV(PT(15.5f, 10), 1.5f * u, c); DrawCircleV(PT(14.5f, 15.5f), 2.0f * u, c); break;
    case IC_MOVE: LN(12, 3, 12, 21); LN(3, 12, 21, 12); LN(9, 6, 12, 3); LN(12, 3, 15, 6); LN(9, 18, 12, 21); LN(12, 21, 15, 18);
        LN(6, 9, 3, 12); LN(3, 12, 6, 15); LN(18, 9, 21, 12); LN(21, 12, 18, 15); break;
    case IC_CANVAS: rrect_line((Rectangle){ ox + 3.5f * u, oy + 5.5f * u, 17 * u, 13 * u }, 2.2f * u, th, c);
        LN(7, 9.5f, 7, 15.5f); LN(7, 9.5f, 10.5f, 9.5f); LN(17, 14.5f, 17, 8.5f); LN(17, 14.5f, 13.5f, 14.5f); break;
    case IC_SCRIPT: LN(9, 7, 4, 12); LN(4, 12, 9, 17); LN(15, 7, 20, 12); LN(20, 12, 15, 17); LN(13.5f, 5, 10.5f, 19); break;
    case IC_EXPORT: LN(4.5f, 14, 4.5f, 19.5f); LN(4.5f, 19.5f, 19.5f, 19.5f); LN(19.5f, 19.5f, 19.5f, 14);
        LN(12, 15, 12, 3.5f); LN(7, 8.5f, 12, 3.5f); LN(12, 3.5f, 17, 8.5f); break;
    case IC_LAYERS: LN(12, 4, 21, 9); LN(21, 9, 12, 14); LN(12, 14, 3, 9); LN(3, 9, 12, 4); LN(3, 13, 12, 18); LN(12, 18, 21, 13); break;
    case IC_BLEND: DrawRing(PT(9, 12), 6 * u, 6 * u + th, 0, 360, 28, c); DrawRing(PT(15, 12), 6 * u, 6 * u + th, 0, 360, 28, c); break;
    case IC_FOLDER: LN(3, 7, 10, 7); LN(10, 7, 12, 9.5f); LN(12, 9.5f, 21, 9.5f); LN(21, 9.5f, 21, 19); LN(21, 19, 3, 19); LN(3, 19, 3, 7); break;
    case IC_SPLIT: LN(12, 3, 12, 21); rrect_line((Rectangle){ ox + 3 * u, oy + 8 * u, 6.5f * u, 8 * u }, 1.8f * u, th, c); rrect_line((Rectangle){ ox + 14.5f * u, oy + 8 * u, 6.5f * u, 8 * u }, 1.8f * u, th, c); break;
    case IC_NEW: rrect_line((Rectangle){ ox + 3.5f * u, oy + 3.5f * u, 17 * u, 17 * u }, 3 * u, th, c); LN(12, 8, 12, 16); LN(8, 12, 16, 12); break;
    case IC_VIDEO: rrect_line((Rectangle){ ox + 3 * u, oy + 5 * u, 18 * u, 14 * u }, 3 * u, th, c);
        tri(PT(10, 9), PT(10, 15), PT(15.5f, 12), c); break;
    case IC_FUSION: DrawCircleV(PT(5, 6), 2.5f * u, c); DrawCircleV(PT(5, 18), 2.5f * u, c); DrawCircleV(PT(20, 12), 2.5f * u, c);
        LN(7, 7, 12, 12); LN(7, 17, 12, 12); LN(12, 12, 17.5f, 12); DrawCircleV(PT(12, 12), 1.8f * u, c); break;
    default: break;
    }
#undef PT
#undef LN
}
static void icon_c(int ic, Rectangle btn, float size, Color c) {
    draw_icon(ic, (Rectangle){ btn.x + (btn.width - size) / 2, btn.y + (btn.height - size) / 2, size, size }, c);
}

/* ============================== input / gestures ============================== */
static Vector2 mp, mp_press;
static bool m_down, m_pressed, m_released, dragged, pinching;
static float m_dx, m_dy, wheel, pinch_scale = 1, pinch_prev;
static Vector2 pinch_c;
static int axis; static unsigned active;
static float vel_x, vel_y;
static bool key_ctrl, key_shift;
static struct { bool on, down, pressed, released; Vector2 pos; } sim;   /* scripted input for --uitest */

static void input_update(void) {
    Vector2 np; bool nd;
    if (sim.on) { np = sim.pos; nd = sim.down; m_pressed = sim.pressed; m_released = sim.released; sim.pressed = sim.released = false; }
    else { np = GetMousePosition(); m_pressed = IsMouseButtonPressed(MOUSE_BUTTON_LEFT); m_released = IsMouseButtonReleased(MOUSE_BUTTON_LEFT); nd = IsMouseButtonDown(MOUSE_BUTTON_LEFT); }
    m_down = nd;
    if (m_pressed) { m_dx = m_dy = 0; } else { m_dx = np.x - mp.x; m_dy = np.y - mp.y; }
    mp = np;
    wheel = sim.on ? 0 : GetMouseWheelMove();
    key_ctrl = !sim.on && (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL));
    key_shift = !sim.on && (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT));
    if (!sim.on && GetTouchPointCount() > 0) touch_mode = true;
    pinch_scale = 1;
    if (!sim.on && GetTouchPointCount() >= 2) {
        Vector2 a = GetTouchPosition(0), b = GetTouchPosition(1);
        float dd = hypotf(a.x - b.x, a.y - b.y);
        if (pinching && pinch_prev > 1) pinch_scale = dd / pinch_prev;
        pinch_prev = dd; pinching = true; pinch_c = (Vector2){ (a.x + b.x) / 2, (a.y + b.y) / 2 };
        axis = 3; dragged = true;
    } else pinching = false;
    if (m_pressed) { mp_press = mp; axis = 0; dragged = false; active = 0; vel_x = vel_y = 0; }
    if (m_down) { vel_x = vel_x * 0.55f + m_dx * 0.45f; vel_y = vel_y * 0.55f + m_dy * 0.45f; }
    if (m_down && !axis) {
        float ax = fabsf(mp.x - mp_press.x), ay = fabsf(mp.y - mp_press.y), slop = D(8);
        if (ax > slop || ay > slop) { axis = ax > ay * 0.9f ? 1 : 2; dragged = true; }
    }
}
static bool pressed_in(Rectangle r) { return !blocked && in_rect(r, mp_press) && in_rect(clip_r, mp_press); }
static bool tap(Rectangle r) { return m_released && !dragged && pressed_in(r) && in_rect(r, mp); }
static bool held(Rectangle r) { return m_down && !dragged && pressed_in(r) && in_rect(r, mp); }
static bool hdrag(unsigned id, Rectangle r, float *dx) {
    if (!blocked && axis == 1 && m_down && in_rect(r, mp_press) && in_rect(clip_r, mp_press) && (active == 0 || active == id)) { active = id; *dx = m_dx; return true; }
    return false;
}

/* ============================== text editing (keyboard + on-screen keyboard) ============================== */
typedef struct {
    bool active, numeric, multi, shift; int page;
    unsigned id, done_id; unsigned long began_frame;
    char title[96]; char buf[65536]; char done_buf[1024]; int len, cur; float scroll_y; bool ensure_vis;
} TextEdit;
static TextEdit te;

static void edit_begin(unsigned id, const char *title, const char *init, bool numeric, bool multi) {
    te.active = true; te.id = id; te.numeric = numeric; te.multi = multi; te.shift = false; te.page = numeric ? 2 : 0;
    snprintf(te.title, sizeof te.title, "%s", title);
    snprintf(te.buf, sizeof te.buf, "%s", init ? init : "");
    te.len = (int)strlen(te.buf); te.cur = te.len; te.scroll_y = 0; te.ensure_vis = true;
    te.began_frame = frame_no; te.done_id = 0;
}
static bool edit_done(unsigned id) { if (id && te.done_id == id) { te.done_id = 0; return true; } return false; }   /* consumed on read */
static void edit_commit(void) { te.done_id = te.id; snprintf(te.done_buf, sizeof te.done_buf, "%s", te.buf); te.active = false; }
static void edit_cancel(void) { te.active = false; te.done_id = 0; }
static void edit_insert(const char *s) {
    int n = (int)strlen(s);
    if (te.len + n >= (int)sizeof te.buf - 1) return;
    memmove(te.buf + te.cur + n, te.buf + te.cur, (size_t)(te.len - te.cur + 1));
    memcpy(te.buf + te.cur, s, (size_t)n);
    te.cur += n; te.len += n; te.ensure_vis = true;
}
static void edit_backspace(void) {
    if (te.cur <= 0) return;
    int s = te.cur - 1;
    while (s > 0 && (te.buf[s] & 0xC0) == 0x80) s--;
    memmove(te.buf + s, te.buf + te.cur, (size_t)(te.len - te.cur + 1));
    te.len -= te.cur - s; te.cur = s; te.ensure_vis = true;
}
static void edit_delete(void) {
    if (te.cur >= te.len) return;
    int e = te.cur + 1;
    while (e < te.len && (te.buf[e] & 0xC0) == 0x80) e++;
    memmove(te.buf + te.cur, te.buf + e, (size_t)(te.len - e + 1));
    te.len -= e - te.cur; te.ensure_vis = true;
}
static void edit_left(void) { if (te.cur > 0) { te.cur--; while (te.cur > 0 && (te.buf[te.cur] & 0xC0) == 0x80) te.cur--; } te.ensure_vis = true; }
static void edit_right(void) { if (te.cur < te.len) { te.cur++; while (te.cur < te.len && (te.buf[te.cur] & 0xC0) == 0x80) te.cur++; } te.ensure_vis = true; }
static int line_start(int i) { while (i > 0 && te.buf[i - 1] != '\n') i--; return i; }
static int line_end(int i) { while (i < te.len && te.buf[i] != '\n') i++; return i; }
static void edit_vert(int dir) {
    int ls = line_start(te.cur), col = te.cur - ls;
    if (dir < 0) { if (ls == 0) return; int ps = line_start(ls - 1); int pl = ls - 1 - ps; te.cur = ps + (col < pl ? col : pl); }
    else { int le = line_end(te.cur); if (le >= te.len) return; int ns = le + 1, ne = line_end(ns); te.cur = ns + (col < ne - ns ? col : ne - ns); }
    te.ensure_vis = true;
}
static void edit_poll_keys(void) {
    if (sim.on) return;
    int c;
    while ((c = GetCharPressed()) > 0) {
        if (te.numeric && !strchr("0123456789.-+", c)) continue;
        int n = 0; const char *u = CodepointToUTF8(c, &n);
        char b[8] = { 0 }; memcpy(b, u, (size_t)n); edit_insert(b);
    }
    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) edit_backspace();
    if (IsKeyPressed(KEY_DELETE) || IsKeyPressedRepeat(KEY_DELETE)) edit_delete();
    if (IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT)) edit_left();
    if (IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT)) edit_right();
    if (IsKeyPressed(KEY_HOME)) te.cur = te.multi ? line_start(te.cur) : 0;
    if (IsKeyPressed(KEY_END)) te.cur = te.multi ? line_end(te.cur) : te.len;
    if (te.multi) {
        if (IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP)) edit_vert(-1);
        if (IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN)) edit_vert(1);
        if (IsKeyPressed(KEY_TAB)) edit_insert("  ");
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressedRepeat(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) edit_insert("\n");
    } else if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) edit_commit();
    if (!te.multi && IsKeyPressed(KEY_ESCAPE)) edit_cancel();
    if (key_ctrl && IsKeyPressed(KEY_V)) {
        const char *cb = GetClipboardText();
        if (cb) { char tmp[2048]; snprintf(tmp, sizeof tmp, "%s", cb); if (!te.multi) for (char *q = tmp; *q; q++) if (*q == '\n' || *q == '\r') *q = ' '; edit_insert(tmp); }
    }
}

/* ---- on-screen keyboard ---- */
enum { KA_INS, KA_SHIFT, KA_BKSP, KA_PAGE, KA_ENTER, KA_LEFT, KA_RIGHT, KA_CLEAR, KA_CANCEL, KA_SPACE };
typedef struct { const char *l; int act; float w; } Key;
static float kb_h(void) { return portrait ? D(46) : D(36); }
static float osk_height(void) { return (te.active && touch_mode) ? (te.numeric ? 4 : 5) * kb_h() : 0; }
static double osk_rep_t; static int osk_rep_act = -1;
static void osk_do(int act, const char *ins) {
    switch (act) {
    case KA_INS: {
        char b[8]; snprintf(b, sizeof b, "%s", ins);
        if (te.shift && b[0] >= 'a' && b[0] <= 'z') { b[0] -= 32; te.shift = false; }
        edit_insert(b); break; }
    case KA_SPACE: edit_insert(" "); break;
    case KA_SHIFT: te.shift = !te.shift; break;
    case KA_BKSP: edit_backspace(); break;
    case KA_PAGE: te.page = te.page == 0 ? 1 : 0; break;
    case KA_ENTER: if (te.multi) edit_insert("\n"); else edit_commit(); break;
    case KA_LEFT: edit_left(); break;
    case KA_RIGHT: edit_right(); break;
    case KA_CLEAR: te.buf[0] = 0; te.len = te.cur = 0; break;
    case KA_CANCEL: edit_cancel(); break;
    }
}
static void osk_row(int row, int nrows, const Key *keys, int n) {
    float kh = kb_h(), y = SH - (nrows - row) * kh, tot = 0, m = D(2.5f);
    for (int i = 0; i < n; i++) tot += keys[i].w;
    float unit = SW / tot, x = 0;
    for (int i = 0; i < n; i++) {
        Rectangle r = { x + m, y + m, keys[i].w * unit - 2 * m, kh - 2 * m };
        x += keys[i].w * unit;
        bool hit = m_down && in_rect(r, mp_press) && in_rect(r, mp);
        bool special = keys[i].act != KA_INS && keys[i].act != KA_SPACE;
        char lab[8]; snprintf(lab, sizeof lab, "%s", keys[i].l);
        if (keys[i].act == KA_INS && te.shift && lab[0] >= 'a' && lab[0] <= 'z') lab[0] -= 32;
        bool shon = keys[i].act == KA_SHIFT && te.shift;
        Color bg = hit ? C_ACC : shon ? C_ACC2 : (keys[i].act == KA_ENTER ? mix_c(C_ACC, C_SURF2, 0.15f) : special ? C_SURF3 : C_SURF2);
        rrect(r, D(7), bg);
        dtext_fit(FB, lab, r, T_BODY * 1.1f, hit || shon || keys[i].act == KA_ENTER ? C_INK : C_TEXT, 1);
        if (m_pressed && in_rect(r, mp)) { osk_do(keys[i].act, keys[i].l); osk_rep_act = keys[i].act; osk_rep_t = GetTime() + 0.45; }
        else if (m_down && hit && osk_rep_act == keys[i].act && (keys[i].act == KA_BKSP || keys[i].act == KA_LEFT || keys[i].act == KA_RIGHT) && GetTime() > osk_rep_t) { osk_do(keys[i].act, keys[i].l); osk_rep_t = GetTime() + 0.05; }
    }
}
static void draw_osk(void) {
    if (!(te.active && touch_mode)) return;
    int nrows = te.numeric ? 4 : 5;
    float kh = kb_h();
    DrawRectangle(0, (int)(SH - nrows * kh), SW, (int)(nrows * kh), (Color){ 8, 8, 12, 255 });
    static const char *CH[2][4] = {
        { "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm" },
        { "1234567890", "()[]{}<>=+", "-*/%^#_~&|", "\"'.,:;!?\\@" } };
    if (te.numeric) {
        Key r0[] = { { "1", 0, 1 }, { "2", 0, 1 }, { "3", 0, 1 }, { "<", KA_BKSP, 1 } };
        Key r1[] = { { "4", 0, 1 }, { "5", 0, 1 }, { "6", 0, 1 }, { "C", KA_CLEAR, 1 } };
        Key r2[] = { { "7", 0, 1 }, { "8", 0, 1 }, { "9", 0, 1 }, { "x", KA_CANCEL, 1 } };
        Key r3[] = { { ".", 0, 1 }, { "0", 0, 1 }, { "-", 0, 1 }, { "OK", KA_ENTER, 1 } };
        osk_row(0, 4, r0, 4); osk_row(1, 4, r1, 4); osk_row(2, 4, r2, 4); osk_row(3, 4, r3, 4);
        return;
    }
    int pg = te.page ? 1 : 0;
    for (int row = 0; row < 4; row++) {
        Key ks[12]; int n = 0;
        static char lab[4][12][2];
        const char *s = CH[pg][row];
        if (pg == 0 && row == 3) ks[n++] = (Key){ "^", KA_SHIFT, 1.5f };
        for (int i = 0; s[i]; i++) { lab[row][i][0] = s[i]; lab[row][i][1] = 0; ks[n++] = (Key){ lab[row][i], KA_INS, 1 }; }
        if (pg == 0 && row == 3) ks[n++] = (Key){ "<", KA_BKSP, 1.5f };
        osk_row(row, 5, ks, n);
    }
    Key last[] = { { te.page ? "abc" : "?123", KA_PAGE, 1.5f }, { "<-", KA_LEFT, 1 }, { "space", KA_SPACE, 3.5f }, { "->", KA_RIGHT, 1 },
                   { te.page ? "<" : ".", te.page ? KA_BKSP : KA_INS, 1 }, { te.multi ? "enter" : "OK", KA_ENTER, 2 } };
    osk_row(4, 5, last, 6);
}

static Rectangle bar_rect(void) { float bh = D(92); return (Rectangle){ 0, SH - osk_height() - bh, (float)SW, bh }; }
static int caret_from_x(Font f, const char *s, int len, float x, float sz) {
    int i = 0;
    while (i < len) {
        int j = i + 1; while (j < len && (s[j] & 0xC0) == 0x80) j++;
        char *sub = malloc((size_t)j + 1); memcpy(sub, s, (size_t)j); sub[j] = 0;
        float w = tw(f, sub, sz); free(sub);
        char *sub0 = malloc((size_t)i + 1); memcpy(sub0, s, (size_t)i); sub0[i] = 0;
        float w0 = tw(f, sub0, sz); free(sub0);
        if (x < (w0 + w) / 2) return i;
        i = j;
    }
    return len;
}
static void draw_edit_bar(void) {
    if (!te.active || te.multi) return;
    Rectangle b = bar_rect();
    DrawRectangleRec(b, C_SURF2); DrawRectangle((int)b.x, (int)b.y, SW, (int)fmaxf(2, D(1.5f)), C_ACC);
    dtext(F, te.title, D(16), b.y + D(8), T_SMALL, C_DIM);
    float bs = D(48), g = D(8);
    Rectangle fld = { D(12), b.y + D(30), SW - D(24) - 2 * (bs + g), bs };
    Rectangle bok = { fld.x + fld.width + g, fld.y, bs, bs }, bno = { bok.x + bs + g, fld.y, bs, bs };
    rrect(fld, D(10), C_FIELD);
    float tx = fld.x + D(12), avail = fld.width - D(24);
    char pre[1024]; snprintf(pre, sizeof pre, "%.*s", te.cur < 1000 ? te.cur : 1000, te.buf);
    float cx = tw(F, pre, T_TITLE), off = cx > avail - 4 ? cx - avail + 4 : 0;
    BeginScissorMode((int)fld.x + 2, (int)fld.y, (int)fld.width - 4, (int)fld.height);
    dtext(F, te.buf, tx - off, fld.y + (fld.height - T_TITLE) / 2, T_TITLE, C_TEXT);
    if (((int)(GetTime() * 2)) % 2 == 0) DrawRectangle((int)(tx - off + cx), (int)(fld.y + D(10)), (int)fmaxf(2, D(1.5f)), (int)(fld.height - D(20)), C_ACC);
    EndScissorMode();
    rrect(bok, D(12), C_GREEN); icon_c(IC_CHECK, bok, D(24), C_INK);
    rrect(bno, D(12), C_SURF3); icon_c(IC_CLOSE, bno, D(22), C_TEXT);
    dbg_reg("edit-ok", bok);
    if (m_released && !dragged) {
        if (in_rect(bok, mp) && in_rect(bok, mp_press)) edit_commit();
        else if (in_rect(bno, mp) && in_rect(bno, mp_press)) edit_cancel();
        else if (in_rect(fld, mp) && in_rect(fld, mp_press)) te.cur = caret_from_x(F, te.buf, te.len, mp.x - tx + off, T_TITLE);
    }
}

/* ============================== widgets ============================== */
static bool pill(Rectangle r, const char *label, bool on, Color acc) {
    dbg_reg(label, r);
    bool h = held(r);
    rrect(r, r.height / 2, on ? acc : (h ? C_SURF3 : C_SURF2));
    dtext_fit(FB, label, inset_xy(r, D(10), 0), T_BODY, on ? C_INK : C_TEXT, 1);
    return tap(r);
}
static bool chip(Rectangle r, const char *label, bool on) {
    dbg_reg(label, r);
    bool h = held(r);
    rrect(r, D(10), on ? alpha_c(C_ACC, 0.18f) : (h ? C_SURF3 : C_SURF2));
    if (on) rrect_line(r, D(10), fmaxf(1.5f, D(1.5f)), C_ACC);
    dtext_fit(on ? FB : F, label, inset_xy(r, D(6), 0), T_SMALL * 1.05f, on ? C_ACC : C_TEXT, 1);
    return tap(r);
}
static bool icon_btn(Rectangle r, int ic, float isz, Color col, const char *dbg) {
    if (dbg) dbg_reg(dbg, r);
    bool h = held(r);
    if (h) DrawCircleV((Vector2){ r.x + r.width / 2, r.y + r.height / 2 }, fminf(r.width, r.height) / 2, alpha_c(C_TEXT, 0.12f));
    icon_c(ic, r, isz, col);
    return tap(r);
}
static bool toggle(Rectangle r, bool on) {
    float w = D(46), h = D(26);
    Rectangle t = { r.x + r.width - w, r.y + (r.height - h) / 2, w, h };
    rrect(t, h / 2, on ? C_ACC : C_SURF3);
    DrawCircleV((Vector2){ on ? t.x + w - h / 2 : t.x + h / 2, t.y + h / 2 }, h / 2 - D(3), on ? C_INK : C_TEXT);
    return tap(r);
}
static bool slider(unsigned id, Rectangle r, float *v, float mn, float mx, Color fill) {
    if (mx - mn < 1e-6f) mx = mn + 1e-6f;
    float u = clampf((*v - mn) / (mx - mn), 0, 1), th = D(4), cy = r.y + r.height / 2, pad = D(10);
    Rectangle tr = { r.x + pad, cy - th / 2, r.width - 2 * pad, th };
    rrect(tr, th / 2, C_SURF3);
    rrect((Rectangle){ tr.x, tr.y, fmaxf(th, tr.width * u), th }, th / 2, fill);
    bool on = active == id;
    DrawCircleV((Vector2){ tr.x + u * tr.width, cy }, on ? D(11) : D(9), alpha_c(BLACK, 0.35f));
    DrawCircleV((Vector2){ tr.x + u * tr.width, cy - 0.5f }, on ? D(10) : D(8.5f), C_TEXT);
    static float v0; static unsigned v0_id;
    bool claim = !blocked && m_down && axis == 1 && in_rect(r, mp_press) && in_rect(clip_r, mp_press) && (active == 0 || active == id);
    if (claim) {
        if (v0_id != id || active != id) { v0 = *v; v0_id = id; }
        active = id;
        *v = clampf(v0 + (mp.x - mp_press.x) / tr.width * (mx - mn), mn, mx);
        return true;
    }
    if (tap(r)) { *v = mn + clampf((mp.x - tr.x) / tr.width, 0, 1) * (mx - mn); return true; }
    return false;
}

/* scrolling body with inertia */
typedef struct { float y, h, vel; } Scroll;
enum { SC_ADD, SC_TRANSFORM, SC_STYLE, SC_FX, SC_ANIM, SC_LAYERS, SC_CANVAS, SC_SCRIPT, SC_EXPORT, SC_TL, SC_PROJ, SC_MEDIA, SC_FUSION, SC_COUNT };
static Scroll scr[SC_COUNT];
static float scroll_begin(Rectangle body, Scroll *s) {
    float maxs = fmaxf(0, s->h - body.height);
    if (!blocked && axis == 2 && m_down && active == 0 && in_rect(body, mp_press) && in_rect(clip_r, mp_press)) { s->y -= m_dy; s->vel = -vel_y; }
    else if (!m_down || axis != 2) { s->y += s->vel; s->vel *= 0.93f; if (fabsf(s->vel) < 0.3f) s->vel = 0; }
    if (!blocked && in_rect(body, mp) && wheel != 0) s->y -= wheel * D(60);
    if (s->y < 0 || s->y > maxs) s->vel = 0;
    s->y = clampf(s->y, 0, maxs);
    BeginScissorMode((int)body.x, (int)body.y, (int)body.width, (int)body.height);
    clip_r = body;
    return body.y - s->y;
}
static void scroll_end(Rectangle body, Scroll *s, float y_end) {
    s->h = y_end - (body.y - s->y) + D(24);
    EndScissorMode();
    clip_r = (Rectangle){ 0, 0, (float)SW, (float)SH };
    if (s->h > body.height) {
        float th = fmaxf(D(24), body.height * body.height / s->h), ty = body.y + (body.height - th) * (s->y / (s->h - body.height));
        rrect((Rectangle){ body.x + body.width - D(4), ty, D(3), th }, D(1.5f), (Color){ 255, 255, 255, 50 });
    }
}
