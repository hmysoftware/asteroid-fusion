#define _GNU_SOURCE
// Asteroid Fusion media: video import (proxy frame sequence), frame cache with background prefetch.
//   desktop : ffprobe + ffmpeg  -> JPEG proxy frames
//   android : NDK MediaExtractor/MediaCodec (ByteBuffer output) -> JPEG proxy frames
#include "afx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <unistd.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

char afx_cache_dir[PATH_LEN] = "";

#define MEDIA_SLOTS 6
typedef struct { int idx, w, h; uint8_t *px; unsigned long used; bool pinned; } Slot;

struct Media {
    char path[PATH_LEN], dir[PATH_LEN];
    volatile int state;                 /* 0 idle, 1 importing, 2 ready, 3 error */
    int w, h; float fps, dur; bool audio;
    volatile int total, extracted;
    char err[200];
    pthread_mutex_t mu;
    Slot slots[MEDIA_SLOTS];
    unsigned long tick;
    int last_idx, want, dirn;
    bool queued, thumb_busy;
    Media *next;
};
static Media *g_media;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

/* ---------- name helpers ---------- */
static const char *VID_EXT[] = { ".mp4", ".mov", ".m4v", ".mkv", ".webm", ".avi", ".3gp", ".3g2", ".mpg", ".mpeg", ".ts", ".mts", ".m2ts", ".wmv", ".flv", ".ogv", NULL };
static const char *IMG_EXT[] = { ".png", ".jpg", ".jpeg", ".webp", ".bmp", NULL };
static bool has_ext(const char *name, const char **list) {
    const char *x = strrchr(name, '.');
    if (!x) return false;
    for (int i = 0; list[i]; i++) if (!strcasecmp(x, list[i])) return true;
    return false;
}
bool media_is_video_ext(const char *name) { return has_ext(name, VID_EXT); }
bool media_is_image_ext(const char *name) { return has_ext(name, IMG_EXT); }

static void mkdir_p(const char *dir) {
    char tmp[PATH_LEN]; snprintf(tmp, sizeof tmp, "%s", dir);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}
static unsigned long long hash_str(const char *s, unsigned long long h) {
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; }
    return h;
}
static void frame_path(const Media *m, int idx, char *buf, size_t cap) { snprintf(buf, cap, "%s/f%06d.jpg", m->dir, idx + 1); }

/* ---------- info file ---------- */
static void write_info(Media *m, bool done) {
    char p[PATH_LEN + 16]; snprintf(p, sizeof p, "%s/info.txt", m->dir);
    FILE *f = fopen(p, "wb");
    if (f) { fprintf(f, "%d %d %.6f %.6f %d %d\n", m->w, m->h, (double)m->fps, (double)m->dur, m->audio ? 1 : 0, m->total); fclose(f); }
    if (done) { snprintf(p, sizeof p, "%s/done", m->dir); f = fopen(p, "wb"); if (f) { fputs("ok\n", f); fclose(f); } }
}
static bool read_cached(Media *m) {
    char p[PATH_LEN + 16];
    snprintf(p, sizeof p, "%s/done", m->dir);
    if (access(p, F_OK) != 0) return false;
    snprintf(p, sizeof p, "%s/info.txt", m->dir);
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    int a = 0, tot = 0, w = 0, h = 0; double fps = 0, dur = 0;
    int n = fscanf(f, "%d %d %lf %lf %d %d", &w, &h, &fps, &dur, &a, &tot);
    fclose(f);
    if (n < 6 || tot <= 0 || w <= 0 || h <= 0) return false;
    m->w = w; m->h = h; m->fps = (float)fps; m->dur = (float)dur; m->audio = a != 0; m->total = tot; m->extracted = tot;
    return true;
}

