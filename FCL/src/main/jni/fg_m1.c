/*
 * fg_m1.c - M1：金字塔 Lucas-Kanade 光流 + 双向 warp 合成
 *
 * 设计取向（为什么这么做）：
 *
 *  - **只用 fragment shader，不依赖 compute**。MG 是「桌面 GL -> 宿主 GLES」的
 *    翻译层，compute 未必被映射；一旦依赖它，算法可能在目标机器上直接不可用。
 *    LK 用 25 抽头窗口解 2x2 线性系统，fragment 完全够。
 *
 *  - **两级金字塔**（1/2、1/4）。纯 1/4 分辨率下单层 LK 只能跟住约 +-3px/帧的位移，
 *    而 144Hz 下快速转视角可以到 20px/帧；多一级金字塔把有效范围放大一倍以上。
 *
 *  - **flow 以 uv 为单位存储**。这样上层 flow 直接双线性采样就能当下一层的初值，
 *    不需要单独的 upsample pass，也避免了分辨率换算错误这类难查的 bug。
 *
 *  - **合成用双向 warp**：prev 按 +f 采样、cur 按 -f 采样，按 t 混合；
 *    两帧差异大的地方（遮挡/新露出）向时间上更近的一侧靠，避免半透明重影。
 *
 * 呈现时序（这是插帧能对的前提）：
 *
 *      采集 cur -> 合成 mid(prev,cur,t=0.5) -> swap（屏上是 mid）
 *              -> 画 cur                    -> swap（屏上是 cur）
 *
 *    每来一帧游戏画面输出两屏帧，时间顺序为 mid, cur，不会倒放。
 *    代价是整体延迟约半帧到一帧，这个在方案里已经计入。
 */

#include <dlfcn.h>
#include <string.h>
#include <time.h>

#include <android/log.h>
#include <GLES3/gl3.h>

#include "fg_config.h"
#include "fg_internal.h"
#include "fg_shaders.h"

#define TAG "FclFrameGen"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* ------------------------------------------------------------------ */
/* GL 函数指针                                                        */
/* ------------------------------------------------------------------ */

#define FG_GL_FUNCS(F) \
    F(void,      GetIntegerv,    GLenum, GLint *) \
    F(GLenum,    GetError,       void) \
    F(void,      GenTextures,    GLsizei, GLuint *) \
    F(void,      DeleteTextures, GLsizei, const GLuint *) \
    F(void,      BindTexture,    GLenum, GLuint) \
    F(void,      ActiveTexture,  GLenum) \
    F(void,      TexImage2D,     GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *) \
    F(void,      TexParameteri,  GLenum, GLenum, GLint) \
    F(void,      GenFramebuffers,     GLsizei, GLuint *) \
    F(void,      DeleteFramebuffers,  GLsizei, const GLuint *) \
    F(void,      BindFramebuffer,     GLenum, GLuint) \
    F(void,      FramebufferTexture2D, GLenum, GLenum, GLenum, GLuint, GLint) \
    F(GLenum,    CheckFramebufferStatus, GLenum) \
    F(GLuint,    CreateShader,   GLenum) \
    F(void,      ShaderSource,   GLuint, GLsizei, const GLchar *const *, const GLint *) \
    F(void,      CompileShader,  GLuint) \
    F(void,      GetShaderiv,    GLuint, GLenum, GLint *) \
    F(void,      GetShaderInfoLog, GLuint, GLsizei, GLsizei *, GLchar *) \
    F(void,      DeleteShader,   GLuint) \
    F(GLuint,    CreateProgram,  void) \
    F(void,      AttachShader,   GLuint, GLuint) \
    F(void,      LinkProgram,    GLuint) \
    F(void,      GetProgramiv,   GLuint, GLenum, GLint *) \
    F(void,      GetProgramInfoLog, GLuint, GLsizei, GLsizei *, GLchar *) \
    F(void,      DeleteProgram,  GLuint) \
    F(void,      UseProgram,     GLuint) \
    F(GLint,     GetUniformLocation, GLuint, const GLchar *) \
    F(void,      Uniform1i,      GLint, GLint) \
    F(void,      Uniform1f,      GLint, GLfloat) \
    F(void,      Uniform2f,      GLint, GLfloat, GLfloat) \
    F(void,      Viewport,       GLint, GLint, GLsizei, GLsizei) \
    F(void,      DrawArrays,     GLenum, GLint, GLsizei) \
    F(void,      ClearColor,     GLfloat, GLfloat, GLfloat, GLfloat) \
    F(void,      Clear,          GLbitfield) \
    F(void,      Disable,        GLenum) \
    F(void,      Enable,         GLenum) \
    F(GLboolean, IsEnabled,      GLenum) \
    F(void,      ColorMask,      GLboolean, GLboolean, GLboolean, GLboolean) \
    F(void,      Finish,         void)

