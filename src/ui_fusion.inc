/* ui_fusion.inc -- Fusion node graph editor for the selected clip (Fusion tool).
 * Included by main.c after ui_sheets.inc.  Works in the portrait bottom sheet and in the landscape side panel. */

static int fus_uid, fus_luid, fus_pick = -1;
static bool fus_add_open, fus_rm_armed;
static float fus_pan;
static Rectangle fus_body;

static unsigned nid(const Layer *l, const Node *n, int k) { return 100000u + (unsigned)l->uid * 8192u + (unsigned)n->uid * 64u + (unsigned)k; }

static Color node_color(const Node *n) {
    switch (n->kind) {
    case N_IN: return C_GREEN;
    case N_OUT: return C_RED;
    case N_GEN: return C_ACC;
    case N_FX: return C_ACC2;
    case N_XFORM: return C_VIOLET;
    case N_MERGE: return C_TEXT;
    }
    return C_DIM;
}

/* turn the clip's plain effect chain into graph nodes:  Source -> fx1 -> fx2 -> ... -> Output */
static void fusion_enable(Layer *l) {
    Graph *g = graph_new();
    int prev = g->nd[0].uid;
    for (int i = 0; i < l->nfx; i++) {
        Node *n = graph_add(P, g, N_FX, l->fx[i].type);
        if (!n) break;
        for (int k = 0; k < 3; k++) prop_free(P, &n->fx.p[k]);
        n->fx = l->fx[i];
        memset(&l->fx[i], 0, sizeof l->fx[i]);
        graph_connect(g, prev, n->uid, 0);
        prev = n->uid;
    }
    graph_connect(g, prev, graph_out_uid(g), 0);
    l->nfx = 0;
    l->g = g;
    graph_autolayout(g);
    P->rev++;
    fus_uid = 0; fus_pick = -1; fus_pan = 0;
    set_status(0, "Fusion graph enabled");
}

/* the node that currently feeds the Output */
static Node *fusion_tail(Graph *g) {
    Node *out = graph_find(g, graph_out_uid(g));
    Node *t = out && out->in[0] ? graph_find(g, out->in[0]) : NULL;
    return t ? t : &g->nd[0];
}

/* put `nn` right after `after`: everything that read `after` now reads `nn` */
static void fusion_insert_after(Graph *g, Node *after, Node *nn) {
    int au = after->uid, nu = nn->uid;
    for (int i = 0; i < g->n; i++) {
        Node *c = &g->nd[i];
        if (c == nn) continue;
        for (int k = 0; k < node_nin(c); k++) if (c->in[k] == au) c->in[k] = nu;
    }
    graph_connect(g, au, nu, 0);
}

static void fusion_add(Layer *l, int kind, int sub) {
    Graph *g = l->g;
    Node *sel = graph_find(g, fus_uid);
    if (sel && sel->kind == N_OUT) sel = NULL;
    Node *after = sel ? sel : fusion_tail(g);
    if (kind == N_GEN) {
        Node *gn = graph_add(P, g, N_GEN, sub);
        if (!gn) { set_status(1, "Node limit reached (%d)", MAX_NODES); return; }
        Node *mg = graph_add(P, g, N_MERGE, 0);
        if (!mg) { graph_remove(P, g, gn->uid); set_status(1, "Node limit reached (%d)", MAX_NODES); return; }
        int gu = gn->uid, mu = mg->uid;
        Node *tail = fusion_tail(g);
        if (tail == mg || tail == gn) tail = &g->nd[0];
        int tu = tail->uid;
        Node *out = graph_find(g, graph_out_uid(g));
        graph_connect(g, tu, mu, 0);
        graph_connect(g, gu, mu, 1);
        if (out) { out->in[0] = 0; graph_connect(g, mu, out->uid, 0); }
        fus_uid = gu;
        if (sub == L_VIDEO) set_status(0, "Type or pick the video path in the node, it imports automatically");
    } else {
        Node *nn = graph_add(P, g, kind, sub);
        if (!nn) { set_status(1, "Node limit reached (%d)", MAX_NODES); return; }
        fusion_insert_after(g, after, nn);
        fus_uid = nn->uid;
    }
    graph_autolayout(g);
    P->rev++;
    fus_add_open = false; fus_pick = -1;
}

