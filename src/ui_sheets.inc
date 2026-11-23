/* ui_sheets.inc -- tool sheets (Add, Transform, Style, Effects, Animate, Layers, Canvas, Script, Export).
 * Included by main.c after ui_core.inc. */

static void open_script_editor(void);
static void open_media_picker(void);
static void save_project(void);

/* ============================== helpers ============================== */
static Layer *sel_layer(void) { return (sel >= 0 && sel < P->nlayers) ? &P->layers[sel] : NULL; }
static float frame_snap(float t) { return roundf(t * P->fps) / P->fps; }
static void set_time(float t) { cur_t = clampf(frame_snap(t), 0, fmaxf(0, P->duration - 1.0f / P->fps)); }
static void set_prop_val(Prop *p, float t, float v) { if (p->nkeys > 0) prop_set_key(p, t, v, -1); else p->base = v; P->rev++; }
static unsigned rid(const Layer *l, int slot) { return (unsigned)l->uid * 64u + (unsigned)slot; }
static const char *EASE_ABBR[EASE_COUNT] = { "Linear", "Ease in", "Ease out", "Ease in-out", "Hold" };
static const char *LTYPE_UI[9] = { "Solid", "Shape", "Text", "SVG", "Image", "Asteroid", "Stars", "Adjustment", "Video" };
static const int LTYPE_ICON[9] = { IC_SOLID, IC_SHAPE, IC_TEXT, IC_SVG, IC_IMAGE, IC_ASTEROID, IC_STARS, IC_ADJUST, IC_VIDEO };
static const Color LTYPE_COL[9] = { { 120, 132, 190, 255 }, { 255, 154, 46, 255 }, { 235, 235, 248, 255 }, { 110, 210, 130, 255 },
                                    { 76, 201, 240, 255 }, { 214, 168, 120, 255 }, { 176, 134, 255, 255 }, { 255, 118, 196, 255 }, { 255, 98, 98, 255 } };

static void empty_hint(Rectangle body, const char *msg) {
    dtext_fit(F, msg, (Rectangle){ body.x + D(20), body.y + D(30), body.width - D(40), D(40) }, T_BODY, C_DIM, 1);
}
static void section(float *y, float x, const char *t) { dtext(FB, t, x, *y + D(10), T_SMALL * 0.95f, C_DIM); *y += D(32); }
static int chip_grid(float *y, float x, float w, const char *const *lab, int n, int cols, int selected) {
    float g = D(8), cw = (w - (cols - 1) * g) / cols, ch = D(40); int hit = -1;
    for (int i = 0; i < n; i++) {
        Rectangle r = { x + (i % cols) * (cw + g), *y + (i / cols) * (ch + g), cw, ch };
        if (chip(r, lab[i], i == selected)) hit = i;
    }
    *y += ((n + cols - 1) / cols) * (ch + g);
    return hit;
}
static bool field_row(float *y, float x, float w, const char *label, const char *val) {
    Rectangle rl = { x, *y, w * 0.28f, D(46) }, rv = { x + w * 0.28f, *y + D(3), w * 0.72f, D(40) };
    dtext_fit(F, label, rl, T_BODY, C_TEXT, 0);
    rrect(rv, D(10), C_FIELD);
    dtext_fit(F, val, inset_xy(rv, D(12), 0), T_BODY, C_TEXT, 0);
    dbg_reg(label, rv);
    *y += D(52);
    return tap(rv);
}
static bool toggle_row(float *y, float x, float w, const char *label, bool on) {
    Rectangle r = { x, *y, w, D(46) };
    dtext_fit(F, label, (Rectangle){ r.x, r.y, w - D(60), r.height }, T_BODY, C_TEXT, 0);
    dbg_reg(label, r);
    *y += D(50);
    return toggle(r, on);
}
/* plain slider row (label + value pill on top, slider underneath). returns true if changed; *out = new value */
static bool slider_row(unsigned id, const char *label, float v, float smn, float smx, float hmn, float hmx, float x, float y, float w, float *out) {
    Rectangle rv = { x + w - D(84), y + D(3), D(84), D(28) }, rl = { x, y, w - D(94), D(34) }, rs = { x, y + D(32), w, D(32) };
    dtext_fit(F, label, rl, T_BODY, C_TEXT, 0);
    char b[32]; fmt_num(b, v);
    rrect(rv, D(8), C_FIELD); rrect_line(rv, D(8), 1.2f, C_LINE);
    dtext_fit(F, b, rv, T_SMALL * 1.1f, C_TEXT, 1);
    bool ch = false; float nv = clampf(v, smn, smx);
    if (slider(id * 8 + 1, rs, &nv, smn, smx, C_ACC)) {
        float q = (smx - smn) > 400 ? 1 : (smx - smn) > 40 ? 0.1f : 0.01f;
        *out = roundf(nv / q) * q; ch = true;
    }
    if (tap(rv)) edit_begin(id * 8 + 2, label, b, true, false);
    if (edit_done(id * 8 + 2)) { *out = clampf(strtof(te.done_buf, NULL), hmn, hmx); ch = true; }
    return ch;
}

static void prop_slider_range(const Layer *l, int pid, float *mn, float *mx, float *hmn, float *hmx) {
    float W = (float)P->width, H = (float)P->height;
    *mn = 0; *mx = 100; *hmn = -1e5f; *hmx = 1e5f;
    if (l->type == L_STARS) {
        switch (pid) {
        case P_ROT: *mn = 0; *mx = 360; break;
        case P_OPA: *hmn = 0; *hmx = 100; break;
        case P_W: *mx = 100; *hmn = 0; break;
        case P_A: *mx = 500; *hmn = 0; break;
        case P_B: *mx = 300; *hmn = 0; break;
        case P_C: *mn = 0.5f; *mx = 8; *hmn = 0; break;
        }
        return;
    }
    switch (pid) {
    case P_X: *mn = -W * 0.25f; *mx = W * 1.25f; break;
    case P_Y: *mn = -H * 0.25f; *mx = H * 1.25f; break;
    case P_SX: case P_SY: *mn = 0; *mx = 400; break;
    case P_ROT: *mn = -360; *mx = 360; break;
    case P_OPA: *mn = 0; *mx = 100; *hmn = 0; *hmx = 100; break;
    case P_AX: *mn = -W / 2; *mx = W / 2; break;
    case P_AY: *mn = -H / 2; *mx = H / 2; break;
    case P_W:
        if (l->type == L_ASTEROID) { *mn = 4; *mx = fmaxf(W, H) / 2; *hmn = 1; }
        else { *mn = 0; *mx = W * 2; *hmn = 0; }
        break;
    case P_H: *mn = 0; *mx = H * 2; *hmn = 0; break;
    case P_A:
        if (l->type == L_TEXT) { *mn = 4; *mx = 300; *hmn = 1; }
        else if (l->type == L_SHAPE) { if (l->shape == SH_RECT) { *mn = 0; *mx = 200; } else { *mn = 3; *mx = 12; } }
        else if (l->type == L_ASTEROID) { *mn = 0; *mx = 100; }
        break;
    case P_B: if (l->type == L_SHAPE) { *mn = 0.05f; *mx = 0.95f; } else { *mn = 0; *mx = 1; } break;
    case P_C: if (l->type == L_SHAPE) { *mn = 0; *mx = 40; *hmn = 0; } else { *mn = 0; *mx = 1; } break;
    }
}

