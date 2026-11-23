/* ui_main.inc -- screen layout (portrait + landscape), preview, timeline, toolbar, dialogs, undo, projects, test harness.
 * Included by main.c after ui_sheets.inc. */

/* ============================== undo / redo ============================== */
#define UNDO_MAX 40
static char *undo_s[UNDO_MAX], *redo_s[UNDO_MAX], *cur_snap;
static int undo_n, redo_n, snap_rev = -1, last_rev = -1, saved_rev = -1, pending_undo;
static double rev_stable_t;
static int prev_rev_invalidate;      /* set to force a preview refresh */
static void snap_reset(void) {
    for (int i = 0; i < undo_n; i++) free(undo_s[i]);
    for (int i = 0; i < redo_n; i++) free(redo_s[i]);
    undo_n = redo_n = 0;
    free(cur_snap); cur_snap = project_serialize(P);
    snap_rev = last_rev = P->rev; saved_rev = P->rev;
}
static void undo_commit(void) {
    if (!cur_snap || P->rev == snap_rev) return;
    char *now = project_serialize(P);
    snap_rev = P->rev;
    if (!strcmp(now, cur_snap)) { free(now); return; }
    if (undo_n == UNDO_MAX) { free(undo_s[0]); memmove(undo_s, undo_s + 1, sizeof(char *) * (UNDO_MAX - 1)); undo_n--; }
    undo_s[undo_n++] = cur_snap; cur_snap = now;
    for (int i = 0; i < redo_n; i++) free(redo_s[i]);
    redo_n = 0;
}
static void undo_restore(void) {
    project_load_text(P, cur_snap, "undo");
    if (sel >= P->nlayers) sel = P->nlayers - 1;
    cur_t = clampf(cur_t, 0, fmaxf(0, P->duration - 1.0f / P->fps));
    snap_rev = last_rev = P->rev; prev_rev_invalidate = 1;
}
static void do_undo(void) {
    undo_commit();
    if (!undo_n) { set_status(0, "Nothing to undo"); return; }
    if (redo_n == UNDO_MAX) { free(redo_s[0]); memmove(redo_s, redo_s + 1, sizeof(char *) * (UNDO_MAX - 1)); redo_n--; }
    redo_s[redo_n++] = cur_snap; cur_snap = undo_s[--undo_n];
    undo_restore();
}
static void do_redo(void) {
    if (!redo_n) { set_status(0, "Nothing to redo"); return; }
    undo_s[undo_n++] = cur_snap; cur_snap = redo_s[--redo_n];
    undo_restore();
}
static void undo_tick(void) {
    if (P->rev != last_rev) { last_rev = P->rev; rev_stable_t = GetTime(); }
    if (P->rev != snap_rev && !m_down && !te.active && GetTime() - rev_stable_t > 0.3) undo_commit();
}

/* ============================== projects on disk ============================== */
static char files[96][PATH_LEN]; static long fmtime[96]; static int nfiles;
static void scan_files(void) {
    nfiles = 0;
    DIR *d = opendir(proj_dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && nfiles < 96) {
        size_t n = strlen(e->d_name);
        if (n > 4 && !strcmp(e->d_name + n - 4, ".lua")) {
            char p[PATH_LEN]; snprintf(files[nfiles], PATH_LEN, "%s", e->d_name);
            snprintf(p, sizeof p, "%s/%s", proj_dir, e->d_name);
            fmtime[nfiles] = (long)GetFileModTime(p); nfiles++;
        }
    }
    closedir(d);
    for (int i = 1; i < nfiles; i++)         /* newest first */
        for (int j = i; j > 0 && fmtime[j] > fmtime[j - 1]; j--) {
            char tmp[PATH_LEN]; long tt = fmtime[j];
            memcpy(tmp, files[j], PATH_LEN); memcpy(files[j], files[j - 1], PATH_LEN); memcpy(files[j - 1], tmp, PATH_LEN);
            fmtime[j] = fmtime[j - 1]; fmtime[j - 1] = tt;
        }
}
static float tl_pps = 80, tl_vel;
static void tl_fit(void) { tl_pps = clampf((float)SW / DP * 0.72f / fmaxf(P->duration, 0.5f), 12, 400); tl_vel = 0; }
static void reset_view(void) {
    sel = -1; cur_t = 0; playing = false; tool = TOOL_NONE; color_open = 0; fx_add_open = false;
    for (int i = 0; i < SC_COUNT; i++) { scr[i].y = 0; scr[i].vel = 0; }
    prev_rev_invalidate = 1; exp_name[0] = 0; tl_fit(); snap_reset();
}
static void safe_name(char *out, size_t cap, const char *name) {
    snprintf(out, cap, "%s", name);
    for (char *q = out; *q; q++) if (*q == ' ' || *q == '/' || *q == '\\' || *q == ':') *q = '_';
}
static void unique_name(char *name, size_t cap) {
    char base[NAME_LEN]; snprintf(base, sizeof base, "%s", name);
    for (int i = 2; i < 999; i++) {
        char sn[NAME_LEN], path[PATH_LEN]; safe_name(sn, sizeof sn, name);
        snprintf(path, sizeof path, "%s/%s.lua", proj_dir, sn);
        if (!FileExists(path)) return;
        snprintf(name, cap, "%.40s %d", base, i);
    }
}
static void save_project(void) {
    char path[PATH_LEN];
    if (P->filepath[0]) snprintf(path, sizeof path, "%s", P->filepath);
    else { char nm[NAME_LEN]; safe_name(nm, sizeof nm, P->name); snprintf(path, sizeof path, "%s/%s.lua", proj_dir, nm); }
    if (project_save_file(P, path)) saved_rev = P->rev;
}
static const char *DEMO_LUA =
    "afx.comp{ name=\"Asteroid Demo\", width=640, height=360, fps=30, duration=6, bg={6,8,20,255} }\n"
    "afx.stars{ name=\"Far stars\", count=140, speed=14, size=1.2, opacity=70 }\n"
    "afx.stars{ name=\"Near stars\", count=60, speed=60, size=2.2, seed=7 }\n"
    "local r = afx.asteroid{ name=\"Rock\", x=-60, y=200, radius=46, roughness=0.4, craters=0.7, fx={{\"glow\", radius=8, amount=0.4}} }\n"
    "r:key(\"x\", 0, -60); r:key(\"x\", 6, 700)\n"
    "r:expr(\"y\", \"v + wiggle(1.2, 14)\")\n"
    "r:expr(\"rot\", \"t*40\")\n"
    "afx.text{ name=\"Title\", text=\"HMY ASTEROID MOTION\", y=300, size=34, fill=\"#ffb43c\", fx={{\"glow\", radius=14, amount=1.0}, {\"rgbsplit\", amount=2}} }\n"
    "afx.adjust{ name=\"Look\", fx={{\"vignette\", amount=0.7}, {\"scanlines\", amount=0.15}} }\n"
    "--@user\n";
static void new_project(int kind) {   /* 0 blank 9:16, 1 blank 16:9, 2 demo */
    P->filepath[0] = 0;
    if (kind == 2) project_load_text(P, DEMO_LUA, "demo");
    else { project_reset(P); snprintf(P->name, NAME_LEN, "Project"); P->width = kind == 0 ? 540 : 960; P->height = kind == 0 ? 960 : 540; P->duration = 6; }
    unique_name(P->name, NAME_LEN);
    char nm[NAME_LEN]; safe_name(nm, sizeof nm, P->name);
    snprintf(P->filepath, sizeof P->filepath, "%s/%s.lua", proj_dir, nm);
    reset_view();
    save_project();
}
static bool open_project(const char *file) {
    char path[PATH_LEN]; snprintf(path, sizeof path, "%s/%s", proj_dir, file);
    if (!project_load_file(P, path)) return false;
    snprintf(P->filepath, sizeof P->filepath, "%s", path);
    reset_view();
    return true;
}
static void autosave_tick(void) {
    if (!P->filepath[0] || P->rev == saved_rev || m_down || te.active || job) return;
    if (GetTime() - rev_stable_t > 1.5) save_project();
}