/* the graph canvas: wires + node cards, pan sideways by dragging */
static void fusion_canvas(Rectangle cv, Layer *l) {
    Graph *g = l->g;
    float bw = D(92), bh = D(40), cw = D(112), rh = D(54), padx = D(12), pady = D(12);
    float maxgx = 0;
    for (int i = 0; i < g->n; i++) if (g->nd[i].gx > maxgx) maxgx = g->nd[i].gx;
    float content = padx * 2 + maxgx * cw + bw, minpan = fminf(0, cv.width - content);
    float dx;
    if (hdrag(9100, cv, &dx)) fus_pan += dx;
    fus_pan = clampf(fus_pan, minpan, 0);
    rrect(cv, D(14), C_FIELD);
    Rectangle vis = GetCollisionRec(cv, fus_body);
    if (vis.width <= 1 || vis.height <= 1) return;
    EndScissorMode();
    BeginScissorMode((int)vis.x, (int)vis.y, (int)vis.width, (int)vis.height);
    clip_r = vis;
    Rectangle nr[MAX_NODES];
    for (int i = 0; i < g->n; i++) nr[i] = (Rectangle){ cv.x + padx + fus_pan + g->nd[i].gx * cw, cv.y + pady + g->nd[i].gy * rh, bw, bh };
    for (int i = 0; i < g->n; i++) {            /* wires */
        Node *n = &g->nd[i];
        for (int k = 0; k < node_nin(n); k++) {
            int j = n->in[k] ? graph_index(g, n->in[k]) : -1;
            if (j < 0) continue;
            Vector2 a = { nr[j].x + bw, nr[j].y + bh / 2 };
            Vector2 b = { nr[i].x, nr[i].y + (node_nin(n) == 2 ? (k == 0 ? bh * 0.3f : bh * 0.7f) : bh / 2) };
            bool hot = n->uid == fus_uid || g->nd[j].uid == fus_uid;
            DrawLineBezier(a, b, fmaxf(1.5f, D(hot ? 2.6f : 1.8f)), hot ? C_ACC : (Color){ 120, 120, 148, 255 });
        }
    }
    for (int i = 0; i < g->n; i++) {            /* node cards */
        Node *n = &g->nd[i];
        Rectangle r = nr[i];
        if (r.x + r.width < cv.x || r.x > cv.x + cv.width) continue;
        Color col = node_color(n);
        bool on = n->uid == fus_uid, h = held(r);
        rrect(r, D(10), h ? C_SURF3 : C_SURF2);
        rrect_line(r, D(10), on ? fmaxf(2, D(2.2f)) : 1.2f, on ? col : alpha_c(col, 0.55f));
        DrawRectangle((int)(r.x + D(6)), (int)(r.y + D(6)), (int)fmaxf(2, D(3)), (int)(r.height - D(12)), n->on ? col : alpha_c(col, 0.3f));
        dtext_fit(FB, n->name, (Rectangle){ r.x + D(14), r.y + D(4), r.width - D(20), D(18) }, T_SMALL, n->on ? C_TEXT : C_DIM, 0);
        dtext_fit(F, node_kind_label(n), (Rectangle){ r.x + D(14), r.y + D(21), r.width - D(20), D(14) }, T_TINY, C_DIM, 0);
        int nin = node_nin(n);
        for (int k = 0; k < nin; k++) DrawCircleV((Vector2){ r.x, r.y + (nin == 2 ? (k == 0 ? bh * 0.3f : bh * 0.7f) : bh / 2) }, D(4), n->in[k] ? C_ACC : C_SURF3);
        if (n->kind != N_OUT) DrawCircleV((Vector2){ r.x + bw, r.y + bh / 2 }, D(4), col);
        dbg_reg(n->name, r);
        if (tap(r)) { fus_uid = n->uid; fus_pick = -1; fus_rm_armed = false; }
    }
    EndScissorMode();
    BeginScissorMode((int)fus_body.x, (int)fus_body.y, (int)fus_body.width, (int)fus_body.height);
    clip_r = fus_body;
}

