// AFX export: video via ffmpeg pipe, built-in GIF fallback, PNG/JPG frames
#define _GNU_SOURCE
#include "afx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/stat.h>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#ifdef PLATFORM_ANDROID
#define ANDROID_ENC 1
#else
#define ANDROID_ENC 0
#endif
#include "msf_gif.h"   /* implementation lives in libraylib (SUPPORT_GIF_RECORDING) */

const char *VIDFMT_NAMES[VF_COUNT] = { "GIF", "MP4", "WEBM", "AVI" };
const char *VIDFMT_EXT[VF_COUNT]   = { "gif", "mp4", "webm", "avi" };

struct ExportJob {
    Project *P;
    VidFmt fmt;
    int from, to, cur;          /* frame range [from,to], next frame = cur */
    int ow, oh;                 /* output size */
    FILE *pipe;                 /* ffmpeg stdin, or NULL for built-in gif */
    bool builtin_gif;
    MsfGifState gif;
    uint8_t *rgba;              /* ow*oh*4 scratch */
    FBuf fb;
    char err[256];
    char path[PATH_LEN];
    bool failed;
    void *aenc;                 /* android MediaCodec encoder (AEnc) */
    bool use_ff, media_ok;      /* ffmpeg pipe is started lazily, once the video proxies are ready (audio needs their info) */
    int quality;
};

struct ExportJob;
/* ---- audio of video clips, mixed with ffmpeg ---- */
typedef struct { char path[PATH_LEN]; float tin, len, trim, speed, vol; } ASrc;
static int collect_audio(Project *P, ASrc *out, int max) {
    int n = 0;
    for (int i = 0; i < P->nlayers && n < max; i++) {
        Layer *l = &P->layers[i];
        if (!l->visible) continue;
        const Layer *cand[1 + MAX_NODES]; int nc = 0;
        if (l->type == L_VIDEO) cand[nc++] = l;
        if (l->g) for (int k = 0; k < l->g->n; k++) { Node *nd = &l->g->nd[k]; if (nd->kind == N_GEN && nd->on && nd->gen && nd->gen->type == L_VIDEO) cand[nc++] = nd->gen; }
        for (int k = 0; k < nc && n < max; k++) {
            const Layer *v = cand[k];
            if (!v->path[0] || v->vol <= 0.001f || strpbrk(v->path, "\"$`\\\n")) continue;
            Media *m = media_get(v->path, false);
            if (!m || media_state(m) != 2 || !media_has_audio(m)) continue;
            float sp = v->speed > 0.01f ? v->speed : 1, len = project_layer_out(P, l) - l->tin;
            if (len <= 0.01f) continue;
            snprintf(out[n].path, PATH_LEN, "%s", v->path);
            out[n].tin = l->tin; out[n].len = len; out[n].trim = v->trim; out[n].speed = sp; out[n].vol = v->vol; n++;
        }
    }
    return n;
}
static void atempo_chain(char *buf, size_t cap, float s) {
    buf[0] = 0; size_t n = 0;
    while (s > 2.0f && n + 24 < cap) { n += (size_t)snprintf(buf + n, cap - n, "atempo=2.0,"); s /= 2; }
    while (s < 0.5f && n + 24 < cap) { n += (size_t)snprintf(buf + n, cap - n, "atempo=0.5,"); s *= 2; }
    snprintf(buf + n, cap - n, "atempo=%.5f", (double)s);
}