static void key_toggle(Prop *p, float t, int ki, Prop *link) {
    int kl = link ? prop_key_at(link, t, 0.5f / P->fps) : -1;
    if (ki >= 0) {
        if (p->nkeys == 1) p->base = p->keys[0].v;
        prop_del_key(p, ki);
        if (link && kl >= 0) { if (link->nkeys == 1) link->base = link->keys[0].v; prop_del_key(link, kl); }
    } else {
        prop_set_key(p, t, prop_keyed(p, t), EASE_LINEAR);
        if (link && kl < 0) prop_set_key(link, t, prop_keyed(link, t), EASE_LINEAR);
    }
    P->rev++;
}

/* one animatable property: label + value pill + keyframe diamond, slider underneath. returns y after the row */
static float prop_row(unsigned id, Layer *l, Prop *p, int pid, const char *label, float smn, float smx, float hmn, float hmx, float x, float y, float w, Prop *link) {
    float t = cur_t, v = prop_eval(P, l, p, pid, t);
    int ki = prop_key_at(p, t, 0.5f / P->fps);
    bool has_expr = p->expr != NULL || p->fn;
    Rectangle rk = { x + w - D(40), y, D(40), D(34) };
    Rectangle rv = { rk.x - D(8) - D(78), y + D(3), D(78), D(28) };
    Rectangle rl = { x, y, rv.x - x - D(6), D(34) };
    Rectangle rs = { x, y + D(32), w, D(32) };
    dtext_fit(F, label, rl, T_BODY, C_TEXT, 0);
    Color border = has_expr ? C_VIOLET : p->nkeys ? C_ACC2 : C_LINE;
    rrect(rv, D(8), C_FIELD); rrect_line(rv, D(8), 1.2f, border);
    char b[32]; fmt_num(b, v);
    dtext_fit(F, b, rv, T_SMALL * 1.1f, C_TEXT, 1);
    bool ch = false; float nv = v;
    if (tap(rv)) edit_begin(id * 8 + 2, label, b, true, false);
    if (edit_done(id * 8 + 2)) { nv = clampf(strtof(te.done_buf, NULL), hmn, hmx); ch = true; }
    if (has_expr) {
        rrect((Rectangle){ rs.x + D(10), rs.y + rs.height / 2 - D(2), rs.width - D(20), D(4) }, D(2), alpha_c(C_VIOLET, 0.35f));
        dtext_fit(F, "\xC6\x92  driven by expression", (Rectangle){ rs.x + D(10), rs.y, rs.width, rs.height }, T_SMALL, C_VIOLET, 0);
    } else {
        float sv = clampf(v, smn, smx);
        if (slider(id * 8 + 1, rs, &sv, smn, smx, p->nkeys ? C_ACC2 : C_ACC)) {
            float q = (smx - smn) > 400 ? 1 : (smx - smn) > 40 ? 0.1f : 0.01f;
            nv = roundf(sv / q) * q; ch = true;
        }
    }
    if (ch) { set_prop_val(p, t, nv); if (link) set_prop_val(link, t, nv); }
    if (icon_btn(rk, ki >= 0 ? IC_DIA_FILL : IC_DIA, D(22), ki >= 0 ? C_ACC2 : C_DIM, NULL)) key_toggle(p, t, ki, link);
    return y + D(68);
}

static unsigned color_open;
static float color_row(float y, float x, float w, const char *label, uint8_t *c, unsigned id, bool alpha) {
    Rectangle r = { x, y, w, D(48) };
    dtext_fit(F, label, (Rectangle){ r.x, r.y, w * 0.3f, r.height }, T_BODY, C_TEXT, 0);
    Rectangle sw_ = { r.x + w * 0.3f, r.y + D(5), w * 0.7f, r.height - D(10) };
    rrect(sw_, D(10), (Color){ c[0], c[1], c[2], 255 }); rrect_line(sw_, D(10), D(1.5f), C_LINE);
    char hx[24]; snprintf(hx, sizeof hx, "#%02X%02X%02X%02X", c[0], c[1], c[2], c[3]);
    dtext_fit(FM, hx, sw_, T_SMALL, (c[0] * 3 + c[1] * 6 + c[2]) / 10 > 140 ? BLACK : WHITE, 1);
    dbg_reg(label, sw_);
    if (tap(sw_)) color_open = color_open == id ? 0 : id;
    y += r.height + D(6);
    if (color_open == id) {
        static const uint8_t PRE[12][3] = { { 255, 255, 255 }, { 0, 0, 0 }, { 240, 84, 84 }, { 255, 154, 46 }, { 255, 220, 80 }, { 110, 210, 130 },
                                            { 76, 201, 240 }, { 80, 110, 240 }, { 170, 100, 240 }, { 255, 110, 190 }, { 120, 120, 135 }, { 24, 28, 52 } };
        float g = D(6), sz = (w - 5 * g) / 6;
        for (int i = 0; i < 12; i++) {
            Rectangle pr = { x + (i % 6) * (sz + g), y + (i / 6) * (sz * 0.75f + g), sz, sz * 0.75f };
            rrect(pr, D(8), (Color){ PRE[i][0], PRE[i][1], PRE[i][2], 255 });
            if (tap(pr)) { c[0] = PRE[i][0]; c[1] = PRE[i][1]; c[2] = PRE[i][2]; P->rev++; }
        }
        y += 2 * (sz * 0.75f + g) + D(4);
        static const char *ch[4] = { "R", "G", "B", "A" };
        static const Color cc[4] = { { 240, 84, 84, 255 }, { 110, 210, 130, 255 }, { 90, 130, 255, 255 }, { 200, 200, 210, 255 } };
        for (int k = 0; k < (alpha ? 4 : 3); k++) {
            float v = c[k];
            dtext(FB, ch[k], x + D(2), y + (D(40) - T_BODY) / 2, T_BODY, C_DIM);
            Rectangle sr = { x + D(24), y, w - D(24) - D(44), D(40) };
            if (slider(id * 8 + (unsigned)k + 3, sr, &v, 0, 255, cc[k])) { c[k] = (uint8_t)(v + 0.5f); P->rev++; }
            char b[8]; snprintf(b, sizeof b, "%d", c[k]);
            dtext_fit(F, b, (Rectangle){ sr.x + sr.width, y, D(44), D(40) }, T_SMALL, C_TEXT, 2);
            y += D(42);
        }
        y += D(4);
    }
    return y;
}

