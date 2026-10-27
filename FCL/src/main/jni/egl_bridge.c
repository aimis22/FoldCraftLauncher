#include "egl_bridge.h"
#include <jni.h>
#include <assert.h>
#include <dlfcn.h>
#include <limits.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#include <EGL/egl.h>
#include "GL/osmesa.h"
#include "GL/gl.h"
#include "ctxbridges/osmesa_loader.h"
#include "ctxbridges/egl_loader.h"
#include "virgl/virgl.h"

#ifdef GLES_TEST
#include <GLES2/gl2.h>
#endif

#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/rect.h>
#include <string.h>
#include <inttypes.h>
#include "environ/environ.h"
#include <android/dlext.h>
#include "ctxbridges/bridge_tbl.h"
#include "ctxbridges/osm_bridge.h"
#include <androidnsbypass/nsbypass_t.h>
#include <androidnsbypass/nsbypass.h>
#include "global_state.h"
#include "log.h"
#include <stdatomic.h>

// 由 input_bridge_v3.c 提供，上报 monitor size 到 Java 侧 GLFW
extern void updateMonitorSize(int width, int height);

#define GLFW_CLIENT_API 0x22001
/* Consider GLFW_NO_API as Vulkan API */
#define GLFW_NO_API 0
#define GLFW_OPENGL_API 0x30001

// This means that the function is an external API and that it will be used
#define EXTERNAL_API __attribute__((used))
// This means that you are forced to have this function/variable for ABI compatibility
#define ABI_COMPAT __attribute__((unused))

EGLConfig config;
struct PotatoBridge potatoBridge;

// MC 最近一次请求的交换间隔；-1 表示尚未请求
static int lastSwapInterval = -1;

#define RENDERER_GL4ES 1
#define RENDERER_VK_ZINK 2
#define RENDERER_VULKAN 4

static atomic_uint fps = 0;

/* ===================== fcl_framegen hook begin ===================== */
#include <android/log.h>
#define FG_LOG(...) __android_log_print(ANDROID_LOG_INFO, "FclFrameGen", __VA_ARGS__)

/* 必须与 framegen/include/fg_api.h 里的 FCL_FG_ABI_VERSION 保持一致 */
#define FCL_FG_ABI_VERSION 2

typedef struct {
    int  (*abi)(void);
    int  (*init)(void);
    void (*shutdown)(void);
    void (*frame_ready)(int, int);
    int  (*present_extra)(int, int);
    int  (*present_m1)(int, int);
    int  (*present_real)(int, int);
} FclFgApi;

static FclFgApi g_fgApi;
static int      g_fgState;   /* 0=未加载/已释放  1=可用  2=加载失败 */
static int      g_fgW, g_fgH;
static int      g_fgSinceRelease;

static void fclFgLoad(void) {
    if (g_fgState == 1) return;
    if (g_fgState == 2) return;

    void *h = dlopen("libfcl_framegen.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        g_fgState = 2;
        FG_LOG("libfcl_framegen.so not found: %s", dlerror());
        return;
    }

    g_fgApi.abi           = (int  (*)(void))     dlsym(h, "fcl_fg_abi_version");
    g_fgApi.init          = (int  (*)(void))     dlsym(h, "fcl_fg_init");
    g_fgApi.shutdown      = (void (*)(void))     dlsym(h, "fcl_fg_shutdown");
    g_fgApi.frame_ready   = (void (*)(int, int)) dlsym(h, "fcl_fg_on_frame_ready");
    g_fgApi.present_extra = (int  (*)(int, int)) dlsym(h, "fcl_fg_present_extra");
    g_fgApi.present_m1    = (int  (*)(int, int)) dlsym(h, "fcl_fg_present_m1");
    g_fgApi.present_real  = (int  (*)(int, int)) dlsym(h, "fcl_fg_present_real");

    if (!g_fgApi.frame_ready || !g_fgApi.present_extra) {
        g_fgState = 2;
        FG_LOG("symbols missing in libfcl_framegen.so");
        return;
    }
    if (g_fgApi.abi && g_fgApi.abi() != FCL_FG_ABI_VERSION) {
        g_fgState = 2;
        FG_LOG("abi mismatch, module disabled");
        return;
    }
    if (g_fgApi.init && g_fgApi.init() != 0) {
        g_fgState = 2;
        FG_LOG("init() failed, module disabled");
        return;
    }
    g_fgState = 1;
    g_fgSinceRelease++;
    FG_LOG("module attached (session #%d)", g_fgSinceRelease);
}

