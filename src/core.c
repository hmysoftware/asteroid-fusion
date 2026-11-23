#define _GNU_SOURCE
// Asteroid Fusion core: project model, keyframes, expressions, clips
#include "afx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include "lua.h"
#include "lauxlib.h"

const char *EASE_NAMES[EASE_COUNT] = { "linear", "in", "out", "inout", "hold" };
const char *PROP_NAMES[P_COUNT] = { "x", "y", "sx", "sy", "rot", "opacity", "ax", "ay", "w", "h", "a", "b", "c" };
const char *LAYER_TYPE_NAMES[L_TYPE_COUNT] = { "solid", "shape", "text", "svg", "image", "asteroid", "stars", "adjust", "video" };
const char *SHAPE_NAMES[SH_COUNT] = { "rect", "ellipse", "polygon", "star" };
const char *BLEND_NAMES[BM_COUNT] = { "normal", "add", "multiply", "screen" };

const FxDef FX_DEFS[FX_COUNT] = {
    { "blur",       1, { "radius" },                         { 4 },              { 0 },          { 200 },       { 0.2f } },
    { "glow",       3, { "radius", "amount", "threshold" },  { 10, 0.8f, 0.55f },{ 0, 0, 0 },    { 200, 5, 1 }, { 0.2f, 0.01f, 0.01f } },
    { "brightness", 3, { "brightness", "contrast", "saturation" }, { 0, 1, 1 },  { -1, 0, 0 },   { 1, 4, 4 },   { 0.01f, 0.01f, 0.01f } },
    { "tint",       1, { "amount" },                         { 1 },              { 0 },          { 1 },         { 0.01f } },
    { "hue",        1, { "shift" },                          { 0 },              { -360 },       { 360 },       { 1 } },
    { "invert",     1, { "amount" },                         { 1 },              { 0 },          { 1 },         { 0.01f } },
    { "noise",      2, { "amount", "size" },                 { 0.15f, 1 },       { 0, 1 },       { 1, 16 },     { 0.005f, 0.1f } },
    { "pixelate",   1, { "size" },                           { 8 },              { 1 },          { 128 },       { 0.2f } },
    { "rgbsplit",   2, { "amount", "angle" },                { 4, 0 },           { -100, -360 }, { 100, 360 },  { 0.1f, 1 } },
    { "vignette",   2, { "amount", "softness" },             { 0.6f, 0.5f },     { 0, 0.01f },   { 1, 1 },      { 0.01f, 0.01f } },
    { "scanlines",  3, { "amount", "spacing", "offset" },    { 0.3f, 3, 0 },     { 0, 2, -100 }, { 1, 40, 100 },{ 0.01f, 0.1f, 0.1f } },
    { "wave",       3, { "amount", "frequency", "phase" },   { 10, 0.02f, 0 },   { -200, 0.001f, -3600 }, { 200, 0.5f, 3600 }, { 0.2f, 0.001f, 1 } },
    { "posterize",  1, { "levels" },                         { 4 },              { 2 },          { 32 },        { 0.1f } },
};

/* ---------- log ---------- */
LogFn afx_log_fn = NULL;
void afx_log(int level, const char *fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    if (afx_log_fn) afx_log_fn(level, buf);
    else fprintf(level ? stderr : stdout, "%s%s\n", level ? "error: " : "", buf);
}

/* ---------- buffers ---------- */
FBuf fbuf_make(int w, int h) { FBuf b = { w, h, calloc((size_t)w * h * 4, sizeof(float)) }; return b; }
void fbuf_free(FBuf *b) { free(b->p); b->p = NULL; b->w = b->h = 0; }
Bmp *bmp_new(int w, int h) {
    Bmp *b = calloc(1, sizeof *b);
    b->w = w; b->h = h; b->unit = 1.0f;
    b->px = calloc((size_t)w * h * 4, sizeof(float));
    return b;
}
void bmp_free(Bmp *b) { if (b) { free(b->px); free(b); } }
void layer_free_cache(Layer *l) { bmp_free(l->cache); l->cache = NULL; l->cache_key[0] = 0; }

/* ---------- easing / props ---------- */
float ease_apply(int e, float u) {
    switch (e) {
    case EASE_IN:    return u * u;
    case EASE_OUT:   return 1 - (1 - u) * (1 - u);
    case EASE_INOUT: return u < 0.5f ? 2 * u * u : 1 - 2 * (1 - u) * (1 - u);
    case EASE_HOLD:  return 0;
    default:         return u;
    }
}

void prop_init(Prop *p, float v) { memset(p, 0, sizeof *p); p->base = v; p->ref = EXPR_NONE; }