/*
 * 注意：glBindVertexArray 不在强制列表里。
 * 全屏三角形靠 gl_VertexID 生成顶点，根本不需要 VAO/属性；
 * GLES3 规范里 VAO 0 就是默认 VAO，可直接画。
 * 部分翻译层（MG / GL4ES）的 glBindVertexArray 是模拟的、甚至缺失，
 * 把它列成必需项会导致 M1 在完全能跑的设备上被误判为不可用。
 */
typedef void (*FgFn_BindVertexArray)(GLuint);

#define FG_DECL(ret, name, ...) typedef ret (*FgFn_##name)(__VA_ARGS__);
FG_GL_FUNCS(FG_DECL)
#undef FG_DECL

static struct {
#define FG_MEMBER(ret, name, ...) FgFn_##name name;
    FG_GL_FUNCS(FG_MEMBER)
#undef FG_MEMBER
} gl;

static void *fg_sym(const char *n) {
    void *p = dlsym(RTLD_DEFAULT, n);
    if (p) return p;
    static void *h;
    if (!h) h = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    return h ? dlsym(h, n) : NULL;
}

static int gl_load(void) {
    int missing = 0;
#define FG_LOAD(ret, name, ...)                                  \
    gl.name = (FgFn_##name) fg_sym("gl" #name);                  \
    if (!gl.name) { LOGE("missing gl%s", #name); missing++; }
    FG_GL_FUNCS(FG_LOAD)
#undef FG_LOAD
    if (missing) return -1;

    /*
     * 可选符号：拿得到就顺手清一下 VAO 绑定，拿不到也无所谓。
     * 有些翻译层没有导出它，但那不影响 gl_VertexID 路径。
     */
    {
        FgFn_BindVertexArray bindVao =
            (FgFn_BindVertexArray) fg_sym("glBindVertexArray");
        if (bindVao) bindVao(0);
        else         LOGI("glBindVertexArray absent, using default VAO path");
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 状态                                                               */
/* ------------------------------------------------------------------ */

#define FG_LV 2                      /* 0 = 1/2 分辨率，1 = 1/4 分辨率 */

static int    s_inited;
static int    s_ok;                  /* 能力齐全，M1 可用 */
static int    s_w, s_h;
static int    s_lw[FG_LV], s_lh[FG_LV];

static GLuint s_prevColor;
static GLuint s_luma[2][FG_LV];      /* [集合][层] */
static int    s_set;                 /* 当前集合索引 */
static int    s_prevValid;

static GLuint s_flowHi[2];           /* 1/4 分辨率 flow，乒乓 */
static GLuint s_flowLo[2];           /* 1/2 分辨率 flow，乒乓 */

static GLuint s_fbo;
static GLuint s_pLuma, s_pLk, s_pComp, s_pCopy;

/* LK 迭代次数与限幅 */
#define FG_LK_ITER_HI 3
#define FG_LK_ITER_LO 2
#define FG_LK_STEP    0.02f          /* uv 空间单次迭代上限，约 1/4 分辨率的 2.5px */

/* ------------------------------------------------------------------ */
/* 统计                                                               */
/* ------------------------------------------------------------------ */

static long long m1_frames;
static long long m1_ns;
static long long m1_gpuNs;

static long long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void m1_report(void) {
    const double n   = (double) m1_frames;
#if FG_BENCH_FINISH
    const double ms  = (double) m1_gpuNs / n / 1e6;
#else
    const double ms  = (double) m1_ns / n / 1e6;
#endif
    /* 合成本身要能在「基帧槽位 - 采集开销」里装下，留 1ms 给第二次呈现 */
    const double bud = FG_SLOT_MS - 1.0;
    const char  *verdict = (ms < bud * 0.5) ? "PASS"
                         : (ms < bud)       ? "TIGHT" : "ABORT";

    LOGI("[M1] compose=%.2fms  budget=%.1fms  %s  (n=%lld)",
         ms, bud, verdict, m1_frames);

#if FG_BENCH_FINISH
    LOGI("[M1] submit-only: compose=%.3fms", (double) m1_ns / n / 1e6);
#endif

    m1_frames = m1_ns = m1_gpuNs = 0;
}

/* ------------------------------------------------------------------ */
/* 纹理 / 程序                                                        */
/* ------------------------------------------------------------------ */

static GLuint make_tex(GLenum ifmt, GLsizei w, GLsizei h,
                       GLenum fmt, GLenum type, GLenum filter) {
    GLuint t = 0;
    gl.GenTextures(1, &t);
    gl.BindTexture(GL_TEXTURE_2D, t);
    gl.TexImage2D(GL_TEXTURE_2D, 0, (GLint) ifmt, w, h, 0, fmt, type, NULL);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint) filter);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint) filter);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

static GLuint build_program(const char *fsrc, const char *name) {
    GLint ok = 0;
    char log[512];

    GLuint vs = gl.CreateShader(GL_VERTEX_SHADER);
    const char *vsrc = FG_VS_FULL;
    gl.ShaderSource(vs, 1, (const GLchar *const *) &vsrc, NULL);
    gl.CompileShader(vs);
    gl.GetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        gl.GetShaderInfoLog(vs, (GLsizei) sizeof(log), NULL, log);
        LOGE("VS(%s) compile failed: %s", name, log);
        gl.DeleteShader(vs);
        return 0;
    }

    GLuint fs = gl.CreateShader(GL_FRAGMENT_SHADER);
    gl.ShaderSource(fs, 1, (const GLchar *const *) &fsrc, NULL);
    gl.CompileShader(fs);
    gl.GetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        gl.GetShaderInfoLog(fs, (GLsizei) sizeof(log), NULL, log);
        LOGE("FS(%s) compile failed: %s", name, log);
        gl.DeleteShader(vs);
        gl.DeleteShader(fs);
        return 0;
    }

    GLuint p = gl.CreateProgram();
    gl.AttachShader(p, vs);
    gl.AttachShader(p, fs);
    gl.LinkProgram(p);
    gl.GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        gl.GetProgramInfoLog(p, (GLsizei) sizeof(log), NULL, log);
        LOGE("program(%s) link failed: %s", name, log);
        gl.DeleteProgram(p);
        p = 0;
    }
    gl.DeleteShader(vs);
    gl.DeleteShader(fs);
    return p;
}