/* ============================== ADD ============================== */
static void add_layer(int type) {
    Layer *n = project_add(P, (LayerType)type, NULL);
    if (!n) return;
    sel = P->nlayers - 1;
    float t = frame_snap(cur_t);
    if (t > 0.05f && t < P->duration - 0.2f) n->tin = t;
    P->rev++;
    tool = portrait ? TOOL_NONE : TOOL_TRANSFORM;
    set_status(0, "Added %s", n->name);
    if (type == L_SVG || type == L_IMAGE || type == L_VIDEO) open_media_picker();
}
static void sheet_add(Rectangle body) {
    Scroll *s = &scr[SC_ADD];
    float pad = D(16), x = body.x + pad, w = body.width - 2 * pad, y = scroll_begin(body, s) + D(8);
    static const int ORDER[9] = { L_VIDEO, L_IMAGE, L_TEXT, L_SHAPE, L_SOLID, L_ASTEROID, L_STARS, L_SVG, L_ADJUST };
    static const char *SUB[9] = { "Clip", "Photo", "Title", "Shape", "Color", "Rock", "Starfield", "Vector", "Effects" };
    int cols = body.width > D(520) ? 5 : 4;
    float g = D(10), tw_ = (w - (cols - 1) * g) / cols, th = tw_ + D(30);
    for (int i = 0; i < 9; i++) {
        int ty = ORDER[i];
        Rectangle t = { x + (i % cols) * (tw_ + g), y + (i / cols) * (th + g), tw_, th };
        Rectangle ic = { t.x, t.y, t.width, t.width };
        bool h = held(t);
        rrect(ic, D(16), h ? C_SURF3 : C_SURF2);
        Color col = LTYPE_COL[ty];
        DrawCircleV((Vector2){ ic.x + ic.width / 2, ic.y + ic.height / 2 }, ic.width * 0.30f, alpha_c(col, 0.18f));
        icon_c(LTYPE_ICON[ty], ic, ic.width * 0.40f, col);
        dtext_fit(FB, ty == L_ADJUST ? "Adjust" : LTYPE_UI[ty], (Rectangle){ t.x, t.y + t.width + D(2), t.width, D(18) }, T_SMALL, C_TEXT, 1);
        dtext_fit(F, SUB[i], (Rectangle){ t.x, t.y + t.width + D(17), t.width, D(14) }, T_TINY, C_DIM, 1);
        dbg_reg(LTYPE_UI[ty], t);
        if (tap(t)) add_layer(ty);
    }
    y += ((9 + cols - 1) / cols) * (th + g) + D(8);
    dtext_fit(F, "New layers start at the playhead.", (Rectangle){ x, y, w, D(22) }, T_SMALL, C_DIM, 0);
    y += D(26);
    scroll_end(body, s, y);
}

/* ============================== TRANSFORM ============================== */
static bool uniform_scale = true;
static void sheet_transform(Rectangle body) {
    Scroll *s = &scr[SC_TRANSFORM];
    Layer *l = sel_layer();
    if (!l) { empty_hint(body, "Select a layer on the timeline"); return; }
    float pad = D(16), x = body.x + pad, w = body.width - 2 * pad, y = scroll_begin(body, s) + D(4);
    static const int PIDS[8] = { P_X, P_Y, P_SX, P_SY, P_ROT, P_OPA, P_AX, P_AY };
    bool has_scale = prop_label(l, P_SX) != NULL;
    if (has_scale && toggle_row(&y, x, w, "Uniform scale", uniform_scale)) uniform_scale = !uniform_scale;
    for (int k = 0; k < 8; k++) {
        int pid = PIDS[k];
        const char *lab = prop_label(l, pid);
        if (!lab) continue;
        Prop *link = NULL;
        if (uniform_scale && pid == P_SY) continue;
        if (uniform_scale && pid == P_SX) { link = &l->p[P_SY]; lab = "Scale %"; }
        float mn, mx, hmn, hmx;
        prop_slider_range(l, pid, &mn, &mx, &hmn, &hmx);
        y = prop_row(rid(l, pid), l, &l->p[pid], pid, lab, mn, mx, hmn, hmx, x, y, w, link);
    }
    y += D(6);
    section(&y, x, "TIMING");
    float out = project_layer_out(P, l), nv;
    if (slider_row(rid(l, 49), "Start (s)", l->tin, 0, P->duration, 0, P->duration, x, y, w, &nv)) { l->tin = clampf(nv, 0, out - 0.05f); P->rev++; }
    y += D(68);
    if (slider_row(rid(l, 50), "End (s)", out, 0, P->duration, 0, P->duration, x, y, w, &nv)) { float o = fmaxf(nv, l->tin + 0.05f); l->tout = o >= P->duration - 1e-3f ? -1 : o; P->rev++; }
    y += D(68);
    scroll_end(body, s, y);
}