/* ============================== layer actions ============================== */
enum { ACT_NONE, ACT_BACK, ACT_SPLIT, ACT_DUP, ACT_DEL, ACT_HIDE, ACT_UP, ACT_DOWN };
static void do_action(int act) {
    Layer *l = sel_layer();
    if (act == ACT_BACK) { sel = -1; tool = TOOL_NONE; return; }
    if (!l) return;
    int uid = l->uid;
    switch (act) {
    case ACT_SPLIT: {
        float t = cur_t, out = project_layer_out(P, l);
        if (t > l->tin + 0.03f && t < out - 0.03f) {
            float old_tout = l->tout;
            int n = project_duplicate(P, sel);
            if (n >= 0) { P->layers[sel].tout = t; P->layers[n].tin = t; P->layers[n].tout = old_tout; sel = n; P->rev++; set_status(0, "Split at %.2fs", (double)t); }
        } else set_status(1, "Move the playhead inside the clip to split it");
        break; }
    case ACT_DUP: { int n = project_duplicate(P, sel); if (n >= 0) { sel = n; set_status(0, "Duplicated"); } break; }
    case ACT_DEL: project_remove(P, sel); sel = -1; if (portrait) tool = TOOL_NONE; set_status(0, "Layer deleted - use undo to restore"); break;
    case ACT_HIDE: l->visible = !l->visible; P->rev++; break;
    case ACT_UP: project_move(P, sel, 1); select_uid(uid); break;
    case ACT_DOWN: project_move(P, sel, -1); select_uid(uid); break;
    }
}

/* ============================== layout ============================== */
typedef struct { Rectangle top, prev, trans, tl, tb, sheet, panel; bool sheet_on; } Layout;
static Layout LY;
static void layout_compute(void) {
    portrait = SH > SW;
    float W = (float)SW, H = (float)SH;
    memset(&LY, 0, sizeof LY);
    if (portrait) {
        LY.top = (Rectangle){ 0, 0, W, D(54) };
        if (tool != TOOL_NONE) {
            float sh = clampf(H * 0.47f, D(300), H * 0.62f);
            LY.sheet_on = true; LY.sheet = (Rectangle){ 0, H - sh, W, sh };
            LY.prev = (Rectangle){ 0, LY.top.height, W, H - sh - LY.top.height };
        } else {
            float tbh = D(68), tlh = clampf(H * 0.27f, D(150), D(300)), trh = D(46);
            LY.tb = (Rectangle){ 0, H - tbh, W, tbh };
            LY.tl = (Rectangle){ 0, LY.tb.y - tlh, W, tlh };
            LY.trans = (Rectangle){ 0, LY.tl.y - trh, W, trh };
            LY.prev = (Rectangle){ 0, LY.top.height, W, LY.trans.y - LY.top.height };
        }
    } else {
        LY.top = (Rectangle){ 0, 0, W, D(48) };
        float pw = clampf(W * 0.38f, D(330), D(470)), lw = W - pw;
        float tlh = clampf(H * 0.36f, D(118), D(230)), trh = D(40);
        LY.panel = (Rectangle){ lw, LY.top.height, pw, H - LY.top.height };
        LY.tl = (Rectangle){ 0, H - tlh, lw, tlh };
        LY.trans = (Rectangle){ 0, LY.tl.y - trh, lw, trh };
        LY.prev = (Rectangle){ 0, LY.top.height, lw, LY.trans.y - LY.top.height };
    }
}

/* ============================== preview ============================== */
static Texture2D prev_tex; static uint8_t *prev_pix; static int prev_w, prev_h, prev_rev = -1; static float prev_t = -1; static FBuf prev_fb;
static void preview_update(void) {
    if (P->width != prev_w || P->height != prev_h) {
        if (prev_tex.id) UnloadTexture(prev_tex);
        free(prev_pix);
        prev_w = P->width; prev_h = P->height;
        Image im = GenImageColor(prev_w, prev_h, BLACK);
        prev_tex = LoadTextureFromImage(im); UnloadImage(im);
        SetTextureFilter(prev_tex, TEXTURE_FILTER_BILINEAR);
        prev_pix = malloc((size_t)prev_w * prev_h * 4);
        prev_rev = -1;
    }
    if (prev_rev_invalidate) { prev_rev = -1; prev_rev_invalidate = 0; }
    if (P->rev != prev_rev || cur_t != prev_t) {
        render_comp(P, cur_t, &prev_fb);
        prev_rev = P->rev; prev_t = cur_t;
        fbuf_to_rgba8(&prev_fb, prev_pix, false);
        UpdateTexture(prev_tex, prev_pix);
    }
}
static void draw_preview(Rectangle area) {
    DrawRectangleRec(area, (Color){ 0, 0, 0, 255 });
    float m = D(8);
    float sc = fminf((area.width - 2 * m) / P->width, (area.height - 2 * m) / P->height);
    if (sc <= 0) return;
    Rectangle pr = { area.x + (area.width - P->width * sc) / 2, area.y + (area.height - P->height * sc) / 2, P->width * sc, P->height * sc };
    if (P->bg[3] < 255) {
        float cs = fmaxf(8, D(10));
        for (float yy = 0; yy < pr.height; yy += cs) for (float xx = 0; xx < pr.width; xx += cs) {
            bool a = ((int)(xx / cs) + (int)(yy / cs)) & 1;
            DrawRectangle((int)(pr.x + xx), (int)(pr.y + yy), (int)fminf(cs, pr.width - xx) + 1, (int)fminf(cs, pr.height - yy) + 1, a ? (Color){ 52, 52, 66, 255 } : (Color){ 40, 40, 52, 255 });
        }
    }
    DrawTexturePro(prev_tex, (Rectangle){ 0, 0, (float)prev_w, (float)prev_h }, pr, (Vector2){ 0, 0 }, 0, WHITE);
    DrawRectangleLinesEx(pr, 1, alpha_c(C_TEXT, 0.12f));
    Layer *l = sel_layer();
    if (l && l->type != L_ADJUST && l->type != L_STARS && cur_t >= l->tin && cur_t < project_layer_out(P, l)) {
        float lx = prop_eval(P, l, &l->p[P_X], P_X, cur_t), ly = prop_eval(P, l, &l->p[P_Y], P_Y, cur_t);
        Vector2 c = { pr.x + lx * sc, pr.y + ly * sc };
        if (in_rect(pr, c)) {
            float rr = D(16), th = fmaxf(1.5f, D(1.5f));
            DrawRing(c, rr - th, rr, 0, 360, 36, C_ACC);
            DrawLineEx((Vector2){ c.x - rr * 1.6f, c.y }, (Vector2){ c.x - rr * 0.5f, c.y }, th, C_ACC); DrawLineEx((Vector2){ c.x + rr * 0.5f, c.y }, (Vector2){ c.x + rr * 1.6f, c.y }, th, C_ACC);
            DrawLineEx((Vector2){ c.x, c.y - rr * 1.6f }, (Vector2){ c.x, c.y - rr * 0.5f }, th, C_ACC); DrawLineEx((Vector2){ c.x, c.y + rr * 0.5f }, (Vector2){ c.x, c.y + rr * 1.6f }, th, C_ACC);
        }
        const unsigned PID = 9100;
        if (!blocked && m_down && dragged && in_rect(pr, mp_press) && (active == 0 || active == PID) && axis != 3 && !l->p[P_X].expr && !l->p[P_Y].expr && !l->p[P_X].fn && !l->p[P_Y].fn) {
            active = PID;
            set_prop_val(&l->p[P_X], cur_t, lx + m_dx / sc);
            set_prop_val(&l->p[P_Y], cur_t, ly + m_dy / sc);
        }
    }
    if (tap(area)) { playing = !playing; if (playing && cur_t >= P->duration - 1.5f / P->fps) cur_t = 0; }
}