static float fusion_gen_rows(Layer *l, Node *n, float x, float y, float w) {
    Layer *q = n->gen;
    if (!q) return y;
    if (q->type == L_TEXT) {
        unsigned id = nid(l, n, 1) * 8;
        if (field_row(&y, x, w, "Text", q->text)) edit_begin(id, "Text", q->text, false, false);
        if (edit_done(id)) { snprintf(q->text, TEXT_LEN, "%s", te.done_buf); layer_free_cache(q); P->rev++; }
    }
    if (q->type == L_SVG || q->type == L_IMAGE || q->type == L_VIDEO) {
        unsigned id = nid(l, n, 2) * 8;
        const char *base = strrchr(q->path, '/'); base = base ? base + 1 : q->path;
        if (field_row(&y, x, w, "File", base[0] ? base : "(tap to type a path)")) edit_begin(id, "File path", q->path, false, false);
        if (edit_done(id)) { snprintf(q->path, PATH_LEN, "%s", te.done_buf); layer_free_cache(q); P->rev++; }
    }
    if (q->type == L_VIDEO) {
        unsigned id = nid(l, n, 3);
        float v = q->trim, nv;
        if (slider_row(id, "Trim (s)", v, 0, 60, 0, 36000, x, y, w, &nv)) { q->trim = nv; P->rev++; } y += D(68);
        v = q->speed;
        if (slider_row(id + 1, "Speed", v, 0.25f, 4, 0.05f, 16, x, y, w, &nv)) { q->speed = nv; P->rev++; } y += D(68);
    }
    if (q->type == L_SHAPE) {
        section(&y, x, "SHAPE");
        int hit = chip_grid(&y, x, w, SHAPE_NAMES, SH_COUNT, 4, q->shape);
        if (hit >= 0 && hit != q->shape) { q->shape = (uint8_t)hit; q->p[P_A].base = (hit == SH_POLYGON || hit == SH_STAR) ? 5 : 0; q->p[P_B].base = 0.45f; P->rev++; }
        y += D(4);
    }
    if (q->type != L_ADJUST && q->type != L_SVG && q->type != L_IMAGE && q->type != L_VIDEO) {
        y = color_row(y, x, w, q->type == L_SOLID ? "Color" : "Fill", q->fill, nid(l, n, 4), true);
        if (q->type == L_SHAPE) y = color_row(y, x, w, "Stroke", q->stroke, nid(l, n, 5), true);
    }
    static const int PIDS[8] = { P_X, P_Y, P_SX, P_SY, P_ROT, P_OPA, P_W, P_H };
    static const int PIDS2[3] = { P_A, P_B, P_C };
    int nidx = (int)(n - l->g->nd);
    for (int pass = 0; pass < 2; pass++) {
        int cnt = pass == 0 ? 8 : 3;
        for (int k = 0; k < cnt; k++) {
            int pid = pass == 0 ? PIDS[k] : PIDS2[k];
            const char *lab = prop_label(q, pid);
            if (!lab) { if (pass == 0 && pid <= P_OPA) lab = PROP_NAMES[pid]; else continue; }
            float mn, mx, hmn, hmx;
            prop_slider_range(q, pid, &mn, &mx, &hmn, &hmx);
            y = prop_row(nid(l, n, 8 + pid), q, &q->p[pid], 300 + nidx * 8 + pid % 8 + (pid / 8) * 64, lab, mn, mx, hmn, hmx, x, y, w, NULL);
        }
    }
    return y;
}