static void fclFgSize(void) {
    g_fgW = g_fgH = 0;
    if (pojav_environ->pojavWindow == NULL) return;
    g_fgW = ANativeWindow_getWidth(pojav_environ->pojavWindow);
    g_fgH = ANativeWindow_getHeight(pojav_environ->pojavWindow);
}

static void fclFgRelease(const char *why) {
    if (g_fgState != 1) return;
    if (g_fgApi.shutdown) g_fgApi.shutdown();
    g_fgState = 0;
    g_fgApi.frame_ready = NULL;
    g_fgApi.present_extra = NULL;
    FG_LOG("module released (%s)", why);
}
/* ===================== fcl_framegen hook end ===================== */

EXTERNAL_API void pojavTerminate() {
    printf("EGLBridge: Terminating\n");
    fclFgRelease("terminate");

    switch (pojav_environ->config_renderer) {
        case RENDERER_GL4ES: {
            eglMakeCurrent_p(potatoBridge.eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE,
                             EGL_NO_CONTEXT);
            eglDestroySurface_p(potatoBridge.eglDisplay, potatoBridge.eglSurface);
            eglDestroyContext_p(potatoBridge.eglDisplay, potatoBridge.eglContext);
            eglTerminate_p(potatoBridge.eglDisplay);
            eglReleaseThread_p();

            potatoBridge.eglContext = EGL_NO_CONTEXT;
            potatoBridge.eglDisplay = EGL_NO_DISPLAY;
            potatoBridge.eglSurface = EGL_NO_SURFACE;
        }
            break;

            //case RENDERER_VIRGL:
        case RENDERER_VK_ZINK: {
            // Nothing to do here
        }
            break;
    }
}

JNIEXPORT void JNICALL
Java_org_lwjgl_glfw_CallbackBridge_setupBridgeWindow(JNIEnv *env, ABI_COMPAT jclass clazz,
                                                     jobject surface) {
    // 首个窗口由 pojavInit 应用交换间隔；此处处理窗口重建（旋转、分屏等）：
    // 生产者状态会随新窗口重置，若不重新应用，MC 不会再次发起交换间隔调用，帧率会退回锁定在屏幕刷新率
    bool windowRecreated = pojav_environ->pojavWindow != NULL;
    if (windowRecreated) fclFgRelease("surface recreated");   /* 尺寸可能变了，强制重建缓冲 */
    pojav_environ->pojavWindow = ANativeWindow_fromSurface(env, surface);
    if (windowRecreated && pojav_environ->config_renderer != RENDERER_VULKAN) {
        if (lastSwapInterval >= 0) setNativeWindowSwapInterval(pojav_environ->pojavWindow, lastSwapInterval);
        else if (!getenv("POJAV_VSYNC_IN_ZINK")) setNativeWindowSwapInterval(pojav_environ->pojavWindow, 0);
    }
    if (br_setup_window != NULL) br_setup_window();
}


JNIEXPORT void JNICALL
Java_net_kdt_pojavlaunch_utils_JREUtils_releaseBridgeWindow(ABI_COMPAT JNIEnv *env,
                                                            ABI_COMPAT jclass clazz) {
    fclFgRelease("surface released");
    ANativeWindow_release(pojav_environ->pojavWindow);
}

