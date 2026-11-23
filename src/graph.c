#define _GNU_SOURCE
// Asteroid Fusion: per-clip node graph (Fusion-style compositing)
#include "afx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

const char *NODE_KIND_NAMES[N_KIND_COUNT] = { "in", "out", "gen", "fx", "xform", "merge" };

int node_nin(const Node *n) {
    switch (n->kind) {
    case N_OUT: case N_FX: case N_XFORM: return 1;
    case N_MERGE: return 2;
    default: return 0;
    }
}

const char *node_kind_label(const Node *n) {
    switch (n->kind) {
    case N_IN: return "Source";
    case N_OUT: return "Output";
    case N_GEN: return n->gen ? LAYER_TYPE_NAMES[n->gen->type] : "Generator";
    case N_FX: return FX_DEFS[n->fx.type].name;
    case N_XFORM: return "Transform";
    case N_MERGE: return "Merge";
    }
    return "?";
}

static void node_release(Project *P, Node *n) {
    for (int k = 0; k < NODE_NP; k++) prop_free(P, &n->mp[k]);
    for (int k = 0; k < 3; k++) prop_free(P, &n->fx.p[k]);
    if (n->gen) { layer_release_all(P, n->gen); free(n->gen); n->gen = NULL; }
}

Graph *graph_new(void) {
    Graph *g = calloc(1, sizeof *g);
    g->next_uid = 1;
    Node *a = &g->nd[0], *b = &g->nd[1];
    a->kind = N_IN; a->on = true; a->uid = g->next_uid++; snprintf(a->name, NAME_LEN, "Source"); a->gx = 0; a->gy = 0;
    b->kind = N_OUT; b->on = true; b->uid = g->next_uid++; snprintf(b->name, NAME_LEN, "Output"); b->gx = 2; b->gy = 0;
    b->in[0] = a->uid;
    g->n = 2;
    return g;
}

void graph_free(Project *P, Graph *g) {
    if (!g) return;
    for (int i = 0; i < g->n; i++) node_release(P, &g->nd[i]);
    free(g);
}

Node *graph_find(Graph *g, int uid) {
    if (!g || uid <= 0) return NULL;
    for (int i = 0; i < g->n; i++) if (g->nd[i].uid == uid) return &g->nd[i];
    return NULL;
}
int graph_index(const Graph *g, int uid) {
    for (int i = 0; i < g->n; i++) if (g->nd[i].uid == uid) return i;
    return -1;
}
int graph_out_uid(const Graph *g) {
    for (int i = 0; i < g->n; i++) if (g->nd[i].kind == N_OUT) return g->nd[i].uid;
    return 0;
}

static Prop *node_slot(Node *n, int k) {
    if (n->kind == N_XFORM || n->kind == N_MERGE) return k >= 0 && k < NODE_NP ? &n->mp[k] : NULL;
    if (n->kind == N_FX) return k >= 0 && k < 3 ? &n->fx.p[k] : NULL;
    return NULL;
}

const char *node_prop_label(const Node *n, int k) {
    static const char *XF[NODE_NP] = { "Offset X", "Offset Y", "Scale X %", "Scale Y %", "Rotation" };
    static const char *MG[NODE_NP] = { "Opacity %", "Offset X", "Offset Y", NULL, NULL };
    if (n->kind == N_XFORM) return k >= 0 && k < NODE_NP ? XF[k] : NULL;
    if (n->kind == N_MERGE) return k >= 0 && k < NODE_NP ? MG[k] : NULL;
    if (n->kind == N_FX) { const FxDef *d = &FX_DEFS[n->fx.type]; return k >= 0 && k < d->np ? d->pn[k] : NULL; }
    return NULL;
}

