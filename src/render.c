#define _GNU_SOURCE
// Asteroid Fusion render engine: multi-threaded software compositor, dirty-rect buffers, video layers, Fusion node graphs
#include "afx.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

#define PI_F 3.14159265358979f
static inline float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float smoothf(float a, float b, float x) { float t = clampf((x - a) / (b - a), 0, 1); return t * t * (3 - 2 * t); }

/* ---------- assets ---------- */
const char *afx_asset_root = NULL;
const char *afx_find_asset(const char *rel, char *buf, size_t cap) {
    if (afx_asset_root) {
        snprintf(buf, cap, "%s/%s", afx_asset_root, rel);
        FILE *f0 = fopen(buf, "rb");
        if (f0) { fclose(f0); return buf; }
    }
    char exe[PATH_LEN] = { 0 };
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) { exe[n] = 0; char *s = strrchr(exe, '/'); if (s) *s = 0; }
    const char *fmts[] = { "assets/%s", "%s/assets/%s", "%s/../assets/%s", "%s/../../assets/%s", AFX_ASSET_DIR "/%s" };
    for (int i = 0; i < 5; i++) {
        if (i == 0 || i == 4) snprintf(buf, cap, fmts[i], rel); else snprintf(buf, cap, fmts[i], exe, rel);
        FILE *f = fopen(buf, "rb");
        if (f) { fclose(f); return buf; }
    }
    return NULL;
}

/* ---------- hashing / noise ---------- */
static uint32_t hash_u(uint32_t x) { x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return x; }
static float hash_f(uint32_t x) { return (hash_u(x) & 0xFFFFFF) / 16777216.0f; }
static float vnoise2(float x, float y, uint32_t seed) {
    int xi = (int)floorf(x), yi = (int)floorf(y);
    float fx = x - xi, fy = y - yi;
    fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
    float a = hash_f(hash_u(xi * 73856093u ^ yi * 19349663u ^ seed));
    float b = hash_f(hash_u((xi + 1) * 73856093u ^ yi * 19349663u ^ seed));
    float c = hash_f(hash_u(xi * 73856093u ^ (yi + 1) * 19349663u ^ seed));
    float d = hash_f(hash_u((xi + 1) * 73856093u ^ (yi + 1) * 19349663u ^ seed));
    return lerpf(lerpf(a, b, fx), lerpf(c, d, fx), fy);
}


/* ---------- rects ---------- */
static const IRect IR_NONE = { 0, 0, 0, 0 };
static inline IRect ir_make(int x0, int y0, int x1, int y1) { IRect r = { x0, y0, x1, y1 }; return r; }
static inline bool ir_empty(IRect r) { return r.x1 <= r.x0 || r.y1 <= r.y0; }
static inline IRect ir_union(IRect a, IRect b) {
    if (ir_empty(a)) return b;
    if (ir_empty(b)) return a;
    return ir_make(a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0, a.x1 > b.x1 ? a.x1 : b.x1, a.y1 > b.y1 ? a.y1 : b.y1);
}
static inline IRect ir_clip(IRect r, int W, int H) {
    if (r.x0 < 0) r.x0 = 0;
    if (r.y0 < 0) r.y0 = 0;
    if (r.x1 > W) r.x1 = W;
    if (r.y1 > H) r.y1 = H;
    return ir_empty(r) ? IR_NONE : r;
}
static inline IRect ir_grow(IRect r, int n, int W, int H) {
    if (ir_empty(r)) return r;
    return ir_clip(ir_make(r.x0 - n, r.y0 - n, r.x1 + n, r.y1 + n), W, H);
}
static inline IRect ir_full(int W, int H) { return ir_make(0, 0, W, H); }

/* ---------- thread pool (row-parallel loops) ---------- */
typedef void (*RowFn)(int, int, void *);
static struct {
    pthread_t th[15]; int nth; bool init;
    pthread_mutex_t mu; pthread_cond_t cv_start, cv_done;
    RowFn fn; void *ctx; int rows, chunk; atomic_int next; int pending; unsigned gen;
} PL = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv_start = PTHREAD_COND_INITIALIZER, .cv_done = PTHREAD_COND_INITIALIZER };
static void run_chunks(RowFn fn, void *ctx, int rows, int chunk) {
    for (;;) {
        int y = atomic_fetch_add(&PL.next, chunk);
        if (y >= rows) break;
        fn(y, y + chunk < rows ? y + chunk : rows, ctx);
    }
}
static void *pool_main(void *arg) {
    (void)arg;
    unsigned seen = 0;
    for (;;) {
        pthread_mutex_lock(&PL.mu);
        while (PL.gen == seen) pthread_cond_wait(&PL.cv_start, &PL.mu);
        seen = PL.gen;
        RowFn fn = PL.fn; void *ctx = PL.ctx; int rows = PL.rows, chunk = PL.chunk;
        pthread_mutex_unlock(&PL.mu);
        run_chunks(fn, ctx, rows, chunk);
        pthread_mutex_lock(&PL.mu);
        if (--PL.pending == 0) pthread_cond_signal(&PL.cv_done);
        pthread_mutex_unlock(&PL.mu);
    }
    return NULL;
}
static void pool_init(void) {
    PL.init = true;
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    const char *e = getenv("AFX_THREADS");
    if (e) n = atol(e);
    if (n < 1) n = 1;
    if (n > 8) n = 8;
    for (int i = 0; i < n - 1; i++) if (pthread_create(&PL.th[PL.nth], NULL, pool_main, NULL) == 0) PL.nth++;
}
void par_rows(int rows, RowFn fn, void *ctx) {
    if (rows <= 0) return;
    if (!PL.init) pool_init();
    int chunk = rows / ((PL.nth + 1) * 4);
    if (chunk < 2) chunk = 2;
    if (PL.nth == 0 || rows <= chunk) { fn(0, rows, ctx); return; }
    pthread_mutex_lock(&PL.mu);
    PL.fn = fn; PL.ctx = ctx; PL.rows = rows; PL.chunk = chunk;
    atomic_store(&PL.next, 0);
    PL.pending = PL.nth; PL.gen++;
    pthread_cond_broadcast(&PL.cv_start);
    pthread_mutex_unlock(&PL.mu);
    run_chunks(fn, ctx, rows, chunk);
    pthread_mutex_lock(&PL.mu);
    while (PL.pending > 0) pthread_cond_wait(&PL.cv_done, &PL.mu);
    pthread_mutex_unlock(&PL.mu);
}

/* ---------- buffer helpers (rect limited, parallel) ---------- */
typedef struct { FBuf *b; const FBuf *s; IRect r; float mul; } RectCtx;
static void clear_rows(int r0, int r1, void *vp) {
    RectCtx *c = vp;
    for (int y = c->r.y0 + r0; y < c->r.y0 + r1; y++)
        memset(&c->b->p[((size_t)y * c->b->w + c->r.x0) * 4], 0, (size_t)(c->r.x1 - c->r.x0) * 4 * sizeof(float));
}
static void clear_rect(FBuf *b, IRect r) {
    r = ir_clip(r, b->w, b->h);
    if (ir_empty(r)) return;
    RectCtx c = { b, NULL, r, 0 };
    par_rows(r.y1 - r.y0, clear_rows, &c);
}
static void copy_rows(int r0, int r1, void *vp) {
    RectCtx *c = vp;
    for (int y = c->r.y0 + r0; y < c->r.y0 + r1; y++) {
        size_t o = ((size_t)y * c->b->w + c->r.x0) * 4;
        memcpy(&c->b->p[o], &c->s->p[o], (size_t)(c->r.x1 - c->r.x0) * 4 * sizeof(float));
    }
}
static void copy_rect(FBuf *dst, const FBuf *src, IRect r) {
    r = ir_clip(r, dst->w, dst->h);
    if (ir_empty(r)) return;
    RectCtx c = { dst, src, r, 0 };
    par_rows(r.y1 - r.y0, copy_rows, &c);
}
static void mul_rows(int r0, int r1, void *vp) {
    RectCtx *c = vp;
    for (int y = c->r.y0 + r0; y < c->r.y0 + r1; y++) {
        float *p = &c->b->p[((size_t)y * c->b->w + c->r.x0) * 4];
        for (int i = 0; i < (c->r.x1 - c->r.x0) * 4; i++) p[i] *= c->mul;
    }
}
static void ensure(FBuf *b, int w, int h) {
    if (b->p && b->w == w && b->h == h) return;
    fbuf_free(b); *b = fbuf_make(w, h);
}
/* ---------- transforms ---------- */
static float g_rsx = 1, g_rsy = 1, g_rs = 1;      /* render scale of the frame in progress (x, y, mean) */
typedef struct { float m00, m01, m10, m11, tx, ty; } Xf;
static Xf make_xf(float px, float py, float sx, float sy, float rot, float ax, float ay) {
    float r = rot * PI_F / 180, c = cosf(r), s = sinf(r);
    Xf x;
    x.m00 = c * sx; x.m01 = -s * sy; x.m10 = s * sx; x.m11 = c * sy;
    x.tx = px - (x.m00 * ax + x.m01 * ay);
    x.ty = py - (x.m10 * ax + x.m11 * ay);
    /* local (comp pixels) -> frame pixels */
    x.m00 *= g_rsx; x.m01 *= g_rsx; x.m10 *= g_rsy; x.m11 *= g_rsy; x.tx *= g_rsx; x.ty *= g_rsy;
    return x;
}
static bool inv_xf(const Xf *m, Xf *o) {
    float det = m->m00 * m->m11 - m->m01 * m->m10;
    if (fabsf(det) < 1e-9f) return false;
    float id = 1 / det;
    o->m00 = m->m11 * id; o->m01 = -m->m01 * id; o->m10 = -m->m10 * id; o->m11 = m->m00 * id;
    o->tx = m->tx; o->ty = m->ty;   /* local = I * (p - t) */
    return true;
}
static IRect xf_bbox(const Xf *m, float x0, float y0, float x1, float y1, int W, int H) {
    float xs[4] = { x0, x1, x1, x0 }, ys[4] = { y0, y0, y1, y1 };
    float mnx = 1e9f, mny = 1e9f, mxx = -1e9f, mxy = -1e9f;
    for (int i = 0; i < 4; i++) {
        float px = m->m00 * xs[i] + m->m01 * ys[i] + m->tx, py = m->m10 * xs[i] + m->m11 * ys[i] + m->ty;
        mnx = fminf(mnx, px); mxx = fmaxf(mxx, px); mny = fminf(mny, py); mxy = fmaxf(mxy, py);
    }
    if (!isfinite(mnx) || !isfinite(mxx) || !isfinite(mny) || !isfinite(mxy)) return IR_NONE;
    mnx = fmaxf(mnx, -1e6f); mny = fmaxf(mny, -1e6f); mxx = fminf(mxx, 1e6f); mxy = fminf(mxy, 1e6f);
    return ir_clip(ir_make((int)floorf(mnx) - 1, (int)floorf(mny) - 1, (int)ceilf(mxx) + 2, (int)ceilf(mxy) + 2), W, H);
}


