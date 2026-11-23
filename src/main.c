// HMY Asteroid Motion (AFX) - mobile-first editor UI (raylib, touch + mouse, portrait + landscape), plus headless --render CLI
#define _GNU_SOURCE
#include "afx.h"
#include "raylib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef PLATFORM_ANDROID
#include <android_native_app_glue.h>
#include <android/configuration.h>
extern struct android_app *GetAndroidApp(void);
#endif

#include "ui_core.inc"
#include "ui_sheets.inc"
#include "ui_fusion.inc"
#include "ui_main.inc"

/* ============================== headless CLI ============================== */
static int cli_render(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: afx --render project.lua out.(png|jpg|gif|mp4|webm|avi) [--frame N] [--scale PCT] [--quality Q]\n"); return 1; }
    const char *in = argv[2], *out = argv[3];
    int frame = 0, scale = 100, quality = 85;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--frame") && i + 1 < argc) frame = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--quality") && i + 1 < argc) quality = atoi(argv[++i]);
    }
    FILE *f = fopen(in, "rb"); if (!f) { fprintf(stderr, "cannot open %s\n", in); return 2; } fclose(f);
    Project *pj = project_new();
    bool ok = project_load_file(pj, in);
    if (!ok) fprintf(stderr, "warning: project loaded with script errors\n");
    char err[256] = { 0 };
    const char *ext = strrchr(out, '.');
    bool img = ext && (!strcasecmp(ext, ".png") || !strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg"));
    int rc = 0;
    if (img) {
        if (!export_frame_image(pj, frame, out, quality, err, sizeof err)) { fprintf(stderr, "%s\n", err); rc = 3; }
        else printf("wrote %s (frame %d, %dx%d)\n", out, frame, pj->width, pj->height);
    } else {
        ExportJob *j = export_begin(pj, out, export_fmt_from_path(out, VF_GIF), 0, project_nframes(pj) - 1, scale, quality, err, sizeof err);
        if (!j) { fprintf(stderr, "%s\n", err); rc = 3; }
        else {
            int r; while ((r = export_step(j)) == 1) {}
            if (r < 0) { fprintf(stderr, "export failed: %s\n", export_error(j)); rc = 4; }
            export_end(j, r < 0);
            if (!rc) printf("wrote %s (%d frames)\n", out, project_nframes(pj));
        }
    }
    project_free(pj);
    return rc;
}


static int cli_convert(int argc, char **argv) {       /* afx --convert in.lua out.lua : load, normalize and save */
    if (argc < 4) { fprintf(stderr, "usage: afx --convert in.lua out.lua\n"); return 1; }
    Project *pj = project_new();
    bool ok = project_load_file(pj, argv[2]);
    bool sv = project_save_file(pj, argv[3]);
    project_free(pj);
    return ok && sv ? 0 : 2;
}

#ifdef PLATFORM_ANDROID
#include <jni.h>
/* ask for read access to the user's videos/photos (runtime permission); NativeActivity has no Java side, so this goes through JNI */
static void android_request_media_access(struct android_app *app) {
    JavaVM *vm = app->activity->vm; JNIEnv *env = NULL;
    if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK || !env) return;
    jobject act = app->activity->clazz;
    jclass cact = (*env)->GetObjectClass(env, act), cver = (*env)->FindClass(env, "android/os/Build$VERSION"), cstr = (*env)->FindClass(env, "java/lang/String");
    if (!cact || !cver || !cstr) { (*env)->ExceptionClear(env); (*vm)->DetachCurrentThread(vm); return; }
    jfieldID fsdk = (*env)->GetStaticFieldID(env, cver, "SDK_INT", "I");
    int sdk = fsdk ? (*env)->GetStaticIntField(env, cver, fsdk) : 0;
    const char *want[2]; int nw = 0;
    if (sdk >= 33) { want[nw++] = "android.permission.READ_MEDIA_VIDEO"; want[nw++] = "android.permission.READ_MEDIA_IMAGES"; }
    else if (sdk >= 23) want[nw++] = "android.permission.READ_EXTERNAL_STORAGE";
    if (nw) {
        jmethodID mcheck = (*env)->GetMethodID(env, cact, "checkSelfPermission", "(Ljava/lang/String;)I");
        jmethodID mreq = (*env)->GetMethodID(env, cact, "requestPermissions", "([Ljava/lang/String;I)V");
        jobjectArray arr = (*env)->NewObjectArray(env, nw, cstr, NULL);
        bool denied = false;
        for (int i = 0; i < nw && mcheck && arr; i++) {
            jstring js = (*env)->NewStringUTF(env, want[i]);
            (*env)->SetObjectArrayElement(env, arr, i, js);
            if ((*env)->CallIntMethod(env, act, mcheck, js) != 0) denied = true;
            (*env)->DeleteLocalRef(env, js);
        }
        if (denied && mreq && arr) (*env)->CallVoidMethod(env, act, mreq, arr, 1001);
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*vm)->DetachCurrentThread(vm);
}
#endif

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "--render")) return cli_render(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "--convert")) return cli_convert(argc, argv);
    const char *ut_script = NULL, *open_arg = NULL; int win_w = 1280, win_h = 720; float dp_arg = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--uitest") && i + 1 < argc) ut_script = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &win_w, &win_h);
        else if (!strcmp(argv[i], "--dp") && i + 1 < argc) dp_arg = (float)atof(argv[++i]);
        else if (argv[i][0] != '-') open_arg = argv[i];
    }
    SetTraceLogLevel(LOG_WARNING);