Prop *node_prop(Node *n, const char *name) {
    if (n->kind == N_GEN) return n->gen ? layer_prop(n->gen, name) : NULL;
    if (n->kind == N_XFORM) {
        static const char *nm[NODE_NP] = { "x", "y", "sx", "sy", "rot" };
        for (int k = 0; k < NODE_NP; k++) if (!strcmp(name, nm[k])) return &n->mp[k];
        if (!strcmp(name, "scalex")) return &n->mp[2];
        if (!strcmp(name, "scaley")) return &n->mp[3];
        if (!strcmp(name, "rotation")) return &n->mp[4];
        return NULL;
    }
    if (n->kind == N_MERGE) {
        if (!strcmp(name, "opacity") || !strcmp(name, "opa")) return &n->mp[0];
        if (!strcmp(name, "ox") || !strcmp(name, "x")) return &n->mp[1];
        if (!strcmp(name, "oy") || !strcmp(name, "y")) return &n->mp[2];
        return NULL;
    }
    if (n->kind == N_FX) {
        const FxDef *d = &FX_DEFS[n->fx.type];
        for (int k = 0; k < d->np; k++) if (!strcmp(name, d->pn[k])) return &n->fx.p[k];
    }
    return NULL;
}

void node_defaults(Project *P, Node *n) {
    for (int k = 0; k < NODE_NP; k++) prop_init(&n->mp[k], 0);
    n->on = true; n->blend = BM_NORMAL;
    switch (n->kind) {
    case N_XFORM: n->mp[2].base = 100; n->mp[3].base = 100; break;
    case N_MERGE: n->mp[0].base = 100; break;
    default: break;
    }
    (void)P;
}

Node *graph_add(Project *P, Graph *g, int kind, int sub) {
    if (!g || g->n >= MAX_NODES) { afx_log(1, "node limit reached (%d)", MAX_NODES); return NULL; }
    if (kind == N_IN || kind == N_OUT || kind < 0 || kind >= N_KIND_COUNT) return NULL;
    Node *n = &g->nd[g->n];
    memset(n, 0, sizeof *n);
    n->kind = (uint8_t)kind; n->uid = g->next_uid++;
    node_defaults(P, n);
    switch (kind) {
    case N_FX:
        if (sub < 0 || sub >= FX_COUNT) { g->next_uid--; return NULL; }
        fx_defaults(&n->fx, sub);
        snprintf(n->name, NAME_LEN, "%s", FX_DEFS[sub].name);
        break;
    case N_GEN: {
        if (sub < 0 || sub >= L_TYPE_COUNT || sub == L_ADJUST) { g->next_uid--; return NULL; }
        n->gen = calloc(1, sizeof(Layer));
        int save = P->next_uid;
        layer_init(P, n->gen, (LayerType)sub);
        P->next_uid = save;
        snprintf(n->gen->name, NAME_LEN, "%s", LAYER_TYPE_NAMES[sub]);
        snprintf(n->name, NAME_LEN, "%s", LAYER_TYPE_NAMES[sub]);
        break; }
    case N_XFORM: snprintf(n->name, NAME_LEN, "Transform"); break;
    case N_MERGE: snprintf(n->name, NAME_LEN, "Merge"); break;
    }
    g->n++;
    P->rev++;
    /* place it to the right of the lowest free spot */
    float mx = 0, my = 0;
    for (int i = 0; i < g->n - 1; i++) { if (g->nd[i].gx > mx) mx = g->nd[i].gx; if (g->nd[i].gy > my) my = g->nd[i].gy; }
    n->gx = mx > 1 ? mx - 0.5f : 1; n->gy = my + 1;
    return n;
}

/* does `from` (transitively) depend on `target`? */
static bool depends_on(Graph *g, Node *from, int target, int depth) {
    if (!from || depth > MAX_NODES + 2) return depth > MAX_NODES + 2;
    if (from->uid == target) return true;
    for (int k = 0; k < node_nin(from); k++) {
        Node *in = from->in[k] ? graph_find(g, from->in[k]) : NULL;
        if (in && depends_on(g, in, target, depth + 1)) return true;
    }
    return false;
}