static float sd_poly(float px, float py, const float *v, int n) {
    float d = (px - v[0]) * (px - v[0]) + (py - v[1]) * (py - v[1]);
    float s = 1;
    for (int i = 0, j = n - 1; i < n; j = i, i++) {
        float ex = v[2 * j] - v[2 * i], ey = v[2 * j + 1] - v[2 * i + 1];
        float wx = px - v[2 * i], wy = py - v[2 * i + 1];
        float t = clampf((wx * ex + wy * ey) / (ex * ex + ey * ey + 1e-12f), 0, 1);
        float bx = wx - ex * t, by = wy - ey * t;
        d = fminf(d, bx * bx + by * by);
        bool c1 = py >= v[2 * i + 1], c2 = py < v[2 * j + 1], c3 = ex * wy > ey * wx;
        if ((c1 && c2 && c3) || (!c1 && !c2 && !c3)) s = -s;
    }
    return s * sqrtf(d);
}

static void sample_bmp(const Bmp *bm, float u, float v, float *o) {
    float fx = u - 0.5f, fy = v - 0.5f;
    int ix = (int)floorf(fx), iy = (int)floorf(fy);
    float wx = fx - ix, wy = fy - iy;
    o[0] = o[1] = o[2] = o[3] = 0;
    for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) {
        int x = ix + i, y = iy + j;
        if (x < 0 || y < 0 || x >= bm->w || y >= bm->h) continue;
        float wgt = (i ? wx : 1 - wx) * (j ? wy : 1 - wy);
        const float *s = &bm->px[((size_t)y * bm->w + x) * 4];
        o[0] += s[0] * wgt; o[1] += s[1] * wgt; o[2] += s[2] * wgt; o[3] += s[3] * wgt;
    }
}

typedef struct {
    FBuf *b; Xf ix; int kind; float w, h, rad, sw, ks; float vert[128]; int nv;
    float fc[3], sc[3], fa, sa; IRect r;
} ShapeCtx;
static void shape_rows(int r0, int r1, void *vp) {
    ShapeCtx *c = vp; FBuf *b = c->b; const Xf *ix = &c->ix;
    for (int y = c->r.y0 + r0; y < c->r.y0 + r1; y++) for (int x = c->r.x0; x < c->r.x1; x++) {
        float dx = x + 0.5f - ix->tx, dy = y + 0.5f - ix->ty;
        float lx = ix->m00 * dx + ix->m01 * dy, ly = ix->m10 * dx + ix->m11 * dy, sd;
        if (c->kind == SH_RECT) {
            float qx = fabsf(lx) - (c->w / 2 - c->rad), qy = fabsf(ly) - (c->h / 2 - c->rad);
            sd = hypotf(fmaxf(qx, 0), fmaxf(qy, 0)) + fminf(fmaxf(qx, qy), 0) - c->rad;
        } else if (c->kind == SH_ELLIPSE) {
            float rx = c->w / 2, ry = c->h / 2;
            float k0 = hypotf(lx / rx, ly / ry), k1 = hypotf(lx / (rx * rx), ly / (ry * ry));
            sd = k1 > 1e-9f ? k0 * (k0 - 1) / k1 : -fminf(rx, ry);
        } else sd = sd_poly(lx, ly, c->vert, c->nv);
        float cf = clampf(0.5f - sd * c->ks, 0, 1);
        float o[4] = { c->fc[0] * cf, c->fc[1] * cf, c->fc[2] * cf, c->fa * cf };
        if (c->sw > 0) {
            float cs = clampf(0.5f - (fabsf(sd) - c->sw / 2) * c->ks, 0, 1);
            float a = c->sa * cs, inv = 1 - a;
            o[0] = c->sc[0] * cs + o[0] * inv; o[1] = c->sc[1] * cs + o[1] * inv; o[2] = c->sc[2] * cs + o[2] * inv; o[3] = a + o[3] * inv;
        }
        if (o[3] <= 0) continue;
        memcpy(&b->p[((size_t)y * b->w + x) * 4], o, sizeof o);
    }
}
static IRect draw_shape(Layer *l, FBuf *b, const float *pv) {
    float w = fmaxf(pv[P_W], 0), h = fmaxf(pv[P_H], 0);
    if (w <= 0 || h <= 0) return IR_NONE;
    ShapeCtx *c = calloc(1, sizeof *c);
    int kind = l->type == L_SOLID ? SH_RECT : l->shape;
    float sw = l->type == L_SOLID ? 0 : fmaxf(pv[P_C], 0);
    float sxf = pv[P_SX] / 100, syf = pv[P_SY] / 100;
    Xf xf = make_xf(pv[P_X], pv[P_Y], sxf, syf, pv[P_ROT], pv[P_AX], pv[P_AY]);
    if (!inv_xf(&xf, &c->ix)) { free(c); return IR_NONE; }
    float ks = sqrtf(fabsf(sxf * syf)) * g_rs;
    if (ks < 1e-6f) { free(c); return IR_NONE; }
    int nv = 0;
    if (kind == SH_POLYGON || kind == SH_STAR) {
        int n = (int)clampf(pv[P_A] + 0.5f, 3, 32);
        float inner = clampf(pv[P_B], 0.05f, 0.95f);
        nv = kind == SH_STAR ? n * 2 : n;
        for (int i = 0; i < nv; i++) {
            float ang = -PI_F / 2 + 2 * PI_F * i / nv;
            float r = (kind == SH_STAR && (i & 1)) ? inner : 1;
            c->vert[2 * i] = cosf(ang) * r * w / 2; c->vert[2 * i + 1] = sinf(ang) * r * h / 2;
        }
    }
    c->nv = nv; c->kind = kind; c->w = w; c->h = h; c->sw = sw; c->ks = ks; c->b = b;
    c->rad = l->type == L_SHAPE && kind == SH_RECT ? clampf(pv[P_A], 0, fminf(w, h) / 2) : 0;
    IRect r = xf_bbox(&xf, -w / 2 - sw, -h / 2 - sw, w / 2 + sw, h / 2 + sw, b->w, b->h);
    c->r = r;
    c->fa = l->fill[3] / 255.0f; c->sa = l->stroke[3] / 255.0f;
    for (int i = 0; i < 3; i++) { c->fc[i] = l->fill[i] / 255.0f * c->fa; c->sc[i] = l->stroke[i] / 255.0f * c->sa; }
    if (!ir_empty(r)) par_rows(r.y1 - r.y0, shape_rows, c);
    free(c);
    return r;
}