static bool ff_start(ExportJob *j) {
    Project *P = j->P;
    static ASrc as[8];
    int na = j->fmt == VF_GIF ? 0 : collect_audio(P, as, 8);
    char *cmd = malloc(16384), *fc = malloc(8192);
    if (!cmd || !fc) { free(cmd); free(fc); snprintf(j->err, sizeof j->err, "out of memory"); j->failed = true; return false; }
    int crf = 40 - j->quality * 22 / 100;          /* quality 100 -> 18, 1 -> ~40 */
    size_t n = (size_t)snprintf(cmd, 16384, "ffmpeg -y -hide_banner -loglevel error -f rawvideo -pix_fmt rgba -s %dx%d -r %g -i - ", j->ow, j->oh, (double)P->fps);
    for (int i = 0; i < na; i++) n += (size_t)snprintf(cmd + n, 16384 - n, "-i \"%s\" ", as[i].path);
    double total = (double)(j->to - j->from + 1) / P->fps;
    fc[0] = 0;
    if (na) {
        size_t m = 0; char at[160];
        for (int i = 0; i < na; i++) {
            atempo_chain(at, sizeof at, as[i].speed);
            int ms = (int)(as[i].tin * 1000.0f + 0.5f) - (int)(j->from * 1000.0 / P->fps);
            if (ms < 0) ms = 0;
            m += (size_t)snprintf(fc + m, 8192 - m, "[%d:a]atrim=start=%.4f:end=%.4f,asetpts=PTS-STARTPTS,%s,volume=%.3f,aresample=44100,aformat=channel_layouts=stereo,adelay=%d|%d[a%d];",
                                  i + 1, (double)as[i].trim, (double)(as[i].trim + as[i].len * as[i].speed), at, (double)as[i].vol, ms, ms, i);
        }
        for (int i = 0; i < na; i++) m += (size_t)snprintf(fc + m, 8192 - m, "[a%d]", i);
        snprintf(fc + m, 8192 - m, "amix=inputs=%d:normalize=0:duration=longest,atrim=0:%.4f[aout]", na, total);
    }
    size_t k = n;
    switch (j->fmt) {
    case VF_GIF:
        snprintf(cmd + k, 16384 - k, "-vf \"split[a][b];[a]palettegen=reserve_transparent=1[p];[b][p]paletteuse\" -loop 0 \"%s\"", j->path); break;
    case VF_MP4:
        if (na) snprintf(cmd + k, 16384 - k, "-filter_complex \"%s\" -map 0:v -map \"[aout]\" -vf \"format=yuv420p\" -c:v libx264 -preset veryfast -crf %d -c:a aac -b:a 160k -movflags +faststart \"%s\" 2>&1", fc, crf, j->path);
        else snprintf(cmd + k, 16384 - k, "-vf \"format=yuv420p\" -c:v libx264 -preset veryfast -crf %d -movflags +faststart \"%s\" 2>&1", crf, j->path);
        break;
    case VF_WEBM:
        if (na) snprintf(cmd + k, 16384 - k, "-filter_complex \"%s\" -map 0:v -map \"[aout]\" -c:v libvpx-vp9 -pix_fmt yuva420p -b:v 0 -crf %d -c:a libopus -b:a 128k \"%s\"", fc, crf + 4, j->path);
        else snprintf(cmd + k, 16384 - k, "-c:v libvpx-vp9 -pix_fmt yuva420p -b:v 0 -crf %d \"%s\"", crf + 4, j->path);
        break;
    default:
        if (na) snprintf(cmd + k, 16384 - k, "-filter_complex \"%s\" -map 0:v -map \"[aout]\" -c:v mpeg4 -q:v %d -c:a aac -b:a 160k \"%s\"", fc, 2 + (100 - j->quality) * 29 / 100, j->path);
        else snprintf(cmd + k, 16384 - k, "-c:v mpeg4 -q:v %d \"%s\"", 2 + (100 - j->quality) * 29 / 100, j->path);
        break;
    }
    j->pipe = popen(cmd, "w");
    free(cmd); free(fc);
    if (!j->pipe) { snprintf(j->err, sizeof j->err, "cannot start ffmpeg"); j->failed = true; return false; }
    return true;
}

bool export_ffmpeg_ok(void) {
    static int ok = -1;
    if (ok < 0) ok = system("ffmpeg -hide_banner -loglevel quiet -version >/dev/null 2>&1") == 0;
    return ok == 1;
}

VidFmt export_fmt_from_path(const char *path, VidFmt def) {
    const char *dot = strrchr(path, '.');
    if (!dot) return def;
    for (int i = 0; i < VF_COUNT; i++) if (!strcasecmp(dot + 1, VIDFMT_EXT[i])) return (VidFmt)i;
    return def;
}