void script_free_ref(Project *P, int ref) { if (P->L && ref >= 0) luaL_unref(P->L, LUA_REGISTRYINDEX, ref); }

void prop_free(Project *P, Prop *p) {
    free(p->expr); p->expr = NULL;
    script_free_ref(P, p->ref);
    p->ref = EXPR_NONE; p->fn = false;
}

void prop_set_expr(Project *P, Prop *p, const char *src) {
    prop_free(P, p);
    if (src && *src) p->expr = strdup(src);
    P->rev++;
}

float prop_keyed(const Prop *p, float t) {
    int n = p->nkeys;
    if (n == 0) return p->base;
    if (t <= p->keys[0].t) return p->keys[0].v;
    if (t >= p->keys[n - 1].t) return p->keys[n - 1].v;
    int i = 0;
    while (i < n - 2 && t >= p->keys[i + 1].t) i++;
    const AKey *a = &p->keys[i], *b = &p->keys[i + 1];
    if (a->ease == EASE_HOLD) return a->v;
    float u = (t - a->t) / (b->t - a->t);
    return a->v + (b->v - a->v) * ease_apply(a->ease, u);
}

int prop_key_at(const Prop *p, float t, float eps) {
    for (int i = 0; i < p->nkeys; i++) if (fabsf(p->keys[i].t - t) <= eps) return i;
    return -1;
}

void prop_set_key(Prop *p, float t, float v, int ease) {
    int i = prop_key_at(p, t, 1e-4f);
    if (i >= 0) { p->keys[i].v = v; if (ease >= 0) p->keys[i].ease = (uint8_t)ease; return; }
    if (p->nkeys >= MAX_KEYS) { afx_log(1, "too many keyframes (max %d)", MAX_KEYS); return; }
    int pos = 0;
    while (pos < p->nkeys && p->keys[pos].t < t) pos++;
    memmove(&p->keys[pos + 1], &p->keys[pos], (size_t)(p->nkeys - pos) * sizeof(AKey));
    p->keys[pos].t = t; p->keys[pos].v = v; p->keys[pos].ease = (uint8_t)(ease >= 0 ? ease : EASE_LINEAR);
    p->nkeys++;
}

void prop_del_key(Prop *p, int i) {
    if (i < 0 || i >= p->nkeys) return;
    memmove(&p->keys[i], &p->keys[i + 1], (size_t)(p->nkeys - i - 1) * sizeof(AKey));
    p->nkeys--;
}

/* ---------- expressions ---------- */
static bool compile_expr(Project *P, Prop *p) {
    lua_State *L = P->L;
    size_t n = strlen(p->expr);
    char *code = malloc(n + 160);
    const char *hdr = "local t,lt,frame,v,i,fps,w,h,pid = ...\n";
    bool raw = strncmp(p->expr, "return", 6) == 0 || strchr(p->expr, '\n');
    snprintf(code, n + 160, raw ? "%s%s" : "%sreturn (%s)", hdr, p->expr);
    int rc = luaL_loadstring(L, code);
    free(code);
    if (rc != LUA_OK) {
        afx_log(1, "expression error: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        p->ref = EXPR_BAD;
        return false;
    }
    p->ref = luaL_ref(L, LUA_REGISTRYINDEX);
    return true;
}

float prop_eval(Project *P, Layer *l, Prop *p, int pid, float t) {
    float v = prop_keyed(p, t);
    if (!p->expr && !p->fn) return v;
    if (p->ref == EXPR_BAD) return v;
    if (p->ref == EXPR_NONE && p->expr && !compile_expr(P, p)) return v;
    if (p->ref < 0 || !P->L) return v;
    lua_State *L = P->L;
    int li = (l >= P->layers && l < P->layers + MAX_LAYERS) ? (int)(l - P->layers) : (l->uid & 1023) + 1000;
    lua_pushinteger(L, li * 131 + pid * 7 + 1);
    lua_setglobal(L, "_seed");
    lua_rawgeti(L, LUA_REGISTRYINDEX, p->ref);
    lua_pushnumber(L, t);
    lua_pushnumber(L, t - l->tin);
    lua_pushinteger(L, (lua_Integer)floorf(t * P->fps + 0.5f));
    lua_pushnumber(L, v);
    lua_pushinteger(L, li + 1);
    lua_pushnumber(L, P->fps);
    lua_pushinteger(L, P->width);
    lua_pushinteger(L, P->height);
    lua_pushinteger(L, pid);
    if (lua_pcall(L, 9, 1, 0) != LUA_OK) {
        afx_log(1, "expression error in '%s': %s", l->name, lua_tostring(L, -1));
        lua_pop(L, 1);
        if (!p->fn) script_free_ref(P, p->ref);
        p->ref = EXPR_BAD;
        return v;
    }
    if (lua_isnumber(L, -1)) v = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);
    return v;
}