typedef struct { const Bmp *bm; const uint8_t *u8; int w, h; float fit; FBuf *b; Xf ix; IRect r; } BmpCtx;
static void sample_u8(const uint8_t *px, int w, int h, float u, float v, float *o) {
    float fx = u - 0.5f, fy = v - 0.5f;
    int ix = (int)floorf(fx), iy = (int)floorf(fy);
    float wx = fx - ix, wy = fy - iy;
    o[0] = o[1] = o[2] = o[3] = 0;
    for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) {
        int x = ix + i, y = iy + j;
        if (x < 0 || y < 0 || x >= w || y >= h) continue;
        float wgt = (i ? wx : 1 - wx) * (j ? wy : 1 - wy) * (1.0f / 255.0f);
        const uint8_t *s = &px[((size_t)y * w + x) * 4];
        float a = s[3] * wgt, k = (1.0f / 255.0f) * a;
        o[0] += s[0] * k; o[1] += s[1] * k; o[2] += s[2] * k; o[3] += a;
    }
}
static void bmp_rows(int r0, int r1, void *vp) {
    BmpCtx *c = vp; const Xf *ix = &c->ix;
    for (int y = c->r.y0 + r0; y < c->r.y0 + r1; y++) for (int x = c->r.x0; x < c->r.x1; x++) {
        float dx = x + 0.5f - ix->tx, dy = y + 0.5f - ix->ty;
        float lx = ix->m00 * dx + ix->m01 * dy, ly = ix->m10 * dx + ix->m11 * dy;
        float o[4];
        if (c->bm) sample_bmp(c->bm, lx / c->fit + c->bm->w / 2.0f, ly / c->fit + c->bm->h / 2.0f, o);
        else sample_u8(c->u8, c->w, c->h, lx / c->fit + c->w / 2.0f, ly / c->fit + c->h / 2.0f, o);
        if (o[3] <= 0) continue;
        memcpy(&c->b->p[((size_t)y * c->b->w + x) * 4], o, sizeof o);
    }
}
/* draws a float bitmap (bm) or an RGBA8 image (u8,w,h) centred on the layer anchor */
static IRect draw_any(const Bmp *bm, const uint8_t *u8, int uw, int uh, float fit, FBuf *b, const float *pv) {
    if ((!bm && !u8) || fit <= 0) return IR_NONE;
    float sxf = pv[P_SX] / 100, syf = pv[P_SY] / 100;
    BmpCtx c = { bm, u8, uw, uh, fit, b, { 0 }, IR_NONE };
    Xf xf = make_xf(pv[P_X], pv[P_Y], sxf, syf, pv[P_ROT], pv[P_AX], pv[P_AY]);
    if (!inv_xf(&xf, &c.ix)) return IR_NONE;
    float iw = bm ? (float)bm->w : (float)uw, ih = bm ? (float)bm->h : (float)uh;
    float hw = iw * fit / 2, hh = ih * fit / 2;
    c.r = xf_bbox(&xf, -hw, -hh, hw, hh, b->w, b->h);
    if (!ir_empty(c.r)) par_rows(c.r.y1 - c.r.y0, bmp_rows, &c);
    return c.r;
}
static IRect draw_bmp(const Bmp *bm, float fit, FBuf *b, const float *pv) { return draw_any(bm, NULL, 0, 0, fit, b, pv); }


/* ----- text ----- */
static unsigned char *g_font_data; static stbtt_fontinfo g_font; static bool g_font_ok, g_font_tried;
static bool font_load(void) {
    if (g_font_tried) return g_font_ok;
    g_font_tried = true;
    char buf[PATH_LEN];
    const char *cands[3] = { afx_find_asset("fonts/DejaVuSans-Bold.ttf", buf, sizeof buf),
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf" };
    for (int i = 0; i < 3; i++) {
        if (!cands[i]) continue;
        FILE *f = fopen(cands[i], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        g_font_data = malloc((size_t)n);
        if (fread(g_font_data, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(g_font_data); g_font_data = NULL; continue; }
        fclose(f);
        if (stbtt_InitFont(&g_font, g_font_data, 0)) { g_font_ok = true; return true; }
    }
    afx_log(1, "no usable font found (put a .ttf at assets/fonts/DejaVuSans-Bold.ttf)");
    return false;
}

static int utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    int c = *p;
    if (c < 0x80) { (*s)++; return c; }
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
    c &= (0x3F >> n);
    (*s)++;
    for (int i = 0; i < n && ((unsigned char)**s & 0xC0) == 0x80; i++, (*s)++) c = (c << 6) | ((unsigned char)**s & 0x3F);
    return c;
}

static float line_width(const char *s, size_t len, float sc) {
    float w = 0; const char *p = s, *e = s + len;
    int prev = 0;
    while (p < e) {
        int cp = utf8_next(&p), adv, lsb;
        stbtt_GetCodepointHMetrics(&g_font, cp, &adv, &lsb);
        w += adv * sc;
        if (prev) w += stbtt_GetCodepointKernAdvance(&g_font, prev, cp) * sc;
        prev = cp;
    }
    return w;
}

static Bmp *make_text_bmp(const char *text, float size, const uint8_t col[4]) {
    if (!font_load() || !*text) return NULL;
    size = clampf(size, 4, 400);
    float sc = stbtt_ScaleForPixelHeight(&g_font, size);
    int asc, desc, gap; stbtt_GetFontVMetrics(&g_font, &asc, &desc, &gap);
    float lh = (asc - desc + gap) * sc;
    int nl = 1; float maxw = 0;
    for (const char *p = text, *st = text;; p++) {
        if (*p == '\n' || !*p) { float w = line_width(st, (size_t)(p - st), sc); if (w > maxw) maxw = w; if (!*p) break; nl++; st = p + 1; }
    }
    int pad = 4, W = (int)ceilf(maxw) + pad * 2, H = (int)ceilf(lh * nl) + pad * 2;
    if (W < 1 || H < 1 || (long)W * H > 16000000) return NULL;
    Bmp *bm = bmp_new(W, H);
    float *alpha = calloc((size_t)W * H, sizeof(float));
    int line = 0;
    for (const char *p = text, *st = text;; p++) {
        if (*p == '\n' || !*p) {
            float lw = line_width(st, (size_t)(p - st), sc);
            float x = pad + (maxw - lw) / 2, base = pad + line * lh + asc * sc;
            const char *q = st; int prev = 0;
            while (q < p) {
                int cp = utf8_next(&q), adv, lsb, gx0, gy0, gx1, gy1;
                if (prev) x += stbtt_GetCodepointKernAdvance(&g_font, prev, cp) * sc;
                stbtt_GetCodepointHMetrics(&g_font, cp, &adv, &lsb);
                stbtt_GetCodepointBitmapBox(&g_font, cp, sc, sc, &gx0, &gy0, &gx1, &gy1);
                int gw = gx1 - gx0, gh = gy1 - gy0;
                if (gw > 0 && gh > 0) {
                    unsigned char *g = calloc((size_t)gw * gh, 1);
                    stbtt_MakeCodepointBitmap(&g_font, g, gw, gh, gw, sc, sc, cp);
                    int ox = (int)floorf(x) + gx0, oy = (int)floorf(base) + gy0;
                    for (int j = 0; j < gh; j++) for (int i = 0; i < gw; i++) {
                        int px = ox + i, py = oy + j;
                        if (px < 0 || py < 0 || px >= W || py >= H) continue;
                        float a = g[j * gw + i] / 255.0f, *d = &alpha[(size_t)py * W + px];
                        *d = fmaxf(*d, a);
                    }
                    free(g);
                }
                x += adv * sc; prev = cp;
            }
            if (!*p) break;
            line++; st = p + 1;
        }
    }
    float ca = col[3] / 255.0f;
    for (size_t i = 0; i < (size_t)W * H; i++) {
        float a = alpha[i] * ca;
        bm->px[i * 4 + 0] = col[0] / 255.0f * a; bm->px[i * 4 + 1] = col[1] / 255.0f * a;
        bm->px[i * 4 + 2] = col[2] / 255.0f * a; bm->px[i * 4 + 3] = a;
    }
    free(alpha);
    return bm;
}

/* ----- image / svg ----- */
static Bmp *load_image_bmp(const char *path) {
    int w, h, n;
    unsigned char *d = stbi_load(path, &w, &h, &n, 4);
    if (!d) { afx_log(1, "cannot load image '%s'", path); return NULL; }
    int k = 1; while (w / k > 2048 || h / k > 2048) k++;
    int W = w / k, H = h / k;
    Bmp *bm = bmp_new(W, H); bm->unit = (float)k;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        float s[4] = { 0, 0, 0, 0 };
        for (int j = 0; j < k; j++) for (int i = 0; i < k; i++) {
            const unsigned char *p = &d[((size_t)(y * k + j) * w + (x * k + i)) * 4];
            float a = p[3] / 255.0f;
            s[0] += p[0] / 255.0f * a; s[1] += p[1] / 255.0f * a; s[2] += p[2] / 255.0f * a; s[3] += a;
        }
        float inv = 1.0f / (k * k);
        float *o = &bm->px[((size_t)y * W + x) * 4];
        o[0] = s[0] * inv; o[1] = s[1] * inv; o[2] = s[2] * inv; o[3] = s[3] * inv;
    }
    stbi_image_free(d);
    return bm;
}

static Bmp *load_svg_bmp(const char *path) {
    NSVGimage *img = nsvgParseFromFile(path, "px", 96.0f);
    if (!img || img->width <= 0 || img->height <= 0) { afx_log(1, "cannot load svg '%s'", path); if (img) nsvgDelete(img); return NULL; }
    float s = clampf(1024.0f / fmaxf(img->width, img->height), 0.25f, 4.0f);
    int W = (int)ceilf(img->width * s), H = (int)ceilf(img->height * s);
    NSVGrasterizer *r = nsvgCreateRasterizer();
    unsigned char *buf = calloc((size_t)W * H, 4);
    nsvgRasterize(r, img, 0, 0, s, buf, W, H, W * 4);
    Bmp *bm = bmp_new(W, H); bm->unit = 1.0f / s;
    for (size_t i = 0; i < (size_t)W * H; i++) {
        float a = buf[i * 4 + 3] / 255.0f;
        bm->px[i * 4 + 0] = buf[i * 4 + 0] / 255.0f * a; bm->px[i * 4 + 1] = buf[i * 4 + 1] / 255.0f * a;
        bm->px[i * 4 + 2] = buf[i * 4 + 2] / 255.0f * a; bm->px[i * 4 + 3] = a;
    }
    free(buf); nsvgDeleteRasterizer(r); nsvgDelete(img);
    return bm;
}

/* ----- asteroid ----- */
static Bmp *gen_asteroid(int seed, float rough, float craters, const uint8_t col[4]) {
    const int N = 256; const float C = N / 2.0f;
    Bmp *bm = bmp_new(N, N);
    uint32_t sd = hash_u((uint32_t)seed * 2654435761u + 17);
    float amp[7], ph[7];
    for (int k = 1; k <= 6; k++) { amp[k] = (0.35f + 0.65f * hash_f(sd + k * 31)) / (0.8f + 0.5f * k); ph[k] = hash_f(sd + k * 77) * 2 * PI_F; }
    enum { NT = 720 };
    float rt[NT], mx = 0;
    for (int i = 0; i < NT; i++) {
        float th = 2 * PI_F * i / NT, r = 1;
        for (int k = 1; k <= 6; k++) r += rough * amp[k] * sinf(k * th + ph[k]);
        r += rough * 0.25f * (vnoise2(cosf(th) * 3 + 10, sinf(th) * 3 + 10, sd) - 0.5f);
        rt[i] = fmaxf(r, 0.35f); mx = fmaxf(mx, rt[i]);
    }
    for (int i = 0; i < NT; i++) rt[i] = rt[i] / mx * (C - 4);
    float Lx = -0.5f, Ly = -0.6f, Lz = 0.62f, ll = sqrtf(Lx * Lx + Ly * Ly + Lz * Lz); Lx /= ll; Ly /= ll; Lz /= ll;
    float lxy = hypotf(Lx, Ly);
    int nc = (int)(craters * 14 + 0.5f);
    float ccx[20], ccy[20], ccr[20];
    for (int i = 0; i < nc && i < 20; i++) {
        float a = hash_f(sd + 500 + i * 3) * 2 * PI_F, d = sqrtf(hash_f(sd + 501 + i * 3)) * 0.62f * C;
        ccx[i] = C + cosf(a) * d; ccy[i] = C + sinf(a) * d; ccr[i] = (0.07f + 0.17f * hash_f(sd + 502 + i * 3)) * C;
    }
    for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
        float dx = x + 0.5f - C, dy = y + 0.5f - C, d = hypotf(dx, dy);
        float th = atan2f(dy, dx); if (th < 0) th += 2 * PI_F;
        float fi = th / (2 * PI_F) * NT; int i0 = (int)fi % NT, i1 = (i0 + 1) % NT; float fr = fi - floorf(fi);
        float rr = lerpf(rt[i0], rt[i1], fr);
        float cov = clampf(rr - d + 0.5f, 0, 1);
        if (cov <= 0) continue;
        float u = fminf(d / rr, 0.999f), nz = sqrtf(1 - u * u), nx = dx / rr, ny = dy / rr;
        float lam = fmaxf(0, nx * Lx + ny * Ly + nz * Lz);
        float shade = 0.16f + 0.95f * lam;
        float tex = 0.62f * vnoise2(x / 22.0f, y / 22.0f, sd) + 0.25f * vnoise2(x / 8.0f, y / 8.0f, sd + 9) + 0.13f * vnoise2(x / 3.0f, y / 3.0f, sd + 21);
        tex = 0.7f + 0.55f * tex;
        float add = 0;
        for (int c = 0; c < nc && c < 20; c++) {
            float cx = x + 0.5f - ccx[c], cy = y + 0.5f - ccy[c], dc = hypotf(cx, cy) / ccr[c];
            if (dc > 1.35f) continue;
            float ux = cx / (dc * ccr[c] + 1e-6f), uy = cy / (dc * ccr[c] + 1e-6f);
            float lit = (ux * -Lx + uy * -Ly) / (lxy + 1e-6f);      /* >0 : wall facing the light */
            if (dc < 1) { float bowl = 1 - dc * dc; shade *= 1 - 0.42f * bowl; shade += 0.10f * bowl * fmaxf(0, lit); }
            float rim = expf(-((dc - 1) * (dc - 1)) / 0.03f);
            add += rim * (0.45f * fmaxf(0, lit) - 0.12f * fmaxf(0, -lit));
        }
        float k = (shade + add) * tex;
        float rim = 0.05f * powf(u, 3.0f);
        float *o = &bm->px[((size_t)y * N + x) * 4];
        o[0] = clampf(col[0] / 255.0f * k + rim * 0.5f, 0, 1) * cov;
        o[1] = clampf(col[1] / 255.0f * k + rim * 0.6f, 0, 1) * cov;
        o[2] = clampf(col[2] / 255.0f * k + rim * 0.9f, 0, 1) * cov;
        o[3] = cov;
    }
    return bm;
}