EXTERNAL_API void *pojavGetCurrentContext() {
    if (pojav_environ->config_renderer == RENDERER_VIRGL) {
        return virglGetCurrentContext();
    }
    // SDL 模式下渲染桥未初始化（br_* 为 NULL），返回 NULL 而非空指针调用
    if (br_get_current == NULL) {
        return NULL;
    }
    return br_get_current();
}

// 已加载的 Vulkan 句柄缓存（set_vulkan_ptr 与 loadCustomVulkan 共用，各 ABI 都需要）
static void* g_vulkan_ptr = NULL;

#ifdef CUSTOM_VKDRIVER_POSSIBLE

// 门控当前设备是否为所选驱动适配的 GPU 家族（DRIVER_GPU：adreno / mali / any，
// 未设置时按 adreno 处理以兼容旧环境）；经 GLES 上下文读取 GL_VENDOR/GL_RENDERER 判定
bool checkDriverGpuFamily(const char* vendor, const char* renderer) {
    if (!vendor || !renderer)
        return false;
    const char* family = getenv("DRIVER_GPU");
    if (family && strcmp(family, "mali") == 0)
        return strcmp(vendor, "ARM") == 0 && strstr(renderer, "Mali") != NULL;
    if (family && strcmp(family, "any") == 0)
        return true;
    return strcmp(vendor, "Qualcomm") == 0 && strstr(renderer, "Adreno") != NULL;
}