/* ---------- registry ---------- */
Media *media_get(const char *path, bool create) {
    if (!path || !*path) return NULL;
    pthread_mutex_lock(&g_mu);
    Media *m;
    for (m = g_media; m; m = m->next) if (!strcmp(m->path, path)) break;
    if (!m && create) {
        m = calloc(1, sizeof *m);
        snprintf(m->path, sizeof m->path, "%s", path);
        pthread_mutex_init(&m->mu, NULL);
        struct stat st; unsigned long long h = hash_str(path, 1469598103934665603ULL);
        if (stat(path, &st) == 0) { h ^= (unsigned long long)st.st_size * 0x9E3779B97F4A7C15ULL; h ^= (unsigned long long)st.st_mtime; }
        if (!afx_cache_dir[0]) snprintf(afx_cache_dir, sizeof afx_cache_dir, "/tmp/afx_cache");
        snprintf(m->dir, sizeof m->dir, "%s/%016llx", afx_cache_dir, h);
        m->last_idx = -1; m->dirn = 1;
        if (read_cached(m)) m->state = 2;
        m->next = g_media; g_media = m;
    }
    pthread_mutex_unlock(&g_mu);
    return m;
}
int media_state(const Media *m) { return m ? m->state : 3; }
int media_progress(const Media *m, int *total) { if (total) *total = m->total; return m->extracted; }
void media_info(const Media *m, int *w, int *h, float *fps, float *dur) {
    if (w) *w = m->w;
    if (h) *h = m->h;
    if (fps) *fps = m->fps > 0 ? m->fps : 30;
    if (dur) *dur = m->dur;
}
const char *media_error(const Media *m) { return m->err; }
const char *media_path(const Media *m) { return m->path; }
bool media_has_audio(const Media *m) { return m->audio; }

/* ---------- command helpers (desktop) ---------- */
#ifndef PLATFORM_ANDROID
static bool run_capture(const char *cmd, char *buf, size_t cap) {
    FILE *p = popen(cmd, "r");
    if (!p) return false;
    size_t n = fread(buf, 1, cap - 1, p); buf[n] = 0;
    pclose(p);
    return true;
}
static void sh_quote(const char *s, char *out, size_t cap) {
    size_t n = 0; out[n++] = '\'';
    for (; *s && n + 6 < cap; s++) { if (*s == '\'') { memcpy(out + n, "'\\''", 4); n += 4; } else out[n++] = *s; }
    out[n++] = '\''; out[n] = 0;
}
static float parse_rate(const char *s) {
    double a = 0, b = 1;
    if (sscanf(s, "%lf/%lf", &a, &b) >= 1 && b > 0 && a > 0) return (float)(a / b);
    return 0;
}
#endif

/* ---------- android extractor ---------- */
#ifdef PLATFORM_ANDROID
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <time.h>

typedef struct { int fmt, w, h, stride, slice, cl, ct, cr, cb, rot; } YuvInfo;
static inline uint8_t clamp8(float v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5f); }
static bool yuv_to_jpeg(const uint8_t *d, size_t size, const YuvInfo *y, int ow, int oh, int dw, int dh, const char *path) {
    int cw = y->cr - y->cl + 1, ch = y->cb - y->ct + 1;
    int stride = y->stride > 0 ? y->stride : y->w, slice = y->slice > 0 ? y->slice : y->h;
    const uint8_t *Y = d, *U, *V; int cstep, cstride;
    if (y->fmt == 19) { U = d + (size_t)stride * slice; V = U + (size_t)(stride / 2) * (slice / 2); cstep = 1; cstride = stride / 2; }
    else { U = d + (size_t)stride * slice; V = U + 1; cstep = 2; cstride = stride; }
    size_t need = (size_t)stride * slice * 3 / 2;
    if (size < need - (size_t)stride) return false;
    uint8_t *rgb = malloc((size_t)ow * oh * 3);
    if (!rgb) return false;
    float sx_scale = (float)dw / ow, sy_scale = (float)dh / oh;
    for (int py = 0; py < oh; py++) for (int px = 0; px < ow; px++) {
        float acc[3] = { 0, 0, 0 };
        for (int t = 0; t < 4; t++) {
            float u = (px + 0.25f + 0.5f * (t & 1)) * sx_scale, v = (py + 0.25f + 0.5f * (t >> 1)) * sy_scale, sx, sy;
            switch (y->rot) {
            case 90: sx = v; sy = ch - u; break;
            case 180: sx = cw - u; sy = ch - v; break;
            case 270: sx = cw - v; sy = u; break;
            default: sx = u; sy = v; break;
            }
            int ix = (int)sx, iy = (int)sy;
            if (ix < 0) ix = 0; if (iy < 0) iy = 0; if (ix >= cw) ix = cw - 1; if (iy >= ch) iy = ch - 1;
            ix += y->cl; iy += y->ct;
            float yy = (Y[(size_t)iy * stride + ix] - 16) * 1.164f;
            float uu = U[(size_t)(iy / 2) * cstride + (size_t)(ix / 2) * cstep] - 128.0f;
            float vv = V[(size_t)(iy / 2) * cstride + (size_t)(ix / 2) * cstep] - 128.0f;
            acc[0] += yy + 1.793f * vv; acc[1] += yy - 0.213f * uu - 0.533f * vv; acc[2] += yy + 2.112f * uu;
        }
        uint8_t *o = &rgb[((size_t)py * ow + px) * 3];
        o[0] = clamp8(acc[0] * 0.25f); o[1] = clamp8(acc[1] * 0.25f); o[2] = clamp8(acc[2] * 0.25f);
    }
    int ok = stbi_write_jpg(path, ow, oh, 3, rgb, 85);
    free(rgb);
    return ok != 0;
}
static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }

static bool android_extract(Media *m) {
    AMediaExtractor *ex = AMediaExtractor_new();
    if (AMediaExtractor_setDataSource(ex, m->path) != AMEDIA_OK) {
        snprintf(m->err, sizeof m->err, "cannot open the video (allow media access, or copy it into the projects/media folder)");
        AMediaExtractor_delete(ex); return false;
    }
    int ntr = (int)AMediaExtractor_getTrackCount(ex), vt = -1; AMediaFormat *vf = NULL; char mime[96] = ""; bool audio = false;
    for (int i = 0; i < ntr; i++) {
        AMediaFormat *f = AMediaExtractor_getTrackFormat(ex, i);
        const char *mm = NULL; AMediaFormat_getString(f, "mime", &mm);
        if (mm && !strncmp(mm, "video/", 6) && vt < 0) { vt = i; vf = f; snprintf(mime, sizeof mime, "%s", mm); }
        else { if (mm && !strncmp(mm, "audio/", 6)) audio = true; AMediaFormat_delete(f); }
    }
    if (vt < 0) { snprintf(m->err, sizeof m->err, "no video track in this file"); AMediaExtractor_delete(ex); return false; }
    int32_t vw = 0, vh = 0, fr = 0, rot = 0; int64_t durus = 0;
    AMediaFormat_getInt32(vf, "width", &vw); AMediaFormat_getInt32(vf, "height", &vh);
    AMediaFormat_getInt32(vf, "frame-rate", &fr); AMediaFormat_getInt32(vf, "rotation-degrees", &rot);
    AMediaFormat_getInt64(vf, "durationUs", &durus);
    if (vw <= 0 || vh <= 0) { snprintf(m->err, sizeof m->err, "unreadable video size"); AMediaFormat_delete(vf); AMediaExtractor_delete(ex); return false; }
    AMediaExtractor_selectTrack(ex, vt);
    float fps = fr > 0 ? (float)fr : 30; if (fps > PROXY_FPS_MAX) fps = PROXY_FPS_MAX; if (fps < 1) fps = 30;
    rot = ((rot % 360) + 360) % 360;
    int dw = (rot == 90 || rot == 270) ? vh : vw, dh = (rot == 90 || rot == 270) ? vw : vh;
    int shortside = dw < dh ? dw : dh, S = PROXY_SHORT_DEFAULT < shortside ? PROXY_SHORT_DEFAULT : shortside;
    float sc = (float)S / shortside;
    int ow = ((int)(dw * sc + 0.5f)) & ~1, oh = ((int)(dh * sc + 0.5f)) & ~1;
    if (ow < 2) ow = 2; if (oh < 2) oh = 2;
    m->w = ow; m->h = oh; m->fps = fps; m->dur = durus > 0 ? (float)(durus / 1e6) : 0; m->audio = audio;
    m->total = m->dur > 0 ? (int)ceilf(m->dur * fps) : 0;
    AMediaCodec *codec = AMediaCodec_createDecoderByType(mime);
    if (!codec) { snprintf(m->err, sizeof m->err, "no decoder for %s", mime); AMediaFormat_delete(vf); AMediaExtractor_delete(ex); return false; }
    AMediaFormat_setInt32(vf, "color-format", 21);
    if (AMediaCodec_configure(codec, vf, NULL, NULL, 0) != AMEDIA_OK || AMediaCodec_start(codec) != AMEDIA_OK) {
        snprintf(m->err, sizeof m->err, "the decoder refused this video (%s)", mime);
        AMediaCodec_delete(codec); AMediaFormat_delete(vf); AMediaExtractor_delete(ex); return false;
    }
    YuvInfo yi = { 21, vw, vh, vw, vh, 0, 0, vw - 1, vh - 1, (int)rot };
    bool in_eos = false, out_eos = false, ok = true;
    int written = 0; int64_t next_us = 0; double step_us = 1e6 / fps, last_progress = now_s();
    char fp[PATH_LEN + 24];
    while (!out_eos && ok) {
        if (!in_eos) {
            ssize_t ib = AMediaCodec_dequeueInputBuffer(codec, 5000);
            if (ib >= 0) {
                size_t cap = 0; uint8_t *buf = AMediaCodec_getInputBuffer(codec, (size_t)ib, &cap);
                ssize_t sz = AMediaExtractor_readSampleData(ex, buf, cap);
                if (sz < 0) { AMediaCodec_queueInputBuffer(codec, (size_t)ib, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM); in_eos = true; }
                else { AMediaCodec_queueInputBuffer(codec, (size_t)ib, 0, (size_t)sz, (uint64_t)AMediaExtractor_getSampleTime(ex), 0); AMediaExtractor_advance(ex); }
            }
        }
        AMediaCodecBufferInfo info;
        ssize_t ob = AMediaCodec_dequeueOutputBuffer(codec, &info, 5000);
        if (ob >= 0) {
            last_progress = now_s();
            if (info.size > 0 && info.presentationTimeUs >= next_us - 1000) {
                size_t osz = 0; uint8_t *data = AMediaCodec_getOutputBuffer(codec, (size_t)ob, &osz);
                if (data && osz >= (size_t)info.offset + (size_t)info.size) {
                    snprintf(fp, sizeof fp, "%s/f%06d.jpg", m->dir, written + 1);
                    if (!yuv_to_jpeg(data + info.offset, (size_t)info.size, &yi, ow, oh, dw, dh, fp)) { snprintf(m->err, sizeof m->err, "unsupported decoder color format %d", yi.fmt); ok = false; }
                    else { written++; m->extracted = written; if (written > m->total) m->total = written; next_us += (int64_t)step_us; if (next_us < info.presentationTimeUs) next_us = info.presentationTimeUs + (int64_t)step_us; }
                }
            }
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) out_eos = true;
            AMediaCodec_releaseOutputBuffer(codec, (size_t)ob, false);
        } else if (ob == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat *of = AMediaCodec_getOutputFormat(codec);
            int32_t v = 0;
            if (AMediaFormat_getInt32(of, "color-format", &v)) yi.fmt = v;
            if (AMediaFormat_getInt32(of, "width", &v)) yi.w = v;
            if (AMediaFormat_getInt32(of, "height", &v)) yi.h = v;
            yi.stride = yi.w; yi.slice = yi.h;
            if (AMediaFormat_getInt32(of, "stride", &v) && v > 0) yi.stride = v;
            if (AMediaFormat_getInt32(of, "slice-height", &v) && v > 0) yi.slice = v;
            yi.cl = 0; yi.ct = 0; yi.cr = yi.w - 1; yi.cb = yi.h - 1;
            if (AMediaFormat_getInt32(of, "crop-left", &v)) yi.cl = v;
            if (AMediaFormat_getInt32(of, "crop-top", &v)) yi.ct = v;
            if (AMediaFormat_getInt32(of, "crop-right", &v)) yi.cr = v;
            if (AMediaFormat_getInt32(of, "crop-bottom", &v)) yi.cb = v;
            if (yi.fmt != 21 && yi.fmt != 19 && yi.fmt != 0x7F420888) { snprintf(m->err, sizeof m->err, "unsupported decoder color format 0x%x", yi.fmt); ok = false; }
            if (yi.fmt == 0x7F420888) yi.fmt = 21;
            AMediaFormat_delete(of);
        } else if (now_s() - last_progress > 20.0) { snprintf(m->err, sizeof m->err, "decoder stalled"); ok = false; }
        if (m->state == 4) { ok = false; snprintf(m->err, sizeof m->err, "cancelled"); }
    }
    AMediaCodec_stop(codec); AMediaCodec_delete(codec); AMediaFormat_delete(vf); AMediaExtractor_delete(ex);
    if (ok && written > 0) { m->total = written; m->extracted = written; }
    else if (ok) { snprintf(m->err, sizeof m->err, "no frames could be decoded"); ok = false; }
    return ok;
}
#endif