/* ------------------------------------------------------------------ */
/* 状态保存 / 恢复                                                    */
/* ------------------------------------------------------------------ */

/*
 * 清空 GL 错误队列。
 * GL 的错误是累积的：游戏自己上一帧留下的错误码会被我们读成「M1 出错」，
 * 于是功能被误判为不可用并自动关闭 —— 这类问题在日志里表现为
 * 「compose reported GL error」但实际画面完全正常，极难定位。
 */
static void drain_errors(void) {
    for (int i = 0; i < 32 && gl.GetError() != GL_NO_ERROR; ++i) { /* spin */ }
}

typedef struct {
    GLint viewport[4];
    GLint readFbo, drawFbo, activeTex, texBinding;
    GLint colorMask[4];
    GLboolean scissor;
} FgState;

static void state_save(FgState *s) {
    gl.GetIntegerv(GL_VIEWPORT, s->viewport);
    gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &s->readFbo);
    gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s->drawFbo);
    gl.GetIntegerv(GL_ACTIVE_TEXTURE, &s->activeTex);
    gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &s->texBinding);
    gl.GetIntegerv(GL_COLOR_WRITEMASK, s->colorMask);
    s->scissor = gl.IsEnabled(GL_SCISSOR_TEST);
    if (s->scissor) gl.Disable(GL_SCISSOR_TEST);
    gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