bool checkDriverGpu() {
    EGLDisplay eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (eglDisplay == EGL_NO_DISPLAY || eglInitialize(eglDisplay, NULL, NULL) != EGL_TRUE)
        return false;

    EGLint egl_attributes[] = {
        EGL_BLUE_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_RED_SIZE, 8,
        EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE
    };

    EGLint num_configs = 0;
    if (eglChooseConfig(eglDisplay, egl_attributes, NULL, 0, &num_configs) != EGL_TRUE || num_configs == 0) {
        eglTerminate(eglDisplay);
        return false;
    }

    EGLConfig eglConfig;
    eglChooseConfig(eglDisplay, egl_attributes, &eglConfig, 1, &num_configs);

    const EGLint egl_context_attributes[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext context = eglCreateContext(eglDisplay, eglConfig, EGL_NO_CONTEXT, egl_context_attributes);
    if (context == EGL_NO_CONTEXT) {
        eglTerminate(eglDisplay);
        return false;
    }

    if (eglMakeCurrent(eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, context) != EGL_TRUE) {
        eglDestroyContext(eglDisplay, context);
        eglTerminate(eglDisplay);
        return false;
    }

    const char* vendor = (const char*)glGetString(GL_VENDOR);
    const char* renderer = (const char*)glGetString(GL_RENDERER);

    bool gpuMatches = checkDriverGpuFamily(vendor, renderer);

    eglMakeCurrent(eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(eglDisplay, context);
    eglTerminate(eglDisplay);

    return gpuMatches;
}

static struct android_namespace_t* vulkanLoaderNs;

void* loadCustomVulkan() {
    if (g_vulkan_ptr) return g_vulkan_ptr;
    if (!checkDriverGpu())
        return NULL;

    const char* cache_dir = getenv("TMPDIR");
    vulkanLoaderNs = private_create_namespace(
            "vulkan-loader-NS",
            NULL,
            NULL,
            ANDROID_NAMESPACE_TYPE_SHARED_ISOLATED,
            NULL,
            NULL,
            __builtin_return_address(0)
            );
    // 先加载 hook，使其符号优先进入符号表从而拦截 android_dlopen_ext
    linker_ns_dlopen("liblinkerhook.so", RTLD_LOCAL | RTLD_NOW, vulkanLoaderNs);
    // 授予命名空间访问系统库的权限
    private_link_namespaces_all_libs(vulkanLoaderNs, get_escape_namespace());
#if defined(__aarch64__) || defined(__x86_64__)
#define VULKAN_LOADER_PATH "/system/lib64/libvulkan.so"
#elif defined(__arm__) || defined(__i386__)
#define VULKAN_LOADER_PATH "/system/lib/libvulkan.so"
#endif
    return linker_ns_dlopen_unique(
            VULKAN_LOADER_PATH,
            cache_dir,
            RTLD_LOCAL | RTLD_NOW,
            vulkanLoaderNs);
}

#endif

static void set_vulkan_ptr(void* ptr) {
    g_vulkan_ptr = ptr;
    char envval[64];
    sprintf(envval, "%"PRIxPTR, (uintptr_t)ptr);
    setenv("VULKAN_PTR", envval, 1);
}

void load_vulkan() {
    if(getenv("VULKAN_DRIVER_SYSTEM") == NULL && android_get_device_api_level() >= 28) {
#ifdef CUSTOM_VKDRIVER_POSSIBLE
        void* result = loadCustomVulkan();
        if(result != NULL) {
            const char* driverLib = getenv("DRIVER_LIB");
            FCL_LOG("DriverSupp: Loaded custom driver (%s), loader address: %p",
                    driverLib && *driverLib ? driverLib : "libvulkan_freedreno.so", result);
            set_vulkan_ptr(result);
            return;
        }
#endif
    }
    FCL_LOG("OSMDroid: loading vulkan regularly...");
    // 启动器检测到设备缺失 VK_EXT_vertex_attribute_divisor 且提供 KHR 扩展时
    // 注入 VKSHIM_ENABLE，此时经 vkshim 包装加载，由 shim 用 KHR 合成 EXT，
    // LWJGL 与 SDL 统一经 VULKAN_PTR 拿到包装后的加载器；其余设备直连系统加载器
    void* vulkan_ptr = NULL;
    if (getenv("VKSHIM_ENABLE") != NULL) {
        vulkan_ptr = dlopen("libvkshim.so", RTLD_LAZY | RTLD_LOCAL);
        if (vulkan_ptr == NULL) {
            FCL_LOG("vkshim unavailable (%s), fallback to system loader", dlerror());
        }
    }
    if (vulkan_ptr == NULL) {
        vulkan_ptr = dlopen("libvulkan.so", RTLD_LAZY | RTLD_LOCAL);
    }
    FCL_LOG("OSMDroid: loaded vulkan, ptr=%p", vulkan_ptr);
    set_vulkan_ptr(vulkan_ptr);
}

int pojavInitOpenGL() {
    // Only affects GL4ES as of now
    const char *forceVsync = getenv("FORCE_VSYNC");
    if (!strcmp(forceVsync, "true"))
        pojav_environ->force_vsync = true;

    // NOTE: Override for now.
    const char *renderer = getenv("POJAV_RENDERER");
    if (!strncmp("opengles", renderer, 8)) {
        pojav_environ->config_renderer = RENDERER_GL4ES;
        if (!strcmp(renderer, "opengles3_desktopgl_zink_kopper")) {
            load_vulkan();
            setenv("GALLIUM_DRIVER", "zink", 1);
            setenv("MESA_ANDROID_NO_KMS_SWRAST", "1", 1);
            setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink", 1);
        }
        set_gl_bridge_tbl();
    }

    if (!strcmp(renderer, "gallium_virgl")) {
        pojav_environ->config_renderer = RENDERER_VIRGL;
        setenv("GALLIUM_DRIVER", "virpipe", 1);
        loadSymbolsVirGL();
        virglInit();
        return 0;
    }

    if (!strcmp(renderer, "vulkan_zink")) {
        pojav_environ->config_renderer = RENDERER_VK_ZINK;
        load_vulkan();
        setenv("GALLIUM_DRIVER", "zink", 1);
        set_osm_bridge_tbl();
    }

    if (!strcmp(renderer, "gallium_freedreno")) {
        pojav_environ->config_renderer = RENDERER_VK_ZINK;
        load_vulkan();
        setenv("GALLIUM_DRIVER", "freedreno", 1);
        setenv("MESA_LOADER_DRIVER_OVERRIDE", "kgsl", 1);
        set_osm_bridge_tbl();
    }

    if (!strcmp(renderer, "custom_gallium")) {
        pojav_environ->config_renderer = RENDERER_VK_ZINK;
        load_vulkan();
        set_osm_bridge_tbl();
    }

    if (br_init()) br_setup_window();

    return 0;
}

// 获取当前线程的 JNIEnv（未附着则先 Attach，不 Detach，保留渲染线程的附着状态）
static JNIEnv *get_attached_env(JavaVM *jvm) {
    JNIEnv *jvm_env = NULL;
    jint env_result = (*jvm)->GetEnv(jvm, (void **) &jvm_env, JNI_VERSION_1_4);
    if (env_result == JNI_EDETACHED) {
        env_result = (*jvm)->AttachCurrentThread(jvm, &jvm_env, NULL);
    }
    if (env_result != JNI_OK) {
        printf("get_attached_env failed: %i\n", env_result);
        return NULL;
    }
    return jvm_env;
}

EXTERNAL_API int pojavInit() {
    pojav_environ->glfwThreadVmEnv = get_attached_env(pojav_environ->runtimeJavaVMPtr);
    if (pojav_environ->glfwThreadVmEnv == NULL) {
        printf("Failed to attach Java-side JNIEnv to GLFW thread\n");
        return 0;
    }
    ANativeWindow_acquire(pojav_environ->pojavWindow);
    pojav_environ->savedWidth = ANativeWindow_getWidth(pojav_environ->pojavWindow);
    pojav_environ->savedHeight = ANativeWindow_getHeight(pojav_environ->pojavWindow);
    ANativeWindow_setBuffersGeometry(pojav_environ->pojavWindow, pojav_environ->savedWidth,
                                     pojav_environ->savedHeight,
                                     AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM);
    updateMonitorSize(pojav_environ->savedWidth, pojav_environ->savedHeight);
    pojavInitOpenGL();
    // 垂直同步开关关闭时主动切入异步模式，解除帧率对屏幕刷新率的锁定；开启时交由 MC 的交换间隔调用决定
    if (pojav_environ->config_renderer != RENDERER_VULKAN && !getenv("POJAV_VSYNC_IN_ZINK")) {
        setNativeWindowSwapInterval(pojav_environ->pojavWindow, 0);
    }
    return 1;
}

EXTERNAL_API void pojavSetWindowHint(int hint, int value) {
    if (hint != GLFW_CLIENT_API) return;
    switch (value) {
        case GLFW_NO_API:
            pojav_environ->config_renderer = RENDERER_VULKAN;
            /* Nothing to do: initialization is handled in Java-side */
            // pojavInitVulkan();
            break;
        case GLFW_OPENGL_API: {
            const char *renderer = getenv("POJAV_RENDERER");
            if (strncmp(renderer, "opengles", 8) == 0) {
                pojav_environ->config_renderer = RENDERER_GL4ES;
            } else if (!strcmp(renderer, "vulkan_zink")) {
                pojav_environ->config_renderer = RENDERER_VK_ZINK;
            }
            /* Nothing to do: initialization is called in pojavCreateContext */
            // pojavInitOpenGL();
            break;
        }
        default:
            printf("GLFW: Unimplemented API 0x%x\n", value);
            abort();
    }
}

EXTERNAL_API void pojavSwapBuffers() {
    atomic_fetch_add(&fps, 1);

    /* ---------------- framegen: 采集 + 可选的插值帧合成 ---------------- */
    int fgInterp = 0;
    fclFgLoad();
    if (g_fgState == 1) {
        fclFgSize();
        if (g_fgW > 0 && g_fgH > 0) {
            g_fgApi.frame_ready(g_fgW, g_fgH);
            /*
             * M1：把插值帧画进 backbuffer，把游戏这一帧覆盖掉。
             * 于是下面那次 swap 上屏的是插值帧，随后再 swap 一次出真实帧，
             * 时间顺序为 mid -> cur，不会倒放。
             */
            if (g_fgApi.present_m1) fgInterp = g_fgApi.present_m1(g_fgW, g_fgH);
        }
    }

    // SDL 模式（config_renderer 未匹配任何渲染桥）下这些调用必须为 no-op，
    // 否则 br_* 函数指针为 NULL 会空指针崩溃（fixPojavGLContext 路径）
    if (pojav_environ->config_renderer == RENDERER_VK_ZINK
     || pojav_environ->config_renderer == RENDERER_GL4ES) {
        br_swap_buffers();
    }
    if (pojav_environ->config_renderer == RENDERER_VIRGL) {
        virglSwapBuffers();
    }

    /* ---------------- framegen: 第二次上屏 ---------------- */
    if (g_fgState == 1 && g_fgW > 0) {
        int second = 0;
        if (fgInterp && g_fgApi.present_real) {
            second = g_fgApi.present_real(g_fgW, g_fgH);   /* M1：真实帧 */
        } else if (g_fgApi.present_extra) {
            second = g_fgApi.present_extra(g_fgW, g_fgH);  /* M0：重复帧，测开销 */
        }
        if (second) {
            if (pojav_environ->config_renderer == RENDERER_VIRGL) {
                virglSwapBuffers();
            } else if (br_swap_buffers != NULL) {
                br_swap_buffers();
            }
        }
    }
}


EXTERNAL_API void pojavMakeCurrent(void *window) {
    if (pojav_environ->config_renderer == RENDERER_VK_ZINK
     || pojav_environ->config_renderer == RENDERER_GL4ES) {
        br_make_current((basic_render_window_t *) window);
    }
    if (pojav_environ->config_renderer == RENDERER_VIRGL) {
        virglMakeCurrent(window);
    }
}

EXTERNAL_API void *pojavCreateContext(void *contextSrc) {
    if (pojav_environ->config_renderer == RENDERER_VULKAN)
        return (void *) pojav_environ->pojavWindow;

    if (pojav_environ->config_renderer == RENDERER_VIRGL)
        return virglCreateContext(contextSrc);

    if (br_init_context == NULL)
        return NULL;

    return br_init_context((basic_render_window_t *) contextSrc);
}

void *maybe_load_vulkan() {
    // We use the env var because
    // 1. it's easier to do that
    // 2. it won't break if something will try to load vulkan and osmesa simultaneously
    if (getenv("VULKAN_PTR") == NULL) load_vulkan();
    return (void *) strtoul(getenv("VULKAN_PTR"), NULL, 0x10);
}

EXTERNAL_API JNIEXPORT jlong JNICALL
Java_org_lwjgl_vulkan_VK_getVulkanDriverHandle(ABI_COMPAT JNIEnv *env, ABI_COMPAT jclass thiz) {
    printf("EGLBridge: LWJGL-side Vulkan loader requested the Vulkan handle\n");
    return (jlong) maybe_load_vulkan();
}

EXTERNAL_API void pojavSwapInterval(int interval) {
    lastSwapInterval = interval;

    if (pojav_environ->config_renderer == RENDERER_VK_ZINK
     || pojav_environ->config_renderer == RENDERER_GL4ES) {
        br_swap_interval(interval);
    }
    if (pojav_environ->config_renderer == RENDERER_VIRGL) {
        virglSwapInterval(interval);
    }
}

JNIEXPORT jint JNICALL
Java_org_lwjgl_glfw_CallbackBridge_getFps(JNIEnv *env, jclass clazz) {
    return atomic_exchange(&fps, 0);
}

// SDL 路径下由 eglSwapBuffers 代理调用来计帧（sdl_hook.c）
EXTERNAL_API void calculateFPS(void) {
    atomic_fetch_add(&fps, 1);
}

EXTERNAL_API JNIEXPORT void JNICALL
Java_org_lwjgl_vulkan_VK_updateFps(ABI_COMPAT JNIEnv *env, ABI_COMPAT jclass thiz) {
    atomic_fetch_add(&fps, 1);
}