/* ---------- import thread ---------- */
static bool do_extract(Media *m) {
#ifdef PLATFORM_ANDROID
    return android_extract(m);
#else
    char q[PATH_LEN * 2], cmd[PATH_LEN * 3 + 600], out[2048];
    sh_quote(m->path, q, sizeof q);
    if (access(m->path, R_OK) != 0) { snprintf(m->err, sizeof m->err, "cannot read '%s'", m->path); return false; }
    if (!export_ffmpeg_ok()) { snprintf(m->err, sizeof m->err, "ffmpeg is needed to import video on this system"); return false; }
    snprintf(cmd, sizeof cmd, "ffprobe -v error -select_streams v:0 -show_entries stream=width,height,r_frame_rate,avg_frame_rate,duration:format=duration -of default=nw=1 %s 2>/dev/null", q);
    out[0] = 0;
    if (!run_capture(cmd, out, sizeof out) || !out[0]) { snprintf(m->err, sizeof m->err, "this file has no readable video stream"); return false; }
    int vw = 0, vh = 0; float rate = 0, avg = 0, dur = 0;
    for (char *line = strtok(out, "\n"); line; line = strtok(NULL, "\n")) {
        if (!strncmp(line, "width=", 6)) vw = atoi(line + 6);
        else if (!strncmp(line, "height=", 7)) vh = atoi(line + 7);
        else if (!strncmp(line, "r_frame_rate=", 13)) rate = parse_rate(line + 13);
        else if (!strncmp(line, "avg_frame_rate=", 15)) avg = parse_rate(line + 15);
        else if (!strncmp(line, "duration=", 9) && dur <= 0) dur = (float)atof(line + 9);
    }
    if (vw <= 0 || vh <= 0) { snprintf(m->err, sizeof m->err, "unreadable video size"); return false; }
    float fps = avg > 0 ? avg : rate > 0 ? rate : 30;
    if (fps > PROXY_FPS_MAX) fps = PROXY_FPS_MAX;
    if (fps < 1) fps = 30;
    if (dur <= 0) dur = 1;
    int sh = vw < vh ? vw : vh, S = PROXY_SHORT_DEFAULT < sh ? PROXY_SHORT_DEFAULT : sh;
    S &= ~1; if (S < 2) S = 2;
    m->fps = fps; m->dur = dur; m->total = (int)ceilf(dur * fps);
    snprintf(cmd, sizeof cmd, "ffprobe -v error -select_streams a:0 -show_entries stream=codec_type -of csv=p=0 %s 2>/dev/null", q);
    out[0] = 0; run_capture(cmd, out, sizeof out);
    m->audio = strstr(out, "audio") != NULL;
    snprintf(cmd, sizeof cmd,
        "ffmpeg -y -hide_banner -loglevel error -nostats -progress pipe:1 -i %s -an -sn -vf \"fps=%.4f,scale='if(gt(iw,ih),-2,%d)':'if(gt(iw,ih),%d,-2)'\" -q:v 4 \"%s/f%%06d.jpg\" 2>&1",
        q, (double)fps, S, S, m->dir);
    FILE *p = popen(cmd, "r");
    if (!p) { snprintf(m->err, sizeof m->err, "cannot start ffmpeg"); return false; }
    char line[512], lasterr[200] = "";
    bool got_size = false;
    while (fgets(line, sizeof line, p)) {
        if (!strncmp(line, "frame=", 6)) {
            int n = atoi(line + 6);
            if (n > m->extracted) m->extracted = n;
            if (!got_size && n > 0) {
                char fp[PATH_LEN + 24]; snprintf(fp, sizeof fp, "%s/f%06d.jpg", m->dir, 1);
                int w, h, c; if (stbi_info(fp, &w, &h, &c)) { m->w = w; m->h = h; got_size = true; }
            }
        } else if (strncmp(line, "out_time", 8) && strncmp(line, "progress=", 9) && strncmp(line, "bitrate", 7) && strncmp(line, "total_size", 10) &&
                   strncmp(line, "fps=", 4) && strncmp(line, "stream_", 7) && strncmp(line, "dup_frames", 10) && strncmp(line, "drop_frames", 11) && strncmp(line, "speed=", 6)) {
            line[strcspn(line, "\r\n")] = 0; snprintf(lasterr, sizeof lasterr, "%s", line);
        }
        if (m->state == 4) { pclose(p); snprintf(m->err, sizeof m->err, "cancelled"); return false; }
    }
    int rc = pclose(p);
    /* count the frames that really exist */
    int n = 0; char fp[PATH_LEN + 24];
    for (n = (int)m->extracted > 0 ? (int)m->extracted - 1 : 0; ; n++) { snprintf(fp, sizeof fp, "%s/f%06d.jpg", m->dir, n + 1); if (access(fp, F_OK) != 0) break; }
    if (n <= 0) { snprintf(m->err, sizeof m->err, "ffmpeg failed%s%s", lasterr[0] ? ": " : "", lasterr); (void)rc; return false; }
    if (!got_size) { snprintf(fp, sizeof fp, "%s/f%06d.jpg", m->dir, 1); int w, h, c; if (stbi_info(fp, &w, &h, &c)) { m->w = w; m->h = h; } }
    m->total = n; m->extracted = n;
    return true;
#endif
}
static void *import_main(void *arg) {
    Media *m = arg;
    mkdir_p(m->dir);
    bool ok = do_extract(m);
    if (ok) { write_info(m, true); m->state = 2; afx_log(0, "imported video '%s' (%d frames @ %.4g fps)", strrchr(m->path, '/') ? strrchr(m->path, '/') + 1 : m->path, m->total, (double)m->fps); }
    else { m->state = 3; afx_log(1, "video import failed: %s", m->err[0] ? m->err : "unknown error"); }
    return NULL;
}
void media_start(Media *m) {
    if (!m) return;
    pthread_mutex_lock(&g_mu);
    if (m->state == 0) {
        m->state = 1; m->err[0] = 0;
        pthread_t th;
        if (pthread_create(&th, NULL, import_main, m) == 0) pthread_detach(th);
        else { m->state = 3; snprintf(m->err, sizeof m->err, "cannot start import thread"); }
    }
    pthread_mutex_unlock(&g_mu);
}
bool media_project_ready(Project *P) {
    bool all = true;
    for (int i = 0; i < P->nlayers; i++) {
        Layer *l = &P->layers[i];
        const char *paths[1 + MAX_NODES]; int np = 0;
        if (l->type == L_VIDEO && l->path[0]) paths[np++] = l->path;
        if (l->g) for (int k = 0; k < l->g->n; k++) { Node *n = &l->g->nd[k]; if (n->kind == N_GEN && n->gen && n->gen->type == L_VIDEO && n->gen->path[0]) paths[np++] = n->gen->path; }
        for (int k = 0; k < np; k++) {
            Media *m = media_get(paths[k], true);
            if (!m) continue;
            media_start(m);
            if (m->state == 0 || m->state == 1) all = false;
        }
    }
    return all;
}
void media_retry(Media *m) { if (m && m->state == 3) { m->state = 0; m->err[0] = 0; m->extracted = 0; } }
bool media_platform_extract(const char *path, const char *dir, int max_short, Media *m) { (void)path; (void)dir; (void)max_short; return m && do_extract(m); }