/* nearest-neighbour-ish box downscale of the rendered frame into job->rgba */
static void scale_frame(ExportJob *j, const FBuf *src) {
    int W = src->w, H = src->h, ow = j->ow, oh = j->oh;
    uint8_t *full = malloc((size_t)W * H * 4);
    fbuf_to_rgba8(src, full, j->fmt != VF_GIF || j->builtin_gif);
    if (ow == W && oh == H) { memcpy(j->rgba, full, (size_t)W * H * 4); free(full); return; }
    for (int y = 0; y < oh; y++) {
        int y0 = y * H / oh, y1 = (y + 1) * H / oh; if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < ow; x++) {
            int x0 = x * W / ow, x1 = (x + 1) * W / ow; if (x1 <= x0) x1 = x0 + 1;
            unsigned acc[4] = { 0, 0, 0, 0 }, n = 0;
            for (int yy = y0; yy < y1; yy++) for (int xx = x0; xx < x1; xx++) {
                const uint8_t *p = &full[((size_t)yy * W + xx) * 4];
                acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; acc[3] += p[3]; n++;
            }
            uint8_t *d = &j->rgba[((size_t)y * ow + x) * 4];
            for (int c = 0; c < 4; c++) d[c] = (uint8_t)(acc[c] / n);
        }
    }
    free(full);
}


/* ---------- Android: H.264 MP4 through MediaCodec + MediaMuxer (no ffmpeg on the phone) ---------- */
#ifdef PLATFORM_ANDROID
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaMuxer.h>
#include <fcntl.h>
typedef struct { AMediaCodec *codec; AMediaMuxer *mux; int fd; int track; bool started; } AEnc;