/* ============================== transport ============================== */
static void toggle_play(void) { playing = !playing; if (playing && cur_t >= P->duration - 1.5f / P->fps) cur_t = 0; }
static void draw_transport(Rectangle r) {
    DrawRectangleRec(r, C_BG);
    char a[24], b[24], line[64];
    fmt_time(a, cur_t); fmt_time(b, P->duration);
    snprintf(line, sizeof line, "%s", a);
    float fs = T_SMALL * 1.1f;
    dtext(FM, line, r.x + D(16), r.y + (r.height - fs) / 2, fs, C_TEXT);
    snprintf(line, sizeof line, "/ %s", b);
    dtext(FM, line, r.x + D(16) + tw(FM, a, fs) + D(8), r.y + (r.height - fs) / 2, fs, C_DIM);
    float bs = r.height;
    if (icon_btn((Rectangle){ r.x + (r.width - bs) / 2, r.y, bs, bs }, playing ? IC_PAUSE : IC_PLAY, D(26), C_TEXT, "play")) toggle_play();
    if (icon_btn((Rectangle){ r.x + r.width - bs - D(6), r.y, bs, bs }, IC_LOOP, D(22), loop_play ? C_ACC : C_DIM, "loop")) loop_play = !loop_play;
}

/* ============================== timeline ============================== */
static void timeline_coast(void) {
    if (playing || m_down) return;
    if (fabsf(tl_vel) > 1e-6f) {
        cur_t = clampf(cur_t + tl_vel, 0, P->duration); tl_vel *= 0.92f;
        if (fabsf(tl_vel) * tl_pps * DP < 0.25f) { tl_vel = 0; set_time(cur_t); }
    }
}
static void draw_timeline(Rectangle r) {
    DrawRectangleRec(r, C_SURF);
    const unsigned SCRUB = 9001;
    float pxs = tl_pps * DP, rh = D(26), row_h = D(40), gap = D(5), cx = r.x + r.width / 2, dur = P->duration;
    Rectangle ruler = { r.x, r.y, r.width, rh }, rows = { r.x, r.y + rh, r.width, r.height - rh };
#define TX(t) (cx + ((t) - cur_t) * pxs)
    if (m_pressed && in_rect(r, mp)) tl_vel = 0;
    if (pinch_scale != 1 && in_rect(r, pinch_c)) tl_pps = clampf(tl_pps * pinch_scale, 8, 800);
    if (!blocked && in_rect(r, mp) && wheel != 0 && key_ctrl) tl_pps = clampf(tl_pps * (1 + wheel * 0.12f), 8, 800);
    pxs = tl_pps * DP;
    /* ruler */
    DrawRectangleRec(ruler, C_SURF2);
    static const float cand[] = { 0.1f, 0.25f, 0.5f, 1, 2, 5, 10, 15, 30, 60, 120 };
    float step = 120;
    for (unsigned i = 0; i < sizeof cand / sizeof *cand; i++) if (cand[i] * pxs >= D(54)) { step = cand[i]; break; }
    float tv0 = cur_t - (cx - r.x) / pxs, tv1 = cur_t + (r.x + r.width - cx) / pxs;
    for (float t = floorf(tv0 / step) * step; t <= tv1 + step; t += step) {
        if (t < -1e-4f || t > dur + 1e-4f) continue;
        float xx = TX(t);
        DrawRectangle((int)xx, (int)(ruler.y + rh * 0.62f), 1, (int)(rh * 0.38f), C_DIM);
        char b[16]; if (t >= 60) snprintf(b, sizeof b, "%d:%02d", (int)t / 60, (int)t % 60); else snprintf(b, sizeof b, "%gs", (double)t);
        dtext(F, b, xx + D(3), ruler.y + D(3), T_TINY, C_DIM);
        float hx = TX(t + step / 2);
        if (t + step / 2 <= dur) DrawRectangle((int)hx, (int)(ruler.y + rh * 0.78f), 1, (int)(rh * 0.22f), alpha_c(C_DIM, 0.5f));
    }
    /* tracks */
    float y0 = scroll_begin(rows, &scr[SC_TL]);
    int n = P->nlayers; bool tapped_clip = false;
    float x0 = TX(0), x1 = TX(dur);
    if (x0 > r.x) DrawRectangle((int)r.x, (int)rows.y, (int)fminf(x0 - r.x, r.width), (int)rows.height, alpha_c(BLACK, 0.35f));
    if (x1 < r.x + r.width) DrawRectangle((int)fmaxf(x1, r.x), (int)rows.y, (int)(r.x + r.width - fmaxf(x1, r.x)), (int)rows.height, alpha_c(BLACK, 0.35f));
    for (int k = 0; k < n; k++) {
        int li = n - 1 - k; Layer *l = &P->layers[li];
        float ry = y0 + k * (row_h + gap) + gap;
        float out = project_layer_out(P, l), a = TX(l->tin), b = TX(out);
        Rectangle bar = { a, ry, fmaxf(b - a, D(12)), row_h };
        bool is = li == sel;
        if (ry + row_h < rows.y || ry > rows.y + rows.height) continue;
        if (bar.x > r.x + r.width + D(40) || bar.x + bar.width < r.x - D(40)) continue;
        Color bc = LTYPE_COL[l->type];
        Color fill = l->visible ? mix_c(bc, C_SURF, is ? 0.0f : 0.38f) : alpha_c(bc, 0.25f);
        rrect(bar, D(8), fill);
        float lx0 = fmaxf(bar.x, r.x) + D(8), lx1 = fminf(bar.x + bar.width, r.x + r.width) - D(8);
        if (lx1 - lx0 > D(34)) {
            icon_c(LTYPE_ICON[l->type], (Rectangle){ lx0, bar.y, D(20), bar.height }, D(15), C_INK);
            dtext_fit(FB, l->name, (Rectangle){ lx0 + D(22), bar.y, lx1 - lx0 - D(22), bar.height }, T_SMALL, C_INK, 0);
        }
        float hw = fminf(D(15), bar.width / 3);
        if (is) {
            rrect_line(bar, D(8), D(2.2f), WHITE);
            Rectangle hl = { bar.x, bar.y, hw, bar.height }, hr = { bar.x + bar.width - hw, bar.y, hw, bar.height }, bd = { bar.x + hw, bar.y, bar.width - 2 * hw, bar.height };
            rrect(hl, D(6), WHITE); rrect(hr, D(6), WHITE);
            for (int s2 = 0; s2 < 2; s2++) { Rectangle h = s2 ? hr : hl; DrawRectangle((int)(h.x + h.width / 2 - D(0.8f)), (int)(h.y + h.height * 0.3f), (int)fmaxf(2, D(1.6f)), (int)(h.height * 0.4f), C_INK); }
            Rectangle hlx = { hl.x - D(10), hl.y, hl.width + D(10), hl.height }, hrx = { hr.x, hr.y, hr.width + D(10), hr.height };
            float d, dt_ = 1.0f / pxs;
            if (hdrag(7000u + (unsigned)li * 4, hlx, &d)) { l->tin = clampf(l->tin + d * dt_, 0, out - 0.05f); P->rev++; }
            else if (hdrag(7001u + (unsigned)li * 4, hrx, &d)) { float o = clampf(out + d * dt_, l->tin + 0.05f, dur); l->tout = o >= dur - 1e-3f ? -1 : o; P->rev++; }
            else if (hdrag(7002u + (unsigned)li * 4, bd, &d)) {
                float len = out - l->tin, ni = clampf(l->tin + d * dt_, 0, dur - len);
                l->tin = ni; l->tout = ni + len >= dur - 1e-3f ? -1 : ni + len; P->rev++;
            }
            for (int pid = 0; pid < P_COUNT + MAX_FX * 3; pid++) {
                Prop *p = pid < P_COUNT ? &l->p[pid] : (pid - P_COUNT) / 3 < l->nfx ? &l->fx[(pid - P_COUNT) / 3].p[(pid - P_COUNT) % 3] : NULL;
                if (!p) continue;
                for (int q = 0; q < p->nkeys; q++) {
                    float kx = TX(p->keys[q].t);
                    Rectangle kr = { kx - D(11), bar.y + bar.height - D(20), D(22), D(18) };
                    if (kx < r.x - D(12) || kx > r.x + r.width + D(12)) continue;
                    draw_icon(IC_DIA_FILL, (Rectangle){ kx - D(6), kr.y + D(2), D(12), D(12) }, C_INK);
                    draw_icon(IC_DIA_FILL, (Rectangle){ kx - D(4.5f), kr.y + D(3.5f), D(9), D(9) }, WHITE);
                    if (tap(kr)) { set_time(p->keys[q].t); playing = false; tapped_clip = true; }
                }
            }
        }
        if (tap(bar)) { sel = li; tapped_clip = true; }
    }
    if (!n) dtext_fit(F, "Tap + to add your first layer", (Rectangle){ r.x, rows.y + rows.height / 2 - D(16), r.width, D(32) }, T_BODY, C_DIM, 1);
    scroll_end(rows, &scr[SC_TL], y0 + n * (row_h + gap) + D(64));
    Rectangle fab = { r.x + r.width - D(58), r.y + r.height - D(58), D(46), D(46) };
    bool fab_hit = tap(fab);
    if (tap(rows) && !tapped_clip && !fab_hit) sel = -1;
    if (tap(ruler)) { cur_t = clampf(cur_t + (mp.x - cx) / pxs, 0, dur); }
    /* scrubbing: drag anywhere that is not a handle */
    if (!blocked && axis == 1 && m_down && in_rect(r, mp_press) && (active == 0 || active == SCRUB)) {
        active = SCRUB; playing = false;
        cur_t = clampf(cur_t - m_dx / pxs, 0, dur);
        tl_vel = tl_vel * 0.5f + (-m_dx / pxs) * 0.5f;
    }
    if (m_released && active == SCRUB && fabsf(tl_vel) * pxs < 1.0f) { tl_vel = 0; set_time(cur_t); }
    /* playhead */
    DrawRectangle((int)(cx - D(1)), (int)r.y, (int)fmaxf(2, D(2)), (int)r.height, WHITE);
    DrawCircleV((Vector2){ cx, r.y + D(5) }, D(5), WHITE);
    /* add button */
    DrawCircleV((Vector2){ fab.x + fab.width / 2 + 1, fab.y + fab.height / 2 + 2 }, fab.width / 2, alpha_c(BLACK, 0.4f));
    DrawCircleV((Vector2){ fab.x + fab.width / 2, fab.y + fab.height / 2 }, fab.width / 2, held(fab) ? mix_c(C_ACC, WHITE, 0.25f) : C_ACC);
    icon_c(IC_PLUS, fab, D(24), C_INK);
    dbg_reg("timeline-add", fab);
    if (fab_hit) tool = TOOL_ADD;
#undef TX
}