/* ============================== STYLE ============================== */
static void sheet_style(Rectangle body) {
    Scroll *s = &scr[SC_STYLE];
    Layer *l = sel_layer();
    if (!l) { empty_hint(body, "Select a layer on the timeline"); return; }
    float pad = D(16), x = body.x + pad, w = body.width - 2 * pad, y = scroll_begin(body, s) + D(4);
    if (field_row(&y, x, w, "Name", l->name)) edit_begin(rid(l, 48) * 8, "Layer name", l->name, false, false);
    if (edit_done(rid(l, 48) * 8)) { snprintf(l->name, NAME_LEN, "%s", te.done_buf); P->rev++; }
    if (l->type == L_TEXT) {
        if (field_row(&y, x, w, "Text", l->text)) edit_begin(rid(l, 51) * 8, "Text", l->text, false, false);
        if (edit_done(rid(l, 51) * 8)) { snprintf(l->text, TEXT_LEN, "%s", te.done_buf); layer_free_cache(l); P->rev++; }
    }
    if (l->type == L_SVG || l->type == L_IMAGE || l->type == L_VIDEO) {
        const char *base = strrchr(l->path, '/'); base = base ? base + 1 : l->path;
        if (field_row(&y, x, w, "File", base[0] ? base : "(none)")) open_media_picker();
    }
    if (l->type == L_VIDEO) {
        Media *m = l->path[0] ? media_get(l->path, false) : NULL;
        int ms = m ? media_state(m) : 0; float vdur = 0, vfps = 0; int vw = 0, vh = 0;
        if (m && ms == 2) media_info(m, &vw, &vh, &vfps, &vdur);
        char info[160];
        if (!l->path[0]) snprintf(info, sizeof info, "No video yet - tap File");
        else if (ms == 1) { int tot; int n = media_progress(m, &tot); snprintf(info, sizeof info, "Importing proxy... %d / %d frames", n, tot > 0 ? tot : n); }
        else if (ms == 2) snprintf(info, sizeof info, "Ready - %dx%d proxy, %.3g fps, %.1f s", vw, vh, (double)vfps, (double)vdur);
        else if (ms == 3) snprintf(info, sizeof info, "Import failed: %s", media_error(m));
        else snprintf(info, sizeof info, "Not imported yet");
        dtext_fit(F, info, (Rectangle){ x, y, w, D(24) }, T_SMALL, ms == 3 ? C_RED : ms == 2 ? C_GREEN : C_DIM, 0); y += D(28);
        if (ms == 3 || (ms == 0 && l->path[0])) { if (pill((Rectangle){ x, y, w, D(40) }, ms == 3 ? "Retry import" : "Import now", true, C_ACC)) { if (m && ms == 3) media_retry(m); media_start(m ? m : media_get(l->path, true)); } y += D(48); }
        section(&y, x, "VIDEO");
        float nv, tmax = vdur > 0 ? vdur : 60;
        if (slider_row(rid(l, 56) * 8, "Start in source (s)", l->trim, 0, tmax, 0, 36000, x, y, w, &nv)) { l->trim = nv; P->rev++; } y += D(68);
        if (slider_row(rid(l, 57) * 8, "Speed", l->speed, 0.25f, 4, 0.05f, 16, x, y, w, &nv)) { l->speed = nv; P->rev++; } y += D(68);
        if (slider_row(rid(l, 58) * 8, "Volume %", l->vol * 100, 0, 100, 0, 100, x, y, w, &nv)) { l->vol = nv / 100; P->rev++; } y += D(68);
        static const char *FIT[2] = { "Fill (crop)", "Fit (bars)" };
        int hit = chip_grid(&y, x, w, FIT, 2, 2, l->fitm);
        if (hit >= 0 && hit != l->fitm) { l->fitm = (uint8_t)hit; P->rev++; }
        y += D(6);
    }
    if (l->type == L_SHAPE) {
        section(&y, x, "SHAPE");
        int hit = chip_grid(&y, x, w, SHAPE_NAMES, SH_COUNT, 4, l->shape);
        if (hit >= 0 && hit != l->shape) {
            l->shape = (uint8_t)hit;
            l->p[P_A].base = (hit == SH_POLYGON || hit == SH_STAR) ? 5 : 0; l->p[P_B].base = 0.45f; P->rev++;
        }
        y += D(4);
    }
    if (l->type != L_ADJUST && l->type != L_SVG && l->type != L_IMAGE && l->type != L_VIDEO) {
        section(&y, x, "COLOR");
        y = color_row(y, x, w, l->type == L_SOLID ? "Color" : "Fill", l->fill, rid(l, 53), true);
        if (l->type == L_SHAPE) y = color_row(y, x, w, "Stroke", l->stroke, rid(l, 54), true);
    }
    bool any = false;
    static const int PIDS[5] = { P_W, P_H, P_A, P_B, P_C };
    for (int k = 0; k < 5; k++) {
        int pid = PIDS[k];
        const char *lab = prop_label(l, pid);
        if (!lab) continue;
        if (!any) { section(&y, x, "PARAMETERS"); any = true; }
        float mn, mx, hmn, hmx;
        prop_slider_range(l, pid, &mn, &mx, &hmn, &hmx);
        y = prop_row(rid(l, pid), l, &l->p[pid], pid, lab, mn, mx, hmn, hmx, x, y, w, NULL);
    }
    if (l->type == L_STARS) {   /* the stars layer keeps its look in the parameters; stars have no direction/opacity here */
        (void)0;
    }
    section(&y, x, "BLEND");
    int hit = chip_grid(&y, x, w, BLEND_NAMES, BM_COUNT, 4, l->blend);
    if (hit >= 0 && hit != l->blend) { l->blend = (uint8_t)hit; P->rev++; }
    y += D(6);
    if (toggle_row(&y, x, w, "Visible", l->visible)) { l->visible = !l->visible; P->rev++; }
    scroll_end(body, s, y);
}

