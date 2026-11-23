#define _GNU_SOURCE
// AFX scripting: Lua bindings, project (de)serialization
#include "afx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#define LAYER_MT "AFX.Layer"
#define USER_MARK "--@user"

/* ---------- helpers ---------- */
static Project *getP(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "afx_project");
    Project *P = lua_touserdata(L, -1);
    lua_pop(L, 1);
    return P;
}
static void push_layer(lua_State *L, Layer *l) {
    int *u = lua_newuserdatauv(L, sizeof(int), 0);
    *u = l->uid;
    luaL_setmetatable(L, LAYER_MT);
}
static Layer *check_layer(lua_State *L, int i) {
    int *u = luaL_checkudata(L, i, LAYER_MT);
    Layer *l = project_find_uid(getP(L), *u);
    if (!l) luaL_error(L, "layer no longer exists");
    return l;
}
static int find_name(const char *const *names, int n, const char *s) {
    for (int i = 0; i < n; i++) if (!strcmp(names[i], s)) return i;
    return -1;
}
static void copy_str(char *dst, size_t cap, const char *s) { snprintf(dst, cap, "%s", s); }

static bool parse_hex(const char *s, uint8_t out[4]) {
    if (*s == '#') s++;
    size_t n = strlen(s);
    if (n != 6 && n != 8) return false;
    unsigned v[4] = { 0, 0, 0, 255 };
    for (size_t i = 0; i < n / 2; i++) { char b[3] = { s[i * 2], s[i * 2 + 1], 0 }; char *e; v[i] = (unsigned)strtoul(b, &e, 16); if (*e) return false; }
    for (int i = 0; i < 4; i++) out[i] = (uint8_t)v[i];
    return true;
}
static bool get_color(lua_State *L, int idx, uint8_t out[4]) {
    idx = lua_absindex(L, idx);
    if (lua_type(L, idx) == LUA_TSTRING) return parse_hex(lua_tostring(L, idx), out);
    if (lua_type(L, idx) != LUA_TTABLE) return false;
    uint8_t c[4] = { 0, 0, 0, 255 };
    for (int i = 1; i <= 4; i++) {
        lua_rawgeti(L, idx, i);
        if (lua_isnumber(L, -1)) { double v = lua_tonumber(L, -1); c[i - 1] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5); }
        else if (i < 4) { lua_pop(L, 1); return false; }
        lua_pop(L, 1);
    }
    memcpy(out, c, 4);
    return true;
}
static void push_color(lua_State *L, const uint8_t c[4]) {
    lua_createtable(L, 4, 0);
    for (int i = 0; i < 4; i++) { lua_pushinteger(L, c[i]); lua_rawseti(L, -2, i + 1); }
}
static int get_ease(lua_State *L, int idx) {
    if (lua_isnoneornil(L, idx)) return -1;
    if (lua_type(L, idx) == LUA_TNUMBER) return (int)lua_tointeger(L, idx);
    const char *s = luaL_checkstring(L, idx);
    int e = find_name(EASE_NAMES, EASE_COUNT, s);
    if (e < 0) luaL_error(L, "unknown ease '%s' (linear, in, out, inout, hold)", s);
    return e;
}

/* ---------- expression / prop assignment ---------- */
static void set_fn_expr(lua_State *L, Project *P, Prop *p, int idx) {
    prop_free(P, p);
    lua_pushvalue(L, idx);
    p->ref = luaL_ref(L, LUA_REGISTRYINDEX);
    p->fn = true;
}
static void set_prop_lua(lua_State *L, Project *P, Prop *p, int idx) {
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
    case LUA_TNUMBER: p->base = (float)lua_tonumber(L, idx); break;
    case LUA_TSTRING: prop_set_expr(P, p, lua_tostring(L, idx)); break;
    case LUA_TFUNCTION: set_fn_expr(L, P, p, idx); break;
    case LUA_TTABLE: {
        lua_getfield(L, idx, "base");
        if (!lua_isnumber(L, -1)) { lua_pop(L, 1); lua_getfield(L, idx, "v"); }
        if (lua_isnumber(L, -1)) p->base = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, idx, "keys");
        if (lua_istable(L, -1)) {
            p->nkeys = 0;
            int n = (int)luaL_len(L, -1);
            for (int i = 1; i <= n && i <= MAX_KEYS; i++) {
                lua_rawgeti(L, -1, i);
                lua_rawgeti(L, -1, 1); lua_rawgeti(L, -2, 2); lua_rawgeti(L, -3, 3);
                if (lua_isnumber(L, -3) && lua_isnumber(L, -2))
                    prop_set_key(p, (float)lua_tonumber(L, -3), (float)lua_tonumber(L, -2), get_ease(L, -1) < 0 ? 0 : get_ease(L, -1));
                lua_pop(L, 4);
            }
        }
        lua_pop(L, 1);
        lua_getfield(L, idx, "expr");
        if (lua_type(L, -1) == LUA_TSTRING) prop_set_expr(P, p, lua_tostring(L, -1));
        else if (lua_type(L, -1) == LUA_TFUNCTION) set_fn_expr(L, P, p, -1);
        lua_pop(L, 1);
        break;
    }
    case LUA_TBOOLEAN: case LUA_TNIL: prop_free(P, p); break;
    default: luaL_error(L, "bad property value (%s)", luaL_typename(L, idx));
    }
}

static void shape_defaults(Layer *l) {
    l->p[P_A].base = (l->shape == SH_POLYGON || l->shape == SH_STAR) ? 5 : 0;
    l->p[P_B].base = 0.45f;
}

/* effects from lua: name + params table */
static int fx_params_lua(lua_State *L, Project *P, Layer *l, int fi, int tidx) {
    tidx = lua_absindex(L, tidx);
    Effect *e = &l->fx[fi];
    const FxDef *d = &FX_DEFS[e->type];
    lua_pushnil(L);
    while (lua_next(L, tidx)) {
        if (lua_type(L, -2) == LUA_TSTRING) {
            const char *k = lua_tostring(L, -2);
            if (!strcmp(k, "on")) e->on = lua_toboolean(L, -1);
            else if (!strcmp(k, "color")) { if (!get_color(L, -1, e->color)) luaL_error(L, "bad color"); }
            else {
                int pi = -1;
                for (int i = 0; i < d->np; i++) if (!strcmp(k, d->pn[i])) pi = i;
                if (pi < 0) luaL_error(L, "effect '%s' has no parameter '%s'", d->name, k);
                set_prop_lua(L, P, &e->p[pi], -1);
            }
        }
        lua_pop(L, 1);
    }
    return 0;
}
static int add_fx_lua(lua_State *L, Project *P, Layer *l, const char *name, int tidx) {
    int t = -1;
    for (int i = 0; i < FX_COUNT; i++) if (!strcmp(FX_DEFS[i].name, name)) t = i;
    if (t < 0) return luaL_error(L, "unknown effect '%s'", name);
    int fi = layer_add_fx(P, l, t);
    if (fi < 0) return luaL_error(L, "effect limit reached (%d)", MAX_FX);
    if (tidx && lua_istable(L, tidx)) fx_params_lua(L, P, l, fi, tidx);
    return fi;
}

