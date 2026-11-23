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
    const char *want[3]; int nw = 0;
    if (sdk >= 33) { want[nw++] = "android.permission.READ_MEDIA_VIDEO"; want[nw++] = "android.permission.READ_MEDIA_IMAGES"; }
    else if (sdk >= 23) {
        want[nw++] = "android.permission.READ_EXTERNAL_STORAGE";
        if (sdk < 29) want[nw++] = "android.permission.WRITE_EXTERNAL_STORAGE";      /* only Android 6-9 need it to write Movies/ directly */
    }
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


/* ---------- publish exports to the gallery ---------- */
#ifdef PLATFORM_ANDROID
#define JX(call) ((call), (*env)->ExceptionCheck(env) ? ((*env)->ExceptionClear(env), 1) : 0)
static bool copy_to_movies_legacy(const char *path, const char *rel, const char *name, char *where, size_t cap) {
    char dir[PATH_LEN], dst[PATH_LEN * 2];
    snprintf(dir, sizeof dir, "/storage/emulated/0/%s", rel);
    char tmp[PATH_LEN]; snprintf(tmp, sizeof tmp, "/storage/emulated/0/%s", rel);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(tmp, 0775); *p = '/'; }
    mkdir(dir, 0775);
    snprintf(dst, sizeof dst, "%s/%s", dir, name);
    FILE *fi = fopen(path, "rb"), *fo = fopen(dst, "wb");
    if (!fi || !fo) { if (fi) fclose(fi); if (fo) fclose(fo); return false; }
    static char buf[1 << 16]; size_t n; bool ok = true;
    while ((n = fread(buf, 1, sizeof buf, fi)) > 0) if (fwrite(buf, 1, n, fo) != n) { ok = false; break; }
    fclose(fi); if (fclose(fo) != 0) ok = false;
    if (ok) snprintf(where, cap, "%s/%s", rel, name); else remove(dst);
    return ok;
}