static inline uint8_t cl8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
static void rgba_to_nv12(const uint8_t *rgba, int w, int h, uint8_t *dst) {
    uint8_t *Y = dst, *UV = dst + (size_t)w * h;
    for (int y = 0; y < h; y += 2) for (int x = 0; x < w; x += 2) {
        int su = 0, sv = 0;
        for (int dy = 0; dy < 2; dy++) for (int dx = 0; dx < 2; dx++) {
            const uint8_t *p = &rgba[((size_t)(y + dy) * w + (x + dx)) * 4];
            int r = p[0], g = p[1], b = p[2];
            Y[(size_t)(y + dy) * w + x + dx] = cl8(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
            su += ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
            sv += ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
        }
        UV[(size_t)(y / 2) * w + x] = cl8(su / 4); UV[(size_t)(y / 2) * w + x + 1] = cl8(sv / 4);
    }
}
/* pull finished packets into the muxer; wait=true blocks until the end-of-stream packet */
static bool aenc_drain(AEnc *e, bool wait) {
    for (int idle = 0; idle < 3000;) {
        AMediaCodecBufferInfo info;
        ssize_t ob = AMediaCodec_dequeueOutputBuffer(e->codec, &info, wait ? 10000 : 0);
        if (ob >= 0) {
            size_t sz = 0; uint8_t *data = AMediaCodec_getOutputBuffer(e->codec, (size_t)ob, &sz);
            if (data && info.size > 0 && !(info.flags & 2) && e->started) AMediaMuxer_writeSampleData(e->mux, (size_t)e->track, data, &info);
            bool eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
            AMediaCodec_releaseOutputBuffer(e->codec, (size_t)ob, false);
            if (eos) return true;
            idle = 0;
        } else if (ob == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat *f = AMediaCodec_getOutputFormat(e->codec);
            e->track = (int)AMediaMuxer_addTrack(e->mux, f); AMediaFormat_delete(f);
            AMediaMuxer_start(e->mux); e->started = true;
        } else if (ob == AMEDIACODEC_INFO_TRY_AGAIN_LATER) { if (!wait) return false; idle++; }
    }
    return false;
}
static bool aenc_open(ExportJob *j, char *err, size_t errcap) {
    AEnc *e = calloc(1, sizeof *e);
    e->fd = open(j->path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (e->fd < 0) { snprintf(err, errcap, "cannot write '%s'", j->path); free(e); return false; }
    e->mux = AMediaMuxer_new(e->fd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
    e->codec = AMediaCodec_createEncoderByType("video/avc");
    if (!e->mux || !e->codec) { snprintf(err, errcap, "this phone has no H.264 encoder"); goto fail; }
    AMediaFormat *f = AMediaFormat_new();
    double fps = j->P->fps;
    int br = (int)((double)j->ow * j->oh * fps * (0.04 + 0.16 * j->quality / 100.0));
    if (br > 40000000) br = 40000000; if (br < 400000) br = 400000;
    AMediaFormat_setString(f, "mime", "video/avc");
    AMediaFormat_setInt32(f, "width", j->ow); AMediaFormat_setInt32(f, "height", j->oh);
    AMediaFormat_setInt32(f, "color-format", 21);
    AMediaFormat_setInt32(f, "bitrate", br);
    AMediaFormat_setInt32(f, "frame-rate", (int)(fps + 0.5));
    AMediaFormat_setInt32(f, "i-frame-interval", 1);
    media_status_t st = AMediaCodec_configure(e->codec, f, NULL, NULL, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    AMediaFormat_delete(f);
    if (st != AMEDIA_OK || AMediaCodec_start(e->codec) != AMEDIA_OK) { snprintf(err, errcap, "the H.264 encoder refused %dx%d", j->ow, j->oh); goto fail; }
    j->aenc = e;
    return true;
fail:
    if (e->codec) AMediaCodec_delete(e->codec);
    if (e->mux) AMediaMuxer_delete(e->mux);
    close(e->fd); remove(j->path); free(e);
    return false;
}
static bool aenc_put(ExportJob *j) {
    AEnc *e = j->aenc;
    for (int tries = 0; tries < 2000; tries++) {
        ssize_t ib = AMediaCodec_dequeueInputBuffer(e->codec, 10000);
        if (ib >= 0) {
            size_t cap = 0, need = (size_t)j->ow * j->oh * 3 / 2;
            uint8_t *buf = AMediaCodec_getInputBuffer(e->codec, (size_t)ib, &cap);
            if (!buf || cap < need) { snprintf(j->err, sizeof j->err, "encoder buffer too small (%zu < %zu)", cap, need); return false; }
            rgba_to_nv12(j->rgba, j->ow, j->oh, buf);
            uint64_t pts = (uint64_t)((double)(j->cur - j->from) * 1e6 / j->P->fps);
            AMediaCodec_queueInputBuffer(e->codec, (size_t)ib, 0, need, pts, 0);
            aenc_drain(e, false);
            return true;
        }
        aenc_drain(e, false);
    }
    snprintf(j->err, sizeof j->err, "the encoder stopped responding");
    return false;
}
static void aenc_close(ExportJob *j, bool cancel) {
    AEnc *e = j->aenc;
    if (!e) return;
    if (!cancel && !j->failed) {
        for (int tries = 0; tries < 500; tries++) {
            ssize_t ib = AMediaCodec_dequeueInputBuffer(e->codec, 10000);
            if (ib >= 0) { AMediaCodec_queueInputBuffer(e->codec, (size_t)ib, 0, 0, (uint64_t)((double)(j->to - j->from + 1) * 1e6 / j->P->fps), AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM); break; }
            aenc_drain(e, false);
        }
        aenc_drain(e, true);
    }
    if (e->started) AMediaMuxer_stop(e->mux);
    AMediaCodec_stop(e->codec); AMediaCodec_delete(e->codec); AMediaMuxer_delete(e->mux);
    close(e->fd);
    if (cancel || j->failed || !e->started) remove(j->path);
    free(e); j->aenc = NULL;
}
#endif

ExportJob *export_begin(Project *P, const char *path, VidFmt f, int from, int to, int scale_pct, int quality, char *err, size_t errcap) {
    if (scale_pct < 10) scale_pct = 10;
    if (scale_pct > 200) scale_pct = 200;
    int nf = project_nframes(P);
    if (from < 0) from = 0;
    if (to >= nf) to = nf - 1;
    if (to < from) { snprintf(err, errcap, "empty frame range"); return NULL; }
    ExportJob *j = calloc(1, sizeof *j);
    j->P = P; j->fmt = f; j->from = from; j->to = to; j->cur = from;
    j->ow = (P->width * scale_pct / 100) & ~1; j->oh = (P->height * scale_pct / 100) & ~1;
    if (j->ow < 2) j->ow = 2;
    if (j->oh < 2) j->oh = 2;
    snprintf(j->path, sizeof j->path, "%s", path);
    j->rgba = malloc((size_t)j->ow * j->oh * 4);
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;

    if (export_ffmpeg_ok()) {
        j->use_ff = true; j->quality = quality;
    } else if (f == VF_MP4 && ANDROID_ENC) {
#ifdef PLATFORM_ANDROID
        j->ow = (j->ow + 8) & ~15; j->oh = (j->oh + 8) & ~15;          /* hardware encoders want 16-aligned frames */
        free(j->rgba); j->rgba = malloc((size_t)j->ow * j->oh * 4);
        j->quality = quality;
        if (!aenc_open(j, err, errcap)) { free(j->rgba); free(j); return NULL; }
#endif
    } else if (f == VF_GIF) {
        j->builtin_gif = true;
        if (!msf_gif_begin(&j->gif, j->ow, j->oh)) { snprintf(err, errcap, "gif encoder init failed"); free(j->rgba); free(j); return NULL; }
    } else {
        snprintf(err, errcap, "%s export needs ffmpeg (not found); GIF or PNG frames still work", VIDFMT_NAMES[f]);
        free(j->rgba); free(j);
        return NULL;
    }
    err[0] = 0;
    return j;
}

int export_step(ExportJob *j) {
    if (j->failed) return -1;
    if (j->cur > j->to) return 0;
    Project *P = j->P;
    if (!j->media_ok) { if (!media_project_ready(P)) { usleep(8000); return 1; } j->media_ok = true; }
    if (j->use_ff && !j->pipe && !ff_start(j)) return -1;
    render_comp(P, (float)j->cur / P->fps, &j->fb);
    scale_frame(j, &j->fb);
    if (j->builtin_gif) {
        int cs = (int)floorf(100.0f / P->fps + 0.5f); if (cs < 2) cs = 2;
        int depth = 12;                                    /* msf: 16 is best, lower is faster */
        msf_gif_frame(&j->gif, j->rgba, cs, depth, j->ow * 4);
#ifdef PLATFORM_ANDROID
    } else if (j->aenc) {
        if (!aenc_put(j)) { j->failed = true; return -1; }
#endif
    } else {
        size_t want = (size_t)j->ow * j->oh * 4;
        if (fwrite(j->rgba, 1, want, j->pipe) != want) {
            snprintf(j->err, sizeof j->err, "ffmpeg closed the pipe (bad format/path?)");
            j->failed = true;
            return -1;
        }
    }
    j->cur++;
    return j->cur <= j->to ? 1 : 0;
}

void export_progress(const ExportJob *j, int *cur, int *total) {
    if (cur) *cur = j->cur - j->from;
    if (total) *total = j->to - j->from + 1;
}
const char *export_error(const ExportJob *j) { return j->err; }

void export_end(ExportJob *j, bool cancel) {
    if (!j) return;
#ifdef PLATFORM_ANDROID
    if (j->aenc) aenc_close(j, cancel);
#endif
    if (j->builtin_gif) {
        MsfGifResult r = msf_gif_end(&j->gif);
        if (!cancel && r.data && !j->failed) {
            FILE *f = fopen(j->path, "wb");
            if (f) { fwrite(r.data, 1, r.dataSize, f); fclose(f); }
            else snprintf(j->err, sizeof j->err, "cannot write '%s'", j->path);
        }
        msf_gif_free(r);
    } else if (j->pipe) {
        int rc = pclose(j->pipe);
        if (cancel || j->failed) remove(j->path);
        else if (rc != 0) snprintf(j->err, sizeof j->err, "ffmpeg exited with an error");
    }
    if (cancel) remove(j->path);
    free(j->rgba);
    fbuf_free(&j->fb);
    free(j);
}

bool export_frame_image(Project *P, int frame, const char *path, int quality, char *err, size_t errcap) {
    FBuf fb = { 0 };
    for (int tries = 0; tries < 3000 && !media_project_ready(P); tries++) usleep(10000);   /* wait for video proxies (up to ~30 s) */
    render_comp(P, (float)frame / P->fps, &fb);
    uint8_t *px = malloc((size_t)fb.w * fb.h * 4);
    const char *dot = strrchr(path, '.');
    bool jpg = dot && (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg"));
    fbuf_to_rgba8(&fb, px, jpg);
    int ok = jpg ? stbi_write_jpg(path, fb.w, fb.h, 4, px, quality < 1 ? 90 : quality)
                 : stbi_write_png(path, fb.w, fb.h, 4, px, fb.w * 4);
    free(px); fbuf_free(&fb);
    if (!ok) { snprintf(err, errcap, "cannot write '%s'", path); return false; }
    return true;
}