/* ---------- aliases ---------- */
#define M(t) (1u << (t))
#define ALLT 0xFFFFu
typedef struct { const char *n; int pid; unsigned mask; } Alias;
static const Alias ALIASES[] = {
    { "x", P_X, ALLT }, { "y", P_Y, ALLT }, { "sx", P_SX, ALLT }, { "sy", P_SY, ALLT },
    { "scalex", P_SX, ALLT }, { "scaley", P_SY, ALLT }, { "rot", P_ROT, ALLT }, { "rotation", P_ROT, ALLT },
    { "opacity", P_OPA, ALLT }, { "opa", P_OPA, ALLT }, { "ax", P_AX, ALLT }, { "ay", P_AY, ALLT },
    { "w", P_W, ALLT }, { "h", P_H, ALLT }, { "a", P_A, ALLT }, { "b", P_B, ALLT }, { "c", P_C, ALLT },
    { "width", P_W, ALLT }, { "height", P_H, ALLT },
    { "corner", P_A, M(L_SHAPE) }, { "sides", P_A, M(L_SHAPE) }, { "points", P_A, M(L_SHAPE) },
    { "inner", P_B, M(L_SHAPE) }, { "stroke_width", P_C, M(L_SHAPE) }, { "strokewidth", P_C, M(L_SHAPE) },
    { "size", P_A, M(L_TEXT) },
    { "radius", P_W, M(L_ASTEROID) }, { "seed", P_A, M(L_ASTEROID) }, { "roughness", P_B, M(L_ASTEROID) },
    { "rough", P_B, M(L_ASTEROID) }, { "craters", P_C, M(L_ASTEROID) },
    { "count", P_A, M(L_STARS) }, { "speed", P_B, M(L_STARS) }, { "size", P_C, M(L_STARS) },
    { "seed", P_W, M(L_STARS) }, { "direction", P_ROT, M(L_STARS) },
};

Prop *layer_prop(Layer *l, const char *name) {
    const char *dot = strchr(name, '.');
    if (dot) {
        char fxn[32]; size_t n = (size_t)(dot - name);
        if (n == 0 || n >= sizeof fxn) return NULL;
        memcpy(fxn, name, n); fxn[n] = 0;
        int nth = 1;
        while (n > 0 && fxn[n - 1] >= '0' && fxn[n - 1] <= '9') n--;
        if (n < strlen(fxn)) { nth = atoi(fxn + n); fxn[n] = 0; }
        for (int t = 0; t < FX_COUNT; t++) {
            if (strcmp(fxn, FX_DEFS[t].name)) continue;
            int seen = 0;
            for (int i = 0; i < l->nfx; i++) {
                if (l->fx[i].type != t || ++seen != nth) continue;
                for (int k = 0; k < FX_DEFS[t].np; k++)
                    if (!strcmp(dot + 1, FX_DEFS[t].pn[k])) return &l->fx[i].p[k];
                return NULL;
            }
        }
        return NULL;
    }
    for (size_t i = 0; i < sizeof ALIASES / sizeof *ALIASES; i++)
        if ((ALIASES[i].mask & M(l->type)) && !strcmp(ALIASES[i].n, name)) return &l->p[ALIASES[i].pid];
    return NULL;
}

const char *prop_label(const Layer *l, int pid) {
    LayerType t = l->type;
    if (t == L_ADJUST) return pid == P_OPA ? "Opacity %" : NULL;
    if (t == L_STARS) {
        switch (pid) { case P_ROT: return "Direction"; case P_OPA: return "Opacity %"; case P_W: return "Seed";
        case P_A: return "Count"; case P_B: return "Speed"; case P_C: return "Size"; default: return NULL; }
    }
    switch (pid) {
    case P_X: return "Position X"; case P_Y: return "Position Y";
    case P_SX: return "Scale X %"; case P_SY: return "Scale Y %";
    case P_ROT: return "Rotation"; case P_OPA: return "Opacity %";
    case P_AX: return "Anchor X"; case P_AY: return "Anchor Y";
    }
    switch (t) {
    case L_SOLID: return pid == P_W ? "Width" : pid == P_H ? "Height" : NULL;
    case L_SHAPE:
        if (pid == P_W) return "Width";
        if (pid == P_H) return "Height";
        if (pid == P_A) return l->shape == SH_RECT ? "Corner radius" : l->shape == SH_POLYGON ? "Sides" : l->shape == SH_STAR ? "Points" : NULL;
        if (pid == P_B) return l->shape == SH_STAR ? "Inner ratio" : NULL;
        if (pid == P_C) return "Stroke width";
        return NULL;
    case L_TEXT: return pid == P_A ? "Font size" : NULL;
    case L_SVG: case L_IMAGE: return pid == P_W ? "Width (0=natural)" : NULL;
    case L_VIDEO: return pid == P_W ? "Width (0=auto fit)" : NULL;
    case L_ASTEROID:
        return pid == P_W ? "Radius" : pid == P_A ? "Seed" : pid == P_B ? "Roughness" : pid == P_C ? "Craters" : NULL;
    default: return NULL;
    }
}