bool platform_publish_file(const char *path, char *where, size_t cap) {
    struct android_app *app = GetAndroidApp();
    const char *name = strrchr(path, '/'); name = name ? name + 1 : path;
    const char *ext = strrchr(name, '.'); ext = ext ? ext + 1 : "";
    bool video = !strcasecmp(ext, "mp4") || !strcasecmp(ext, "webm") || !strcasecmp(ext, "avi");
    const char *mime = !strcasecmp(ext, "mp4") ? "video/mp4" : !strcasecmp(ext, "webm") ? "video/webm" : !strcasecmp(ext, "avi") ? "video/x-msvideo" :
                       !strcasecmp(ext, "gif") ? "image/gif" : !strcasecmp(ext, "png") ? "image/png" : "image/jpeg";
    const char *rel = video ? "Movies/AsteroidFusion" : "Pictures/AsteroidFusion";
    JavaVM *vm = app->activity->vm; JNIEnv *env = NULL; bool attached = false, ok = false;
    jint gs = (*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6);
    if (gs == JNI_EDETACHED) { if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) return false; attached = true; }
    else if (gs != JNI_OK || !env) return false;

    jclass cver = (*env)->FindClass(env, "android/os/Build$VERSION");
    jfieldID fsdk = cver ? (*env)->GetStaticFieldID(env, cver, "SDK_INT", "I") : NULL;
    int sdk = fsdk ? (*env)->GetStaticIntField(env, cver, fsdk) : 0;
    (*env)->ExceptionClear(env);
    if (sdk < 29) { ok = copy_to_movies_legacy(path, rel, name, where, cap); goto done; }

    {
        jobject ctx = app->activity->clazz;
        jclass cCtx = (*env)->FindClass(env, "android/content/Context");
        jclass cCR = (*env)->FindClass(env, "android/content/ContentResolver");
        jclass cCV = (*env)->FindClass(env, "android/content/ContentValues");
        jclass cInt = (*env)->FindClass(env, "java/lang/Integer");
        jclass cOS = (*env)->FindClass(env, "java/io/OutputStream");
        jclass cColl = (*env)->FindClass(env, video ? "android/provider/MediaStore$Video$Media" : "android/provider/MediaStore$Images$Media");
        if ((*env)->ExceptionCheck(env) || !cCtx || !cCR || !cCV || !cInt || !cOS || !cColl) { (*env)->ExceptionClear(env); goto done; }
        jmethodID mGetCR = (*env)->GetMethodID(env, cCtx, "getContentResolver", "()Landroid/content/ContentResolver;");
        jmethodID mInsert = (*env)->GetMethodID(env, cCR, "insert", "(Landroid/net/Uri;Landroid/content/ContentValues;)Landroid/net/Uri;");
        jmethodID mOpen = (*env)->GetMethodID(env, cCR, "openOutputStream", "(Landroid/net/Uri;)Ljava/io/OutputStream;");
        jmethodID mUpdate = (*env)->GetMethodID(env, cCR, "update", "(Landroid/net/Uri;Landroid/content/ContentValues;Ljava/lang/String;[Ljava/lang/String;)I");
        jmethodID mDelete = (*env)->GetMethodID(env, cCR, "delete", "(Landroid/net/Uri;Ljava/lang/String;[Ljava/lang/String;)I");
        jmethodID mCVnew = (*env)->GetMethodID(env, cCV, "<init>", "()V");
        jmethodID mPutS = (*env)->GetMethodID(env, cCV, "put", "(Ljava/lang/String;Ljava/lang/String;)V");
        jmethodID mPutI = (*env)->GetMethodID(env, cCV, "put", "(Ljava/lang/String;Ljava/lang/Integer;)V");
        jmethodID mValueOf = (*env)->GetStaticMethodID(env, cInt, "valueOf", "(I)Ljava/lang/Integer;");
        jmethodID mWrite = (*env)->GetMethodID(env, cOS, "write", "([BII)V");
        jmethodID mClose = (*env)->GetMethodID(env, cOS, "close", "()V");
        jfieldID fUri = (*env)->GetStaticFieldID(env, cColl, "EXTERNAL_CONTENT_URI", "Landroid/net/Uri;");
        if ((*env)->ExceptionCheck(env) || !mGetCR || !mInsert || !mOpen || !mUpdate || !mDelete || !mCVnew || !mPutS || !mPutI || !mValueOf || !mWrite || !mClose || !fUri) { (*env)->ExceptionClear(env); goto done; }

        jobject cr = (*env)->CallObjectMethod(env, ctx, mGetCR);
        jobject coll = (*env)->GetStaticObjectField(env, cColl, fUri);
        jobject cv = (*env)->NewObject(env, cCV, mCVnew);
        if ((*env)->ExceptionCheck(env) || !cr || !coll || !cv) { (*env)->ExceptionClear(env); goto done; }
        char relpath[96]; snprintf(relpath, sizeof relpath, "%s/", rel);
        jstring k1 = (*env)->NewStringUTF(env, "_display_name"), v1 = (*env)->NewStringUTF(env, name);
        jstring k2 = (*env)->NewStringUTF(env, "mime_type"), v2 = (*env)->NewStringUTF(env, mime);
        jstring k3 = (*env)->NewStringUTF(env, "relative_path"), v3 = (*env)->NewStringUTF(env, relpath);
        jstring k4 = (*env)->NewStringUTF(env, "is_pending");
        jobject one = (*env)->CallStaticObjectMethod(env, cInt, mValueOf, 1), zero = (*env)->CallStaticObjectMethod(env, cInt, mValueOf, 0);
        (*env)->CallVoidMethod(env, cv, mPutS, k1, v1); (*env)->CallVoidMethod(env, cv, mPutS, k2, v2);
        (*env)->CallVoidMethod(env, cv, mPutS, k3, v3); (*env)->CallVoidMethod(env, cv, mPutI, k4, one);
        if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); goto done; }

        jobject uri = (*env)->CallObjectMethod(env, cr, mInsert, coll, cv);
        if ((*env)->ExceptionCheck(env) || !uri) { (*env)->ExceptionClear(env); goto done; }
        jobject os = (*env)->CallObjectMethod(env, cr, mOpen, uri);
        bool wrote = false;
        if (!(*env)->ExceptionCheck(env) && os) {
            FILE *fi = fopen(path, "rb");
            jbyteArray arr = (*env)->NewByteArray(env, 1 << 18);
            static unsigned char buf[1 << 18];
            if (fi && arr) {
                size_t n; wrote = true;
                while (wrote && (n = fread(buf, 1, sizeof buf, fi)) > 0) {
                    (*env)->SetByteArrayRegion(env, arr, 0, (jsize)n, (const jbyte *)buf);
                    (*env)->CallVoidMethod(env, os, mWrite, arr, 0, (jint)n);
                    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); wrote = false; }
                }
            }
            if (fi) fclose(fi);
            (*env)->CallVoidMethod(env, os, mClose);
            if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); wrote = false; }
        } else (*env)->ExceptionClear(env);
        if (wrote) {
            jobject cv2 = (*env)->NewObject(env, cCV, mCVnew);
            (*env)->CallVoidMethod(env, cv2, mPutI, k4, zero);
            (*env)->CallIntMethod(env, cr, mUpdate, uri, cv2, NULL, NULL);
            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
            snprintf(where, cap, "%s/%s", rel, name);
            ok = true;
        } else {
            (*env)->CallIntMethod(env, cr, mDelete, uri, NULL, NULL);
            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        }
    }
done:
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (attached) (*vm)->DetachCurrentThread(vm);
    if (ok) remove(path);                  /* the gallery copy is the export now; do not keep a second one in the app folder */
    return ok;
}
#else
bool platform_publish_file(const char *path, char *where, size_t cap) { (void)path; (void)where; (void)cap; return false; }
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
            if (dt) {
                FILE *fo = fopen(o, "wb");           /* plain libc: raylib's SaveFileData() prepends internalDataPath to the name */
                if (fo) { fwrite(dt, 1, (size_t)n, fo); fclose(fo); }
                UnloadFileData(dt);
            }
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