/* ----- cache ----- */
static Bmp *layer_bitmap(Layer *l, const float *pv, float *fit) {
    char key[sizeof l->cache_key];
    switch (l->type) {
    case L_TEXT:
        snprintf(key, sizeof key, "T|%d|%u%u%u%u|%s", (int)(pv[P_A] + 0.5f), l->fill[0], l->fill[1], l->fill[2], l->fill[3], l->text); break;
    case L_SVG: snprintf(key, sizeof key, "S|%s", l->path); break;
    case L_IMAGE: snprintf(key, sizeof key, "I|%s", l->path); break;
    case L_ASTEROID:
        snprintf(key, sizeof key, "A|%d|%d|%d|%u,%u,%u", (int)pv[P_A], (int)(pv[P_B] * 100), (int)(pv[P_C] * 100), l->fill[0], l->fill[1], l->fill[2]); break;
    default: return NULL;
    }
    if (strcmp(key, l->cache_key)) {
        layer_free_cache(l);
        snprintf(l->cache_key, sizeof l->cache_key, "%s", key);
        switch (l->type) {
        case L_TEXT: l->cache = make_text_bmp(l->text, pv[P_A], l->fill); break;
        case L_SVG: if (l->path[0]) l->cache = load_svg_bmp(l->path); break;
        case L_IMAGE: if (l->path[0]) l->cache = load_image_bmp(l->path); break;
        case L_ASTEROID: l->cache = gen_asteroid((int)pv[P_A], pv[P_B], pv[P_C], l->fill); break;
        default: break;
        }
    }
    Bmp *bm = l->cache;
    if (!bm) return NULL;
    if (l->type == L_ASTEROID) *fit = 2 * pv[P_W] / bm->w;
    else if ((l->type == L_SVG || l->type == L_IMAGE) && pv[P_W] > 0) *fit = pv[P_W] / bm->w;
    else *fit = bm->unit;
    return bm;
}

/* ----- stars ----- */
static IRect draw_stars(Layer *l, FBuf *b, const float *pv, float t) {
    int n = (int)clampf(pv[P_A], 0, 5000);
    uint32_t seed = hash_u((uint32_t)(int)pv[P_W] * 9176u + 5);
    float W = (float)b->w, H = (float)b->h, speed = pv[P_B] * g_rs, size = fmaxf(pv[P_C], 0.2f) * g_rs;
    float ang = pv[P_ROT] * PI_F / 180, dx = cosf(ang), dy = sinf(ang);
    for (int i = 0; i < n; i++) {
        uint32_t h0 = seed + (uint32_t)i * 4u;
        float r1 = hash_f(h0), r2 = hash_f(h0 + 1), r3 = hash_f(h0 + 2), r4 = hash_f(h0 + 3);
        float depth = 0.15f + 0.85f * r3 * r3;
        float x = fmodf(r1 * W + dx * speed * depth * t, W), y = fmodf(r2 * H + dy * speed * depth * t, H);
        if (x < 0) x += W;
        if (y < 0) y += H;
        float rad = size * (0.45f + depth * 0.9f);
        float tw = 0.75f + 0.25f * sinf(t * (1.5f + r4 * 4) + r4 * 40);
        float br = clampf(0.35f + depth * 0.8f, 0, 1) * tw;
        float cr = l->fill[0] / 255.0f * (0.85f + 0.15f * r4), cg = l->fill[1] / 255.0f * (0.9f + 0.1f * r1), cb = l->fill[2] / 255.0f * (1.0f - 0.2f * r2);
        float ca = l->fill[3] / 255.0f;
        int x0 = (int)fmaxf(0, x - rad - 2), x1 = (int)fminf(b->w - 1, x + rad + 2), y0 = (int)fmaxf(0, y - rad - 2), y1 = (int)fminf(b->h - 1, y + rad + 2);
        for (int py = y0; py <= y1; py++) for (int px = x0; px <= x1; px++) {
            float d = hypotf(px + 0.5f - x, py + 0.5f - y);
            float cov = clampf(rad + 0.5f - d, 0, 1);
            if (rad > 1.5f) cov = fmaxf(cov, 0.35f * expf(-d * d / (rad * rad * 2)));
            float a = cov * br * ca;
            if (a <= 0) continue;
            float *dst = &b->p[((size_t)py * b->w + px) * 4];
            float inv = 1 - a;
            dst[0] = cr * a + dst[0] * inv; dst[1] = cg * a + dst[1] * inv; dst[2] = cb * a + dst[2] * inv; dst[3] = a + dst[3] * inv;
        }
    }
    return ir_full(b->w, b->h);
}