/* ============================== EFFECTS ============================== */
static bool fx_add_open;
static void sheet_fx(Rectangle body) {
    Scroll *s = &scr[SC_FX];
    Layer *l = sel_layer();
    if (!l) { empty_hint(body, "Select a layer on the timeline"); return; }
    float pad = D(16), x = body.x + pad, w = body.width - 2 * pad, y = scroll_begin(body, s) + D(4);
    if (l->g) {
        dtext_fit(F, "This clip uses a Fusion graph.", (Rectangle){ x, y + D(6), w, D(26) }, T_BODY, C_TEXT, 1); y += D(34);
        dtext_fit(F, "Add and edit its effects as nodes.", (Rectangle){ x, y, w, D(24) }, T_SMALL, C_DIM, 1); y += D(34);
        if (pill((Rectangle){ x, y, w, D(44) }, "Open Fusion", true, C_ACC)) tool = TOOL_FUSION;
        y += D(56);
        scroll_end(body, s, y);
        return;
    }
    if (pill((Rectangle){ x, y, w, D(44) }, fx_add_open ? "Close effects" : "+  Add effect", fx_add_open, C_ACC)) fx_add_open = !fx_add_open;
    y += D(54);
    if (fx_add_open) {
        const char *names[FX_COUNT];
        for (int i = 0; i < FX_COUNT; i++) names[i] = FX_DEFS[i].name;
        int hit = chip_grid(&y, x, w, names, FX_COUNT, body.width > D(520) ? 4 : 3, -1);
        if (hit >= 0) { if (layer_add_fx(P, l, hit) < 0) afx_log(1, "effect limit reached (%d)", MAX_FX); else fx_add_open = false; }
        y += D(6);
    }
    for (int fi = 0; fi < l->nfx; fi++) {
        Effect *e = &l->fx[fi]; const FxDef *d = &FX_DEFS[e->type];
        Rectangle hr = { x, y, w, D(46) };
        rrect(hr, D(12), C_SURF2);
        Rectangle eb = { hr.x + D(2), hr.y, D(44), D(46) }, db = { hr.x + hr.width - D(46), hr.y, D(44), D(46) };
        bool tg = icon_btn(eb, e->on ? IC_EYE : IC_EYE_OFF, D(22), e->on ? C_TEXT : C_DIM, NULL);
        char nm[48]; snprintf(nm, sizeof nm, "%d. %s", fi + 1, d->name);
        dtext_fit(FB, nm, (Rectangle){ hr.x + D(50), hr.y, hr.width - D(100), D(46) }, T_BODY, C_ACC, 0);
        bool del = icon_btn(db, IC_TRASH, D(20), C_RED, NULL);
        if (tg) { e->on = !e->on; P->rev++; }
        y += D(52);
        if (del) { layer_del_fx(P, l, fi); break; }
        for (int k = 0; k < d->np; k++)
            y = prop_row(rid(l, 16 + fi * 3 + k), l, &e->p[k], P_COUNT + fi * 3 + k, d->pn[k], d->mn[k], d->mx[k], d->mn[k] - 1e4f, d->mx[k] + 1e4f, x, y, w, NULL);
        if (e->type == FX_TINT) y = color_row(y, x, w, "Tint color", e->color, rid(l, 40 + fi), false);
        y += D(8);
    }
    if (!l->nfx && !fx_add_open) { dtext_fit(F, "No effects on this layer yet", (Rectangle){ x, y + D(6), w, D(30) }, T_BODY, C_DIM, 1); y += D(40); }
    scroll_end(body, s, y);
}