bool graph_connect(Graph *g, int from_uid, int to_uid, int slot) {
    Node *f = graph_find(g, from_uid), *t = graph_find(g, to_uid);
    if (!f || !t || f == t || f->kind == N_OUT) return false;
    if (slot < 0 || slot >= node_nin(t)) return false;
    if (depends_on(g, f, t->uid, 0)) return false;       /* would create a cycle */
    t->in[slot] = from_uid;
    return true;
}
void graph_disconnect(Graph *g, int to_uid, int slot) {
    Node *t = graph_find(g, to_uid);
    if (t && slot >= 0 && slot < 2) t->in[slot] = 0;
}

void graph_remove(Project *P, Graph *g, int uid) {
    int idx = graph_index(g, uid);
    if (idx < 0) return;
    Node *r = &g->nd[idx];
    if (r->kind == N_IN || r->kind == N_OUT) return;
    int heal = r->in[0];
    for (int i = 0; i < g->n; i++) for (int k = 0; k < 2; k++) if (g->nd[i].in[k] == uid) g->nd[i].in[k] = (i == idx) ? 0 : heal;
    node_release(P, r);
    memmove(&g->nd[idx], &g->nd[idx + 1], (size_t)(g->n - idx - 1) * sizeof(Node));
    g->n--;
    memset(&g->nd[g->n], 0, sizeof(Node));
    P->rev++;
}

/* columns = distance from the output node, rows = order inside a column */
void graph_autolayout(Graph *g) {
    int depth[MAX_NODES]; int row_n[MAX_NODES + 2];
    for (int i = 0; i < g->n; i++) depth[i] = -1;
    int out = graph_index(g, graph_out_uid(g));
    if (out >= 0) depth[out] = 0;
    for (int pass = 0; pass < MAX_NODES; pass++) {
        bool ch = false;
        for (int i = 0; i < g->n; i++) {
            if (depth[i] < 0) continue;
            for (int k = 0; k < node_nin(&g->nd[i]); k++) {
                int j = g->nd[i].in[k] ? graph_index(g, g->nd[i].in[k]) : -1;
                if (j >= 0 && depth[j] < depth[i] + 1) { depth[j] = depth[i] + 1; ch = true; }
            }
        }
        if (!ch) break;
    }
    int maxd = 0;
    for (int i = 0; i < g->n; i++) if (depth[i] > maxd) maxd = depth[i];
    for (int i = 0; i < g->n; i++) if (depth[i] < 0) depth[i] = maxd + 1;      /* unconnected nodes go to the far left */
    memset(row_n, 0, sizeof row_n);
    for (int i = 0; i < g->n; i++) {
        int d = depth[i] > MAX_NODES ? MAX_NODES : depth[i];
        g->nd[i].gx = (float)(maxd + 1 - depth[i]);
        g->nd[i].gy = (float)row_n[d]++;
    }
}

static void prop_copy(Project *P, Prop *d, const Prop *s) {
    *d = *s;
    d->expr = s->expr ? strdup(s->expr) : NULL;
    d->ref = EXPR_NONE;
    if (s->fn) { d->fn = false; P->lossy++; }
}

Graph *graph_clone(Project *P, const Graph *src) {
    if (!src) return NULL;
    Graph *g = calloc(1, sizeof *g);
    *g = *src;
    for (int i = 0; i < g->n; i++) {
        Node *d = &g->nd[i]; const Node *s = &src->nd[i];
        d->res = NULL; d->stamp = 0; d->uses = 0;
        for (int k = 0; k < NODE_NP; k++) prop_copy(P, &d->mp[k], &s->mp[k]);
        for (int k = 0; k < 3; k++) prop_copy(P, &d->fx.p[k], &s->fx.p[k]);
        d->gen = NULL;
        if (s->gen) {
            d->gen = calloc(1, sizeof(Layer));
            *d->gen = *s->gen;
            d->gen->cache = NULL; d->gen->cache_key[0] = 0; d->gen->g = NULL;
            for (int k = 0; k < P_COUNT; k++) prop_copy(P, &d->gen->p[k], &s->gen->p[k]);
            for (int f = 0; f < d->gen->nfx; f++) for (int k = 0; k < 3; k++) prop_copy(P, &d->gen->fx[f].p[k], &s->gen->fx[f].p[k]);
        }
    }
    return g;
}
