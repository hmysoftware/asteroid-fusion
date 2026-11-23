// Asteroid Fusion (AFX engine) - core header
#ifndef AFX_H
#define AFX_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define AFX_VERSION "2.0.0"
#define AFX_NAME "Asteroid Fusion"
#ifndef AFX_ASSET_DIR
#define AFX_ASSET_DIR "assets"   /* fallback asset dir (CMake overrides it on desktop) */
#endif
#define MAX_KEYS   32
#define MAX_LAYERS 128
#define MAX_FX     8
#define NAME_LEN   48
#define PATH_LEN   256
#define TEXT_LEN   256

typedef struct lua_State lua_State;

typedef struct { int x0, y0, x1, y1; } IRect;          /* [x0,x1) x [y0,y1) */
typedef struct Graph Graph;

/* ---------- animation ---------- */
enum { EASE_LINEAR, EASE_IN, EASE_OUT, EASE_INOUT, EASE_HOLD, EASE_COUNT };
extern const char *EASE_NAMES[EASE_COUNT];

typedef struct { float t, v; uint8_t ease; } AKey;

#define EXPR_NONE  (-1)   /* no expression / not compiled yet */
#define EXPR_BAD   (-2)   /* compile or runtime error: disabled */
typedef struct {
    float base;
    int   nkeys;
    AKey  keys[MAX_KEYS];
    char *expr;           /* expression source (malloc) or NULL */
    int   ref;            /* lua registry ref of compiled expr / function */
    bool  fn;             /* ref is a user lua function (not serializable) */
} Prop;

/* layer property slots */
enum { P_X, P_Y, P_SX, P_SY, P_ROT, P_OPA, P_AX, P_AY, P_W, P_H, P_A, P_B, P_C, P_COUNT };
extern const char *PROP_NAMES[P_COUNT];

typedef enum { L_SOLID, L_SHAPE, L_TEXT, L_SVG, L_IMAGE, L_ASTEROID, L_STARS, L_ADJUST, L_VIDEO, L_TYPE_COUNT } LayerType;
extern const char *LAYER_TYPE_NAMES[L_TYPE_COUNT];
enum { SH_RECT, SH_ELLIPSE, SH_POLYGON, SH_STAR, SH_COUNT };
extern const char *SHAPE_NAMES[SH_COUNT];
enum { BM_NORMAL, BM_ADD, BM_MULTIPLY, BM_SCREEN, BM_COUNT };
extern const char *BLEND_NAMES[BM_COUNT];

/* ---------- effects ---------- */
enum { FX_BLUR, FX_GLOW, FX_BRIGHT, FX_TINT, FX_HUE, FX_INVERT, FX_NOISE, FX_PIXELATE,
       FX_RGBSPLIT, FX_VIGNETTE, FX_SCANLINES, FX_WAVE, FX_POSTERIZE, FX_COUNT };
typedef struct { const char *name; int np; const char *pn[3]; float def[3]; float mn[3], mx[3]; float step[3]; } FxDef;
extern const FxDef FX_DEFS[FX_COUNT];
typedef struct { uint8_t type; bool on; Prop p[3]; uint8_t color[4]; } Effect;

typedef struct FBuf { int w, h; float *p; } FBuf;     /* premultiplied float RGBA */

/* ---------- bitmap cache (premultiplied float RGBA) ---------- */
typedef struct { int w, h; float unit; float *px; } Bmp;

typedef struct {
    int  uid;
    char name[NAME_LEN];
    LayerType type;
    bool visible, scripted;
    float tin, tout;            /* tout < 0 : comp end */
    uint8_t blend, shape;
    uint8_t fill[4], stroke[4];
    char text[TEXT_LEN];
    char path[PATH_LEN];
    Prop p[P_COUNT];
    int nfx;
    Effect fx[MAX_FX];
    Bmp *cache; char cache_key[PATH_LEN + 64];
    float trim, speed;          /* video: source start (s) and playback speed */
    float vol;                  /* video: audio volume 0..1 (export mix) */
    uint8_t fitm;               /* video/image fit: 0 fill (cover), 1 fit (contain) */
    Graph *g;                   /* Fusion node graph (NULL = plain effect chain) */
} Layer;