#ifdef PLATFORM_ANDROID
    SetConfigFlags(FLAG_VSYNC_HINT);
    InitWindow(0, 0, "Asteroid Fusion");      /* 0x0 = use the whole display (a fixed size would letterbox a desktop-sized screen) */
#else
    SetConfigFlags(FLAG_VSYNC_HINT | (ut_script ? 0 : FLAG_WINDOW_RESIZABLE));
    InitWindow(win_w, win_h, "Asteroid Fusion");
#endif
    SetExitKey(KEY_NULL);
    SetTargetFPS(60);
    SW = GetScreenWidth(); SH = GetScreenHeight();

    float dp = 1.0f;
#ifdef PLATFORM_ANDROID
    {
        touch_mode = true;
        struct android_app *app = GetAndroidApp();
        int dens = app->config ? AConfiguration_getDensity(app->config) : 0;
        float short_px = (float)(SW < SH ? SW : SH);
        dp = (dens > 0 && dens < 0xfff0) ? dens / 160.0f : short_px / 360.0f;
        if (short_px / dp < 300) dp = short_px / 360.0f;           /* never let the layout get narrower than ~300dp */
        const char *base = app->activity->externalDataPath ? app->activity->externalDataPath : app->activity->internalDataPath;
        snprintf(proj_dir, sizeof proj_dir, "%s/projects", base); mkdir(proj_dir, 0755);
        static char root[PATH_LEN]; char d[PATH_LEN];
        snprintf(root, sizeof root, "%s/assets", app->activity->internalDataPath); mkdir(root, 0755);
        snprintf(d, sizeof d, "%s/fonts", root); mkdir(d, 0755);
        const char *fn[3] = { "DejaVuSans.ttf", "DejaVuSans-Bold.ttf", "DejaVuSansMono.ttf" };
        for (int i = 0; i < 3; i++) {
            char a[96], o[PATH_LEN]; int n = 0;
            snprintf(a, sizeof a, "fonts/%s", fn[i]); snprintf(o, sizeof o, "%s/fonts/%s", root, fn[i]);
            unsigned char *dt = LoadFileData(a, &n);
            if (dt) { SaveFileData(o, dt, n); UnloadFileData(dt); }
        }
        afx_asset_root = root;
        snprintf(afx_cache_dir, sizeof afx_cache_dir, "%s/videocache", app->activity->internalDataPath); mkdir(afx_cache_dir, 0755);
        android_request_media_access(app);
    }
#else
    {
        float short_px = (float)(SW < SH ? SW : SH);
        dp = short_px / 480.0f; if (dp < 1.0f) dp = 1.0f; if (dp > 2.5f) dp = 2.5f;
        if (ut_script) snprintf(proj_dir, sizeof proj_dir, "/tmp/afx_uitest_projects"); else snprintf(proj_dir, sizeof proj_dir, "projects");
        mkdir(proj_dir, 0755);
    }
#endif
    if (getenv("AFX_DP")) dp = (float)atof(getenv("AFX_DP"));
    if (dp_arg > 0) dp = dp_arg;
    set_scale(dp);
    load_fonts();

    afx_log_fn = log_cb;
    ffmpeg_ok = export_ffmpeg_ok();
    P = project_new();
    if (ut_script) {
        FILE *f = fopen(ut_script, "rb");
        if (!f) { fprintf(stderr, "cannot open %s\n", ut_script); return 2; }
        char line[512]; ut_lines = calloc(4096, sizeof(char *));
        while (fgets(line, sizeof line, f) && ut_nlines < 4096) { line[strcspn(line, "\r\n")] = 0; ut_lines[ut_nlines++] = strdup(line); }
        fclose(f);
        ut_on = true; sim.on = true; touch_mode = true; sim.pos = (Vector2){ -1, -1 };
        new_project(2);
    } else if (open_arg) { project_load_file(P, open_arg); reset_view(); }
    else {
        scan_files();
        if (!nfiles || !open_project(files[0])) new_project(2);
    }
    afx_log(0, "Asteroid Fusion %s - Lua scripting ready (type help())", AFX_VERSION);

    while (!WindowShouldClose() && !ut_quit) {
        /* keep the UI scale if the user resizes the desktop window; fonts follow dp, not window size */
        ui_frame();
    }
    if (job) export_end(job, true);
    if (P->filepath[0] && P->rev != saved_rev) save_project();
    project_free(P);
    CloseWindow();
    return 0;
}