/* ---------- effects ---------- */
void fx_defaults(Effect *e, int type) {
    memset(e, 0, sizeof *e);
    e->type = (uint8_t)type; e->on = true;
    for (int k = 0; k < 3; k++) prop_init(&e->p[k], k < FX_DEFS[type].np ? FX_DEFS[type].def[k] : 0);
    e->color[0] = 255; e->color[1] = 140; e->color[2] = 40; e->color[3] = 255;
}
int layer_add_fx(Project *P, Layer *l, int type) {
    if (l->nfx >= MAX_FX || type < 0 || type >= FX_COUNT) return -1;
    fx_defaults(&l->fx[l->nfx], type);
    P->rev++;
    return l->nfx++;
}
static void fx_release(Project *P, Effect *e) { for (int k = 0; k < 3; k++) prop_free(P, &e->p[k]); }
void layer_del_fx(Project *P, Layer *l, int i) {
    if (i < 0 || i >= l->nfx) return;
    fx_release(P, &l->fx[i]);
    memmove(&l->fx[i], &l->fx[i + 1], (size_t)(l->nfx - i - 1) * sizeof(Effect));
    l->nfx--;
    memset(&l->fx[l->nfx], 0, sizeof(Effect));
    P->rev++;
}

/* ---------- layers ---------- */
void layer_release_all(Project *P, Layer *l) {
    for (int i = 0; i < P_COUNT; i++) prop_free(P, &l->p[i]);
    for (int i = 0; i < l->nfx; i++) fx_release(P, &l->fx[i]);
    layer_free_cache(l);
    if (l->g) { graph_free(P, l->g); l->g = NULL; }
}
#define layer_release layer_release_all

void layer_init(Project *P, Layer *l, LayerType t) {
    memset(l, 0, sizeof *l);
    l->uid = P->next_uid++;
    l->type = t; l->visible = true; l->tin = 0; l->tout = -1;
    l->speed = 1; l->vol = 1; l->fitm = 0;
    float W = (float)P->width, H = (float)P->height;
    float d[P_COUNT] = { W / 2, H / 2, 100, 100, 0, 100, 0, 0, 0, 0, 0, 0, 0 };
    switch (t) {
    case L_SOLID: d[P_W] = W; d[P_H] = H; break;
    case L_SHAPE: d[P_W] = 200; d[P_H] = 200; break;
    case L_TEXT: d[P_A] = 48; break;
    case L_ASTEROID: d[P_W] = 60; d[P_A] = 1; d[P_B] = 0.35f; d[P_C] = 0.6f; break;
    case L_STARS: d[P_A] = 150; d[P_B] = 40; d[P_C] = 1.6f; d[P_W] = 1; d[P_ROT] = 180; break;
    default: break;
    }
    for (int i = 0; i < P_COUNT; i++) prop_init(&l->p[i], d[i]);
    static const uint8_t fills[L_TYPE_COUNT][4] = {
        { 24, 28, 52, 255 }, { 255, 180, 60, 255 }, { 255, 255, 255, 255 }, { 255, 255, 255, 255 },
        { 255, 255, 255, 255 }, { 150, 135, 120, 255 }, { 255, 255, 255, 255 }, { 255, 255, 255, 255 }, { 255, 255, 255, 255 } };
    memcpy(l->fill, fills[t], 4);
    l->stroke[0] = l->stroke[1] = l->stroke[2] = l->stroke[3] = 255;
    l->shape = SH_RECT;
    if (t == L_TEXT) snprintf(l->text, sizeof l->text, "Asteroid Fusion");
    snprintf(l->name, sizeof l->name, "%s %d", LAYER_TYPE_NAMES[t], P->nlayers + 1);
}