typedef struct Project {
    char name[NAME_LEN];
    int width, height;
    float fps, duration;
    uint8_t bg[4];
    int nlayers;
    Layer *layers;              /* [0] = bottom, render order ascending */
    int next_uid;
    lua_State *L;
    char *user_code;            /* text after the --@user marker */
    char filepath[PATH_LEN];
    int nhooks; int hooks[8];
    bool in_user;               /* running user code: new layers are "scripted" */
    int lossy;                  /* count of things that cannot be serialized */
    float cur_t; int cur_frame; /* time being rendered (for scripts) */
    FBuf lb, t1, t2;            /* scratch buffers */
    IRect lbd;                  /* region of lb that may be non-zero */
    float rs;                   /* render scale of the frame being produced (1 = full) */
    int rev;                    /* bumped on any change */
} Project;


/* ---------- Fusion node graph (per clip) ---------- */
enum { N_IN, N_OUT, N_GEN, N_FX, N_XFORM, N_MERGE, N_KIND_COUNT };
extern const char *NODE_KIND_NAMES[N_KIND_COUNT];
#define MAX_NODES 24
#define NODE_NP 5                    /* transform: x y sx sy rot   merge: opacity ox oy */
typedef struct Node {
    uint8_t kind, blend; bool on;
    int uid; char name[NAME_LEN];
    float gx, gy;                    /* position on the graph canvas */
    int in[2];                       /* input node uids (0 = none) */
    Layer *gen;                      /* N_GEN: embedded generator (type, colors, text, props) */
    Effect fx;                       /* N_FX */
    Prop mp[NODE_NP];                /* N_XFORM / N_MERGE */
    FBuf *res; IRect rbb; int stamp; int uses;   /* runtime: per-frame memo (buffer comes from the render pool) */
} Node;
struct Graph { int n; Node nd[MAX_NODES]; int next_uid; int stamp; };

/* ---------- log ---------- */
typedef void (*LogFn)(int level, const char *msg);  /* level 0 info, 1 error */
extern LogFn afx_log_fn;
void afx_log(int level, const char *fmt, ...);

/* ---------- core.c ---------- */
Project *project_new(void);
void     project_free(Project *P);
void     project_reset(Project *P);                  /* empty comp, fresh lua state */
int      project_nframes(const Project *P);
float    project_layer_out(const Project *P, const Layer *l);
Layer   *project_add(Project *P, LayerType t, const char *name);
Layer   *project_find_uid(Project *P, int uid);
int      project_index_of(Project *P, const Layer *l);
void     project_remove(Project *P, int idx);
int      project_duplicate(Project *P, int idx);
int      project_move(Project *P, int idx, int delta);   /* returns new index */
void     layer_init(Project *P, Layer *l, LayerType t);
void     layer_release_all(Project *P, Layer *l);        /* free props, fx, cache, graph (struct itself is not freed) */
void     layer_free_cache(Layer *l);
void     prop_free(Project *P, Prop *p);
void     prop_init(Prop *p, float v);
void     prop_set_expr(Project *P, Prop *p, const char *src);
float    prop_keyed(const Prop *p, float t);
float    prop_eval(Project *P, Layer *l, Prop *p, int pid, float t);
int      prop_key_at(const Prop *p, float t, float eps);
void     prop_set_key(Prop *p, float t, float v, int ease);
void     prop_del_key(Prop *p, int i);
Prop    *layer_prop(Layer *l, const char *name);          /* name or alias or "fx.param" */
int      layer_add_fx(Project *P, Layer *l, int type);
void     layer_del_fx(Project *P, Layer *l, int i);
float    ease_apply(int ease, float u);
const char *prop_label(const Layer *l, int pid);          /* NULL if prop unused by layer */
void     fx_defaults(Effect *e, int type);
Bmp     *bmp_new(int w, int h);
void     bmp_free(Bmp *b);
FBuf     fbuf_make(int w, int h);
void     fbuf_free(FBuf *b);

/* ---------- graph.c ---------- */
const char *node_kind_label(const Node *n);
int      node_nin(const Node *n);
Graph   *graph_new(void);
Graph   *graph_clone(Project *P, const Graph *src);
void     graph_free(Project *P, Graph *g);
Node    *graph_find(Graph *g, int uid);
int      graph_index(const Graph *g, int uid);
Node    *graph_add(Project *P, Graph *g, int kind, int sub);      /* sub: fx type (N_FX) or layer type (N_GEN) */
bool     graph_connect(Graph *g, int from_uid, int to_uid, int slot);
void     graph_disconnect(Graph *g, int to_uid, int slot);
void     graph_remove(Project *P, Graph *g, int uid);
void     graph_autolayout(Graph *g);
int      graph_out_uid(const Graph *g);
Prop    *node_prop(Node *n, const char *name);
const char *node_prop_label(const Node *n, int k);              /* NULL if slot unused */
void     node_defaults(Project *P, Node *n);

