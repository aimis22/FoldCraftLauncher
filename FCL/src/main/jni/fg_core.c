/*
 * fcl_framegen - M0 阶段实现
 *
 * M0 的目标只有一个：测量「采集 + 额外一次全屏呈现」的固定开销。
 * 不插帧，不合成，只做：
 *   1) 从默认帧缓冲拷出颜色 + 深度
 *   2) 在一次正常 swap 之后，把这张图再 blit 回去并请求第二次 swap
 *
 * 如果这个开销 > 2ms，后面的算法全部没有意义，可以就地放弃。
 */

#include <dlfcn.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include <android/log.h>
#include <GLES3/gl3.h>

#include "fg_api.h"
#include "fg_config.h"
#include "fg_internal.h"

#define TAG "FclFrameGen"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#ifndef GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS
#define GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS 0x90EB
#endif

/* ---------------- GL 函数指针（避免链接期依赖具体 libGL） ---------------- */

typedef const GLubyte *(*PFN_GetString)(GLenum);
typedef void          (*PFN_GetIntegerv)(GLenum, GLint *);
typedef GLenum        (*PFN_GetError)(void);
typedef void          (*PFN_GenFramebuffers)(GLsizei, GLuint *);
typedef void          (*PFN_BindFramebuffer)(GLenum, GLuint);
typedef void          (*PFN_DeleteFramebuffers)(GLsizei, const GLuint *);
typedef void          (*PFN_FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum        (*PFN_CheckFramebufferStatus)(GLenum);
typedef void          (*PFN_GenTextures)(GLsizei, GLuint *);
typedef void          (*PFN_BindTexture)(GLenum, GLuint);
typedef void          (*PFN_ActiveTexture)(GLenum);
typedef void          (*PFN_DeleteTextures)(GLsizei, const GLuint *);
typedef void          (*PFN_TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint,
                                        GLenum, GLenum, const void *);
typedef void          (*PFN_TexParameteri)(GLenum, GLenum, GLint);
typedef void          (*PFN_CopyTexSubImage2D)(GLenum, GLint, GLint, GLint, GLint, GLint,
                                               GLsizei, GLsizei);
typedef void          (*PFN_BlitFramebuffer)(GLint, GLint, GLint, GLint,
                                             GLint, GLint, GLint, GLint, GLbitfield, GLenum);
typedef void          (*PFN_Finish)(void);
typedef void          (*PFN_Enable)(GLenum);
typedef void          (*PFN_Disable)(GLenum);
typedef GLboolean     (*PFN_IsEnabled)(GLenum);

static struct {
    PFN_GetString              GetString;
    PFN_GetIntegerv            GetIntegerv;
    PFN_GetError               GetError;
    PFN_GenFramebuffers        GenFramebuffers;
    PFN_BindFramebuffer        BindFramebuffer;
    PFN_DeleteFramebuffers     DeleteFramebuffers;
    PFN_FramebufferTexture2D   FramebufferTexture2D;
    PFN_CheckFramebufferStatus CheckFramebufferStatus;
    PFN_GenTextures            GenTextures;
    PFN_BindTexture            BindTexture;
    PFN_ActiveTexture          ActiveTexture;
    PFN_DeleteTextures         DeleteTextures;
    PFN_TexImage2D             TexImage2D;
    PFN_TexParameteri          TexParameteri;
    PFN_CopyTexSubImage2D      CopyTexSubImage2D;
    PFN_BlitFramebuffer        BlitFramebuffer;
    PFN_Finish                 Finish;
    PFN_Enable                 Enable;
    PFN_Disable                Disable;
    PFN_IsEnabled              IsEnabled;
} gl;

/*
 * 优先从全局命名空间取符号 —— 这样拿到的是「当前真正在用的」实现，
 * MG 走 ANGLE 时也能命中它自己加载的那套 libGLESv2。
 * 取不到再回落到系统 libGLESv2.so。
 */
static void *gles_sym(const char *name) {
    void *p = dlsym(RTLD_DEFAULT, name);
    if (p) return p;
    static void *handle = NULL;
    if (!handle) handle = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    return handle ? dlsym(handle, name) : NULL;
}

#define RESOLVE(field, type, name)                     \
    do {                                               \
        gl.field = (type) gles_sym(name);              \
        if (!gl.field) { LOGE("missing GL symbol: %s", name); return -1; } \
    } while (0)

/* ---------------- GL 状态保存 / 恢复 ----------------
 * 我们在 swap 时刻插入自己的 GL 调用，必须原样还回去。
 * 不恢复的话会污染游戏的贴图绑定 / FBO 绑定 / 活动纹理单元，
 * 表现是偶发的贴图错乱，而这类 bug 会让所有性能测量失去意义。
 */
typedef struct {
    GLint readFbo, drawFbo, activeTex, texBinding;
} FgGlState;

static void fg_state_save(FgGlState *s) {
    gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &s->readFbo);
    gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s->drawFbo);
    gl.GetIntegerv(GL_ACTIVE_TEXTURE, &s->activeTex);
    gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &s->texBinding);
}