static float fusion_node_panel(Layer *l, Node *n, float x, float y, float w) {
    Graph *g = l->g;
    int nidx = (int)(n - g->nd);
    Color col = node_color(n);
    /* header: enable / name / delete */
    Rectangle hr = { x, y, w, D(46) };
    rrect(hr, D(12), C_SURF2);
    if (icon_btn((Rectangle){ hr.x + D(2), hr.y, D(44), D(46) }, n->on ? IC_EYE : IC_EYE_OFF, D(22), n->on ? C_TEXT : C_DIM, NULL)) { n->on = !n->on; P->rev++; }
    bool removable = n->kind != N_IN && n->kind != N_OUT;
    dtext_fit(FB, n->name, (Rectangle){ hr.x + D(50), hr.y, hr.width - (removable ? D(100) : D(60)), D(46) }, T_BODY, col, 0);
    if (tap((Rectangle){ hr.x + D(50), hr.y, hr.width - (removable ? D(100) : D(60)), D(46) })) edit_begin(nid(l, n, 0) * 8, "Node name", n->name, false, false);
    if (edit_done(nid(l, n, 0) * 8)) { snprintf(n->name, NAME_LEN, "%s", te.done_buf); P->rev++; }
    bool del = removable && icon_btn((Rectangle){ hr.x + hr.width - D(46), hr.y, D(44), D(46) }, IC_TRASH, D(20), C_RED, NULL);
    y += D(52);
    if (del) { graph_remove(P, g, n->uid); fus_uid = 0; graph_autolayout(g); return y; }

    /* inputs */
    int nin = node_nin(n);
    for (int k = 0; k < nin; k++) {
        Node *in = n->in[k] ? graph_find(g, n->in[k]) : NULL;
        char lab[24], val[NAME_LEN + 8];
        snprintf(lab, sizeof lab, nin == 2 ? (k == 0 ? "Background" : "Foreground") : "Input");
        snprintf(val, sizeof val, "%s", in ? in->name : "(none)");
        if (field_row(&y, x, w, lab, val)) fus_pick = fus_pick == k ? -1 : k;
        if (fus_pick == k) {
            static char labs[MAX_NODES + 1][NAME_LEN + 4]; const char *lp[MAX_NODES + 1]; int ids[MAX_NODES + 1], cnt = 0, cur = 0;
            snprintf(labs[cnt], sizeof labs[cnt], "None"); lp[cnt] = labs[cnt]; ids[cnt++] = 0;
            for (int i = 0; i < g->n; i++) {
                Node *c = &g->nd[i];
                if (c == n || c->kind == N_OUT) continue;
                snprintf(labs[cnt], sizeof labs[cnt], "%s", c->name); lp[cnt] = labs[cnt]; ids[cnt] = c->uid;
                if (c->uid == n->in[k]) cur = cnt;
                cnt++;
            }
            int hit = chip_grid(&y, x, w, lp, cnt, 3, cur);
            if (hit >= 0) {
                if (ids[hit] == 0) graph_disconnect(g, n->uid, k);
                else if (!graph_connect(g, ids[hit], n->uid, k)) set_status(1, "That connection would make a loop");
                graph_autolayout(g); P->rev++; fus_pick = -1;
            }
            y += D(4);
        }
    }
    if (nin) y += D(2);

    /* parameters */
    switch (n->kind) {
    case N_FX: {
        const FxDef *d = &FX_DEFS[n->fx.type];
        for (int k = 0; k < d->np; k++)
            y = prop_row(nid(l, n, 8 + k), l, &n->fx.p[k], 300 + nidx * 8 + k, d->pn[k], d->mn[k], d->mx[k], d->mn[k] - 1e4f, d->mx[k] + 1e4f, x, y, w, NULL);
        if (n->fx.type == FX_TINT) y = color_row(y, x, w, "Tint color", n->fx.color, nid(l, n, 20), false);
        break; }
    case N_XFORM: {
        static const char *LB[NODE_NP] = { "Offset X", "Offset Y", "Scale X %", "Scale Y %", "Rotation" };
        float W = (float)P->width, H = (float)P->height;
        float mn[NODE_NP] = { -W, -H, 0, 0, -360 }, mx[NODE_NP] = { W, H, 400, 400, 360 };
        for (int k = 0; k < NODE_NP; k++)
            y = prop_row(nid(l, n, 8 + k), l, &n->mp[k], 300 + nidx * 8 + k, LB[k], mn[k], mx[k], -1e5f, 1e5f, x, y, w, NULL);
        break; }
    case N_MERGE: {
        float W = (float)P->width, H = (float)P->height;
        y = prop_row(nid(l, n, 8), l, &n->mp[0], 300 + nidx * 8, "Opacity %", 0, 100, 0, 100, x, y, w, NULL);
        y = prop_row(nid(l, n, 9), l, &n->mp[1], 300 + nidx * 8 + 1, "Offset X", -W / 2, W / 2, -1e5f, 1e5f, x, y, w, NULL);
        y = prop_row(nid(l, n, 10), l, &n->mp[2], 300 + nidx * 8 + 2, "Offset Y", -H / 2, H / 2, -1e5f, 1e5f, x, y, w, NULL);
        section(&y, x, "BLEND");
        int hit = chip_grid(&y, x, w, BLEND_NAMES, BM_COUNT, 4, n->blend);
        if (hit >= 0 && hit != n->blend) { n->blend = (uint8_t)hit; P->rev++; }
        break; }
    case N_GEN: y = fusion_gen_rows(l, n, x, y, w); break;
    case N_IN: dtext_fit(F, "The clip itself (video, image, shape...) before any node.", (Rectangle){ x, y, w, D(26) }, T_SMALL, C_DIM, 0); y += D(30); break;
    case N_OUT: dtext_fit(F, "Whatever feeds this node is what the timeline shows.", (Rectangle){ x, y, w, D(26) }, T_SMALL, C_DIM, 0); y += D(30); break;
    }
    return y;
}