/* ============================== top bar ============================== */
static void draw_topbar(Rectangle r) {
    DrawRectangleRec(r, C_BG);
    float bs = r.height;
    if (icon_btn((Rectangle){ r.x + D(4), r.y, bs, bs }, IC_MENU, D(24), C_TEXT, "menu")) { scan_files(); scr[SC_PROJ].y = 0; dlg = DLG_PROJECTS; }
    float pw = D(84), ph = D(34);
    Rectangle ex = { r.x + r.width - pw - D(12), r.y + (r.height - ph) / 2, pw, ph };
    if (pill(ex, "Export", true, C_ACC)) tool = tool == TOOL_EXPORT ? TOOL_NONE : TOOL_EXPORT;
    Rectangle rd = { ex.x - D(6) - bs * 0.85f, r.y, bs * 0.85f, bs }, ud = { rd.x - bs * 0.85f, r.y, bs * 0.85f, bs };
    if (icon_btn(ud, IC_UNDO, D(22), undo_n || P->rev != snap_rev ? C_TEXT : alpha_c(C_DIM, 0.5f), "undo")) pending_undo = -1;
    if (icon_btn(rd, IC_REDO, D(22), redo_n ? C_TEXT : alpha_c(C_DIM, 0.5f), "redo")) pending_undo = 1;
    Rectangle nm = { r.x + D(54), r.y, ud.x - r.x - D(60), r.height };
    dtext_fit(FB, P->name, nm, T_BODY, C_TEXT, 0);
    dbg_reg("title", nm);
    if (tap(nm)) edit_begin(900001, "Project name", P->name, false, false);
}

/* ============================== tool strip / sheets ============================== */
static const char *TOOL_TITLE[TOOL_COUNT] = { "", "Add layer", "Edit", "Style", "Effects", "Fusion", "Animate", "Layers", "Canvas", "Script", "Export" };
static void draw_tool_content(int t, Rectangle body) {
    switch (t) {
    case TOOL_ADD: sheet_add(body); break;
    case TOOL_TRANSFORM: sheet_transform(body); break;
    case TOOL_STYLE: sheet_style(body); break;
    case TOOL_FX: sheet_fx(body); break;
    case TOOL_FUSION: sheet_fusion(body); break;
    case TOOL_ANIM: sheet_anim(body); break;
    case TOOL_LAYERS: sheet_layers(body); break;
    case TOOL_CANVAS: sheet_canvas(body); break;
    case TOOL_SCRIPT: sheet_script(body); break;
    case TOOL_EXPORT: sheet_export(body); break;
    }
}

static float sheet_slide = 1; static int sheet_last_tool;
static void draw_sheet_portrait(Rectangle r) {
    if (sheet_last_tool == TOOL_NONE) sheet_slide = 0;
    sheet_last_tool = tool;
    sheet_slide += (1 - sheet_slide) * 0.35f; if (sheet_slide > 0.995f) sheet_slide = 1;
    r.y += (1 - sheet_slide) * r.height;
    rrect((Rectangle){ r.x, r.y, r.width, r.height + D(40) }, D(22), C_SURF);
    float hh = D(54);
    dtext(FB, TOOL_TITLE[tool], r.x + D(18), r.y + (hh - T_TITLE) / 2, T_TITLE, C_TEXT);
    Rectangle ok = { r.x + r.width - D(54), r.y + D(4), D(46), D(46) };
    Rectangle pl = { ok.x - D(52), r.y + D(4), D(46), D(46) };
    char tb[24]; fmt_time(tb, cur_t);
    dtext_fit(FM, tb, (Rectangle){ pl.x - D(96), r.y, D(90), hh }, T_SMALL, C_DIM, 2);
    if (icon_btn(pl, playing ? IC_PAUSE : IC_PLAY, D(22), C_TEXT, "sheet-play")) toggle_play();
    DrawCircleV((Vector2){ ok.x + ok.width / 2, ok.y + ok.height / 2 }, D(17), C_ACC);
    icon_c(IC_CHECK, ok, D(20), C_INK);
    dbg_reg("sheet-ok", ok);
    if (tap(ok)) tool = TOOL_NONE;
    Rectangle sc = { r.x + D(10), r.y + hh - D(2), r.width - D(20), D(30) };
    float tv = cur_t;
    if (slider(9002, sc, &tv, 0, fmaxf(P->duration - 1.0f / P->fps, 0.01f), C_ACC2)) { playing = false; set_time(tv); }
    Rectangle body = { r.x, r.y + hh + D(32), r.width, r.height - hh - D(32) };
    draw_tool_content(tool, body);
}