/* ---------- decode + cache + prefetch ---------- */
static uint8_t *decode_file(const char *path, int *w, int *h) {
    int n;
    return stbi_load(path, w, h, &n, 4);
}
static Slot *find_slot(Media *m, int idx) {
    for (int i = 0; i < MEDIA_SLOTS; i++) if (m->slots[i].px && m->slots[i].idx == idx) return &m->slots[i];
    return NULL;
}
static Slot *victim_slot(Media *m) {
    Slot *v = NULL;
    for (int i = 0; i < MEDIA_SLOTS; i++) {
        Slot *s = &m->slots[i];
        if (s->pinned) continue;
        if (!s->px) return s;
        if (!v || s->used < v->used) v = s;
    }
    return v;
}
static void slot_put(Media *m, Slot *s, int idx, uint8_t *px, int w, int h, bool pin) {
    if (s->px) stbi_image_free(s->px);
    s->px = px; s->idx = idx; s->w = w; s->h = h; s->used = ++m->tick; s->pinned = pin;
}

static pthread_t g_pf_th; static bool g_pf_started, g_pf_quit;
static pthread_cond_t g_pf_cv = PTHREAD_COND_INITIALIZER;
static Media *g_pf_q[16]; static int g_pf_n;
static void *pf_main(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (g_pf_n == 0 && !g_pf_quit) pthread_cond_wait(&g_pf_cv, &g_mu);
        if (g_pf_quit) { pthread_mutex_unlock(&g_mu); break; }
        Media *m = g_pf_q[0];
        memmove(g_pf_q, g_pf_q + 1, sizeof(Media *) * (size_t)(--g_pf_n));
        m->queued = false;
        pthread_mutex_unlock(&g_mu);
        int avail = m->state == 1 ? m->extracted : m->total;
        for (int k = 1; k <= 3; k++) {
            int i = m->want + m->dirn * k;
            if (i < 0 || i >= avail) break;
            pthread_mutex_lock(&m->mu);
            bool have = find_slot(m, i) != NULL;
            pthread_mutex_unlock(&m->mu);
            if (have) continue;
            char fp[PATH_LEN + 24]; frame_path(m, i, fp, sizeof fp);
            int w, h; uint8_t *px = decode_file(fp, &w, &h);
            if (!px) break;
            pthread_mutex_lock(&m->mu);
            Slot *v = find_slot(m, i) ? NULL : victim_slot(m);
            if (v) slot_put(m, v, i, px, w, h, false); else stbi_image_free(px);
            pthread_mutex_unlock(&m->mu);
        }
    }
    return NULL;
}
void media_hint(Media *m, int idx) {
    if (!m) return;
    pthread_mutex_lock(&g_mu);
    if (m->last_idx >= 0 && idx != m->last_idx) m->dirn = idx > m->last_idx ? 1 : -1;
    m->last_idx = idx; m->want = idx;
    if (!g_pf_started) { g_pf_started = true; if (pthread_create(&g_pf_th, NULL, pf_main, NULL) != 0) g_pf_started = false; }
    if (g_pf_started && !m->queued && g_pf_n < 16) { m->queued = true; g_pf_q[g_pf_n++] = m; pthread_cond_signal(&g_pf_cv); }
    pthread_mutex_unlock(&g_mu);
}