/* ---------- media.c (video import = proxy frame sequence) ---------- */
typedef struct Media Media;
extern char afx_cache_dir[PATH_LEN];
Media   *media_get(const char *path, bool create);
bool     media_project_ready(Project *P);                       /* starts every video import; true once all are ready or failed */
void     media_retry(Media *m);                                 /* after an error: allow media_start again */
void     media_start(Media *m);                                 /* begin background import if needed */
int      media_state(const Media *m);                           /* 0 idle, 1 importing, 2 ready, 3 error */
int      media_progress(const Media *m, int *total);            /* frames extracted so far */
void     media_info(const Media *m, int *w, int *h, float *fps, float *dur);
const char *media_error(const Media *m);
const uint8_t *media_frame(Media *m, int idx, int *w, int *h);  /* RGBA8, valid until next call; idx is clamped */
bool     media_platform_extract(const char *path, const char *dir, int max_short, Media *m);   /* android: MediaCodec decode; desktop: ffmpeg */
const char *media_path(const Media *m);
bool     media_has_audio(const Media *m);
bool     media_thumb(Media *m, int idx, uint8_t *dst, int tw, int th);   /* RGBA thumbnail (cover-cropped), does not disturb the playback cache */
void     media_hint(Media *m, int idx);                         /* tell the prefetch thread which frame is wanted next */
void     media_cache_clear(void);                               /* delete every proxy frame sequence */
long     media_cache_bytes(void);
void     media_shutdown(void);
bool     media_is_video_ext(const char *name);
bool     media_is_image_ext(const char *name);

/* ---------- video import limits ---------- */
#define PROXY_SHORT_DEFAULT 540        /* short side of the proxy frames (px) */
#define PROXY_FPS_MAX 30

/* ---------- render.c ---------- */
void render_comp_scaled(Project *P, float t, FBuf *out, float rs);   /* out is (width*rs) x (height*rs) */
void render_free_pool(void);
void render_comp(Project *P, float t, FBuf *out);
void par_rows(int rows, void (*fn)(int, int, void *), void *ctx);
void fbuf_to_rgba8(const FBuf *b, uint8_t *dst, bool flatten_black);
const char *afx_find_asset(const char *rel, char *buf, size_t cap);
extern const char *afx_asset_root;   /* optional override dir containing fonts/ (Android: extracted assets) */

/* ---------- script.c ---------- */
bool project_load_text(Project *P, const char *text, const char *origin);
bool project_load_file(Project *P, const char *path);
char *project_serialize(Project *P);                      /* malloc'ed lua text */
bool project_save_file(Project *P, const char *path);
bool script_run_user(Project *P);                         /* re-run user code */
bool script_exec(Project *P, const char *code, const char *chunk);   /* console */
void script_call_hooks(Project *P, float t, int frame);
void script_free_ref(Project *P, int ref);
lua_State *script_new_state(Project *P);
int      afx_pcall(lua_State *L, int nargs, int nres, double budget_s);   /* lua_pcall with a wall-clock budget (stops infinite loops) */
void     project_cancel_pending(void);

/* ---------- export.c ---------- */
typedef enum { VF_GIF, VF_MP4, VF_WEBM, VF_AVI, VF_COUNT } VidFmt;
extern const char *VIDFMT_NAMES[VF_COUNT];
extern const char *VIDFMT_EXT[VF_COUNT];
typedef struct ExportJob ExportJob;
bool export_ffmpeg_ok(void);
ExportJob *export_begin(Project *P, const char *path, VidFmt f, int from, int to, int scale_pct, int quality, char *err, size_t errcap);
int  export_step(ExportJob *j);                           /* 1 more, 0 done, -1 error */
void export_progress(const ExportJob *j, int *cur, int *total);
const char *export_error(const ExportJob *j);
void export_end(ExportJob *j, bool cancel);
bool export_frame_image(Project *P, int frame, const char *path, int quality, char *err, size_t errcap);
VidFmt export_fmt_from_path(const char *path, VidFmt def);

#endif