typedef struct { int ic; const char *label; int tool; int act; bool danger; } TItem;
static int build_items(TItem *it) {
    int n = 0;
    if (sel_layer()) {
        Layer *l = sel_layer();
        it[n++] = (TItem){ IC_BACK, "Back", 0, ACT_BACK, false };
        it[n++] = (TItem){ IC_MOVE, "Edit", TOOL_TRANSFORM, 0, false };
        it[n++] = (TItem){ IC_STYLE, "Style", TOOL_STYLE, 0, false };
        it[n++] = (TItem){ IC_FX, "Effects", TOOL_FX, 0, false };
        it[n++] = (TItem){ IC_FUSION, "Fusion", TOOL_FUSION, 0, false };
        it[n++] = (TItem){ IC_ANIM, "Animate", TOOL_ANIM, 0, false };
        it[n++] = (TItem){ IC_SPLIT, "Split", 0, ACT_SPLIT, false };
        it[n++] = (TItem){ IC_COPY, "Duplicate", 0, ACT_DUP, false };
        it[n++] = (TItem){ l->visible ? IC_EYE : IC_EYE_OFF, l->visible ? "Hide" : "Show", 0, ACT_HIDE, false };
        it[n++] = (TItem){ IC_UP, "Forward", 0, ACT_UP, false };
        it[n++] = (TItem){ IC_DOWN, "Backward", 0, ACT_DOWN, false };
        it[n++] = (TItem){ IC_TRASH, "Delete", 0, ACT_DEL, true };
    } else {
        it[n++] = (TItem){ IC_PLUS, "Add", TOOL_ADD, 0, false };
        it[n++] = (TItem){ IC_LAYERS, "Layers", TOOL_LAYERS, 0, false };
        it[n++] = (TItem){ IC_CANVAS, "Canvas", TOOL_CANVAS, 0, false };
        it[n++] = (TItem){ IC_SCRIPT, "Script", TOOL_SCRIPT, 0, false };
        it[n++] = (TItem){ IC_EXPORT, "Export", TOOL_EXPORT, 0, false };
    }
    return n;
}
static float tb_x, tb_vel;
static void draw_toolbar(Rectangle r) {
    DrawRectangleRec(r, C_BG);
    DrawRectangle((int)r.x, (int)r.y, (int)r.width, 1, C_LINE);
    TItem it[16]; int n = build_items(it);
    float iw = fmaxf(D(68), r.width / n), total = iw * n, maxs = fmaxf(0, total - r.width);
    if (!blocked && axis == 1 && m_down && in_rect(r, mp_press) && (active == 0 || active == 9003)) { active = 9003; tb_x -= m_dx; tb_vel = -vel_x; }
    else if (!m_down) { tb_x += tb_vel; tb_vel *= 0.92f; if (fabsf(tb_vel) < 0.3f) tb_vel = 0; }
    tb_x = clampf(tb_x, 0, maxs);
    BeginScissorMode((int)r.x, (int)r.y, (int)r.width, (int)r.height);
    clip_r = r;
    for (int i = 0; i < n; i++) {
        Rectangle b = { r.x + i * iw - tb_x, r.y, iw, r.height - D(4) };
        bool on = it[i].tool && tool == it[i].tool, h = held(b);
        Color col = on ? C_ACC : it[i].danger ? C_RED : C_TEXT;
        if (h) rrect(inset_xy(b, D(4), D(4)), D(12), C_SURF2);
        icon_c(it[i].ic, (Rectangle){ b.x, b.y + D(8), b.width, D(28) }, D(24), col);
        dtext_fit(F, it[i].label, (Rectangle){ b.x, b.y + D(38), b.width, D(18) }, T_TINY, on ? C_ACC : it[i].danger ? C_RED : C_DIM, 1);
        dbg_reg(it[i].label, b);
        if (tap(b)) { if (it[i].tool) tool = tool == it[i].tool ? TOOL_NONE : it[i].tool; else do_action(it[i].act); }
    }
    EndScissorMode();
    clip_r = (Rectangle){ 0, 0, (float)SW, (float)SH };
}

static float tab_x, tab_vel;
static void draw_panel_landscape(Rectangle r) {
    DrawRectangleRec(r, C_SURF);
    DrawRectangle((int)r.x, (int)r.y, 1, (int)r.height, C_LINE);
    int eff = tool == TOOL_NONE ? TOOL_ADD : tool;
    static const int TABS[10] = { TOOL_ADD, TOOL_LAYERS, TOOL_TRANSFORM, TOOL_STYLE, TOOL_FX, TOOL_FUSION, TOOL_ANIM, TOOL_CANVAS, TOOL_SCRIPT, TOOL_EXPORT };
    static const char *TN[10] = { "Add", "Layers", "Edit", "Style", "Effects", "Fusion", "Animate", "Canvas", "Script", "Export" };
    float th = D(46), tabw[10], total = 0;
    for (int i = 0; i < 10; i++) { tabw[i] = tw(FB, TN[i], T_BODY) + D(28); total += tabw[i]; }
    Rectangle strip = { r.x, r.y, r.width, th };
    float maxs = fmaxf(0, total - r.width);
    if (!blocked && axis == 1 && m_down && in_rect(strip, mp_press) && (active == 0 || active == 9004)) { active = 9004; tab_x -= m_dx; tab_vel = -vel_x; }
    else if (!m_down) { tab_x += tab_vel; tab_vel *= 0.92f; if (fabsf(tab_vel) < 0.3f) tab_vel = 0; }
    tab_x = clampf(tab_x, 0, maxs);
    BeginScissorMode((int)strip.x, (int)strip.y, (int)strip.width, (int)strip.height);
    clip_r = strip;
    float x = r.x - tab_x;
    for (int i = 0; i < 10; i++) {
        Rectangle b = { x, r.y, tabw[i], th };
        bool on = eff == TABS[i], dimmed = (TABS[i] == TOOL_TRANSFORM || TABS[i] == TOOL_STYLE || TABS[i] == TOOL_FX || TABS[i] == TOOL_FUSION || TABS[i] == TOOL_ANIM) && !sel_layer();
        dtext_fit(on ? FB : F, TN[i], b, T_BODY, on ? C_ACC : dimmed ? alpha_c(C_DIM, 0.5f) : C_TEXT, 1);
        if (on) DrawRectangle((int)(b.x + D(10)), (int)(b.y + th - D(3)), (int)(b.width - D(20)), (int)fmaxf(2, D(2.5f)), C_ACC);
        dbg_reg(TN[i], b);
        if (tap(b)) tool = TABS[i];
        x += tabw[i];
    }
    EndScissorMode();
    clip_r = (Rectangle){ 0, 0, (float)SW, (float)SH };
    DrawRectangle((int)r.x, (int)(r.y + th - 1), (int)r.width, 1, C_LINE);
    float y = r.y + th;
    if (sel_layer()) {
        static const int AI[7] = { IC_BACK, IC_SPLIT, IC_COPY, IC_EYE, IC_UP, IC_DOWN, IC_TRASH };
        static const int AA[7] = { ACT_BACK, ACT_SPLIT, ACT_DUP, ACT_HIDE, ACT_UP, ACT_DOWN, ACT_DEL };
        static const char *AN[7] = { "Deselect", "Split", "Duplicate", "Hide", "Forward", "Backward", "Delete" };
        float aw = r.width / 7, ah = D(42);
        for (int i = 0; i < 7; i++) {
            Layer *l = sel_layer();
            int ic = (i == 3 && !l->visible) ? IC_EYE_OFF : AI[i];
            if (icon_btn((Rectangle){ r.x + i * aw, y, aw, ah }, ic, D(20), AA[i] == ACT_DEL ? C_RED : C_TEXT, AN[i])) { do_action(AA[i]); if (!sel_layer()) break; }
        }
        y += ah;
        DrawRectangle((int)r.x, (int)(y - 1), (int)r.width, 1, C_LINE);
    }
    draw_tool_content(eff, (Rectangle){ r.x, y, r.width, r.y + r.height - y });
}