/* ---------- effects ---------- */
typedef struct { const float *s; float *d; int w; IRect r; int k; float inv; } BoxCtx;
static void boxh_rows(int r0, int r1, void *vp) {
    BoxCtx *c = vp;
    for (int y = c->r.y0 + r0; y < c->r.y0 + r1; y++) {
        const float *s = c->s + (size_t)y * c->w * 4; float *d = c->d + (size_t)y * c->w * 4;
        float acc[4] = { 0, 0, 0, 0 };
        for (int i = c->r.x0; i <= c->r.x0 + c->k && i < c->r.x1; i++) for (int q = 0; q < 4; q++) acc[q] += s[i * 4 + q];
        for (int x = c->r.x0; x < c->r.x1; x++) {
            for (int q = 0; q < 4; q++) d[x * 4 + q] = acc[q] * c->inv;
            int add = x + c->k + 1, sub = x - c->k;
            if (add < c->r.x1) for (int q = 0; q < 4; q++) acc[q] += s[add * 4 + q];
            if (sub >= c->r.x0) for (int q = 0; q < 4; q++) acc[q] -= s[sub * 4 + q];
        }
    }
}
#define STRIP 32
static void boxv_strips(int r0, int r1, void *vp) {
    BoxCtx *c = vp;
    size_t st = (size_t)c->w * 4;
    for (int sidx = r0; sidx < r1; sidx++) {
        int xa = c->r.x0 + sidx * STRIP, xb = xa + STRIP < c->r.x1 ? xa + STRIP : c->r.x1, n = xb - xa;
        float acc[STRIP * 4];
        memset(acc, 0, sizeof acc);
        for (int i = c->r.y0; i <= c->r.y0 + c->k && i < c->r.y1; i++) {
            const float *s = c->s + (size_t)i * st + (size_t)xa * 4;
            for (int q = 0; q < n * 4; q++) acc[q] += s[q];
        }
        for (int y = c->r.y0; y < c->r.y1; y++) {
            float *d = c->d + (size_t)y * st + (size_t)xa * 4;
            for (int q = 0; q < n * 4; q++) d[q] = acc[q] * c->inv;
            int add = y + c->k + 1, sub = y - c->k;
            if (add < c->r.y1) { const float *s = c->s + (size_t)add * st + (size_t)xa * 4; for (int q = 0; q < n * 4; q++) acc[q] += s[q]; }
            if (sub >= c->r.y0) { const float *s = c->s + (size_t)sub * st + (size_t)xa * 4; for (int q = 0; q < n * 4; q++) acc[q] -= s[q]; }
        }
    }
}
/* blur of b inside rect *bb (grown by the blur reach); t = scratch */
static void blur_rect(FBuf *b, FBuf *t, IRect *bb, float radius) {
    int k = (int)floorf(radius * 0.55f + 0.5f);
    if (k < 1 || ir_empty(*bb)) return;
    IRect r = ir_grow(*bb, 3 * k + 1, b->w, b->h);
    float inv = 1.0f / (2 * k + 1);
    for (int pass = 0; pass < 3; pass++) {
        BoxCtx h = { b->p, t->p, b->w, r, k, inv };
        par_rows(r.y1 - r.y0, boxh_rows, &h);
        BoxCtx v = { t->p, b->p, b->w, r, k, inv };
        par_rows((r.x1 - r.x0 + STRIP - 1) / STRIP, boxv_strips, &v);
    }
    *bb = r;
}
/* bilinear tap restricted to rect rc (zero outside) */
static void samp_r(const FBuf *b, IRect rc, float x, float y, float *o) {
    float fx = x - 0.5f, fy = y - 0.5f;
    int ix = (int)floorf(fx), iy = (int)floorf(fy);
    float wx = fx - ix, wy = fy - iy;
    o[0] = o[1] = o[2] = o[3] = 0;
    for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) {
        int xx = ix + i, yy = iy + j;
        if (xx < rc.x0 || yy < rc.y0 || xx >= rc.x1 || yy >= rc.y1) continue;
        float wgt = (i ? wx : 1 - wx) * (j ? wy : 1 - wy);
        const float *s = &b->p[((size_t)yy * b->w + xx) * 4];
        o[0] += s[0] * wgt; o[1] += s[1] * wgt; o[2] += s[2] * wgt; o[3] += s[3] * wgt;
    }
}

typedef struct { int type; float v[3], tc[3]; FBuf *b, *src, *g; IRect r, sr; uint32_t frame; float ox, oy, cx, cy, e0, ph, sz; int s; } FxCtx;
#define PXP(c, x, y) (&(c)->b->p[((size_t)(y) * (c)->b->w + (x)) * 4])
static void fx_rows(int r0, int r1, void *vp) {
    FxCtx *c = vp;
    const float *v = c->v;
    for (int y = c->r.y0 + r0; y < c->r.y0 + r1; y++) {
        switch (c->type) {
        case FX_BRIGHT:
            for (int x = c->r.x0; x < c->r.x1; x++) {
                float *s = PXP(c, x, y), a = s[3];
                if (a <= 1e-5f) continue;
                float cc[3] = { s[0] / a, s[1] / a, s[2] / a };
                float lum = 0.299f * cc[0] + 0.587f * cc[1] + 0.114f * cc[2];
                for (int k = 0; k < 3; k++) {
                    float xx = lum + (cc[k] - lum) * v[2];
                    xx = (xx - 0.5f) * v[1] + 0.5f + v[0];
                    s[k] = clampf(xx, 0, 1) * a;
                }
            }
            break;
        case FX_TINT:
            for (int x = c->r.x0; x < c->r.x1; x++) {
                float *s = PXP(c, x, y), a = s[3];
                if (a <= 1e-5f) continue;
                float lum = (0.299f * s[0] + 0.587f * s[1] + 0.114f * s[2]) / a;
                for (int k = 0; k < 3; k++) {
                    float tgt = clampf(lum * c->tc[k] * 1.8f, 0, 1);
                    s[k] = lerpf(s[k] / a, tgt, v[0]) * a;
                }
            }
            break;
        case FX_HUE: {
            float r = v[0] * PI_F / 180, cs = cosf(r), sn = sinf(r);
            float m[9] = { .213f + cs * .787f - sn * .213f, .715f - cs * .715f - sn * .715f, .072f - cs * .072f + sn * .928f,
                           .213f - cs * .213f + sn * .143f, .715f + cs * .285f + sn * .140f, .072f - cs * .072f - sn * .283f,
                           .213f - cs * .213f - sn * .787f, .715f - cs * .715f + sn * .715f, .072f + cs * .928f + sn * .072f };
            for (int x = c->r.x0; x < c->r.x1; x++) {
                float *s = PXP(c, x, y), r0_ = s[0], g0 = s[1], b0 = s[2], a = s[3];
                s[0] = clampf(m[0] * r0_ + m[1] * g0 + m[2] * b0, 0, a);
                s[1] = clampf(m[3] * r0_ + m[4] * g0 + m[5] * b0, 0, a);
                s[2] = clampf(m[6] * r0_ + m[7] * g0 + m[8] * b0, 0, a);
            }
            break; }
        case FX_INVERT:
            for (int x = c->r.x0; x < c->r.x1; x++) { float *s = PXP(c, x, y); for (int k = 0; k < 3; k++) s[k] = lerpf(s[k], s[3] - s[k], v[0]); }
            break;
        case FX_NOISE:
            for (int x = c->r.x0; x < c->r.x1; x++) {
                float *s = PXP(c, x, y);
                if (s[3] <= 1e-5f) continue;
                float nz = hash_f(hash_u((uint32_t)(x / c->sz) * 73856093u ^ (uint32_t)(y / c->sz) * 19349663u ^ c->frame)) * 2 - 1;
                for (int k = 0; k < 3; k++) s[k] = clampf(s[k] + nz * v[0] * s[3], 0, s[3]);
            }
            break;
        case FX_RGBSPLIT:
            for (int x = c->r.x0; x < c->r.x1; x++) {
                float r[4], g[4], bl[4];
                samp_r(c->src, c->sr, x + 0.5f - c->ox, y + 0.5f - c->oy, r);
                samp_r(c->src, c->sr, x + 0.5f, y + 0.5f, g);
                samp_r(c->src, c->sr, x + 0.5f + c->ox, y + 0.5f + c->oy, bl);
                float *s = PXP(c, x, y), a = fmaxf(r[3], fmaxf(g[3], bl[3]));
                s[0] = fminf(r[0], a); s[1] = fminf(g[1], a); s[2] = fminf(bl[2], a); s[3] = a;
            }
            break;
        case FX_VIGNETTE:
            for (int x = c->r.x0; x < c->r.x1; x++) {
                float dx = (x + 0.5f - c->cx) / c->cx, dy = (y + 0.5f - c->cy) / c->cy;
                float dd = sqrtf(dx * dx + dy * dy) * 0.7071f;
                float f = 1 - v[0] * smoothf(c->e0, 1.05f, dd);
                float *s = PXP(c, x, y);
                s[0] *= f; s[1] *= f; s[2] *= f;
            }
            break;
        case FX_SCANLINES: {
            float f = 1 - v[0] * 0.5f * (1 - cosf(2 * PI_F * (y + v[2]) / fmaxf(v[1], 2)));
            for (int x = c->r.x0; x < c->r.x1; x++) { float *s = PXP(c, x, y); s[0] *= f; s[1] *= f; s[2] *= f; }
            break; }
        case FX_WAVE: {
            float dx = v[0] * sinf(y * v[1] * 2 * PI_F + c->ph);
            for (int x = c->r.x0; x < c->r.x1; x++) samp_r(c->src, c->sr, x + 0.5f - dx, y + 0.5f, PXP(c, x, y));
            break; }
        case FX_POSTERIZE: {
            float lv = fmaxf(v[0], 2);
            for (int x = c->r.x0; x < c->r.x1; x++) {
                float *s = PXP(c, x, y), a = s[3];
                if (a <= 1e-5f) continue;
                for (int k = 0; k < 3; k++) s[k] = clampf(floorf(s[k] / a * (lv - 0.001f)) / (lv - 1), 0, 1) * a;
            }
            break; }
        case 100: /* glow: threshold mask b -> g */
            for (int x = c->r.x0; x < c->r.x1; x++) {
                const float *s = PXP(c, x, y); float *d = &c->g->p[((size_t)y * c->g->w + x) * 4], a = s[3], m = 0;
                if (a > 1e-5f) { float mxc = fmaxf(s[0], fmaxf(s[1], s[2])) / a; m = smoothf(v[2] - 0.1f, v[2] + 0.1f, mxc); }
                d[0] = s[0] * m; d[1] = s[1] * m; d[2] = s[2] * m; d[3] = s[3] * m;
            }
            break;
        case 101: /* glow: add blurred g onto b */
            for (int x = c->r.x0; x < c->r.x1; x++) {
                float *s = PXP(c, x, y); const float *q = &c->g->p[((size_t)y * c->g->w + x) * 4];
                s[0] += q[0] * v[1]; s[1] += q[1] * v[1]; s[2] += q[2] * v[1];
                s[3] = fminf(1, s[3] + q[3] * v[1] * (1 - s[3]));
            }
            break;
        }
    }
}
static void pix_rows(int r0, int r1, void *vp) {      /* pixelate: row index = block row */
    FxCtx *c = vp; FBuf *b = c->b; int s = c->s;
    for (int bi = r0; bi < r1; bi++) {
        int by = c->r.y0 + bi * s, y1 = by + s < b->h ? by + s : b->h;
        for (int bx = c->r.x0; bx < c->r.x1; bx += s) {
            int x1 = bx + s < b->w ? bx + s : b->w;
            float acc[4] = { 0, 0, 0, 0 };
            for (int y = by; y < y1; y++) for (int x = bx; x < x1; x++) for (int q = 0; q < 4; q++) acc[q] += b->p[((size_t)y * b->w + x) * 4 + q];
            float inv = 1.0f / ((x1 - bx) * (y1 - by));
            for (int y = by; y < y1; y++) for (int x = bx; x < x1; x++) for (int q = 0; q < 4; q++) b->p[((size_t)y * b->w + x) * 4 + q] = acc[q] * inv;
        }
    }
}