static void fg_state_restore(const FgGlState *s) {
    gl.ActiveTexture((GLenum) s->activeTex);
    gl.BindTexture(GL_TEXTURE_2D, (GLuint) s->texBinding);
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint) s->readFbo);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint) s->drawFbo);
}

/* ---------------- 状态 ---------------- */

static int      g_inited;
static int      g_W, g_H;
static GLuint   g_fbo, g_colorTex, g_depthTex;
static int      g_hasBlit;        /* glBlitFramebuffer 可用 */
static int      g_depthBlitOk;    /* 深度 blit 实测是否成功 */
static GLint    g_maxColorAtt;
static GLint    g_maxComputeWG;
static char     g_probe[768];

static long long g_frames;
static long long g_capNs;      /* 采集：命令提交耗时（CPU 侧） */
static long long g_extraNs;    /* 额外呈现：命令提交耗时（CPU 侧） */
static long long g_capGpuNs;   /* 采集：GPU 真正做完（含 glFinish） */
static long long g_extraGpuNs; /* 额外呈现：同上 */
static int       g_samples;
static long long g_skipped;   /* 因默认帧缓冲未归位而跳过的帧数 */

static long long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* ---------------- 资源 ---------------- */

static void release_targets(void) {
    if (g_fbo)      { gl.DeleteFramebuffers(1, &g_fbo);      g_fbo = 0; }
    if (g_colorTex) { gl.DeleteTextures(2, (GLuint[]){ g_colorTex, g_depthTex }); }
    g_colorTex = g_depthTex = 0;
    g_W = g_H = 0;
}

static int create_targets(int w, int h) {
    FgGlState s;
    fg_state_save(&s);

    GLuint tex[2] = { 0, 0 };
    gl.GenTextures(2, tex);
    g_colorTex = tex[0];
    g_depthTex = tex[1];

    gl.ActiveTexture(GL_TEXTURE0);

    gl.BindTexture(GL_TEXTURE_2D, g_colorTex);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    /*
     * 深度格式用 DEPTH24_STENCIL8：这是绝大多数 Android 默认帧缓冲的实际格式。
     * 深度 blit 要求源 / 目的格式一致，用别的组合在部分驱动上会直接 FAIL。
     */
    gl.BindTexture(GL_TEXTURE_2D, g_depthTex);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, w, h, 0,
                  GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    gl.GenFramebuffers(1, &g_fbo);
    gl.BindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                            GL_TEXTURE_2D, g_colorTex, 0);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                            GL_TEXTURE_2D, g_depthTex, 0);

    GLenum st = gl.CheckFramebufferStatus(GL_FRAMEBUFFER);

    fg_state_restore(&s);

    if (st != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("capture FBO incomplete: 0x%04x", st);
        release_targets();
        return -1;
    }

    g_W = w;
    g_H = h;
    LOGI("capture targets ready: %dx%d", w, h);
    return 0;
}

/* ---------------- 对外接口 ---------------- */

int fcl_fg_abi_version(void) { return FCL_FG_ABI_VERSION; }