/* ============================== ANIMATE ============================== */
static void anim_preset(Layer *l, int k) {
    float tin = l->tin, out = project_layer_out(P, l), W = (float)P->width;
    float d = fminf(0.6f, (out - tin) * 0.5f);
    if (d < 0.05f) return;
    Prop *op = &l->p[P_OPA], *sx = &l->p[P_SX], *sy = &l->p[P_SY], *px = &l->p[P_X], *rt = &l->p[P_ROT];
    switch (k) {
    case 0: { float v = op->nkeys ? 100 : op->base; prop_set_key(op, tin, 0, EASE_OUT); prop_set_key(op, tin + d, v, EASE_LINEAR); break; }
    case 1: { float v = op->nkeys ? 100 : op->base; prop_set_key(op, out - d, v, EASE_IN); prop_set_key(op, out, 0, EASE_LINEAR); break; }
    case 2: { float v1 = sx->nkeys ? 100 : sx->base, v2 = sy->nkeys ? 100 : sy->base;
        prop_set_key(sx, tin, 0, EASE_OUT); prop_set_key(sx, tin + d, v1, EASE_LINEAR);
        prop_set_key(sy, tin, 0, EASE_OUT); prop_set_key(sy, tin + d, v2, EASE_LINEAR); break; }
    case 3: { float v = prop_keyed(px, tin + d); prop_set_key(px, tin, v - W * 0.6f, EASE_OUT); prop_set_key(px, tin + d, v, EASE_LINEAR); break; }
    case 4: { float v = rt->nkeys ? prop_keyed(rt, tin) : rt->base; prop_set_key(rt, tin, v, EASE_LINEAR); prop_set_key(rt, out, v + 360, EASE_LINEAR); break; }
    case 5: {
        Prop *ps[P_COUNT + MAX_FX * 3]; int c = 0;
        for (int i = 0; i < P_COUNT; i++) ps[c++] = &l->p[i];
        for (int f = 0; f < l->nfx; f++) for (int q = 0; q < 3; q++) ps[c++] = &l->fx[f].p[q];
        for (int i = 0; i < c; i++) {
            Prop *p = ps[i];
            if (p->nkeys) { p->base = p->keys[0].v; while (p->nkeys) prop_del_key(p, p->nkeys - 1); }
            if (p->expr) prop_set_expr(P, p, "");
        }
        break; }
    }
    P->rev++;
}
static void anim_row(float *y, float x, float w, unsigned id, Layer *l, Prop *p, const char *label) {
    float t = cur_t, h = D(92);
    Rectangle card = { x, *y, w, h };
    rrect(card, D(14), C_SURF2);
    char hdr[96]; snprintf(hdr, sizeof hdr, "%s", label);
    dtext_fit(FB, hdr, (Rectangle){ x + D(14), *y + D(4), w - D(120), D(32) }, T_BODY, C_TEXT, 0);
    char nk[32]; snprintf(nk, sizeof nk, "%d keys", p->nkeys);
    dtext_fit(F, p->expr || p->fn ? "expression" : nk, (Rectangle){ x + w - D(110), *y + D(4), D(100), D(32) }, T_SMALL, p->expr || p->fn ? C_VIOLET : C_DIM, 2);
    float by = *y + D(40), bh = D(40), bw = D(44), g = D(6), bx = x + D(10);
    float prev = -1, next = -1;
    for (int i = 0; i < p->nkeys; i++) {
        float kt = p->keys[i].t;
        if (kt < t - 0.5f / P->fps && (prev < 0 || kt > prev)) prev = kt;
        if (kt > t + 0.5f / P->fps && (next < 0 || kt < next)) next = kt;
    }
    int ki = prop_key_at(p, t, 0.5f / P->fps);
    if (icon_btn((Rectangle){ bx, by, bw, bh }, IC_PREV, D(20), prev >= 0 ? C_TEXT : alpha_c(C_DIM, 0.4f), NULL) && prev >= 0) { set_time(prev); playing = false; }
    bx += bw + g;
    if (icon_btn((Rectangle){ bx, by, bw, bh }, ki >= 0 ? IC_DIA_FILL : IC_DIA, D(22), C_ACC2, NULL)) key_toggle(p, t, ki, NULL);
    bx += bw + g;
    if (icon_btn((Rectangle){ bx, by, bw, bh }, IC_NEXT, D(20), next >= 0 ? C_TEXT : alpha_c(C_DIM, 0.4f), NULL) && next >= 0) { set_time(next); playing = false; }
    bx += bw + g + D(4);
    float ew = (x + w - D(10) - bx - D(52) - g);
    Rectangle re = { bx, by + D(3), ew, bh - D(6) }, rf = { x + w - D(10) - D(52), by + D(3), D(52), bh - D(6) };
    if (ki >= 0) { if (chip(re, EASE_ABBR[p->keys[ki].ease], true)) { p->keys[ki].ease = (uint8_t)((p->keys[ki].ease + 1) % EASE_COUNT); P->rev++; } }
    else { rrect(re, D(10), C_SURF); dtext_fit(F, "no key here", re, T_SMALL, alpha_c(C_DIM, 0.6f), 1); }
    bool has = p->expr != NULL || p->fn;
    rrect(rf, D(10), has ? alpha_c(C_VIOLET, 0.25f) : C_SURF3);
    dtext_fit(FB, "\xC6\x92", rf, T_TITLE, has ? C_VIOLET : C_TEXT, 1);
    if (tap(rf)) {
        if (p->fn && !p->expr) afx_log(0, "'%s' uses a Lua function expression; edit it in the user script", label);
        else edit_begin(id * 8 + 3, "Expression (Lua), e.g. v + wiggle(2,10)  -  empty clears", p->expr ? p->expr : "", false, false);
    }
    if (edit_done(id * 8 + 3)) prop_set_expr(P, p, te.done_buf);
    *y += h + D(10);
}
static void sheet_anim(Rectangle body) {
    Scroll *s = &scr[SC_ANIM];
    Layer *l = sel_layer();
    if (!l) { empty_hint(body, "Select a layer on the timeline"); return; }
    float pad = D(16), x = body.x + pad, w = body.width - 2 * pad, y = scroll_begin(body, s) + D(4);
    section(&y, x, "ANIMATIONS");
    static const char *PRE[6] = { "Fade in", "Fade out", "Pop in", "Slide in", "Spin", "Clear all" };
    int hit = chip_grid(&y, x, w, PRE, 6, 3, -1);
    if (hit >= 0) anim_preset(l, hit);
    y += D(8);
    int shown = 0;
    for (int pid = 0; pid < P_COUNT + MAX_FX * 3; pid++) {
        Prop *p = NULL; char lab[96] = "";
        if (pid < P_COUNT) { const char *pl = prop_label(l, pid); if (!pl) continue; p = &l->p[pid]; snprintf(lab, sizeof lab, "%s", pl); }
        else {
            int fi = (pid - P_COUNT) / 3, k = (pid - P_COUNT) % 3;
            if (fi >= l->nfx || k >= FX_DEFS[l->fx[fi].type].np) continue;
            p = &l->fx[fi].p[k]; snprintf(lab, sizeof lab, "%s / %s", FX_DEFS[l->fx[fi].type].name, FX_DEFS[l->fx[fi].type].pn[k]);
        }
        if (!p->nkeys && !p->expr && !p->fn) continue;
        if (!shown) section(&y, x, "ANIMATED PROPERTIES");
        shown++;
        anim_row(&y, x, w, rid(l, pid < P_COUNT ? pid : 16 + (pid - P_COUNT)), l, p, lab);
    }
    if (!shown) { dtext_fit(F, "Tap a preset, or use the diamond in Edit to set keyframes", (Rectangle){ x, y, w, D(44) }, T_SMALL, C_DIM, 1); y += D(50); }
    scroll_end(body, s, y);
}

/* ============================== LAYERS ============================== */
static void select_uid(int uid) { for (int i = 0; i < P->nlayers; i++) if (P->layers[i].uid == uid) { sel = i; return; } }
static void sheet_layers(Rectangle body) {
    Scroll *s = &scr[SC_LAYERS];
    float pad = D(16), x = body.x + pad, w = body.width - 2 * pad, y = scroll_begin(body, s) + D(6);
    if (!P->nlayers) { dtext_fit(F, "No layers yet. Use Add to create one.", (Rectangle){ x, y + D(20), w, D(40) }, T_BODY, C_DIM, 1); y += D(80); }
    int suid = sel_layer() ? sel_layer()->uid : -1;
    for (int i = P->nlayers - 1; i >= 0; i--) {
        Layer *q = &P->layers[i];
        Rectangle row = { x, y, w, D(54) };
        rrect(row, D(14), i == sel ? alpha_c(C_ACC, 0.14f) : C_SURF2);
        if (i == sel) rrect_line(row, D(14), D(1.5f), C_ACC);
        Rectangle eye = { row.x + D(2), row.y, D(46), row.height };
        Rectangle ub = { row.x + row.width - D(92), row.y, D(44), row.height }, db = { row.x + row.width - D(46), row.y, D(44), row.height };
        Rectangle nm = { row.x + D(88), row.y, row.width - D(88) - D(100), row.height };
        icon_c(LTYPE_ICON[q->type], (Rectangle){ row.x + D(46), row.y, D(36), row.height }, D(20), LTYPE_COL[q->type]);
        char lab[96]; snprintf(lab, sizeof lab, "%s%s", q->scripted ? "(script) " : "", q->name);
        dtext_fit(F, lab, nm, T_BODY, i == sel ? C_ACC : (q->visible ? C_TEXT : C_DIM), 0);
        bool vis = icon_btn(eye, q->visible ? IC_EYE : IC_EYE_OFF, D(22), q->visible ? C_TEXT : C_DIM, NULL);
        bool up = icon_btn(ub, IC_UP, D(20), C_DIM, NULL), dn = icon_btn(db, IC_DOWN, D(20), C_DIM, NULL);
        if (vis) { q->visible = !q->visible; P->rev++; }
        else if (up) { project_move(P, i, 1); select_uid(suid); break; }
        else if (dn) { project_move(P, i, -1); select_uid(suid); break; }
        else if (tap(nm)) sel = i;
        y += D(60);
    }
    scroll_end(body, s, y);
}