static int fx_reach_px(int type, const float *v) {
    switch (type) {
    case FX_RGBSPLIT: return (int)ceilf(fabsf(v[0])) + 2;
    case FX_WAVE: return (int)ceilf(fabsf(v[0])) + 2;
    default: return 0;
    }
}
/* applies one effect to b inside *bb (which is grown when the effect spreads pixels). pid0 = property id base for expressions */
static void fx_apply_one(Project *P, Layer *l, Effect *e, int pid0, FBuf *b, float t, IRect *bb) {
    if (ir_empty(*bb)) return;
    const FxDef *d = &FX_DEFS[e->type];
    FxCtx c; memset(&c, 0, sizeof c);
    for (int k = 0; k < d->np; k++) c.v[k] = prop_eval(P, l, &e->p[k], pid0 + k, t);
    float *v = c.v;
    c.type = e->type; c.b = b; c.r = *bb; c.frame = (uint32_t)P->cur_frame * 7919u;
    for (int k = 0; k < 3; k++) c.tc[k] = e->color[k] / 255.0f;
    FBuf *t1 = &P->t1, *t2 = &P->t2;
    switch (e->type) {
    case FX_BLUR: blur_rect(b, t1, bb, v[0] * g_rs); return;
    case FX_GLOW: {
        c.type = 100; c.g = t2;
        IRect r0 = *bb, gb = *bb;
        par_rows(r0.y1 - r0.y0, fx_rows, &c);               /* mask into t2 (t2 is zero outside r0 only if cleared) */
        blur_rect(t2, t1, &gb, v[0] * g_rs);
        c.type = 101; c.r = gb;
        par_rows(gb.y1 - gb.y0, fx_rows, &c);
        { RectCtx z = { t2, NULL, gb, 0 }; par_rows(gb.y1 - gb.y0, clear_rows, &z); }   /* leave t2 clean */
        *bb = gb;
        return; }
    case FX_PIXELATE: {
        int s = (int)fmaxf(v[0] * g_rs, 1);
        if (s < 2) return;
        IRect r = *bb;
        r.x0 = r.x0 / s * s; r.y0 = r.y0 / s * s;
        r.x1 = (r.x1 + s - 1) / s * s; r.y1 = (r.y1 + s - 1) / s * s;
        r = ir_clip(r, b->w, b->h);
        c.r = r; c.s = s;
        par_rows((r.y1 - r.y0 + s - 1) / s, pix_rows, &c);
        *bb = r;
        return; }
    case FX_RGBSPLIT: {
        float ang = v[1] * PI_F / 180;
        c.ox = cosf(ang) * v[0] * g_rs; c.oy = sinf(ang) * v[0] * g_rs;
        c.sr = *bb; c.src = t1;
        copy_rect(t1, b, c.sr);
        int rc = (int)ceilf(fmaxf(fabsf(c.ox), fabsf(c.oy))) + 2;
        c.r = ir_grow(*bb, rc, b->w, b->h);
        par_rows(c.r.y1 - c.r.y0, fx_rows, &c);
        *bb = c.r;
        return; }
    case FX_WAVE: {
        v[0] *= g_rs; v[1] /= g_rs;
        c.ph = v[2] * PI_F / 180; c.sr = *bb; c.src = t1;
        copy_rect(t1, b, c.sr);
        int rc = (int)ceilf(fabsf(v[0])) + 2;
        c.r = ir_clip(ir_make(bb->x0 - rc, bb->y0, bb->x1 + rc, bb->y1), b->w, b->h);
        par_rows(c.r.y1 - c.r.y0, fx_rows, &c);
        *bb = c.r;
        return; }
    case FX_NOISE: c.sz = fmaxf(v[1] * g_rs, 1); break;
    case FX_VIGNETTE: c.cx = b->w / 2.0f; c.cy = b->h / 2.0f; c.e0 = 0.9f - v[1] * 0.8f; break;
    case FX_SCANLINES: v[1] *= g_rs; v[2] *= g_rs; break;
    default: break;
    }
    par_rows(c.r.y1 - c.r.y0, fx_rows, &c);
}
static void apply_fx(Project *P, Layer *l, FBuf *b, float t, IRect *bb) {
    for (int i = 0; i < l->nfx; i++) if (l->fx[i].on) fx_apply_one(P, l, &l->fx[i], 20 + i * 3, b, t, bb);
}

/* ---------- compositing ---------- */
typedef struct { FBuf *dst; const FBuf *src; float opa; int mode; IRect r, sr; float dx, dy; bool shift; } BlendCtx;
static inline void blend_px(float *d, const float *s, float opa, int mode) {
    if (s[3] <= 0 && mode != BM_MULTIPLY) return;
    float sr = s[0] * opa, sg = s[1] * opa, sb = s[2] * opa, sa = s[3] * opa;
    if (sa <= 0 && sr <= 0) return;
    switch (mode) {
    case BM_ADD:
        d[0] += sr; d[1] += sg; d[2] += sb; d[3] = d[3] + sa * (1 - d[3]); break;
    case BM_MULTIPLY: {
        float da = d[3], inv = 1 - sa;
        d[0] = sr * d[0] + sr * (1 - da) + d[0] * inv;
        d[1] = sg * d[1] + sg * (1 - da) + d[1] * inv;
        d[2] = sb * d[2] + sb * (1 - da) + d[2] * inv;
        d[3] = sa + da - sa * da; break;
    }
    case BM_SCREEN:
        d[0] = sr + d[0] - sr * d[0]; d[1] = sg + d[1] - sg * d[1]; d[2] = sb + d[2] - sb * d[2];
        d[3] = sa + d[3] - sa * d[3]; break;
    default: {
        float inv = 1 - sa;
        d[0] = sr + d[0] * inv; d[1] = sg + d[1] * inv; d[2] = sb + d[2] * inv; d[3] = sa + d[3] * inv;
    }
    }
}
static void blend_rows(int r0, int r1, void *vp) {
    BlendCtx *c = vp;
    for (int y = c->r.y0 + r0; y < c->r.y0 + r1; y++) for (int x = c->r.x0; x < c->r.x1; x++) {
        float *d = &c->dst->p[((size_t)y * c->dst->w + x) * 4];
        if (!c->shift) blend_px(d, &c->src->p[((size_t)y * c->src->w + x) * 4], c->opa, c->mode);
        else { float s[4]; samp_r(c->src, c->sr, x + 0.5f - c->dx, y + 0.5f - c->dy, s); blend_px(d, s, c->opa, c->mode); }
    }
}
/* blends src (non-zero only inside sr) onto dst, optionally shifted by (dx,dy) frame pixels; returns the touched rect */
static IRect blend_onto(FBuf *dst, const FBuf *src, float opa, int mode, IRect sr, float dx, float dy) {
    BlendCtx c = { dst, src, opa, mode, sr, sr, dx, dy, fabsf(dx) > 1e-3f || fabsf(dy) > 1e-3f };
    if (c.shift) {
        c.r = ir_clip(ir_make((int)floorf(sr.x0 + dx) - 1, (int)floorf(sr.y0 + dy) - 1, (int)ceilf(sr.x1 + dx) + 2, (int)ceilf(sr.y1 + dy) + 2), dst->w, dst->h);
    } else c.r = ir_clip(sr, dst->w, dst->h);
    if (ir_empty(c.r)) return IR_NONE;
    par_rows(c.r.y1 - c.r.y0, blend_rows, &c);
    return c.r;
}