int fcl_fg_init(void) {
    if (g_inited) return 0;

    RESOLVE(GetString,              PFN_GetString,              "glGetString");
    RESOLVE(GetIntegerv,            PFN_GetIntegerv,            "glGetIntegerv");
    RESOLVE(GetError,               PFN_GetError,               "glGetError");
    RESOLVE(GenFramebuffers,        PFN_GenFramebuffers,        "glGenFramebuffers");
    RESOLVE(BindFramebuffer,        PFN_BindFramebuffer,        "glBindFramebuffer");
    RESOLVE(DeleteFramebuffers,     PFN_DeleteFramebuffers,     "glDeleteFramebuffers");
    RESOLVE(FramebufferTexture2D,   PFN_FramebufferTexture2D,   "glFramebufferTexture2D");
    RESOLVE(CheckFramebufferStatus, PFN_CheckFramebufferStatus, "glCheckFramebufferStatus");
    RESOLVE(GenTextures,            PFN_GenTextures,            "glGenTextures");
    RESOLVE(BindTexture,            PFN_BindTexture,            "glBindTexture");
    RESOLVE(ActiveTexture,          PFN_ActiveTexture,          "glActiveTexture");
    RESOLVE(DeleteTextures,         PFN_DeleteTextures,         "glDeleteTextures");
    RESOLVE(TexImage2D,             PFN_TexImage2D,             "glTexImage2D");
    RESOLVE(TexParameteri,          PFN_TexParameteri,          "glTexParameteri");
    RESOLVE(CopyTexSubImage2D,      PFN_CopyTexSubImage2D,      "glCopyTexSubImage2D");
    RESOLVE(Finish,                 PFN_Finish,                 "glFinish");
    RESOLVE(Enable,                 PFN_Enable,                 "glEnable");
    RESOLVE(Disable,                PFN_Disable,                "glDisable");
    RESOLVE(IsEnabled,              PFN_IsEnabled,              "glIsEnabled");

    gl.BlitFramebuffer = (PFN_BlitFramebuffer) gles_sym("glBlitFramebuffer");
    g_hasBlit = (gl.BlitFramebuffer != NULL);

    const char *ver  = (const char *) gl.GetString(GL_VERSION);
    const char *rend = (const char *) gl.GetString(GL_RENDERER);
    const char *glsl = (const char *) gl.GetString(GL_SHADING_LANGUAGE_VERSION);

    gl.GetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &g_maxColorAtt);
    GLenum e = gl.GetError();
    if (e != GL_NO_ERROR) g_maxColorAtt = 1;

    g_maxComputeWG = 0;
    gl.GetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &g_maxComputeWG);
    if (gl.GetError() != GL_NO_ERROR) g_maxComputeWG = 0;

    snprintf(g_probe, sizeof(g_probe),
             "GL_VERSION=%s | GL_RENDERER=%s | GLSL=%s | "
             "MAX_COLOR_ATTACHMENTS=%d | MAX_COMPUTE_WG_INVOCATIONS=%d | DEPTH_BLIT=%s | "
             "cfg: base=%dHz out=%dHz mult=%d tier=%d bench_finish=%d",
             ver ? ver : "?", rend ? rend : "?", glsl ? glsl : "?",
             (int) g_maxColorAtt, (int) g_maxComputeWG,
             g_hasBlit ? "symbol-ok" : "missing",
             FG_BASE_HZ, FG_TARGET_HZ, FG_MULT, FG_DEFAULT_TIER, FG_BENCH_FINISH);

    LOGI("probe: %s", g_probe);

#if FG_M1_ENABLE
    if (fcl_fg_m1_init() != 0)
        LOGW("M1 unavailable, falling back to M0 only");
#endif

    g_inited = 1;
    return 0;
}

/* ---------------- 供 fg_m1 读取的采集结果 ---------------- */

unsigned fcl_fg_current_color_tex(void) {
    return g_inited ? (unsigned) g_colorTex : 0u;
}

int fcl_fg_capture_size(int *w, int *h) {
    if (!g_inited || g_W <= 0 || g_H <= 0) return 0;
    if (w) *w = g_W;
    if (h) *h = g_H;
    return 1;
}

void fcl_fg_shutdown(void) {
    if (!g_inited) return;
#if FG_M1_ENABLE
    fcl_fg_m1_shutdown();
#endif
    release_targets();
    memset(&gl, 0, sizeof(gl));
    g_inited = 0;
    g_frames = g_capNs = g_extraNs = g_capGpuNs = g_extraGpuNs = 0;
    g_samples = 0;
    g_skipped = 0;
    LOGI("shutdown");
}

void fcl_fg_on_frame_ready(int width, int height) {
    if (!g_inited || width <= 0 || height <= 0) return;

    if (width != g_W || height != g_H) {
        release_targets();
        if (create_targets(width, height) != 0) return;
    }

    FgGlState s;
    fg_state_save(&s);

    /*
     * 前提：swap 时刻默认帧缓冲是已绑定的。
     * 个别光影 / 渲染 mod 会在这里仍绑着自己的 FBO，此时读到的是错内容 ——
     * 直接跳过本帧，不要强读。
     */
    if (s.readFbo != 0 || s.drawFbo != 0) {
        g_skipped++;
        fg_state_restore(&s);
        return;
    }

    long long t0 = now_ns();

    /* 颜色：从默认帧缓冲直接拷到纹理，全程在显存里 */
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    gl.BindTexture(GL_TEXTURE_2D, g_colorTex);
    gl.CopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);

    /*
     * 深度：只能 blit，且要求格式一致。
     * g_depthBlitOk: 0 = 还没试过, 1 = 可用, -1 = 试过但失败
     */
    if (g_hasBlit && g_depthBlitOk >= 0) {
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, g_fbo);
        gl.BlitFramebuffer(0, 0, width, height, 0, 0, width, height,
                           GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        if (gl.GetError() != GL_NO_ERROR) {
            g_depthBlitOk = -1;
            LOGW("depth blit failed, falling back to color-only capture");
        } else if (g_depthBlitOk == 0) {
            g_depthBlitOk = 1;
            LOGI("depth blit ok");
        }
    }

    fg_state_restore(&s);

    long long t1 = now_ns();
    g_capNs += t1 - t0;