/* ============================== dialogs ============================== */
static int proj_del_armed = -1;
static void dialog_projects(void) {
    DrawRectangle(0, 0, SW, SH, C_BG);
    float hh = D(56);
    if (icon_btn((Rectangle){ D(4), 0, D(52), hh }, IC_BACK, D(24), C_TEXT, "proj-back")) { dlg = DLG_NONE; proj_del_armed = -1; }
    dtext(FB, "Projects", D(60), (hh - T_TITLE * 1.15f) / 2, T_TITLE * 1.15f, C_TEXT);
    Rectangle body = { 0, hh, (float)SW, SH - hh };
    float cw = fminf(SW - D(32), D(640)), x = (SW - cw) / 2;
    float y = scroll_begin(body, &scr[SC_PROJ]) + D(6);
    section(&y, x, "CREATE");
    static const char *TN[3] = { "Blank 9:16", "Blank 16:9", "Demo scene" };
    static const int TI[3] = { IC_NEW, IC_NEW, IC_ASTEROID };
    float g = D(10), tw3 = (cw - 2 * g) / 3;
    for (int i = 0; i < 3; i++) {
        Rectangle t = { x + i * (tw3 + g), y, tw3, D(92) };
        bool h = held(t);
        rrect(t, D(16), h ? C_SURF3 : C_SURF2);
        icon_c(TI[i], (Rectangle){ t.x, t.y + D(12), t.width, D(34) }, D(30), i == 2 ? C_ACC : C_TEXT);
        dtext_fit(FB, TN[i], (Rectangle){ t.x, t.y + D(54), t.width, D(26) }, T_SMALL * 1.05f, C_TEXT, 1);
        dbg_reg(TN[i], t);
        if (tap(t)) { new_project(i); dlg = DLG_NONE; set_status(0, "Created %s", P->name); }
    }
    y += D(104);
    section(&y, x, "SAVED PROJECTS");
    if (!nfiles) { dtext_fit(F, "Nothing saved yet - projects autosave while you edit.", (Rectangle){ x, y, cw, D(40) }, T_SMALL, C_DIM, 0); y += D(46); }
    for (int i = 0; i < nfiles; i++) {
        Rectangle row = { x, y, cw, D(64) };
        char full[PATH_LEN]; snprintf(full, sizeof full, "%s/%s", proj_dir, files[i]);
        bool cur = P->filepath[0] && !strcmp(full, P->filepath);
        rrect(row, D(14), cur ? alpha_c(C_ACC, 0.14f) : C_SURF2);
        if (cur) rrect_line(row, D(14), D(1.5f), C_ACC);
        char nm[PATH_LEN]; snprintf(nm, sizeof nm, "%s", files[i]); nm[strlen(nm) - 4] = 0;
        Rectangle trash = { row.x + row.width - D(56), row.y, D(52), row.height }, label = { row.x + D(16), row.y, row.width - D(80), row.height };
        dtext_fit(FB, nm, (Rectangle){ label.x, label.y + D(8), label.width, D(26) }, T_BODY, C_TEXT, 0);
        time_t tt = (time_t)fmtime[i]; struct tm *tmv = localtime(&tt); char ds[48] = "";
        if (tmv) strftime(ds, sizeof ds, "%d %b %Y, %H:%M", tmv);
        dtext_fit(F, cur ? "Open now" : ds, (Rectangle){ label.x, label.y + D(32), label.width, D(22) }, T_SMALL, cur ? C_ACC : C_DIM, 0);
        bool arm = proj_del_armed == i;
        if (arm) rrect(inset_xy(trash, D(2), D(10)), D(12), alpha_c(C_RED, 0.25f));
        bool del = icon_btn(trash, IC_TRASH, D(20), C_RED, arm ? "Confirm delete" : NULL);
        if (del) { if (arm) { remove(full); proj_del_armed = -1; scan_files(); break; } else proj_del_armed = i; }
        else if (tap(label)) { if (open_project(files[i])) { dlg = DLG_NONE; set_status(0, "Opened %s", nm); } }
        y += D(70);
    }
    scroll_end(body, &scr[SC_PROJ], y);
}