const uint8_t *media_frame(Media *m, int idx, int *w, int *h) {
    if (!m) return NULL;
    int avail = m->state == 1 ? m->extracted : m->total;
    if (m->state == 3 || avail <= 0) return NULL;
    if (idx < 0) idx = 0;
    if (idx >= avail) idx = avail - 1;
    pthread_mutex_lock(&m->mu);
    for (int i = 0; i < MEDIA_SLOTS; i++) m->slots[i].pinned = false;
    Slot *s = find_slot(m, idx);
    if (s) { s->used = ++m->tick; s->pinned = true; *w = s->w; *h = s->h; const uint8_t *r = s->px; pthread_mutex_unlock(&m->mu); media_hint(m, idx); return r; }
    pthread_mutex_unlock(&m->mu);
    char fp[PATH_LEN + 24]; int fw = 0, fh = 0; uint8_t *px = NULL;
    for (int i = idx; i >= 0 && i > idx - 3 && !px; i--) { frame_path(m, i, fp, sizeof fp); px = decode_file(fp, &fw, &fh); if (px) idx = i; }
    if (!px) return NULL;
    pthread_mutex_lock(&m->mu);
    s = find_slot(m, idx);
    if (s) { stbi_image_free(px); }
    else { s = victim_slot(m); if (!s) s = &m->slots[0]; slot_put(m, s, idx, px, fw, fh, true); }
    s->pinned = true; s->used = ++m->tick; *w = s->w; *h = s->h;
    const uint8_t *r = s->px;
    pthread_mutex_unlock(&m->mu);
    media_hint(m, idx);
    return r;
}