/* ============================== CANVAS ============================== */
static void scale_prop(Prop *p, float k) { p->base *= k; for (int i = 0; i < p->nkeys; i++) p->keys[i].v *= k; }
static void canvas_resize(int nw, int nh, bool remap) {
    int ow = P->width, oh = P->height;
    if (remap) {
        float rx = (float)nw / ow, ry = (float)nh / oh;
        for (int i = 0; i < P->nlayers; i++) {
            Layer *l = &P->layers[i];
            if (l->type == L_STARS || l->type == L_ADJUST) continue;
            scale_prop(&l->p[P_X], rx); scale_prop(&l->p[P_Y], ry);
            if (l->type == L_SOLID && (int)(l->p[P_W].base + 0.5f) == ow && (int)(l->p[P_H].base + 0.5f) == oh) { l->p[P_W].base = (float)nw; l->p[P_H].base = (float)nh; }
        }
    }
    P->width = nw; P->height = nh; P->rev++;
}
static void sheet_canvas(Rectangle body) {
    Scroll *s = &scr[SC_CANVAS];
    float pad = D(16), x = body.x + pad, w = body.width - 2 * pad, y = scroll_begin(body, s) + D(4);
    if (field_row(&y, x, w, "Project", P->name)) edit_begin(900001, "Project name", P->name, false, false);
    section(&y, x, "ASPECT RATIO");
    static const char *AR[6] = { "9:16", "16:9", "1:1", "4:5", "4:3", "21:9" };
    static const int ARW[6] = { 9, 16, 1, 4, 4, 21 }, ARH[6] = { 16, 9, 1, 5, 3, 9 };
    int cur = -1; float ratio = (float)P->width / P->height;
    for (int i = 0; i < 6; i++) if (fabsf(ratio - (float)ARW[i] / ARH[i]) < 0.012f) cur = i;
    int hit = chip_grid(&y, x, w, AR, 6, 3, cur);
    if (hit >= 0 && hit != cur) {
        int sh = P->width < P->height ? P->width : P->height; if (sh < 360) sh = 360;
        int nw, nh;
        if (ARW[hit] < ARH[hit]) { nw = sh; nh = (int)(sh * (float)ARH[hit] / ARW[hit] + 0.5f); }
        else { nh = sh; nw = (int)(sh * (float)ARW[hit] / ARH[hit] + 0.5f); }
        canvas_resize(nw & ~1, nh & ~1, true);
    }
    y += D(6);
    section(&y, x, "FRAME RATE");
    static const char *FP[3] = { "24 fps", "30 fps", "60 fps" }; static const float FV[3] = { 24, 30, 60 };
    int fc = P->fps == 24 ? 0 : P->fps == 30 ? 1 : P->fps == 60 ? 2 : -1;
    hit = chip_grid(&y, x, w, FP, 3, 3, fc);
    if (hit >= 0) { P->fps = FV[hit]; P->rev++; }
    y += D(6);
    float nv;
    if (slider_row(900010, "Duration (s)", P->duration, 1, 60, 0.1f, 600, x, y, w, &nv)) { P->duration = nv; P->rev++; if (cur_t > nv) set_time(nv); }
    y += D(70);
    y = color_row(y, x, w, "Background", P->bg, 900050, true);
    y += D(6);
    section(&y, x, "CUSTOM SIZE (px)");
    if (slider_row(900011, "Width", (float)P->width, 120, 2160, 16, 4096, x, y, w, &nv)) { P->width = (int)nv & ~1; P->rev++; }
    y += D(68);
    if (slider_row(900012, "Height", (float)P->height, 120, 2160, 16, 4096, x, y, w, &nv)) { P->height = (int)nv & ~1; P->rev++; }
    y += D(68);
    char info[160];
    snprintf(info, sizeof info, "%d layers - %d frames @ %g fps%s", P->nlayers, project_nframes(P), (double)P->fps, P->lossy ? " (function expressions are not saved)" : "");
    dtext_fit(F, info, (Rectangle){ x, y, w, D(24) }, T_SMALL, C_DIM, 0);
    y += D(30);
    scroll_end(body, s, y);
}

/* ============================== SCRIPT ============================== */
static int prev_log_n;
static void sheet_script(Rectangle body) {
    float pad = D(16), w = body.width - 2 * pad, x = body.x + pad, g = D(8), y = body.y + D(6);
    float bw = (w - 2 * g) / 3;
    if (pill((Rectangle){ x, y, bw, D(42) }, "Edit script", true, C_ACC)) open_script_editor();
    if (pill((Rectangle){ x + bw + g, y, bw, D(42) }, "Help", false, C_ACC)) script_exec(P, "help()", "=console");
    if (pill((Rectangle){ x + 2 * (bw + g), y, bw, D(42) }, "Clear", false, C_ACC)) { log_n = 0; prev_log_n = 0; }
    y += D(50);
    Rectangle in = { x, body.y + body.height - D(46) - D(6), w, D(46) };
    Rectangle lg = { body.x + D(8), y, body.width - D(16), in.y - y - g };
    Scroll *s = &scr[SC_SCRIPT];
    float lh = T_SMALL * 1.35f; int n = log_n < LOGN ? log_n : LOGN;
    float prev_max = fmaxf(0, (prev_log_n < LOGN ? prev_log_n : LOGN) * lh + D(12) - lg.height);
    bool follow = s->y >= prev_max - 3;
    s->h = n * lh + D(12); prev_log_n = log_n;
    if (follow) s->y = 1e9f;
    float yy = scroll_begin(lg, s) + D(4);
    rrect(lg, D(12), C_FIELD);
    for (int i = 0; i < n; i++) {
        int idx = (log_n < LOGN ? i : log_n + i) % LOGN;
        if (yy + lh > lg.y && yy < lg.y + lg.height) dtext(FM, logs[idx].s, lg.x + D(10), yy, T_SMALL, logs[idx].lvl ? C_RED : C_TEXT);
        yy += lh;
    }
    scroll_end(lg, s, yy);
    float yy2 = in.y;
    if (field_row(&yy2, in.x, in.width, ">", "type Lua here (try: help())")) edit_begin(900101, "Lua console (single line)", "", false, false);
    if (edit_done(900101) && te.done_buf[0]) { afx_log(0, "> %s", te.done_buf); script_exec(P, te.done_buf, "=console"); }
}