/* ---------- buffer pool (all buffers are kept zero outside their dirty rect, so they can be reused without a full clear) ---------- */
#define POOL_N 10
static struct { FBuf b; bool busy; } g_pool[POOL_N];
static FBuf *pool_get(int W, int H) {
    for (int i = 0; i < POOL_N; i++) {
        if (g_pool[i].busy) continue;
        if (g_pool[i].b.p && (g_pool[i].b.w != W || g_pool[i].b.h != H)) fbuf_free(&g_pool[i].b);
        if (!g_pool[i].b.p) {
            g_pool[i].b = fbuf_make(W, H);
            if (!g_pool[i].b.p) return NULL;
        }
        g_pool[i].busy = true;
        return &g_pool[i].b;
    }
    return NULL;
}
static void pool_put(FBuf *b, IRect dirty) {
    if (!b) return;
    for (int i = 0; i < POOL_N; i++) if (&g_pool[i].b == b) {
        clear_rect(b, dirty);
        g_pool[i].busy = false;
        return;
    }
}
void render_free_pool(void) {
    for (int i = 0; i < POOL_N; i++) { fbuf_free(&g_pool[i].b); g_pool[i].busy = false; }
}

/* ---------- video layer ---------- */
static IRect draw_video(Project *P, Layer *l, FBuf *b, const float *pv, float t) {
    if (!l->path[0]) return IR_NONE;
    Media *m = media_get(l->path, true);
    if (!m) return IR_NONE;
    media_start(m);
    if (media_state(m) == 3) return IR_NONE;
    int fw = 0, fh = 0, mw, mh; float fps, dur;
    media_info(m, &mw, &mh, &fps, &dur);
    if (fps <= 0) fps = 30;
    float src = l->trim + (t - l->tin) * (l->speed > 0.01f ? l->speed : 1.0f);
    if (src < 0) src = 0;
    int idx = (int)floorf(src * fps + 0.5f);
    const uint8_t *px = media_frame(m, idx, &fw, &fh);
    if (!px || fw <= 0 || fh <= 0) return IR_NONE;
    float cw = (float)P->width, ch = (float)P->height, fit;
    if (pv[P_W] > 0) fit = pv[P_W] / fw;
    else fit = l->fitm ? fminf(cw / fw, ch / fh) : fmaxf(cw / fw, ch / fh);
    return draw_any(NULL, px, fw, fh, fit, b, pv);
}

/* ---------- layer / generator source ---------- */
static IRect draw_source(Project *P, Layer *l, FBuf *b, const float *pv, float t) {
    switch (l->type) {
    case L_SOLID: case L_SHAPE: return draw_shape(l, b, pv);
    case L_TEXT: case L_SVG: case L_IMAGE: case L_ASTEROID: {
        float fit = 1; Bmp *bm = layer_bitmap(l, pv, &fit);
        return draw_bmp(bm, fit, b, pv);
    }
    case L_STARS: return draw_stars(l, b, pv, t);
    case L_VIDEO: return draw_video(P, l, b, pv, t);
    default: return IR_NONE;
    }
}

/* ---------- transform node ---------- */
typedef struct { const FBuf *src; FBuf *dst; Xf ix; IRect sr, r; } XfCtx;
static void xf_rows(int r0, int r1, void *vp) {
    XfCtx *c = vp;
    for (int y = c->r.y0 + r0; y < c->r.y0 + r1; y++) for (int x = c->r.x0; x < c->r.x1; x++) {
        float dx = x + 0.5f - c->ix.tx, dy = y + 0.5f - c->ix.ty;
        float o[4];
        samp_r(c->src, c->sr, c->ix.m00 * dx + c->ix.m01 * dy, c->ix.m10 * dx + c->ix.m11 * dy, o);
        if (o[3] <= 0 && o[0] <= 0) continue;
        memcpy(&c->dst->p[((size_t)y * c->dst->w + x) * 4], o, sizeof o);
    }
}
static IRect transform_into(const FBuf *src, IRect sr, FBuf *dst, float ox, float oy, float sx, float sy, float rot) {
    float W = (float)src->w, H = (float)src->h, cx = W / 2, cy = H / 2;
    float r = rot * PI_F / 180, c = cosf(r), s = sinf(r);
    Xf f;
    f.m00 = c * sx; f.m01 = -s * sy; f.m10 = s * sx; f.m11 = c * sy;
    f.tx = cx + ox * g_rsx - (f.m00 * cx + f.m01 * cy);
    f.ty = cy + oy * g_rsy - (f.m10 * cx + f.m11 * cy);
    XfCtx x = { src, dst, { 0 }, sr, IR_NONE };
    if (!inv_xf(&f, &x.ix)) return IR_NONE;
    x.r = xf_bbox(&f, (float)sr.x0, (float)sr.y0, (float)sr.x1, (float)sr.y1, dst->w, dst->h);
    if (!ir_empty(x.r)) par_rows(x.r.y1 - x.r.y0, xf_rows, &x);
    return x.r;
}

/* ---------- Fusion graph evaluation ---------- */
typedef struct { Project *P; Layer *l; Graph *g; float t; const float *pv; const FBuf *backdrop; int W, H; int fail; } GEval;