static int graph_from_lua(lua_State *L, Project *P, Layer *l, int vi);
/* returns 1 if the field was handled */
static int layer_set_field(lua_State *L, Project *P, Layer *l, const char *k, int vi) {
    vi = lua_absindex(L, vi);
    P->rev++;
    if (!strcmp(k, "name")) copy_str(l->name, sizeof l->name, luaL_checkstring(L, vi));
    else if (!strcmp(k, "visible")) l->visible = lua_toboolean(L, vi);
    else if (!strcmp(k, "start") || !strcmp(k, "tin")) l->tin = (float)luaL_checknumber(L, vi);
    else if (!strcmp(k, "stop") || !strcmp(k, "tout")) l->tout = lua_isnil(L, vi) ? -1 : (float)luaL_checknumber(L, vi);
    else if (!strcmp(k, "blend")) {
        int b = find_name(BLEND_NAMES, BM_COUNT, luaL_checkstring(L, vi));
        if (b < 0) luaL_error(L, "unknown blend mode (normal, add, multiply, screen)");
        l->blend = (uint8_t)b;
    } else if (!strcmp(k, "shape")) {
        int s = find_name(SHAPE_NAMES, SH_COUNT, luaL_checkstring(L, vi));
        if (s < 0) luaL_error(L, "unknown shape (rect, ellipse, polygon, star)");
        l->shape = (uint8_t)s; shape_defaults(l);
    } else if (!strcmp(k, "fill") || !strcmp(k, "color")) { if (!get_color(L, vi, l->fill)) luaL_error(L, "bad color (use {r,g,b[,a]} or \"#rrggbb\")"); }
    else if (!strcmp(k, "stroke")) { if (!get_color(L, vi, l->stroke)) luaL_error(L, "bad color"); }
    else if (!strcmp(k, "text")) copy_str(l->text, sizeof l->text, luaL_checkstring(L, vi));
    else if (!strcmp(k, "path")) copy_str(l->path, sizeof l->path, luaL_checkstring(L, vi));
    else if (!strcmp(k, "trim")) l->trim = (float)luaL_checknumber(L, vi);
    else if (!strcmp(k, "speed") && l->type == L_VIDEO) { float v = (float)luaL_checknumber(L, vi); l->speed = v < 0.05f ? 0.05f : v > 16 ? 16 : v; }
    else if (!strcmp(k, "volume") || !strcmp(k, "vol")) { float v = (float)luaL_checknumber(L, vi); l->vol = v < 0 ? 0 : v > 1 ? 1 : v; }
    else if (!strcmp(k, "fit")) { const char *f = luaL_checkstring(L, vi); l->fitm = (!strcmp(f, "fit") || !strcmp(f, "contain")) ? 1 : 0; }
    else if (!strcmp(k, "graph")) { P->rev--; return graph_from_lua(L, P, l, vi); }
    else if (!strcmp(k, "scale")) { l->p[P_SX].base = l->p[P_SY].base = (float)luaL_checknumber(L, vi); }
    else if (!strcmp(k, "position")) {
        lua_rawgeti(L, vi, 1); lua_rawgeti(L, vi, 2);
        l->p[P_X].base = (float)luaL_checknumber(L, -2); l->p[P_Y].base = (float)luaL_checknumber(L, -1);
        lua_pop(L, 2);
    } else if (!strcmp(k, "expr") || !strcmp(k, "keys")) {
        luaL_checktype(L, vi, LUA_TTABLE);
        bool is_expr = k[0] == 'e';
        lua_pushnil(L);
        while (lua_next(L, vi)) {
            if (lua_type(L, -2) == LUA_TSTRING) {
                Prop *p = layer_prop(l, lua_tostring(L, -2));
                if (!p) luaL_error(L, "unknown property '%s'", lua_tostring(L, -2));
                if (is_expr) set_prop_lua(L, P, p, -1);
                else {
                    lua_createtable(L, 0, 1); lua_pushvalue(L, -2); lua_setfield(L, -2, "keys");
                    set_prop_lua(L, P, p, -1); lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
        }
    } else {
        Prop *p = layer_prop(l, k);
        if (!p) { P->rev--; return 0; }
        set_prop_lua(L, P, p, vi);
    }
    return 1;
}

/* ---------- Fusion node graph (lua) ---------- */
static int node_set_field(lua_State *L, Project *P, Node *n, const char *k, int vi) {
    vi = lua_absindex(L, vi);
    if (!strcmp(k, "name")) copy_str(n->name, sizeof n->name, luaL_checkstring(L, vi));
    else if (!strcmp(k, "on") || !strcmp(k, "enabled")) n->on = lua_toboolean(L, vi);
    else if (!strcmp(k, "gx")) n->gx = (float)luaL_checknumber(L, vi);
    else if (!strcmp(k, "gy")) n->gy = (float)luaL_checknumber(L, vi);
    else if (!strcmp(k, "blend")) {
        int b = find_name(BLEND_NAMES, BM_COUNT, luaL_checkstring(L, vi));
        if (b < 0) luaL_error(L, "unknown blend mode (normal, add, multiply, screen)");
        n->blend = (uint8_t)b;
    } else if (n->kind == N_FX && !strcmp(k, "color")) { if (!get_color(L, vi, n->fx.color)) luaL_error(L, "bad color"); }
    else if (n->kind == N_GEN && n->gen) { if (!layer_set_field(L, P, n->gen, k, vi)) return 0; }
    else { Prop *p = node_prop(n, k); if (!p) return 0; set_prop_lua(L, P, p, vi); }
    P->rev++;
    return 1;
}
static bool node_kind_from_name(const char *kn, int *kind, int *sub) {
    *sub = 0;
    if (!strcmp(kn, "xform") || !strcmp(kn, "transform")) { *kind = N_XFORM; return true; }
    if (!strcmp(kn, "merge")) { *kind = N_MERGE; return true; }
    int t = find_name(LAYER_TYPE_NAMES, L_TYPE_COUNT, kn);
    if (t >= 0 && t != L_ADJUST) { *kind = N_GEN; *sub = t; return true; }
    for (int i = 0; i < FX_COUNT; i++) if (!strcmp(FX_DEFS[i].name, kn)) { *kind = N_FX; *sub = i; return true; }
    return false;
}
static Graph *layer_graph(Project *P, Layer *l) { if (!l->g) { l->g = graph_new(); P->rev++; } return l->g; }
static int l_graph(lua_State *L) {
    Layer *l = check_layer(L, 1); Project *P = getP(L); Graph *g = layer_graph(P, l);
    lua_createtable(L, 0, 2);
    for (int i = 0; i < g->n; i++) if (g->nd[i].kind == N_IN) { lua_pushinteger(L, g->nd[i].uid); lua_setfield(L, -2, "source"); }
    lua_pushinteger(L, graph_out_uid(g)); lua_setfield(L, -2, "output");
    return 1;
}
static int l_node(lua_State *L) {
    Layer *l = check_layer(L, 1); Project *P = getP(L); Graph *g = layer_graph(P, l);
    const char *kn = luaL_checkstring(L, 2);
    int kind, sub;
    if (!node_kind_from_name(kn, &kind, &sub)) return luaL_error(L, "unknown node '%s' (xform, merge, a layer type, or an effect name)", kn);
    Node *n = graph_add(P, g, kind, sub);
    if (!n) return luaL_error(L, "node limit reached (%d)", MAX_NODES);
    int uid = n->uid;
    if (lua_istable(L, 3)) {
        lua_pushnil(L);
        while (lua_next(L, 3)) {
            if (lua_type(L, -2) == LUA_TSTRING) {
                const char *k = lua_tostring(L, -2);
                if (!strcmp(k, "input") || !strcmp(k, "from")) graph_connect(g, (int)luaL_checkinteger(L, -1), uid, 0);
                else if (!strcmp(k, "input2") || !strcmp(k, "from2")) graph_connect(g, (int)luaL_checkinteger(L, -1), uid, 1);
                else if (!node_set_field(L, P, n, k, -1)) { graph_remove(P, g, uid); return luaL_error(L, "unknown property '%s' for %s node", k, kn); }
            }
            lua_pop(L, 1);
        }
    }
    lua_pushinteger(L, uid);
    return 1;
}
static int l_nodeset(lua_State *L) {
    Layer *l = check_layer(L, 1); Project *P = getP(L);
    Node *n = graph_find(l->g, (int)luaL_checkinteger(L, 2));
    if (!n) return luaL_error(L, "no such node");
    luaL_checktype(L, 3, LUA_TTABLE);
    lua_pushnil(L);
    while (lua_next(L, 3)) {
        if (lua_type(L, -2) == LUA_TSTRING && !node_set_field(L, P, n, lua_tostring(L, -2), -1))
            return luaL_error(L, "unknown property '%s'", lua_tostring(L, -2));
        lua_pop(L, 1);
    }
    return 0;
}
static int l_link(lua_State *L) {
    Layer *l = check_layer(L, 1); Project *P = getP(L);
    bool ok = l->g && graph_connect(l->g, (int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3), (int)luaL_optinteger(L, 4, 0));
    if (ok) P->rev++;
    lua_pushboolean(L, ok); return 1;
}
static int l_unlink(lua_State *L) {
    Layer *l = check_layer(L, 1); Project *P = getP(L);
    if (l->g) { graph_disconnect(l->g, (int)luaL_checkinteger(L, 2), (int)luaL_optinteger(L, 3, 0)); P->rev++; }
    return 0;
}
static int l_unnode(lua_State *L) {
    Layer *l = check_layer(L, 1); Project *P = getP(L);
    if (l->g) graph_remove(P, l->g, (int)luaL_checkinteger(L, 2));
    return 0;
}
static int l_ungraph(lua_State *L) {
    Layer *l = check_layer(L, 1); Project *P = getP(L);
    if (l->g) { graph_free(P, l->g); l->g = NULL; P->rev++; }
    return 0;
}

/* graph={ {id=,kind=,...}, ... } (project file / afx.add) */
static int graph_from_lua(lua_State *L, Project *P, Layer *l, int vi) {
    vi = lua_absindex(L, vi);
    luaL_checktype(L, vi, LUA_TTABLE);
    if (l->g) { graph_free(P, l->g); l->g = NULL; }
    Graph *g = graph_new(); l->g = g;
    int cnt = (int)luaL_len(L, vi);
    Node *nodes[MAX_NODES + 4]; int tix[MAX_NODES + 4]; int nn = 0;
    for (int i = 1; i <= cnt && nn < MAX_NODES; i++) {
        lua_rawgeti(L, vi, i);
        if (!lua_istable(L, -1)) { lua_pop(L, 1); continue; }
        int ti = lua_gettop(L);
        lua_getfield(L, ti, "kind"); const char *kn = luaL_optstring(L, -1, "fx"); lua_pop(L, 1);
        lua_getfield(L, ti, "id"); int id = (int)luaL_optinteger(L, -1, 0); lua_pop(L, 1);
        Node *nd = NULL;
        if (!strcmp(kn, "in")) nd = &g->nd[0];
        else if (!strcmp(kn, "out")) nd = &g->nd[1];
        else {
            int kind = -1, sub = 0;
            if (!strcmp(kn, "xform")) kind = N_XFORM;
            else if (!strcmp(kn, "merge")) kind = N_MERGE;
            else if (!strcmp(kn, "gen")) {
                lua_getfield(L, ti, "type"); int t = find_name(LAYER_TYPE_NAMES, L_TYPE_COUNT, luaL_optstring(L, -1, "solid")); lua_pop(L, 1);
                if (t >= 0 && t != L_ADJUST) { kind = N_GEN; sub = t; }
            } else if (!strcmp(kn, "fx")) {
                lua_getfield(L, ti, "fx"); const char *fn = luaL_optstring(L, -1, ""); lua_pop(L, 1);
                for (int f = 0; f < FX_COUNT; f++) if (!strcmp(FX_DEFS[f].name, fn)) { kind = N_FX; sub = f; }
            }
            if (kind >= 0) nd = graph_add(P, g, kind, sub);
        }
        if (!nd) { lua_pop(L, 1); continue; }
        if (id > 0 && id < 100000 && (!graph_find(g, id) || graph_find(g, id) == nd)) { nd->uid = id; if (id >= g->next_uid) g->next_uid = id + 1; }
        nodes[nn] = nd; tix[nn] = i; nn++;
        if (nd->kind == N_GEN) { lua_getfield(L, ti, "shape"); if (!lua_isnil(L, -1)) layer_set_field(L, P, nd->gen, "shape", -1); lua_pop(L, 1); }
        lua_pushnil(L);
        while (lua_next(L, ti)) {
            if (lua_type(L, -2) == LUA_TSTRING) {
                const char *k = lua_tostring(L, -2);
                if (strcmp(k, "id") && strcmp(k, "kind") && strcmp(k, "inputs") && strcmp(k, "fx") && strcmp(k, "type") && strcmp(k, "shape")) {
                    if (!node_set_field(L, P, nd, k, -1)) afx_log(1, "graph: unknown node property '%s'", k);
                }
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    for (int i = 0; i < g->n; i++) g->nd[i].in[0] = g->nd[i].in[1] = 0;
    for (int j = 0; j < nn; j++) {
        lua_rawgeti(L, vi, tix[j]);
        lua_getfield(L, -1, "inputs");
        if (lua_istable(L, -1)) for (int k = 1; k <= 2; k++) {
            lua_rawgeti(L, -1, k);
            int from = (int)luaL_optinteger(L, -1, 0); lua_pop(L, 1);
            if (from > 0) graph_connect(g, from, nodes[j]->uid, k - 1);
        }
        lua_pop(L, 2);
    }
    P->rev++;
    return 1;
}

/* ---------- layer methods ---------- */
static int l_set(lua_State *L) {
    Layer *l = check_layer(L, 1); Project *P = getP(L);
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_pushnil(L);
    while (lua_next(L, 2)) {
        if (lua_type(L, -2) == LUA_TSTRING) {
            const char *k = lua_tostring(L, -2);
            if (!strcmp(k, "fx")) { /* list of effects */
                int n = (int)luaL_len(L, -1);
                for (int i = 1; i <= n; i++) {
                    lua_rawgeti(L, -1, i);
                    if (lua_type(L, -1) == LUA_TSTRING) add_fx_lua(L, P, l, lua_tostring(L, -1), 0);
                    else if (lua_istable(L, -1)) { lua_rawgeti(L, -1, 1); const char *nm = luaL_checkstring(L, -1); add_fx_lua(L, P, l, nm, lua_gettop(L) - 1); lua_pop(L, 1); }
                    lua_pop(L, 1);
                }
            } else if (!layer_set_field(L, P, l, k, -1)) luaL_error(L, "unknown property '%s' for %s layer", k, LAYER_TYPE_NAMES[l->type]);
        }
        lua_pop(L, 1);
    }
    lua_pushvalue(L, 1);
    return 1;
}
static int l_key(lua_State *L) {
    Layer *l = check_layer(L, 1); Project *P = getP(L);
    const char *pn = luaL_checkstring(L, 2);
    Prop *p = layer_prop(l, pn);
    if (!p) return luaL_error(L, "unknown property '%s'", pn);
    prop_set_key(p, (float)luaL_checknumber(L, 3), (float)luaL_checknumber(L, 4), get_ease(L, 5));
    P->rev++;
    lua_pushvalue(L, 1);
    return 1;
}
static int l_clear(lua_State *L) {
    Layer *l = check_layer(L, 1); Project *P = getP(L);
    const char *pn = luaL_checkstring(L, 2);
    Prop *p = layer_prop(l, pn);
    if (!p) return luaL_error(L, "unknown property '%s'", pn);
    p->nkeys = 0; prop_free(P, p); P->rev++;
    lua_pushvalue(L, 1);
    return 1;
}
static int l_expr(lua_State *L) {
    Layer *l = check_layer(L, 1); Project *P = getP(L);
    const char *pn = luaL_checkstring(L, 2);
    Prop *p = layer_prop(l, pn);
    if (!p) return luaL_error(L, "unknown property '%s'", pn);
    if (lua_type(L, 3) == LUA_TFUNCTION) set_fn_expr(L, P, p, 3);
    else if (lua_type(L, 3) == LUA_TSTRING) prop_set_expr(P, p, lua_tostring(L, 3));
    else prop_free(P, p);
    P->rev++;
    lua_pushvalue(L, 1);
    return 1;
}
static int l_fx(lua_State *L) {
    Layer *l = check_layer(L, 1); Project *P = getP(L);
    add_fx_lua(L, P, l, luaL_checkstring(L, 2), lua_istable(L, 3) ? 3 : 0);
    lua_pushvalue(L, 1);
    return 1;
}
static int l_unfx(lua_State *L) {
    Layer *l = check_layer(L, 1); Project *P = getP(L);
    const char *nm = luaL_checkstring(L, 2);
    for (int i = l->nfx - 1; i >= 0; i--) if (!strcmp(FX_DEFS[l->fx[i].type].name, nm)) { layer_del_fx(P, l, i); break; }
    lua_pushvalue(L, 1);
    return 1;
}
static int prop_pid(Layer *l, Prop *p) { long d = p - l->p; return d >= 0 && d < P_COUNT ? (int)d : 30; }
static int l_get(lua_State *L) {
    Layer *l = check_layer(L, 1); Project *P = getP(L);
    Prop *p = layer_prop(l, luaL_checkstring(L, 2));
    if (!p) return luaL_error(L, "unknown property '%s'", lua_tostring(L, 2));
    float t = (float)luaL_optnumber(L, 3, P->cur_t);
    lua_pushnumber(L, prop_eval(P, l, p, prop_pid(l, p), t));
    return 1;
}
static int l_remove(lua_State *L) {
    Layer *l = check_layer(L, 1); Project *P = getP(L);
    project_remove(P, project_index_of(P, l));
    return 0;
}
static int l_move(lua_State *L) {
    Layer *l = check_layer(L, 1); Project *P = getP(L);
    project_move(P, project_index_of(P, l), (int)luaL_checkinteger(L, 2));
    lua_pushvalue(L, 1);
    return 1;
}
static int l_dup(lua_State *L) {
    Layer *l = check_layer(L, 1); Project *P = getP(L);
    int i = project_duplicate(P, project_index_of(P, l));
    if (i < 0) return luaL_error(L, "cannot duplicate layer");
    push_layer(L, &P->layers[i]);
    return 1;
}
static const luaL_Reg LAYER_METHODS[] = {
    { "set", l_set }, { "key", l_key }, { "clear", l_clear }, { "expr", l_expr }, { "fx", l_fx },
    { "unfx", l_unfx }, { "get", l_get },
    { "graph", l_graph }, { "node", l_node }, { "nodeset", l_nodeset }, { "link", l_link }, { "unlink", l_unlink }, { "unnode", l_unnode }, { "ungraph", l_ungraph }, { "remove", l_remove }, { "move", l_move }, { "duplicate", l_dup }, { NULL, NULL } };

static int layer_index(lua_State *L) {
    const char *k = luaL_checkstring(L, 2);
    lua_getmetatable(L, 1); lua_getfield(L, -1, "__methods"); lua_getfield(L, -1, k);
    if (!lua_isnil(L, -1)) return 1;
    lua_settop(L, 2);
    Layer *l = check_layer(L, 1); Project *P = getP(L);
    if (!strcmp(k, "name")) lua_pushstring(L, l->name);
    else if (!strcmp(k, "type")) lua_pushstring(L, LAYER_TYPE_NAMES[l->type]);
    else if (!strcmp(k, "visible")) lua_pushboolean(L, l->visible);
    else if (!strcmp(k, "scripted")) lua_pushboolean(L, l->scripted);
    else if (!strcmp(k, "index")) lua_pushinteger(L, project_index_of(P, l) + 1);
    else if (!strcmp(k, "start")) lua_pushnumber(L, l->tin);
    else if (!strcmp(k, "stop")) lua_pushnumber(L, project_layer_out(P, l));
    else if (!strcmp(k, "blend")) lua_pushstring(L, BLEND_NAMES[l->blend]);
    else if (!strcmp(k, "shape")) lua_pushstring(L, SHAPE_NAMES[l->shape]);
    else if (!strcmp(k, "fill") || !strcmp(k, "color")) push_color(L, l->fill);
    else if (!strcmp(k, "stroke")) push_color(L, l->stroke);
    else if (!strcmp(k, "text")) lua_pushstring(L, l->text);
    else if (!strcmp(k, "path")) lua_pushstring(L, l->path);
    else if (!strcmp(k, "trim")) lua_pushnumber(L, l->trim);
    else if (!strcmp(k, "speed") && l->type == L_VIDEO) lua_pushnumber(L, l->speed);
    else if (!strcmp(k, "volume") || !strcmp(k, "vol")) lua_pushnumber(L, l->vol);
    else if (!strcmp(k, "fit")) lua_pushstring(L, l->fitm ? "fit" : "fill");
    else {
        Prop *p = layer_prop(l, k);
        if (!p) { lua_pushnil(L); return 1; }
        lua_pushnumber(L, prop_eval(P, l, p, prop_pid(l, p), P->cur_t));
    }
    return 1;
}
static int layer_newindex(lua_State *L) {
    Layer *l = check_layer(L, 1); Project *P = getP(L);
    const char *k = luaL_checkstring(L, 2);
    if (!layer_set_field(L, P, l, k, 3)) return luaL_error(L, "unknown property '%s'", k);
    return 0;
}
static int layer_tostring(lua_State *L) {
    Layer *l = check_layer(L, 1);
    lua_pushfstring(L, "layer<%s:%s>", LAYER_TYPE_NAMES[l->type], l->name);
    return 1;
}

/* ---------- afx.* ---------- */
static int make_layer(lua_State *L, LayerType type, int ai) {
    Project *P = getP(L);
    const char *first = NULL;
    if (lua_type(L, 1) == LUA_TSTRING && ai == 2) first = lua_tostring(L, 1);
    Layer *l = project_add(P, type, NULL);
    if (!l) return luaL_error(L, "layer limit reached (%d)", MAX_LAYERS);
    if (first) {
        if (type == L_TEXT) copy_str(l->text, sizeof l->text, first);
        else if (type == L_SVG || type == L_IMAGE || type == L_VIDEO) copy_str(l->path, sizeof l->path, first);
        else copy_str(l->name, sizeof l->name, first);
    }
    if (lua_istable(L, ai)) {
        lua_getfield(L, ai, "shape");
        if (!lua_isnil(L, -1)) layer_set_field(L, P, l, "shape", -1);
        lua_pop(L, 1);
        lua_pushnil(L);
        while (lua_next(L, ai)) {
            if (lua_type(L, -2) == LUA_TSTRING) {
                const char *k = lua_tostring(L, -2);
                if (!strcmp(k, "fx") || !strcmp(k, "type") || !strcmp(k, "shape")) { /* handled elsewhere */ }
                else if (!layer_set_field(L, P, l, k, -1)) {
                    int idx = project_index_of(P, l);
                    project_remove(P, idx);
                    return luaL_error(L, "unknown property '%s' for %s layer", k, LAYER_TYPE_NAMES[type]);
                }
            }
            lua_pop(L, 1);
        }
        lua_getfield(L, ai, "fx");
        if (lua_istable(L, -1)) {
            int n = (int)luaL_len(L, -1);
            for (int i = 1; i <= n; i++) {
                lua_rawgeti(L, -1, i);
                if (lua_type(L, -1) == LUA_TSTRING) add_fx_lua(L, P, l, lua_tostring(L, -1), 0);
                else if (lua_istable(L, -1)) { lua_rawgeti(L, -1, 1); const char *nm = luaL_checkstring(L, -1); add_fx_lua(L, P, l, nm, lua_gettop(L) - 1); lua_pop(L, 1); }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }
    push_layer(L, l);
    return 1;
}
#define DEF_CTOR(fn, T) static int fn(lua_State *L) { return make_layer(L, T, lua_type(L, 1) == LUA_TSTRING ? 2 : 1); }
DEF_CTOR(a_solid, L_SOLID) DEF_CTOR(a_shape, L_SHAPE) DEF_CTOR(a_text, L_TEXT) DEF_CTOR(a_svg, L_SVG)
DEF_CTOR(a_video, L_VIDEO) DEF_CTOR(a_image, L_IMAGE) DEF_CTOR(a_asteroid, L_ASTEROID) DEF_CTOR(a_stars, L_STARS) DEF_CTOR(a_adjust, L_ADJUST)
static int a_add(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "type");
    int t = find_name(LAYER_TYPE_NAMES, L_TYPE_COUNT, luaL_optstring(L, -1, "solid"));
    if (t < 0) return luaL_error(L, "unknown layer type");
    lua_pop(L, 1);
    return make_layer(L, (LayerType)t, 1);
}

static int a_comp(lua_State *L) {
    Project *P = getP(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "name"); if (lua_isstring(L, -1)) copy_str(P->name, sizeof P->name, lua_tostring(L, -1)); lua_pop(L, 1);
    lua_getfield(L, 1, "width"); if (lua_isnumber(L, -1)) P->width = (int)lua_tointeger(L, -1); lua_pop(L, 1);
    lua_getfield(L, 1, "height"); if (lua_isnumber(L, -1)) P->height = (int)lua_tointeger(L, -1); lua_pop(L, 1);
    lua_getfield(L, 1, "fps"); if (lua_isnumber(L, -1)) P->fps = (float)lua_tonumber(L, -1); lua_pop(L, 1);
    lua_getfield(L, 1, "duration"); if (lua_isnumber(L, -1)) P->duration = (float)lua_tonumber(L, -1); lua_pop(L, 1);
    lua_getfield(L, 1, "bg"); if (!lua_isnil(L, -1) && !get_color(L, -1, P->bg)) luaL_error(L, "bad bg color"); lua_pop(L, 1);
    P->width = P->width < 16 ? 16 : P->width > 4096 ? 4096 : P->width;
    P->height = P->height < 16 ? 16 : P->height > 4096 ? 4096 : P->height;
    P->fps = P->fps < 1 ? 1 : P->fps > 120 ? 120 : P->fps;
    P->duration = P->duration < 0.1f ? 0.1f : P->duration > 600 ? 600 : P->duration;
    P->rev++;
    return 0;
}
static int a_layer(lua_State *L) {
    Project *P = getP(L);
    if (lua_isnumber(L, 1)) {
        int i = (int)lua_tointeger(L, 1) - 1;
        if (i < 0 || i >= P->nlayers) { lua_pushnil(L); return 1; }
        push_layer(L, &P->layers[i]); return 1;
    }
    const char *nm = luaL_checkstring(L, 1);
    for (int i = 0; i < P->nlayers; i++) if (!strcmp(P->layers[i].name, nm)) { push_layer(L, &P->layers[i]); return 1; }
    lua_pushnil(L);
    return 1;
}
static int a_layers(lua_State *L) {
    Project *P = getP(L);
    lua_createtable(L, P->nlayers, 0);
    for (int i = 0; i < P->nlayers; i++) { push_layer(L, &P->layers[i]); lua_rawseti(L, -2, i + 1); }
    return 1;
}
static int a_clear(lua_State *L) {
    Project *P = getP(L);
    while (P->nlayers) project_remove(P, P->nlayers - 1);
    return 0;
}
static int a_on_frame(lua_State *L) {
    Project *P = getP(L);
    luaL_checktype(L, 1, LUA_TFUNCTION);
    if (P->nhooks >= 8) return luaL_error(L, "too many on_frame hooks");
    lua_pushvalue(L, 1);
    P->hooks[P->nhooks++] = luaL_ref(L, LUA_REGISTRYINDEX);
    return 0;
}
static int a_time(lua_State *L) { lua_pushnumber(L, getP(L)->cur_t); return 1; }
static int a_frame(lua_State *L) { lua_pushinteger(L, getP(L)->cur_frame); return 1; }
static int a_fps(lua_State *L) { lua_pushnumber(L, getP(L)->fps); return 1; }
static int a_duration(lua_State *L) { lua_pushnumber(L, getP(L)->duration); return 1; }
static int a_size(lua_State *L) { Project *P = getP(L); lua_pushinteger(L, P->width); lua_pushinteger(L, P->height); return 2; }
static int a_color(lua_State *L) {
    uint8_t c[4];
    if (lua_isnumber(L, 1)) {
        for (int i = 0; i < 4; i++) { double v = luaL_optnumber(L, i + 1, i == 3 ? 255 : 0); c[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
    } else if (!get_color(L, 1, c)) return luaL_error(L, "bad color");
    push_color(L, c);
    return 1;
}
static int a_log(lua_State *L) {
    int n = lua_gettop(L);
    luaL_Buffer b; luaL_buffinit(L, &b);
    for (int i = 1; i <= n; i++) {
        size_t len; const char *s = luaL_tolstring(L, i, &len);
        if (i > 1) luaL_addchar(&b, '\t');
        luaL_addlstring(&b, s, len);
        lua_pop(L, 1);
    }
    luaL_pushresult(&b);
    afx_log(0, "%s", lua_tostring(L, -1));
    return 0;
}

/* ---------- expression helpers ---------- */
static uint32_t hu(uint32_t x) { x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return x; }
static float hf(uint32_t x) { return (hu(x) & 0xFFFFFF) / 16777216.0f; }
static float noise1(float x, uint32_t seed) {
    int xi = (int)floorf(x); float f = x - xi; f = f * f * (3 - 2 * f);
    return (hf(xi * 374761393u + seed * 668265263u) * (1 - f) + hf((xi + 1) * 374761393u + seed * 668265263u) * f) * 2 - 1;
}
static int h_wiggle(lua_State *L) {
    Project *P = getP(L);
    float freq = (float)luaL_checknumber(L, 1), amp = (float)luaL_checknumber(L, 2);
    lua_getglobal(L, "_seed"); uint32_t seed = (uint32_t)lua_tointeger(L, -1); lua_pop(L, 1);
    seed += (uint32_t)luaL_optinteger(L, 3, 0) * 97u;
    float x = P->cur_t * freq;
    lua_pushnumber(L, amp * (0.7f * noise1(x, seed) + 0.3f * noise1(x * 2.3f + 11.0f, seed + 5)));
    return 1;
}
static int h_noise(lua_State *L) {
    float x = (float)luaL_checknumber(L, 1);
    float y = (float)luaL_optnumber(L, 2, 0);
    lua_pushnumber(L, noise1(x, 1) * 0.6f + noise1(y * 1.7f + 3.1f, 2) * 0.4f * (y != 0));
    return 1;
}
static int h_rand(lua_State *L) { lua_pushnumber(L, hf((uint32_t)luaL_checkinteger(L, 1) * 2654435761u + 12345u)); return 1; }
static int h_lerp(lua_State *L) { double a = luaL_checknumber(L, 1), b = luaL_checknumber(L, 2), t = luaL_checknumber(L, 3); lua_pushnumber(L, a + (b - a) * t); return 1; }
static int h_clamp(lua_State *L) { double v = luaL_checknumber(L, 1), a = luaL_checknumber(L, 2), b = luaL_checknumber(L, 3); lua_pushnumber(L, v < a ? a : v > b ? b : v); return 1; }
static int h_smoothstep(lua_State *L) {
    double a = luaL_checknumber(L, 1), b = luaL_checknumber(L, 2), x = luaL_checknumber(L, 3);
    double t = (x - a) / (b - a); t = t < 0 ? 0 : t > 1 ? 1 : t;
    lua_pushnumber(L, t * t * (3 - 2 * t));
    return 1;
}
static int h_ease(lua_State *L) {
    int e = get_ease(L, 1); if (e < 0) e = 0;
    lua_pushnumber(L, ease_apply(e, (float)luaL_checknumber(L, 2)));
    return 1;
}
static int h_help(lua_State *L) {
    (void)L;
    afx_log(0, "AFX scripting quick reference");
    afx_log(0, "  afx.comp{width=,height=,fps=,duration=,bg=}      afx.solid/shape/text/svg/image/asteroid/stars/adjust{...}");
    afx_log(0, "  l = afx.shape{shape='star', x=320, y=180, fill='#ffb43c', fx={{'glow',radius=12}}}");
    afx_log(0, "  l:key('x', time, value [,ease])   l:expr('y','v+wiggle(2,10)')   l:fx('blur',{radius=4})   l:set{...}");
    afx_log(0, "  afx.layer(name|index)  afx.layers()  afx.on_frame(function(t,frame) ... end)  afx.clear()");
    afx_log(0, "  expression vars: t lt frame v i fps w h   helpers: wiggle noise rand lerp clamp smoothstep ease");
    return 0;
}
static int s_print(lua_State *L) { return a_log(L); }

lua_State *script_new_state(Project *P) {
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    lua_pushlightuserdata(L, P); lua_setfield(L, LUA_REGISTRYINDEX, "afx_project");
    if (luaL_newmetatable(L, LAYER_MT)) {
        lua_pushcfunction(L, layer_index); lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, layer_newindex); lua_setfield(L, -2, "__newindex");
        lua_pushcfunction(L, layer_tostring); lua_setfield(L, -2, "__tostring");
        lua_newtable(L); luaL_setfuncs(L, LAYER_METHODS, 0); lua_setfield(L, -2, "__methods");
    }
    lua_pop(L, 1);
    static const luaL_Reg AFXF[] = {
        { "comp", a_comp }, { "add", a_add }, { "solid", a_solid }, { "shape", a_shape }, { "text", a_text }, { "svg", a_svg },
        { "image", a_image }, { "video", a_video }, { "asteroid", a_asteroid }, { "stars", a_stars }, { "adjust", a_adjust },
        { "layer", a_layer }, { "layers", a_layers }, { "clear", a_clear }, { "on_frame", a_on_frame },
        { "time", a_time }, { "frame", a_frame }, { "fps", a_fps }, { "duration", a_duration }, { "size", a_size },
        { "color", a_color }, { "log", a_log }, { NULL, NULL } };
    lua_newtable(L); luaL_setfuncs(L, AFXF, 0);
    lua_pushstring(L, AFX_VERSION); lua_setfield(L, -2, "version");
    lua_setglobal(L, "afx");
    static const luaL_Reg HELP[] = {
        { "wiggle", h_wiggle }, { "noise", h_noise }, { "rand", h_rand }, { "lerp", h_lerp }, { "clamp", h_clamp },
        { "smoothstep", h_smoothstep }, { "ease", h_ease }, { "help", h_help }, { "print", s_print }, { NULL, NULL } };
    lua_pushglobaltable(L); luaL_setfuncs(L, HELP, 0); lua_pop(L, 1);
    lua_pushinteger(L, 1); lua_setglobal(L, "_seed");
    return L;
}

/* ---------- hooks / exec ---------- */
void script_call_hooks(Project *P, float t, int frame) {
    for (int i = 0; i < P->nhooks; i++) {
        lua_State *L = P->L;
        lua_rawgeti(L, LUA_REGISTRYINDEX, P->hooks[i]);
        lua_pushnumber(L, t); lua_pushinteger(L, frame);
        if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
            afx_log(1, "on_frame error: %s (hook disabled)", lua_tostring(L, -1));
            lua_pop(L, 1);
            luaL_unref(L, LUA_REGISTRYINDEX, P->hooks[i]);
            memmove(&P->hooks[i], &P->hooks[i + 1], (size_t)(P->nhooks - i - 1) * sizeof(int));
            P->nhooks--; i--;
        }
    }
}

static bool run_chunk(Project *P, const char *code, size_t len, const char *name) {
    lua_State *L = P->L;
    if (luaL_loadbufferx(L, code, len, name, "t") != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
        afx_log(1, "%s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    return true;
}

bool script_exec(Project *P, const char *code, const char *chunk) {
    lua_State *L = P->L;
    size_t n = strlen(code);
    char *ret = malloc(n + 8);
    snprintf(ret, n + 8, "return %s", code);
    int top = lua_gettop(L);
    bool ok = true;
    if (luaL_loadstring(L, ret) == LUA_OK) {
        if (lua_pcall(L, 0, LUA_MULTRET, 0) != LUA_OK) { afx_log(1, "%s", lua_tostring(L, -1)); ok = false; }
        else for (int i = top + 1; i <= lua_gettop(L); i++) { size_t l; const char *s = luaL_tolstring(L, i, &l); afx_log(0, "= %s", s); lua_pop(L, 1); }
    } else {
        lua_pop(L, 1);
        ok = run_chunk(P, code, n, chunk ? chunk : "=console");
    }
    lua_settop(L, top);
    free(ret);
    P->rev++;
    return ok;
}

bool script_run_user(Project *P) {
    for (int i = P->nlayers - 1; i >= 0; i--) if (P->layers[i].scripted) project_remove(P, i);
    for (int i = 0; i < P->nhooks; i++) luaL_unref(P->L, LUA_REGISTRYINDEX, P->hooks[i]);
    P->nhooks = 0;
    bool ok = true;
    if (P->user_code && *P->user_code) {
        P->in_user = true;
        ok = run_chunk(P, P->user_code, strlen(P->user_code), "=user");
        P->in_user = false;
    }
    P->rev++;
    return ok;
}

/* ---------- load ---------- */
static void set_package_path(Project *P) {
    if (!P->filepath[0]) return;
    char dir[PATH_LEN]; snprintf(dir, sizeof dir, "%s", P->filepath);
    char *s = strrchr(dir, '/');
    if (s) *s = 0; else snprintf(dir, sizeof dir, ".");
    char code[PATH_LEN + 64];
    snprintf(code, sizeof code, "package.path = [[%s/?.lua;]] .. package.path", dir);
    (void)luaL_dostring(P->L, code);
}

bool project_load_text(Project *P, const char *text, const char *origin) {
    char fp[PATH_LEN]; snprintf(fp, sizeof fp, "%s", P->filepath);
    project_reset(P);
    snprintf(P->filepath, sizeof P->filepath, "%s", fp);
    set_package_path(P);
    const char *mark = NULL, *p = text;
    while ((p = strstr(p, USER_MARK))) {
        if ((p == text || p[-1] == '\n') && (p[7] == '\n' || p[7] == '\r' || p[7] == 0)) { mark = p; break; }
        p += 7;
    }
    bool ok = true;
    char name[PATH_LEN]; snprintf(name, sizeof name, "=%s", origin ? origin : "project");
    if (mark) {
        ok = run_chunk(P, text, (size_t)(mark - text), name);
        const char *u = mark + 7; if (*u == '\r') u++; if (*u == '\n') u++;
        P->user_code = strdup(u);
    } else P->user_code = strdup(text);
    if (P->user_code[0]) {
        P->in_user = true;
        ok = run_chunk(P, P->user_code, strlen(P->user_code), "=user") && ok;
        P->in_user = false;
    }
    P->rev++;
    return ok;
}

bool project_load_file(Project *P, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { afx_log(1, "cannot open '%s'", path); return false; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    size_t rd = fread(buf, 1, (size_t)n, f); buf[rd] = 0; fclose(f);
    snprintf(P->filepath, sizeof P->filepath, "%s", path);
    const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
    bool ok = project_load_text(P, buf, base);
    free(buf);
    return ok;
}

/* ---------- serialize ---------- */
typedef struct { char *s; size_t n, cap; } SB;
static void sb_put(SB *b, const char *s) {
    size_t l = strlen(s);
    if (b->n + l + 1 > b->cap) { b->cap = (b->n + l + 1) * 2; b->s = realloc(b->s, b->cap); }
    memcpy(b->s + b->n, s, l + 1); b->n += l;
}
static void sb_fmt(SB *b, const char *fmt, ...) {
    char tmp[2048];
    va_list ap; va_start(ap, fmt); vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
    sb_put(b, tmp);
}
static void sb_str(SB *b, const char *s) {
    sb_put(b, "\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { char t[3] = { '\\', (char)c, 0 }; sb_put(b, t); }
        else if (c == '\n') sb_put(b, "\\n");
        else if (c == '\r') sb_put(b, "\\r");
        else if (c == '\t') sb_put(b, "\\t");
        else if (c < 32) sb_fmt(b, "\\%03d", c);
        else { char t[2] = { (char)c, 0 }; sb_put(b, t); }
    }
    sb_put(b, "\"");
}
static void ser_color(SB *b, const char *key, const uint8_t c[4]) { sb_fmt(b, "  %s={%d,%d,%d,%d},\n", key, c[0], c[1], c[2], c[3]); }

static void ser_prop(SB *b, Project *P, const char *key, const Prop *p, const Prop *def, int *lossy, const char *indent) {
    bool has_expr = p->expr != NULL;
    if (p->fn && !has_expr) (*lossy)++;
    bool changed = !def || p->base != def->base || p->nkeys > 0 || has_expr;
    if (!changed) return;
    if (p->nkeys == 0 && !has_expr) { sb_fmt(b, "%s%s=%.6g,%s", indent, key, p->base, indent[0] == ' ' && indent[1] == ' ' && indent[2] == 0 ? "\n" : " "); return; }
    sb_fmt(b, "%s%s={ base=%.6g", indent, key, p->base);
    if (p->nkeys) {
        sb_put(b, ", keys={ ");
        for (int i = 0; i < p->nkeys; i++) {
            sb_fmt(b, "{%.6g,%.6g", p->keys[i].t, p->keys[i].v);
            if (p->keys[i].ease) sb_fmt(b, ",\"%s\"", EASE_NAMES[p->keys[i].ease]);
            sb_put(b, i + 1 < p->nkeys ? "}, " : "}");
        }
        sb_put(b, " }");
    }
    if (has_expr) { sb_put(b, ", expr="); sb_str(b, p->expr); }
    sb_put(b, indent[0] == ' ' && indent[1] == ' ' && indent[2] == 0 ? " },\n" : " }, ");
}

static void ser_graph(SB *b, Project *P, Layer *l, int *lossy) {
    Graph *g = l->g;
    static const char *XFN[NODE_NP] = { "x", "y", "sx", "sy", "rot" };
    static const float XFD[NODE_NP] = { 0, 0, 100, 100, 0 };
    static const char *MGN[3] = { "opacity", "ox", "oy" };
    static const float MGD[3] = { 100, 0, 0 };
    sb_put(b, "  graph={\n");
    for (int i = 0; i < g->n; i++) {
        Node *n = &g->nd[i];
        sb_fmt(b, "    { id=%d, kind=\"%s\", name=", n->uid, NODE_KIND_NAMES[n->kind]); sb_str(b, n->name);
        sb_fmt(b, ", gx=%.4g, gy=%.4g, ", n->gx, n->gy);
        if (!n->on) sb_put(b, "on=false, ");
        if (n->blend) sb_fmt(b, "blend=\"%s\", ", BLEND_NAMES[n->blend]);
        int nin = node_nin(n);
        if (nin) { sb_put(b, "inputs={"); for (int k = 0; k < nin; k++) sb_fmt(b, "%d%s", n->in[k], k + 1 < nin ? "," : ""); sb_put(b, "}, "); }
        if (n->kind == N_FX) {
            const FxDef *fd = &FX_DEFS[n->fx.type];
            sb_fmt(b, "fx=\"%s\", ", fd->name);
            if (n->fx.type == FX_TINT) sb_fmt(b, "color={%d,%d,%d,%d}, ", n->fx.color[0], n->fx.color[1], n->fx.color[2], n->fx.color[3]);
            for (int k = 0; k < fd->np; k++) { Prop dp = { 0 }; dp.base = fd->def[k]; ser_prop(b, P, fd->pn[k], &n->fx.p[k], &dp, lossy, ""); }
        } else if (n->kind == N_XFORM) {
            for (int k = 0; k < NODE_NP; k++) { Prop dp = { 0 }; dp.base = XFD[k]; ser_prop(b, P, XFN[k], &n->mp[k], &dp, lossy, ""); }
        } else if (n->kind == N_MERGE) {
            for (int k = 0; k < 3; k++) { Prop dp = { 0 }; dp.base = MGD[k]; ser_prop(b, P, MGN[k], &n->mp[k], &dp, lossy, ""); }
        } else if (n->kind == N_GEN && n->gen) {
            Layer *q = n->gen; int save = P->next_uid;
            Layer d; layer_init(P, &d, q->type);
            sb_fmt(b, "type=\"%s\", ", LAYER_TYPE_NAMES[q->type]);
            if (q->type == L_SHAPE) sb_fmt(b, "shape=\"%s\", ", SHAPE_NAMES[q->shape]);
            if (memcmp(q->fill, d.fill, 4)) sb_fmt(b, "fill={%d,%d,%d,%d}, ", q->fill[0], q->fill[1], q->fill[2], q->fill[3]);
            if (q->type == L_SHAPE && memcmp(q->stroke, d.stroke, 4)) sb_fmt(b, "stroke={%d,%d,%d,%d}, ", q->stroke[0], q->stroke[1], q->stroke[2], q->stroke[3]);
            if (q->type == L_TEXT) { sb_put(b, "text="); sb_str(b, q->text); sb_put(b, ", "); }
            if (q->type == L_SVG || q->type == L_IMAGE || q->type == L_VIDEO) { sb_put(b, "path="); sb_str(b, q->path); sb_put(b, ", "); }
            if (q->type == L_VIDEO) {
                if (q->trim != 0) sb_fmt(b, "trim=%.6g, ", q->trim);
                if (q->speed != 1) sb_fmt(b, "speed=%.6g, ", q->speed);
                if (q->vol != 1) sb_fmt(b, "volume=%.6g, ", q->vol);
                if (q->fitm) sb_put(b, "fit=\"fit\", ");
            }
            for (int k = 0; k < P_COUNT; k++) {
                Prop dp = d.p[k];
                if (q->type == L_SHAPE && (k == P_A || k == P_B)) dp.base = -12345;
                ser_prop(b, P, PROP_NAMES[k], &q->p[k], &dp, lossy, "");
            }
            for (int k = 0; k < P_COUNT; k++) prop_free(P, &d.p[k]);
            P->next_uid = save;
        }
        sb_put(b, "},\n");
    }
    sb_put(b, "  },\n");
}

char *project_serialize(Project *P) {
    SB b = { 0 };
    int lossy = 0;
    sb_put(&b, "-- Asteroid Fusion project\n");
    sb_put(&b, "afx.comp{ name="); sb_str(&b, P->name);
    sb_fmt(&b, ", width=%d, height=%d, fps=%.6g, duration=%.6g, bg={%d,%d,%d,%d} }\n\n", P->width, P->height, P->fps, P->duration, P->bg[0], P->bg[1], P->bg[2], P->bg[3]);
    int save_uid = P->next_uid;
    for (int i = 0; i < P->nlayers; i++) {
        Layer *l = &P->layers[i];
        if (l->scripted) continue;
        Layer d; layer_init(P, &d, l->type);
        sb_fmt(&b, "afx.add{\n  type=\"%s\", name=", LAYER_TYPE_NAMES[l->type]); sb_str(&b, l->name); sb_put(&b, ",\n");
        if (!l->visible) sb_put(&b, "  visible=false,\n");
        if (l->tin != 0) sb_fmt(&b, "  start=%.6g,\n", l->tin);
        if (l->tout >= 0) sb_fmt(&b, "  stop=%.6g,\n", l->tout);
        if (l->blend) sb_fmt(&b, "  blend=\"%s\",\n", BLEND_NAMES[l->blend]);
        if (l->type == L_SHAPE) sb_fmt(&b, "  shape=\"%s\",\n", SHAPE_NAMES[l->shape]);
        if (memcmp(l->fill, d.fill, 4)) ser_color(&b, "fill", l->fill);
        if (l->type == L_SHAPE) ser_color(&b, "stroke", l->stroke);
        if (l->type == L_TEXT) { sb_put(&b, "  text="); sb_str(&b, l->text); sb_put(&b, ",\n"); }
        if (l->type == L_SVG || l->type == L_IMAGE || l->type == L_VIDEO) { sb_put(&b, "  path="); sb_str(&b, l->path); sb_put(&b, ",\n"); }
        if (l->type == L_VIDEO) {
            if (l->trim != 0) sb_fmt(&b, "  trim=%.6g,\n", l->trim);
            if (l->speed != 1) sb_fmt(&b, "  speed=%.6g,\n", l->speed);
            if (l->vol != 1) sb_fmt(&b, "  volume=%.6g,\n", l->vol);
            if (l->fitm) sb_put(&b, "  fit=\"fit\",\n");
        }
        for (int k = 0; k < P_COUNT; k++) {
            /* shape default for a/b depends on shape kind; always write when shape is star/polygon */
            Prop dp = d.p[k];
            if (l->type == L_SHAPE && (k == P_A || k == P_B)) dp.base = -12345;
            ser_prop(&b, P, PROP_NAMES[k], &l->p[k], &dp, &lossy, "  ");
        }
        if (l->g) ser_graph(&b, P, l, &lossy);
        if (l->nfx) {
            sb_put(&b, "  fx={\n");
            for (int f = 0; f < l->nfx; f++) {
                Effect *e = &l->fx[f]; const FxDef *fd = &FX_DEFS[e->type];
                sb_fmt(&b, "    {\"%s\", ", fd->name);
                if (!e->on) sb_put(&b, "on=false, ");
                if (e->type == FX_TINT) sb_fmt(&b, "color={%d,%d,%d,%d}, ", e->color[0], e->color[1], e->color[2], e->color[3]);
                for (int k = 0; k < fd->np; k++) { Prop dp = { 0 }; dp.base = fd->def[k]; ser_prop(&b, P, fd->pn[k], &e->p[k], &dp, &lossy, ""); }
                sb_put(&b, "},\n");
            }
            sb_put(&b, "  },\n");
        }
        sb_put(&b, "}\n\n");
        for (int k = 0; k < P_COUNT; k++) prop_free(P, &d.p[k]);
    }
    P->next_uid = save_uid;
    if (lossy) sb_fmt(&b, "-- note: %d function expression(s) could not be saved; use string expressions or the user script\n\n", lossy);
    P->lossy = lossy;
    sb_put(&b, USER_MARK "\n");
    if (P->user_code) sb_put(&b, P->user_code);
    return b.s;
}

bool project_save_file(Project *P, const char *path) {
    char *s = project_serialize(P);
    FILE *f = fopen(path, "wb");
    if (!f) { free(s); afx_log(1, "cannot write '%s'", path); return false; }
    fwrite(s, 1, strlen(s), f); fclose(f); free(s);
    snprintf(P->filepath, sizeof P->filepath, "%s", path);
    return true;
}