static void state_restore(const FgState *s) {
    gl.ColorMask((GLboolean) s->colorMask[0], (GLboolean) s->colorMask[1],
                 (GLboolean) s->colorMask[2], (GLboolean) s->colorMask[3]);
    if (s->scissor) gl.Enable(GL_SCISSOR_TEST);
    gl.ActiveTexture((GLenum) s->activeTex);
    gl.BindTexture(GL_TEXTURE_2D, (GLuint) s->texBinding);
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint) s->readFbo);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint) s->drawFbo);
    gl.Viewport(s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3]);
}

/* ------------------------------------------------------------------ */
/* 资源                                                               */
/* ------------------------------------------------------------------ */

static void release_all(void) {
    if (s_prevColor) { gl.DeleteTextures(1, &s_prevColor); s_prevColor = 0; }
    for (int k = 0; k < 2; ++k) {
        for (int l = 0; l < FG_LV; ++l) {
            if (s_luma[k][l]) { gl.DeleteTextures(1, &s_luma[k][l]); s_luma[k][l] = 0; }
        }
    }
    for (int i = 0; i < 2; ++i) {
        if (s_flowHi[i]) { gl.DeleteTextures(1, &s_flowHi[i]); s_flowHi[i] = 0; }
        if (s_flowLo[i]) { gl.DeleteTextures(1, &s_flowLo[i]); s_flowLo[i] = 0; }
    }
    if (s_fbo) { gl.DeleteFramebuffers(1, &s_fbo); s_fbo = 0; }
    s_prevValid = 0;
    s_w = s_h = 0;
}

static int create_targets(int w, int h) {
    s_lw[0] = (w + 1) / 2;  s_lh[0] = (h + 1) / 2;
    s_lw[1] = (w + 3) / 4;  s_lh[1] = (h + 3) / 4;

    s_prevColor = make_tex(GL_RGBA8, w, h, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR);

    for (int k = 0; k < 2; ++k) {
        for (int l = 0; l < FG_LV; ++l) {
            s_luma[k][l] = make_tex(GL_R8, s_lw[l], s_lh[l], GL_RED, GL_UNSIGNED_BYTE, GL_LINEAR);
        }
    }
    for (int i = 0; i < 2; ++i) {
        s_flowHi[i] = make_tex(GL_RG16F, s_lw[1], s_lh[1], GL_RG, GL_HALF_FLOAT, GL_LINEAR);
        s_flowLo[i] = make_tex(GL_RG16F, s_lw[0], s_lh[0], GL_RG, GL_HALF_FLOAT, GL_LINEAR);
    }

    gl.GenFramebuffers(1, &s_fbo);

    /* RG16F 是否可渲染，直接决定 M1 能不能做（不够就只能退回 RG8 + 缩放） */
    gl.BindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                            GL_TEXTURE_2D, s_flowHi[0], 0);
    GLenum st = gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("RG16F not color-renderable (0x%04x), M1 disabled", st);
        release_all();
        return -1;
    }

    s_w = w;
    s_h = h;
    s_set = 0;
    s_prevValid = 0;
    LOGI("M1 targets: %dx%d  luma %dx%d -> %dx%d  (~%.0fMB)",
         w, h, s_lw[0], s_lh[0], s_lw[1], s_lh[1],
         ((double) w * h * 4 + (double) s_lw[0] * s_lh[0] * 8) / 1048576.0);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 单步绘制                                                           */
/* ------------------------------------------------------------------ */

static void draw_to_tex(GLuint dstTex, int dw, int dh) {
    gl.BindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                            GL_TEXTURE_2D, dstTex, 0);
    gl.Viewport(0, 0, dw, dh);
}

static void draw_to_screen(int w, int h) {
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.Viewport(0, 0, w, h);
}

static void bind3(GLuint p, GLuint t0, GLuint t1, GLuint t2) {
    gl.UseProgram(p);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, t0);
    gl.Uniform1i(gl.GetUniformLocation(p, "uPrev"), 0);
    if (t1) {
        gl.ActiveTexture(GL_TEXTURE1);
        gl.BindTexture(GL_TEXTURE_2D, t1);
        gl.Uniform1i(gl.GetUniformLocation(p, "uCur"), 1);
    }
    if (t2) {
        gl.ActiveTexture(GL_TEXTURE2);
        gl.BindTexture(GL_TEXTURE_2D, t2);
        gl.Uniform1i(gl.GetUniformLocation(p, "uFlow"), 2);
    }
}