Layer *project_add(Project *P, LayerType t, const char *name) {
    if (P->nlayers >= MAX_LAYERS) { afx_log(1, "layer limit reached"); return NULL; }
    Layer *l = &P->layers[P->nlayers];
    layer_init(P, l, t);
    if (name && *name) snprintf(l->name, sizeof l->name, "%s", name);
    l->scripted = P->in_user;
    P->nlayers++; P->rev++;
    return l;
}

Layer *project_find_uid(Project *P, int uid) {
    for (int i = 0; i < P->nlayers; i++) if (P->layers[i].uid == uid) return &P->layers[i];
    return NULL;
}
int project_index_of(Project *P, const Layer *l) { return (int)(l - P->layers); }

void project_remove(Project *P, int idx) {
    if (idx < 0 || idx >= P->nlayers) return;
    layer_release(P, &P->layers[idx]);
    memmove(&P->layers[idx], &P->layers[idx + 1], (size_t)(P->nlayers - idx - 1) * sizeof(Layer));
    P->nlayers--;
    memset(&P->layers[P->nlayers], 0, sizeof(Layer));
    P->rev++;
}

int project_duplicate(Project *P, int idx) {
    if (idx < 0 || idx >= P->nlayers || P->nlayers >= MAX_LAYERS) return -1;
    memmove(&P->layers[idx + 2], &P->layers[idx + 1], (size_t)(P->nlayers - idx - 1) * sizeof(Layer));
    Layer *n = &P->layers[idx + 1], *s = &P->layers[idx];
    *n = *s;
    n->uid = P->next_uid++;
    n->cache = NULL; n->cache_key[0] = 0;
    n->g = s->g ? graph_clone(P, s->g) : NULL;
    n->scripted = false;
    snprintf(n->name, sizeof n->name, "%.40s copy", s->name);
    Prop *ps[P_COUNT + MAX_FX * 3]; Prop *pd[P_COUNT + MAX_FX * 3]; int c = 0;
    for (int i = 0; i < P_COUNT; i++) { ps[c] = &s->p[i]; pd[c++] = &n->p[i]; }
    for (int f = 0; f < s->nfx; f++) for (int k = 0; k < 3; k++) { ps[c] = &s->fx[f].p[k]; pd[c++] = &n->fx[f].p[k]; }
    for (int i = 0; i < c; i++) {
        pd[i]->expr = ps[i]->expr ? strdup(ps[i]->expr) : NULL;
        pd[i]->ref = EXPR_NONE;
        if (ps[i]->fn) { pd[i]->fn = false; P->lossy++; }
    }
    P->nlayers++; P->rev++;
    return idx + 1;
}

int project_move(Project *P, int idx, int delta) {
    int j = idx + delta;
    if (idx < 0 || idx >= P->nlayers || j < 0 || j >= P->nlayers) return idx;
    Layer tmp = P->layers[idx]; P->layers[idx] = P->layers[j]; P->layers[j] = tmp;
    P->rev++;
    return j;
}

/* ---------- project ---------- */
int project_nframes(const Project *P) { int n = (int)floorf(P->duration * P->fps + 0.5f); return n < 1 ? 1 : n; }
float project_layer_out(const Project *P, const Layer *l) { return l->tout < 0 ? P->duration : l->tout; }

static void project_defaults(Project *P) {
    snprintf(P->name, sizeof P->name, "Untitled");
    P->width = 540; P->height = 960; P->fps = 30; P->duration = 5;
    P->bg[0] = 8; P->bg[1] = 10; P->bg[2] = 22; P->bg[3] = 255;
}

void project_reset(Project *P) {
    for (int i = 0; i < P->nlayers; i++) layer_release(P, &P->layers[i]);
    memset(P->layers, 0, sizeof(Layer) * MAX_LAYERS);
    P->nlayers = 0; P->next_uid = 1; P->nhooks = 0; P->lossy = 0; P->in_user = false;
    free(P->user_code); P->user_code = NULL;
    if (P->L) { lua_close(P->L); P->L = NULL; }
    project_defaults(P);
    P->L = script_new_state(P);
    P->rev++;
}

Project *project_new(void) {
    Project *P = calloc(1, sizeof *P);
    P->layers = calloc(MAX_LAYERS, sizeof(Layer));
    project_reset(P);
    return P;
}

void project_free(Project *P) {
    if (!P) return;
    for (int i = 0; i < P->nlayers; i++) layer_release(P, &P->layers[i]);
    if (P->L) lua_close(P->L);
    free(P->layers); free(P->user_code);
    fbuf_free(&P->lb); fbuf_free(&P->t1); fbuf_free(&P->t2);
    free(P);
}