/* ============================== EXPORT ============================== */
static int exp_fmt, exp_scale = 100, exp_quality = 80, exp_from, exp_to;
static bool exp_custom_range, ffmpeg_ok;
static char exp_name[64] = "";
static ExportJob *job;
static const char *EXP_NAMES[6] = { "GIF", "MP4", "WebM", "AVI", "PNG frame", "JPG frame" };
static const char *EXP_EXT[6] = { "gif", "mp4", "webm", "avi", "png", "jpg" };
static void exp_default_name(void) {
    snprintf(exp_name, sizeof exp_name, "%s", P->name);
    for (char *q = exp_name; *q; q++) if (*q == ' ' || *q == '/') *q = '_';
}
static void export_start(void) {
    if (!exp_name[0]) exp_default_name();
    char path[PATH_LEN], err[200] = { 0 };
    snprintf(path, sizeof path, "%s/%s.%s", proj_dir, exp_name, EXP_EXT[exp_fmt]);
    if (exp_fmt >= 4) {
        int fr = (int)floorf(cur_t * P->fps + 0.5f);
        if (export_frame_image(P, fr, path, exp_quality, err, sizeof err)) { afx_log(0, "saved frame %d -> %s", fr, path); set_status(0, "Saved %s", path); }
        else afx_log(1, "%s", err);
        return;
    }
    int nf = project_nframes(P), from = exp_custom_range ? exp_from : 0, to = exp_custom_range ? exp_to : nf - 1;
    job = export_begin(P, path, (VidFmt)exp_fmt, from, to, exp_scale, exp_quality, err, sizeof err);
    if (!job) afx_log(1, "%s", err);
    else afx_log(0, "exporting -> %s", path);
}
static void export_tick(void) {
    if (!job) return;
    double t0 = GetTime(); int r;
    do { r = export_step(job); } while (r == 1 && GetTime() - t0 < 0.03);
    if (r <= 0) {
        char msg[256]; snprintf(msg, sizeof msg, "%s", export_error(job));
        export_end(job, r < 0); job = NULL;
        if (r < 0 || msg[0]) afx_log(1, "export failed: %s", msg); else { afx_log(0, "export finished"); set_status(0, "Export finished: %s", proj_dir); }
    }
}
static void sheet_export(Rectangle body) {
    Scroll *s = &scr[SC_EXPORT];
    float pad = D(16), x = body.x + pad, w = body.width - 2 * pad, y = scroll_begin(body, s) + D(4), nv;
    section(&y, x, "FORMAT");
    int hit = chip_grid(&y, x, w, EXP_NAMES, 6, 3, exp_fmt);
    if (hit >= 0) { if (hit >= 1 && hit <= 3 && !ffmpeg_ok) set_status(1, "ffmpeg not available: use GIF or a frame export"); else exp_fmt = hit; }
    if (!ffmpeg_ok) { dtext_fit(F, "MP4/WebM/AVI need ffmpeg (not on this device). GIF and frames work.", (Rectangle){ x, y, w, D(34) }, T_SMALL, C_ACC, 0); y += D(36); }
    y += D(4);
    if (exp_fmt < 4) {
        if (slider_row(900200, "Scale %", (float)exp_scale, 10, 200, 10, 400, x, y, w, &nv)) exp_scale = (int)nv;
        y += D(68);
    }
    if (slider_row(900201, "Quality", (float)exp_quality, 1, 100, 1, 100, x, y, w, &nv)) exp_quality = (int)nv;
    y += D(68);
    if (exp_fmt < 4) {
        int nf = project_nframes(P);
        if (toggle_row(&y, x, w, "Custom frame range", exp_custom_range)) { exp_custom_range = !exp_custom_range; exp_from = 0; exp_to = nf - 1; }
        if (exp_custom_range) {
            if (exp_to > nf - 1) exp_to = nf - 1;
            if (slider_row(900202, "From frame", (float)exp_from, 0, (float)(nf - 1), 0, (float)(nf - 1), x, y, w, &nv)) exp_from = (int)nv;
            y += D(68);
            if (slider_row(900203, "To frame", (float)exp_to, 0, (float)(nf - 1), 0, (float)(nf - 1), x, y, w, &nv)) exp_to = (int)nv;
            y += D(68);
        }
    } else { dtext_fit(F, "Exports the frame at the playhead.", (Rectangle){ x, y, w, D(28) }, T_SMALL, C_DIM, 0); y += D(32); }
    if (!exp_name[0]) exp_default_name();
    if (field_row(&y, x, w, "File name", exp_name)) edit_begin(900204, "Output file name (no extension)", exp_name, false, false);
    if (edit_done(900204) && te.done_buf[0]) { snprintf(exp_name, sizeof exp_name, "%s", te.done_buf); for (char *q = exp_name; *q; q++) if (*q == '/') *q = '_'; }
    dtext_fit(F, proj_dir, (Rectangle){ x, y, w, D(22) }, T_TINY, C_DIM, 0);
    y += D(30);
    if (job) {
        int c, t; export_progress(job, &c, &t);
        Rectangle pb = { x, y, w, D(30) };
        rrect(pb, D(15), C_SURF3);
        rrect((Rectangle){ pb.x, pb.y, fmaxf(pb.height, pb.width * c / (float)(t ? t : 1)), pb.height }, D(15), C_GREEN);
        char b[48]; snprintf(b, sizeof b, "%d / %d", c, t); dtext_fit(FB, b, pb, T_SMALL, C_INK, 1);
        y += D(42);
        if (pill((Rectangle){ x, y, w, D(48) }, "Cancel export", false, C_ACC)) { export_end(job, true); job = NULL; afx_log(0, "export cancelled"); }
        y += D(56);
    } else {
        if (pill((Rectangle){ x, y, w, D(52) }, exp_fmt >= 4 ? "Save frame" : "Export", true, C_ACC)) export_start();
        y += D(60);
    }
    scroll_end(body, s, y);
}