static void pass_luma(GLuint src, int sw, int sh, GLuint dst, int dw, int dh) {
    draw_to_tex(dst, dw, dh);
    gl.UseProgram(s_pLuma);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, src);
    gl.Uniform1i(gl.GetUniformLocation(s_pLuma, "uSrc"), 0);
    gl.Uniform2f(gl.GetUniformLocation(s_pLuma, "uSrcTexel"),
                 1.0f / (float) sw, 1.0f / (float) sh);
    gl.DrawArrays(GL_TRIANGLES, 0, 3);
}

static void pass_lk(GLuint prevLuma, GLuint curLuma, GLuint flowIn, GLuint flowOut,
                    int lw, int lh) {
    draw_to_tex(flowOut, lw, lh);
    bind3(s_pLk, prevLuma, curLuma, flowIn);
    gl.Uniform2f(gl.GetUniformLocation(s_pLk, "uTexel"),
                 1.0f / (float) lw, 1.0f / (float) lh);
    gl.Uniform1f(gl.GetUniformLocation(s_pLk, "uStep"), FG_LK_STEP);
    gl.DrawArrays(GL_TRIANGLES, 0, 3);
}

static void clear_tex(GLuint tex, int w, int h) {
    draw_to_tex(tex, w, h);
    gl.ClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    gl.Clear(GL_COLOR_BUFFER_BIT);
}

static void pass_copy_to_tex(GLuint src, GLuint dst, int w, int h) {
    draw_to_tex(dst, w, h);
    gl.UseProgram(s_pCopy);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, src);
    gl.Uniform1i(gl.GetUniformLocation(s_pCopy, "uSrc"), 0);
    gl.DrawArrays(GL_TRIANGLES, 0, 3);
}

static void pass_copy_to_screen(GLuint src, int w, int h) {
    draw_to_screen(w, h);
    gl.UseProgram(s_pCopy);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, src);
    gl.Uniform1i(gl.GetUniformLocation(s_pCopy, "uSrc"), 0);
    gl.DrawArrays(GL_TRIANGLES, 0, 3);
}

/* ------------------------------------------------------------------ */
/* 对外                                                               */
/* ------------------------------------------------------------------ */

int fcl_fg_m1_init(void) {
    if (s_inited) return s_ok ? 0 : -1;
    s_inited = 1;

    if (gl_load() != 0) { s_ok = 0; return -1; }

    s_pLuma = build_program(FG_FS_LUMA,    "luma");
    s_pLk   = build_program(FG_FS_LK,      "lk");
    s_pComp = build_program(FG_FS_COMPOSE, "compose");
    s_pCopy = build_program(FG_FS_COPY,    "copy");

    if (!s_pLuma || !s_pLk || !s_pComp || !s_pCopy) {
        LOGE("shader build failed, M1 disabled");
        s_ok = 0;
        return -1;
    }

    s_ok = 1;
    LOGI("M1 ready (fragment-only LK, %d levels, mult %d)", FG_LV, FG_MULT);
    return 0;
}

void fcl_fg_m1_shutdown(void) {
    if (!s_inited) return;
    if (s_ok) release_all();
    if (s_pLuma) { gl.DeleteProgram(s_pLuma); s_pLuma = 0; }
    if (s_pLk)   { gl.DeleteProgram(s_pLk);   s_pLk = 0; }
    if (s_pComp) { gl.DeleteProgram(s_pComp); s_pComp = 0; }
    if (s_pCopy) { gl.DeleteProgram(s_pCopy); s_pCopy = 0; }
    s_inited = 0;
    s_ok = 0;
}