static void sheet_fusion(Rectangle body) {
    Scroll *s = &scr[SC_FUSION];
    Layer *l = sel_layer();
    if (!l) { empty_hint(body, "Select a layer on the timeline"); return; }
    if (fus_luid != l->uid) { fus_luid = l->uid; fus_uid = 0; fus_add_open = false; fus_pick = -1; fus_pan = 0; fus_rm_armed = false; }
    float pad = D(16), x = body.x + pad, w = body.width - 2 * pad, y = scroll_begin(body, s) + D(4);
    fus_body = body;
    if (!l->g) {
        dtext_fit(F, "Fusion turns this clip into a node graph:", (Rectangle){ x, y + D(4), w, D(24) }, T_BODY, C_TEXT, 0); y += D(28);
        dtext_fit(F, "Source  >  effects / transforms / merge  >  Output", (Rectangle){ x, y, w, D(24) }, T_SMALL, C_DIM, 0); y += D(30);
        dtext_fit(F, "Stack generators on top with Merge, mix and match.", (Rectangle){ x, y, w, D(24) }, T_SMALL, C_DIM, 0); y += D(34);
        if (pill((Rectangle){ x, y, w, D(46) }, l->nfx ? "Enable Fusion (keeps your effects as nodes)" : "Enable Fusion graph", true, C_ACC)) fusion_enable(l);
        y += D(60);
        scroll_end(body, s, y);
        return;
    }
    Graph *g = l->g;
    if (fus_uid && !graph_find(g, fus_uid)) fus_uid = 0;
    float rows = 1; for (int i = 0; i < g->n; i++) if (g->nd[i].gy + 1 > rows) rows = g->nd[i].gy + 1;
    float ch = clampf(rows * D(54) + D(24), D(110), D(290));
    fusion_canvas((Rectangle){ x, y, w, ch }, l);
    y += ch + D(10);
    float bw3 = (w - 2 * D(8)) / 3;
    if (pill((Rectangle){ x, y, bw3, D(40) }, fus_add_open ? "Close" : "+ Node", fus_add_open, C_ACC)) fus_add_open = !fus_add_open;
    if (pill((Rectangle){ x + bw3 + D(8), y, bw3, D(40) }, "Tidy", false, C_ACC)) { graph_autolayout(g); fus_pan = 0; }
    if (pill((Rectangle){ x + 2 * (bw3 + D(8)), y, bw3, D(40) }, fus_rm_armed ? "Sure?" : "Remove", fus_rm_armed, C_RED)) {
        if (fus_rm_armed) { graph_free(P, l->g); l->g = NULL; P->rev++; fus_uid = 0; fus_rm_armed = false; scroll_end(body, s, y); return; }
        fus_rm_armed = true;
    }
    y += D(52);
    if (fus_add_open) {
        static const char *KN[2] = { "Transform", "Merge" };
        section(&y, x, "TRANSFORM AND MERGE");
        int hit = chip_grid(&y, x, w, KN, 2, 3, -1);
        if (hit == 0) fusion_add(l, N_XFORM, 0); else if (hit == 1) fusion_add(l, N_MERGE, 0);
        static const int GT[8] = { L_VIDEO, L_IMAGE, L_SVG, L_SHAPE, L_TEXT, L_SOLID, L_ASTEROID, L_STARS };
        const char *gl[8]; for (int i = 0; i < 8; i++) gl[i] = LTYPE_UI[GT[i]];
        section(&y, x, "GENERATORS (added on top with a Merge)");
        hit = chip_grid(&y, x, w, gl, 8, 3, -1);
        if (hit >= 0 && fus_add_open) fusion_add(l, N_GEN, GT[hit]);
        if (fus_add_open) {
            const char *names[FX_COUNT];
            for (int i = 0; i < FX_COUNT; i++) names[i] = FX_DEFS[i].name;
            section(&y, x, "EFFECTS");
            hit = chip_grid(&y, x, w, names, FX_COUNT, 3, -1);
            if (hit >= 0) fusion_add(l, N_FX, hit);
        }
        y += D(6);
    } else {
        Node *n = graph_find(g, fus_uid);
        if (!n) { dtext_fit(F, "Tap a node to edit it", (Rectangle){ x, y + D(4), w, D(30) }, T_BODY, C_DIM, 1); y += D(40); }
        else y = fusion_node_panel(l, n, x, y, w);
    }
    scroll_end(body, s, y);
}