/* small RGBA thumbnail (box filtered) of one frame, for the timeline strip; does not touch the playback cache */
bool media_thumb(Media *m, int idx, uint8_t *dst, int tw, int th) {
    if (!m || m->state == 3) return false;
    int avail = m->state == 1 ? m->extracted : m->total;
    if (avail <= 0) return false;
    if (idx < 0) idx = 0;
    if (idx >= avail) idx = avail - 1;
    char fp[PATH_LEN + 24]; frame_path(m, idx, fp, sizeof fp);
    int w, h; uint8_t *px = decode_file(fp, &w, &h);
    if (!px) return false;
    /* cover-crop into tw x th */
    float sc = fmaxf((float)tw / w, (float)th / h), cw = tw / sc, ch = th / sc, ox = (w - cw) / 2, oy = (h - ch) / 2;
    for (int y = 0; y < th; y++) for (int x = 0; x < tw; x++) {
        int x0 = (int)(ox + x / sc), x1 = (int)(ox + (x + 1) / sc), y0 = (int)(oy + y / sc), y1 = (int)(oy + (y + 1) / sc);
        if (x1 <= x0) x1 = x0 + 1; if (y1 <= y0) y1 = y0 + 1;
        if (x1 > w) x1 = w; if (y1 > h) y1 = h; if (x0 >= x1) x0 = x1 - 1; if (y0 >= y1) y0 = y1 - 1;
        unsigned acc[3] = { 0, 0, 0 }, n = 0;
        for (int yy = y0; yy < y1; yy += (y1 - y0 > 4 ? 2 : 1)) for (int xx = x0; xx < x1; xx += (x1 - x0 > 4 ? 2 : 1)) {
            const uint8_t *p = &px[((size_t)yy * w + xx) * 4]; acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; n++;
        }
        uint8_t *d = &dst[((size_t)y * tw + x) * 4];
        d[0] = (uint8_t)(acc[0] / n); d[1] = (uint8_t)(acc[1] / n); d[2] = (uint8_t)(acc[2] / n); d[3] = 255;
    }
    stbi_image_free(px);
    return true;
}