int fcl_fg_m1_present(int w, int h) {
    if (!s_ok || w <= 0 || h <= 0) return 0;

    GLuint curTex = (GLuint) fcl_fg_current_color_tex();
    if (!curTex) return 0;

    /*
     * 和 fg_core 的采集同一个前提：swap 时刻默认帧缓冲必须是 0。
     * 个别渲染 mod（Iris 的某些分支）会在 swap 时仍绑着自己的 FBO，
     * 那时「画到屏幕」其实是画进别人的 FBO，画面会黑或者错乱。
     * 遇到就跳过本帧，不要硬来。
     *
     * 这个检查必须在 state_save 之前：state_save 会顺手关掉 scissor、
     * 打开 colorMask，提前返回而不恢复就等于污染了游戏状态。
     */
    {
        GLint drawFbo = 0;
        gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
        if (drawFbo != 0) return 0;
    }

    /*
     * 状态保存必须在 create_targets 之前：建纹理/建 FBO 会改动当前纹理绑定，
     * 若在它之后再 save，等于把被污染的状态当成「原件」恢复回去，
     * 游戏下一帧就会用错纹理 —— 表现为偶发贴图错乱，极难定位。
     */
    FgState st;
    state_save(&st);
    drain_errors();
    long long t0 = now_ns();

    if (w != s_w || h != s_h) {
        release_all();
        if (create_targets(w, h) != 0) {
            s_ok = 0;
            state_restore(&st);
            return 0;
        }
    }

    /* 1. 当前帧亮度金字塔 */
    pass_luma(curTex, w, h, s_luma[s_set][0], s_lw[0], s_lh[0]);
    pass_luma(s_luma[s_set][0], s_lw[0], s_lh[0], s_luma[s_set][1], s_lw[1], s_lh[1]);

    if (!s_prevValid) {
        /* 第一帧：没有 prev，只能先缓存，这一帧不合成（调用方走 M0 的重复帧路径） */
        pass_copy_to_tex(curTex, s_prevColor, w, h);
        s_set ^= 1;
        s_prevValid = 1;
        state_restore(&st);
        return 0;
    }

    GLuint prevLumaHi = s_luma[s_set ^ 1][1];
    GLuint curLumaHi  = s_luma[s_set][1];
    GLuint prevLumaLo = s_luma[s_set ^ 1][0];
    GLuint curLumaLo  = s_luma[s_set][0];

    /* 2. 粗层（1/4）光流 */
    clear_tex(s_flowHi[0], s_lw[1], s_lh[1]);
    int src = 0;
    for (int i = 0; i < FG_LK_ITER_HI; ++i) {
        pass_lk(prevLumaHi, curLumaHi, s_flowHi[src], s_flowHi[src ^ 1], s_lw[1], s_lh[1]);
        src ^= 1;
    }
    GLuint flowHi = s_flowHi[src];

    /* 3. 细层（1/2）细化，初值由粗层双线性放大得到 */
    pass_lk(prevLumaLo, curLumaLo, flowHi, s_flowLo[0], s_lw[0], s_lh[0]);
    for (int i = 1; i < FG_LK_ITER_LO; ++i) {
        pass_lk(prevLumaLo, curLumaLo, s_flowLo[(i - 1) & 1], s_flowLo[i & 1], s_lw[0], s_lh[0]);
    }
    GLuint flowLo = s_flowLo[(FG_LK_ITER_LO - 1) & 1];

    /* 4. 合成插值帧到屏幕 */
    draw_to_screen(w, h);
    bind3(s_pComp, s_prevColor, curTex, flowLo);
    gl.Uniform1f(gl.GetUniformLocation(s_pComp, "uT"), 0.5f);
    gl.DrawArrays(GL_TRIANGLES, 0, 3);

    if (gl.GetError() != GL_NO_ERROR) {
        LOGW("M1 compose reported GL error, disabling");
        s_ok = 0;
        state_restore(&st);
        return 0;
    }

#if FG_BENCH_FINISH
    /* 同 M0：不等 GPU 做完，测到的只是命令提交时间（0.1ms 级），会严重低估 */
    gl.Finish();
#endif
    long long t1 = now_ns();
    m1_ns += t1 - t0;
#if FG_BENCH_FINISH
    m1_gpuNs += t1 - t0;
#endif
    if (++m1_frames >= FG_STATS_EVERY) m1_report();

    /* 5. 缓存本帧，供下一次使用 */
    pass_copy_to_tex(curTex, s_prevColor, w, h);

    s_set ^= 1;

    state_restore(&st);
    return 1;
}

int fcl_fg_m1_present_real(int w, int h) {
    if (!s_ok) return 0;

    GLuint curTex = (GLuint) fcl_fg_current_color_tex();
    if (!curTex) return 0;

    FgState st;
    state_save(&st);
    pass_copy_to_screen(curTex, w, h);
    state_restore(&st);
    return 1;
}