#if FG_BENCH_FINISH
    /*
     * 必须等 GPU 真的做完。CopyTexSubImage2D / BlitFramebuffer 都是异步的，
     * 不等的话测到的只是驱动受理命令的时间（0.1ms 级），会把开销严重低估，
     * 得出「这条路很便宜」的错误结论。
     */
    gl.Finish();
    g_capGpuNs += now_ns() - t0;
#endif

    g_frames++;
}

int fcl_fg_present_extra(int width, int height) {
    if (!g_inited || !g_hasBlit) { g_capNs = g_capGpuNs = 0; return 0; }
    if (width != g_W || height != g_H || !g_fbo) { g_capNs = g_capGpuNs = 0; return 0; }

    FgGlState s;
    fg_state_save(&s);

    GLboolean scissor = gl.IsEnabled(GL_SCISSOR_TEST);
    if (scissor) gl.Disable(GL_SCISSOR_TEST);

    long long t0 = now_ns();

    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    gl.BlitFramebuffer(0, 0, width, height, 0, 0, width, height,
                       GL_COLOR_BUFFER_BIT, GL_NEAREST);

    long long t1 = now_ns();
    g_extraNs += t1 - t0;

#if FG_BENCH_FINISH
    gl.Finish();
    g_extraGpuNs += now_ns() - t0;
#endif

    if (scissor) gl.Enable(GL_SCISSOR_TEST);
    fg_state_restore(&s);

    g_samples++;

    if (g_samples >= FG_STATS_EVERY) {
        const double n = (double) g_samples;
#if FG_BENCH_FINISH
        const double cap = (double) g_capGpuNs / n / 1e6;
        const double ext = (double) g_extraGpuNs / n / 1e6;
#else
        const double cap = (double) g_capNs / n / 1e6;
        const double ext = (double) g_extraNs / n / 1e6;
#endif
        const double sum = cap + ext;
        const char *verdict = (sum < FG_ACCEPT_MS) ? "PASS"
                            : (sum < FG_ABORT_MS)  ? "TIGHT" : "ABORT";

        /* 这一行就是 M0 的全部意义。total < 2.0ms 才继续往下做光流 */
        LOGI("[M0] capture=%.2fms  extra-present=%.2fms  total=%.2fms  "
             "slot=%.1fms  %s  (n=%d, skipped=%lld)",
             cap, ext, sum, FG_SLOT_MS, verdict, g_samples, g_skipped);

#if FG_BENCH_FINISH
        /* 对照：不 glFinish 时的数字，用来确认测量方式确实是必要的 */
        LOGI("[M0] submit-only: capture=%.3fms  extra-present=%.3fms",
             (double) g_capNs / n / 1e6, (double) g_extraNs / n / 1e6);
#endif

        g_capNs = g_extraNs = g_capGpuNs = g_extraGpuNs = 0;
        g_samples = 0;
    }

    return 1;
}

const char *fcl_fg_probe(void) { return g_probe; }

/* ---------------- M1 对外包装（ABI 2） ---------------- */

int fcl_fg_present_m1(int width, int height) {
#if FG_M1_ENABLE
    if (!g_inited) return 0;
    return fcl_fg_m1_present(width, height);
#else
    (void) width;
    (void) height;
    return 0;
#endif
}

int fcl_fg_present_real(int width, int height) {
#if FG_M1_ENABLE
    if (!g_inited) return 0;
    return fcl_fg_m1_present_real(width, height);
#else
    (void) width;
    (void) height;
    return 0;
#endif
}

void fcl_fg_stats(void) {
    if (!g_inited) { LOGI("not initialized"); return; }
    LOGI("frames=%lld  skipped=%lld  depthBlit=%s",
         g_frames, g_skipped, g_depthBlitOk > 0 ? "yes" : "no");
}