typedef struct { char path[PATH_LEN]; char name[96]; char tag[24]; long mt; } MFile;
static MFile media_files[200]; static int nmedia; static bool media_video;
static int vid_pending[16], nvid_pending;
static bool img_ext_ok(const char *n) {
    const char *x = strrchr(n, '.');
    return x && (!strcasecmp(x, ".svg") || !strcasecmp(x, ".png") || !strcasecmp(x, ".jpg") || !strcasecmp(x, ".jpeg") || !strcasecmp(x, ".bmp"));
}
static void scan_dir(const char *dir, const char *tag, bool video) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && nmedia < 200) {
        if (e->d_name[0] == '.') continue;
        if (!(video ? media_is_video_ext(e->d_name) : img_ext_ok(e->d_name))) continue;
        MFile *m = &media_files[nmedia];
        snprintf(m->path, PATH_LEN, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(m->path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        snprintf(m->name, sizeof m->name, "%s", e->d_name); snprintf(m->tag, sizeof m->tag, "%s", tag); m->mt = (long)st.st_mtime;
        nmedia++;
    }
    closedir(d);
}
static int mf_cmp(const void *a, const void *b) { long x = ((const MFile *)a)->mt, y = ((const MFile *)b)->mt; return x < y ? 1 : x > y ? -1 : 0; }
static void scan_media(void) {
    char dir[PATH_LEN]; snprintf(dir, sizeof dir, "%s/media", proj_dir); mkdir(dir, 0755);
    nmedia = 0;
    Layer *l = sel_layer(); media_video = l && l->type == L_VIDEO;
    scan_dir(dir, "Project", media_video);
#ifdef PLATFORM_ANDROID
    static const char *DEV[][2] = { { "/storage/emulated/0/Movies", "Movies" }, { "/storage/emulated/0/DCIM/Camera", "Camera" }, { "/storage/emulated/0/DCIM", "DCIM" },
                                    { "/storage/emulated/0/Download", "Download" }, { "/storage/emulated/0/Pictures", "Pictures" } };
    for (int i = 0; i < 5; i++) scan_dir(DEV[i][0], DEV[i][1], media_video);
#else
    const char *home = getenv("HOME");
    if (home) { static const char *SUBS[] = { "Videos", "Movies", "Downloads", "Pictures" }; for (int i = 0; i < 4; i++) { char d2[PATH_LEN]; snprintf(d2, sizeof d2, "%s/%s", home, SUBS[i]); scan_dir(d2, SUBS[i], media_video); } }
#endif
    qsort(media_files, (size_t)nmedia, sizeof media_files[0], mf_cmp);
}
static void open_media_picker(void) { scan_media(); scr[SC_MEDIA].y = 0; dlg = DLG_MEDIA; }

/* a freshly chosen video: start the proxy import and, once it is known, trim the clip to its real length */
static void video_assign(Layer *l) {
    Media *m = media_get(l->path, true);
    media_start(m);
    const char *b = strrchr(l->path, '/'); b = b ? b + 1 : l->path;
    snprintf(l->name, NAME_LEN, "%s", b);
    char *dot = strrchr(l->name, '.'); if (dot && dot != l->name) *dot = 0;
    l->trim = 0;
    for (int i = 0; i < nvid_pending; i++) if (vid_pending[i] == l->uid) return;
    if (nvid_pending < 16) vid_pending[nvid_pending++] = l->uid;
}
static void video_tick(void) {
    for (int i = 0; i < nvid_pending; i++) {
        Layer *l = project_find_uid(P, vid_pending[i]);
        Media *m = l && l->path[0] ? media_get(l->path, false) : NULL;
        int st = m ? media_state(m) : 3;
        if (st == 1) continue;
        if (l && st == 2) {
            float dur = 0; media_info(m, NULL, NULL, NULL, &dur);
            float sp = l->speed > 0.01f ? l->speed : 1, out = l->tin + fmaxf(0.1f, (dur - l->trim) / sp);
            if (out > P->duration) P->duration = fminf(600, ceilf(out * 10) / 10);
            l->tout = fminf(out, P->duration);
            P->rev++;
            set_status(0, "Video ready (%.1f s)", (double)dur);
        } else if (l && st == 3) set_status(1, "Video import failed: %s", m ? media_error(m) : "unknown");
        vid_pending[i--] = vid_pending[--nvid_pending];
    }
}
static void dialog_media(void) {
    DrawRectangle(0, 0, SW, SH, C_BG);
    float hh = D(56);
    if (icon_btn((Rectangle){ D(4), 0, D(52), hh }, IC_BACK, D(24), C_TEXT, "media-back")) dlg = DLG_NONE;
    dtext(FB, media_video ? "Choose a video" : "Choose a file", D(60), (hh - T_TITLE * 1.15f) / 2, T_TITLE * 1.15f, C_TEXT);
    Rectangle body = { 0, hh, (float)SW, SH - hh };
    float cw = fminf(SW - D(32), D(640)), x = (SW - cw) / 2;
    float y = scroll_begin(body, &scr[SC_MEDIA]) + D(6);
    Layer *l = sel_layer();
    if (!l) { dlg = DLG_NONE; EndScissorMode(); clip_r = (Rectangle){ 0, 0, (float)SW, (float)SH }; return; }
    if (pill((Rectangle){ x, y, cw, D(46) }, "Type a file path...", false, C_ACC)) { dlg = DLG_NONE; edit_begin(rid(l, 52) * 8, media_video ? "Video path" : "File path (svg / png / jpg)", l->path, false, false); }
    y += D(54);
    if (pill((Rectangle){ x, y, cw, D(40) }, "Rescan folders", false, C_ACC)) scan_media();
    y += D(50);
    section(&y, x, media_video ? "VIDEOS ON THIS DEVICE" : "IMAGES");
    char dir[PATH_LEN]; snprintf(dir, sizeof dir, "%s/media", proj_dir);
    if (!nmedia) {
        dtext_fit(F, media_video ? "No videos found. Allow media access when Android asks," : "No files found. Copy .svg / .png / .jpg files into the", (Rectangle){ x, y, cw, D(22) }, T_SMALL, C_DIM, 0); y += D(24);
        dtext_fit(F, media_video ? "or copy a video into the project media folder:" : "project media folder:", (Rectangle){ x, y, cw, D(22) }, T_SMALL, C_DIM, 0); y += D(24);
        dtext_fit(F, dir, (Rectangle){ x, y, cw, D(22) }, T_TINY, C_DIM, 0); y += D(30);
    }
    for (int i = 0; i < nmedia; i++) {
        Rectangle row = { x, y, cw, D(56) };
        if (row.y + row.height > body.y && row.y < body.y + body.height) {
            rrect(row, D(12), held(row) ? C_SURF3 : C_SURF2);
            dtext_fit(F, media_files[i].name, (Rectangle){ row.x + D(16), row.y + D(6), row.width - D(96), D(26) }, T_BODY, C_TEXT, 0);
            dtext_fit(F, media_files[i].tag, (Rectangle){ row.x + D(16), row.y + D(31), row.width - D(32), D(18) }, T_TINY, C_DIM, 0);
            if (tap(row)) {
                snprintf(l->path, PATH_LEN, "%s", media_files[i].path);
                layer_free_cache(l); P->rev++; dlg = DLG_NONE;
                if (l->type == L_VIDEO) video_assign(l);
                break;
            }
        }
        y += D(62);
    }
    scroll_end(body, &scr[SC_MEDIA], y);
}

static void open_script_editor(void) { dlg = DLG_SCRIPT; edit_begin(900100, "User script", P->user_code ? P->user_code : "", false, true); }
static void script_store(bool run) {
    free(P->user_code); P->user_code = strdup(te.buf);
    if (run) { bool ok = script_run_user(P); set_status(ok ? 0 : 1, ok ? "Script ran" : "Script has errors (see Script tool)"); }
}
static void dialog_script(void) {
    float oh = osk_height();
    Rectangle full = { 0, 0, (float)SW, SH - oh };
    DrawRectangleRec(full, C_BG);
    float hh = D(52);
    DrawRectangle(0, 0, SW, (int)hh, C_SURF);
    dtext(FB, "User script", D(16), (hh - T_BODY) / 2, T_BODY, C_ACC);
    Rectangle brun = { SW - D(176), D(8), D(80), hh - D(16) }, bclose = { SW - D(90), D(8), D(80), hh - D(16) };
    if (pill(brun, "Run", true, C_ACC)) script_store(true);
    if (pill(bclose, "Close", false, C_ACC)) { script_store(false); edit_cancel(); dlg = DLG_NONE; return; }
    Rectangle area = { 0, hh, (float)SW, full.height - hh };
    float fs = T_BODY * 0.9f, lh = fs * 1.45f, gut = tw(FM, "0000", fs) + D(12);
    int line = 0, nl = 1;
    for (int i = 0; i < te.len; i++) if (te.buf[i] == '\n') { nl++; if (i < te.cur) line++; }
    if (te.ensure_vis) {
        float cy = line * lh;
        if (cy < te.scroll_y) te.scroll_y = cy;
        if (cy + lh > te.scroll_y + area.height) te.scroll_y = cy + lh - area.height;
        te.ensure_vis = false;
    }
    if (m_down && axis == 2 && active == 0 && in_rect(area, mp_press)) te.scroll_y -= m_dy;
    if (in_rect(area, mp)) te.scroll_y -= wheel * lh * 3;
    te.scroll_y = clampf(te.scroll_y, 0, fmaxf(0, nl * lh + lh * 2 - area.height));
    BeginScissorMode((int)area.x, (int)area.y, (int)area.width, (int)area.height);
    const char *p = te.buf; int ln = 0;
    for (;;) {
        const char *e = strchr(p, '\n'); int len = e ? (int)(e - p) : (int)strlen(p);
        float yy = area.y + ln * lh - te.scroll_y;
        if (yy > area.y + area.height) break;
        if (yy + lh >= area.y) {
            char nb[12]; snprintf(nb, sizeof nb, "%d", ln + 1);
            dtext(FM, nb, gut - D(8) - tw(FM, nb, fs), yy, fs, C_DIM);
            char tmp[400]; snprintf(tmp, sizeof tmp, "%.*s", len < 390 ? len : 390, p);
            const char *q = tmp; while (*q == ' ' || *q == '\t') q++;
            dtext(FM, tmp, gut, yy, fs, (q[0] == '-' && q[1] == '-') ? C_GREEN : C_TEXT);
            if (ln == line && ((int)(GetTime() * 2)) % 2 == 0) {
                int col = te.cur - (int)(p - te.buf); if (col > 390) col = 390;
                char pre[400]; snprintf(pre, sizeof pre, "%.*s", col, p);
                DrawRectangle((int)(gut + tw(FM, pre, fs)), (int)yy, (int)fmaxf(2, D(1.5f)), (int)lh, C_ACC);
            }
        }
        if (!e) break;
        p = e + 1; ln++;
    }
    EndScissorMode();
    if (m_released && !dragged && in_rect(area, mp) && in_rect(area, mp_press)) {
        int tl = (int)((mp.y - area.y + te.scroll_y) / lh); if (tl < 0) tl = 0;
        int i = 0, cl = 0;
        while (cl < tl && i < te.len) { if (te.buf[i] == '\n') cl++; i++; }
        int le = line_end(i);
        te.cur = i + caret_from_x(FM, te.buf + i, le - i, mp.x - gut, fs);
    }
}

static void draw_toast(Rectangle anchor) {
    if (GetTime() >= status_until) return;
    float a = clampf((float)(status_until - GetTime()) / 0.5f, 0, 1);
    float tws = tw(FB, status, T_SMALL * 1.05f), w = fminf(tws + D(36), SW - D(24)), h = D(36);
    Rectangle r = { anchor.x + (anchor.width - w) / 2, anchor.y + anchor.height - h - D(12), w, h };
    rrect(r, h / 2, alpha_c(status_lvl ? (Color){ 120, 34, 34, 255 } : (Color){ 44, 44, 58, 255 }, 0.96f * a));
    dtext_fit(FB, status, inset_xy(r, D(14), 0), T_SMALL * 1.05f, alpha_c(C_TEXT, a), 1);
}

/* ============================== test harness (--uitest) ============================== */
typedef struct { int kind; float x, y; char file[160]; } UtAct;
enum { UT_IDLE, UT_PRESS, UT_MOVE, UT_RELEASE, UT_SHOT, UT_QUIT };
static bool ut_on, ut_quit;
static char **ut_lines; static int ut_nlines, ut_next;
static UtAct ut_q[1024]; static int ut_qn, ut_qi;
static char ut_shot_file[160];
static void ut_push(int k, float x, float y, const char *f) { if (ut_qn < 1024) { ut_q[ut_qn].kind = k; ut_q[ut_qn].x = x; ut_q[ut_qn].y = y; snprintf(ut_q[ut_qn].file, 160, "%s", f ? f : ""); ut_qn++; } }
static bool ut_find(const char *label, Vector2 *c) {
    for (int i = 0; i < dbg_pn; i++) if (!strcmp(dbg_prev[i].l, label)) { *c = (Vector2){ dbg_prev[i].r.x + dbg_prev[i].r.width / 2, dbg_prev[i].r.y + dbg_prev[i].r.height / 2 }; return true; }
    return false;
}
static void ut_tap(float x, float y) { ut_push(UT_PRESS, x, y, 0); ut_push(UT_RELEASE, x, y, 0); for (int i = 0; i < 3; i++) ut_push(UT_IDLE, x, y, 0); }
static void ut_drag(float x1, float y1, float x2, float y2, int steps) {
    ut_push(UT_PRESS, x1, y1, 0);
    for (int i = 1; i <= steps; i++) ut_push(UT_MOVE, x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps, 0);
    ut_push(UT_RELEASE, x2, y2, 0);
    for (int i = 0; i < 3; i++) ut_push(UT_IDLE, x2, y2, 0);
}
static void ut_expand(const char *line) {
    char cmd[32] = ""; float a = 0, b = 0, c = 0, d = 0; int n = 0;
    const char *rest = line; while (*rest && *rest != ' ') rest++; while (*rest == ' ') rest++;
    sscanf(line, "%31s", cmd);
    if (!strcmp(cmd, "wait")) { sscanf(rest, "%d", &n); for (int i = 0; i < n; i++) ut_push(UT_IDLE, 0, 0, 0); }
    else if (!strcmp(cmd, "tap")) { sscanf(rest, "%f %f", &a, &b); ut_tap(a, b); }
    else if (!strcmp(cmd, "tapl")) { Vector2 p; if (ut_find(rest, &p)) ut_tap(p.x, p.y); else { fprintf(stderr, "UITEST: label not found: '%s'\n", rest); for (int i = 0; i < dbg_pn && i < 60; i++) fprintf(stderr, "   [%s]", dbg_prev[i].l); fprintf(stderr, "\n"); } }
    else if (!strcmp(cmd, "drag")) { n = 12; sscanf(rest, "%f %f %f %f %d", &a, &b, &c, &d, &n); ut_drag(a, b, c, d, n); }
    else if (!strcmp(cmd, "dragl")) { char lab[48] = ""; n = 12; const char *sp = strrchr(rest, ' '); (void)sp;
        /* syntax: dragl DX DY LABEL... */ sscanf(rest, "%f %f", &a, &b); const char *l2 = rest; for (int k = 0; k < 2; k++) { while (*l2 && *l2 != ' ') l2++; while (*l2 == ' ') l2++; }
        snprintf(lab, sizeof lab, "%s", l2); Vector2 p; if (ut_find(lab, &p)) ut_drag(p.x, p.y, p.x + a, p.y + b, n); else fprintf(stderr, "UITEST: label not found: '%s'\n", lab); }
    else if (!strcmp(cmd, "shot")) { for (int i = 0; i < 4; i++) ut_push(UT_IDLE, 0, 0, 0); ut_push(UT_SHOT, 0, 0, rest); ut_push(UT_IDLE, 0, 0, 0); }
    else if (!strcmp(cmd, "type")) { edit_insert(rest); ut_push(UT_IDLE, 0, 0, 0); }
    else if (!strcmp(cmd, "sel")) { sscanf(rest, "%d", &n); sel = n; }
    else if (!strcmp(cmd, "tool")) { sscanf(rest, "%d", &n); tool = n; }
    else if (!strcmp(cmd, "time")) { sscanf(rest, "%f", &a); set_time(a); }
    else if (!strcmp(cmd, "play")) { sscanf(rest, "%d", &n); playing = n != 0; }
    else if (!strcmp(cmd, "proj")) { new_project(!strcmp(rest, "demo") ? 2 : !strcmp(rest, "wide") ? 1 : 0); }
    else if (!strcmp(cmd, "dump")) { for (int i = 0; i < dbg_pn; i++) fprintf(stderr, "[%s] %.0f,%.0f %.0fx%.0f\n", dbg_prev[i].l, (double)dbg_prev[i].r.x, (double)dbg_prev[i].r.y, (double)dbg_prev[i].r.width, (double)dbg_prev[i].r.height); }
    else if (!strcmp(cmd, "quit")) ut_push(UT_QUIT, 0, 0, 0);
    (void)c; (void)d;
}
static void uitest_pre(void) {
    if (!ut_on) return;
    if (ut_qi >= ut_qn) {
        ut_qi = ut_qn = 0;
        while (ut_next < ut_nlines && ut_qn == 0) { const char *l = ut_lines[ut_next++]; if (l[0] && l[0] != '#') ut_expand(l); }
        if (ut_qn == 0) { ut_quit = true; return; }
    }
    UtAct *a = &ut_q[ut_qi++];
    switch (a->kind) {
    case UT_PRESS: sim.pos = (Vector2){ a->x, a->y }; sim.down = true; sim.pressed = true; break;
    case UT_MOVE: sim.pos = (Vector2){ a->x, a->y }; sim.down = true; break;
    case UT_RELEASE: sim.pos = (Vector2){ a->x, a->y }; sim.down = false; sim.released = true; break;
    case UT_SHOT: snprintf(ut_shot_file, sizeof ut_shot_file, "%s", a->file); break;
    case UT_QUIT: ut_quit = true; break;
    default: break;
    }
}
static void uitest_post(void) {
    if (!ut_shot_file[0]) return;
    Image im = LoadImageFromScreen();
    ExportImage(im, ut_shot_file); UnloadImage(im);
    fprintf(stderr, "UITEST: wrote %s (%dx%d)\n", ut_shot_file, SW, SH);
    ut_shot_file[0] = 0;
}

/* ============================== one UI frame ============================== */
static void apply_path_edit(void) {
    Layer *n = sel_layer();
    if (n && edit_done(rid(n, 52) * 8)) { snprintf(n->path, PATH_LEN, "%s", te.done_buf); layer_free_cache(n); P->rev++; }
    if (edit_done(900001)) { snprintf(P->name, NAME_LEN, "%s", te.done_buf); P->rev++; }
}
static void ui_frame(void) {
    SW = GetScreenWidth(); SH = GetScreenHeight();
    frame_no++;
    video_tick();
    dbg_swap();
    clip_r = (Rectangle){ 0, 0, (float)SW, (float)SH };
    uitest_pre();
    input_update();
    if (pending_undo) { if (pending_undo < 0) do_undo(); else do_redo(); pending_undo = 0; }
    layout_compute();

    Rectangle zone = { 0, 0, 0, 0 };
    if (te.active) { float oh = osk_height(); zone = te.multi ? (Rectangle){ 0, SH - oh, (float)SW, oh } : (Rectangle){ 0, bar_rect().y, (float)SW, bar_rect().height + oh }; }
    if (te.active && !te.multi && m_released && !dragged && !in_rect(zone, mp_press) && te.began_frame != frame_no) edit_commit();
    if (te.active) edit_poll_keys();

    if (!te.active && dlg == DLG_NONE && !sim.on) {
        if (IsKeyPressed(KEY_SPACE)) toggle_play();
        if (IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT)) { set_time(cur_t + 1.0f / P->fps); playing = false; }
        if (IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT)) { set_time(cur_t - 1.0f / P->fps); playing = false; }
        if (IsKeyPressed(KEY_HOME)) set_time(0);
        if (IsKeyPressed(KEY_DELETE) && sel_layer()) do_action(ACT_DEL);
        if (key_ctrl && IsKeyPressed(KEY_S)) { save_project(); set_status(0, "Saved"); }
        if (key_ctrl && IsKeyPressed(KEY_Z)) pending_undo = key_shift ? 1 : -1;
        if (key_ctrl && IsKeyPressed(KEY_Y)) pending_undo = 1;
    }
    if (playing) {
        cur_t += GetFrameTime();
        if (cur_t >= P->duration) { if (loop_play) cur_t = fmodf(cur_t, P->duration); else { cur_t = P->duration - 1.0f / P->fps; playing = false; } }
    }
    timeline_coast();
    if (sel >= P->nlayers) sel = P->nlayers - 1;
    export_tick();
    undo_tick();
    autosave_tick();
    preview_update();

    BeginDrawing();
    ClearBackground(C_BG);
    blocked = dlg != DLG_NONE || (te.active && in_rect(zone, mp_press));
    draw_preview(LY.prev);
    if (portrait) {
        draw_topbar(LY.top);
        if (LY.sheet_on) draw_sheet_portrait(LY.sheet);
        else { draw_transport(LY.trans); draw_timeline(LY.tl); draw_toolbar(LY.tb); }
    } else {
        draw_topbar(LY.top);
        draw_transport(LY.trans);
        draw_timeline(LY.tl);
        draw_panel_landscape(LY.panel);
    }
    apply_path_edit();
    draw_toast(portrait ? (LY.sheet_on ? (Rectangle){ 0, LY.sheet.y - D(40), (float)SW, D(40) } : LY.trans) : LY.prev);

    blocked = false;
    if (dlg == DLG_PROJECTS) dialog_projects();
    else if (dlg == DLG_MEDIA) dialog_media();
    else if (dlg == DLG_SCRIPT) dialog_script();
    draw_edit_bar();
    draw_osk();
    uitest_post();
    sheet_last_tool = (portrait && LY.sheet_on) ? sheet_last_tool : TOOL_NONE;
    EndDrawing();
}