/* ---------- cache maintenance ---------- */
static void rm_dir_files(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[PATH_LEN * 2]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) { rm_dir_files(p); rmdir(p); } else remove(p);
    }
    closedir(d);
}
void media_cache_clear(void) {
    pthread_mutex_lock(&g_mu);
    for (Media *m = g_media; m; m = m->next) {
        if (m->state == 1) continue;
        pthread_mutex_lock(&m->mu);
        for (int i = 0; i < MEDIA_SLOTS; i++) { if (m->slots[i].px) stbi_image_free(m->slots[i].px); memset(&m->slots[i], 0, sizeof m->slots[i]); }
        pthread_mutex_unlock(&m->mu);
        m->state = 0; m->total = m->extracted = 0; m->err[0] = 0;
    }
    pthread_mutex_unlock(&g_mu);
    if (afx_cache_dir[0]) rm_dir_files(afx_cache_dir);
}
static long dir_bytes(const char *dir) {
    long tot = 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[PATH_LEN * 2]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        tot += S_ISDIR(st.st_mode) ? dir_bytes(p) : (long)st.st_size;
    }
    closedir(d);
    return tot;
}
long media_cache_bytes(void) { return afx_cache_dir[0] ? dir_bytes(afx_cache_dir) : 0; }

void media_shutdown(void) {
    pthread_mutex_lock(&g_mu);
    for (Media *m = g_media; m; m = m->next) if (m->state == 1) m->state = 4;     /* ask importers to stop */
    g_pf_quit = true;
    pthread_cond_broadcast(&g_pf_cv);
    pthread_mutex_unlock(&g_mu);
    if (g_pf_started) pthread_join(g_pf_th, NULL);
}