static void graph_count_uses(Graph *g, Node *n, int *seen) {
    int idx = (int)(n - g->nd);
    if (seen[idx]) return;
    seen[idx] = 1;
    for (int k = 0; k < node_nin(n); k++) {
        Node *in = n->in[k] ? graph_find(g, n->in[k]) : NULL;
        if (!in) continue;
        in->uses++;
        graph_count_uses(g, in, seen);
    }
}
/* the result of node `in` (consumed by one consumer); returns an owned buffer: stolen when this is the last consumer, otherwise a copy */
static FBuf *take_own(GEval *e, Node *in, IRect *bb) {
    if (!in || !in->res) { *bb = IR_NONE; return pool_get(e->W, e->H); }
    in->uses--;
    if (in->uses <= 0) { FBuf *b = in->res; in->res = NULL; *bb = in->rbb; return b; }
    FBuf *nb = pool_get(e->W, e->H);
    if (!nb) { e->fail = 1; *bb = IR_NONE; return NULL; }
    copy_rect(nb, in->res, in->rbb);
    *bb = in->rbb;
    return nb;
}
/* read-only access; release_after is set when the caller must hand the buffer back to the pool */
static const FBuf *take_ref(Node *in, IRect *bb, FBuf **release_after) {
    *release_after = NULL; *bb = IR_NONE;
    if (!in || !in->res) return NULL;
    in->uses--;
    *bb = in->rbb;
    const FBuf *b = in->res;
    if (in->uses <= 0) { *release_after = in->res; in->res = NULL; }
    return b;
}
static void node_eval(GEval *e, Node *n);
static Node *node_input(GEval *e, Node *n, int k) {
    Node *in = n->in[k] ? graph_find(e->g, n->in[k]) : NULL;
    if (in) node_eval(e, in);
    return in;
}
static void node_eval(GEval *e, Node *n) {
    Graph *g = e->g;
    if (n->stamp == g->stamp) return;
    n->stamp = g->stamp;
    n->res = NULL; n->rbb = IR_NONE;
    Project *P = e->P; Layer *l = e->l; float t = e->t;
    int nidx = (int)(n - g->nd), pid0 = 300 + nidx * 8;
    Node *in0 = node_nin(n) > 0 ? node_input(e, n, 0) : NULL;
    Node *in1 = node_nin(n) > 1 ? node_input(e, n, 1) : NULL;
    switch (n->kind) {
    case N_IN: {
        FBuf *b = pool_get(e->W, e->H); if (!b) { e->fail = 1; return; }
        IRect bb;
        if (l->type == L_ADJUST) { bb = ir_full(e->W, e->H); copy_rect(b, e->backdrop, bb); }
        else bb = draw_source(P, l, b, e->pv, t);
        n->res = b; n->rbb = bb;
        break; }
    case N_GEN: {
        FBuf *b = pool_get(e->W, e->H); if (!b || !n->gen) { if (b) pool_put(b, IR_NONE); e->fail = 1; return; }
        Layer *gl = n->gen; gl->tin = l->tin;
        float gv[P_COUNT];
        for (int i = 0; i < P_COUNT; i++) gv[i] = prop_eval(P, gl, &gl->p[i], pid0 + i % 8 + (i / 8) * 64, t);
        IRect bb = n->on ? draw_source(P, gl, b, gv, t) : IR_NONE;
        if (!ir_empty(bb)) {
            apply_fx(P, gl, b, t, &bb);
            float op = clampf(gv[P_OPA] / 100, 0, 1);
            if (op < 0.999f) { RectCtx c = { b, NULL, bb, op }; par_rows(bb.y1 - bb.y0, mul_rows, &c); }
        }
        n->res = b; n->rbb = bb;
        break; }
    case N_FX: {
        IRect bb; FBuf *b = take_own(e, in0, &bb); if (!b) return;
        if (n->on && n->fx.on) fx_apply_one(P, l, &n->fx, pid0, b, t, &bb);
        n->res = b; n->rbb = bb;
        break; }
    case N_XFORM: {
        IRect sb; FBuf *rel;
        if (!n->on) { IRect bb; FBuf *b = take_own(e, in0, &bb); n->res = b; n->rbb = bb; break; }
        const FBuf *src = take_ref(in0, &sb, &rel);
        FBuf *dst = pool_get(e->W, e->H); if (!dst) { e->fail = 1; pool_put(rel, sb); return; }
        IRect bb = IR_NONE;
        if (src && !ir_empty(sb)) {
            float v[NODE_NP];
            for (int k = 0; k < NODE_NP; k++) v[k] = prop_eval(P, l, &n->mp[k], pid0 + k, t);
            bb = transform_into(src, sb, dst, v[0], v[1], v[2] / 100, v[3] / 100, v[4]);
        }
        pool_put(rel, sb);
        n->res = dst; n->rbb = bb;
        break; }
    case N_MERGE: {
        IRect bb; FBuf *dst = take_own(e, in0, &bb); if (!dst) return;
        IRect fb; FBuf *rel; const FBuf *fg = take_ref(in1, &fb, &rel);
        if (n->on && fg && !ir_empty(fb)) {
            float opa = clampf(prop_eval(P, l, &n->mp[0], pid0, t) / 100, 0, 1);
            float ox = prop_eval(P, l, &n->mp[1], pid0 + 1, t) * g_rsx, oy = prop_eval(P, l, &n->mp[2], pid0 + 2, t) * g_rsy;
            if (opa > 0) bb = ir_union(bb, blend_onto(dst, fg, opa, n->blend, fb, ox, oy));
        }
        pool_put(rel, fb);
        n->res = dst; n->rbb = bb;
        break; }
    default: break;
    }
}
/* evaluates the clip's graph; returns an owned buffer (or NULL) and its dirty rect */
static FBuf *graph_render(Project *P, Layer *l, float t, const float *pv, const FBuf *backdrop, int W, int H, IRect *bb) {
    Graph *g = l->g;
    *bb = IR_NONE;
    int out_uid = graph_out_uid(g);
    Node *out = out_uid ? graph_find(g, out_uid) : NULL;
    if (!out) return NULL;
    g->stamp++;
    int seen[MAX_NODES]; memset(seen, 0, sizeof seen);
    for (int i = 0; i < g->n; i++) { g->nd[i].uses = 0; g->nd[i].res = NULL; }
    graph_count_uses(g, out, seen);
    GEval e = { P, l, g, t, pv, backdrop, W, H, 0 };
    Node *src = out->in[0] ? graph_find(g, out->in[0]) : NULL;
    if (!src) return NULL;
    node_eval(&e, src);
    FBuf *res = take_own(&e, src, bb);
    for (int i = 0; i < g->n; i++) if (g->nd[i].res) { pool_put(g->nd[i].res, g->nd[i].rbb); g->nd[i].res = NULL; }
    if (e.fail) afx_log(1, "Fusion graph of '%s' is too complex (out of render buffers)", l->name);
    return res;
}

/* ---------- final frame ---------- */
typedef struct { FBuf *dst; const FBuf *src; IRect r; float k; } MixCtx;
static void mix_rows(int r0, int r1, void *vp) {
    MixCtx *c = vp;
    for (int y = c->r.y0 + r0; y < c->r.y0 + r1; y++) {
        float *d = &c->dst->p[((size_t)y * c->dst->w + c->r.x0) * 4]; const float *s = &c->src->p[((size_t)y * c->src->w + c->r.x0) * 4];
        for (int i = 0; i < (c->r.x1 - c->r.x0) * 4; i++) d[i] += (s[i] - d[i]) * c->k;
    }
}
typedef struct { FBuf *b; float c[4]; } FillCtx;
static void fill_rows(int r0, int r1, void *vp) {
    FillCtx *c = vp;
    for (int y = r0; y < r1; y++) { float *p = &c->b->p[(size_t)y * c->b->w * 4]; for (int x = 0; x < c->b->w; x++) memcpy(&p[x * 4], c->c, sizeof c->c); }
}

void render_comp_scaled(Project *P, float t, FBuf *out, float rs) {
    int W = P->width, H = P->height;
    if (rs < 0.999f) { W = (int)(P->width * rs + 0.5f); H = (int)(P->height * rs + 0.5f); if (W < 8) W = 8; if (H < 8) H = 8; }
    else rs = 1;
    g_rsx = (float)W / P->width; g_rsy = (float)H / P->height; g_rs = 0.5f * (g_rsx + g_rsy);
    P->rs = rs;
    ensure(out, W, H); ensure(&P->t1, W, H); ensure(&P->t2, W, H);
    if (!out->p || !P->t1.p || !P->t2.p) { afx_log(1, "out of memory while rendering"); return; }
    for (int i = 0; i < POOL_N; i++) if (g_pool[i].b.p && (g_pool[i].b.w != W || g_pool[i].b.h != H) && !g_pool[i].busy) fbuf_free(&g_pool[i].b);
    P->cur_t = t; P->cur_frame = (int)floorf(t * P->fps + 0.5f);
    script_call_hooks(P, t, P->cur_frame);
    float ba = P->bg[3] / 255.0f;
    FillCtx fc = { out, { P->bg[0] / 255.0f * ba, P->bg[1] / 255.0f * ba, P->bg[2] / 255.0f * ba, ba } };
    par_rows(H, fill_rows, &fc);
    IRect full = ir_full(W, H);
    for (int li = 0; li < P->nlayers; li++) {
        Layer *l = &P->layers[li];
        if (!l->visible || t < l->tin || t >= project_layer_out(P, l) - 1e-6f) continue;
        float pv[P_COUNT];
        for (int i = 0; i < P_COUNT; i++) pv[i] = prop_eval(P, l, &l->p[i], i, t);
        float opa = clampf(pv[P_OPA] / 100, 0, 1);
        if (opa <= 0) continue;
        FBuf *lb; IRect bb;
        if (l->g) {
            lb = graph_render(P, l, t, pv, out, W, H, &bb);
            if (!lb) continue;
        } else if (l->type == L_ADJUST) {
            if (!l->nfx) continue;
            lb = pool_get(W, H); if (!lb) continue;
            bb = full; copy_rect(lb, out, full);
            apply_fx(P, l, lb, t, &bb);
        } else {
            lb = pool_get(W, H); if (!lb) continue;
            bb = draw_source(P, l, lb, pv, t);
            if (!ir_empty(bb)) apply_fx(P, l, lb, t, &bb);
        }
        if (l->type == L_ADJUST) {
            IRect r = ir_clip(bb, W, H);
            if (!ir_empty(r)) { MixCtx m = { out, lb, r, opa }; par_rows(r.y1 - r.y0, mix_rows, &m); }
        } else if (!ir_empty(bb)) blend_onto(out, lb, opa, l->blend, bb, 0, 0);
        pool_put(lb, bb);
    }
}
void render_comp(Project *P, float t, FBuf *out) { render_comp_scaled(P, t, out, 1.0f); }

typedef struct { const FBuf *b; uint8_t *dst; bool flat; } ConvCtx;
static void conv_rows(int r0, int r1, void *vp) {
    ConvCtx *c = vp; const FBuf *b = c->b;
    for (int y = r0; y < r1; y++) {
        const float *s = &b->p[(size_t)y * b->w * 4]; uint8_t *d = &c->dst[(size_t)y * b->w * 4];
        for (int x = 0; x < b->w; x++, s += 4, d += 4) {
            float a = s[3], r, g, bl, al;
            if (c->flat) { r = s[0]; g = s[1]; bl = s[2]; al = 1; }
            else if (a <= 1e-6f) { r = g = bl = 0; al = 0; }
            else { float ia = 1.0f / a; r = s[0] * ia; g = s[1] * ia; bl = s[2] * ia; al = a; }
            d[0] = (uint8_t)(clampf(r, 0, 1) * 255 + 0.5f); d[1] = (uint8_t)(clampf(g, 0, 1) * 255 + 0.5f);
            d[2] = (uint8_t)(clampf(bl, 0, 1) * 255 + 0.5f); d[3] = (uint8_t)(clampf(al, 0, 1) * 255 + 0.5f);
        }
    }
}
void fbuf_to_rgba8(const FBuf *b, uint8_t *dst, bool flatten_black) {
    ConvCtx c = { b, dst, flatten_black };
    par_rows(b->h, conv_rows, &c);
}
