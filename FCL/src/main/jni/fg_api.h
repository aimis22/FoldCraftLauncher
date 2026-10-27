/*
 * fcl_framegen - 帧生成模块对外 ABI
 *
 * 设计原则：本模块与 FCL 主仓之间只有这几个符号的契约，
 * 通过 dlopen/dlsym 动态加载，主仓看不到任何实现细节。
 * 探不到符号 = 功能不存在，主链路零风险。
 */
#ifndef FCL_FG_API_H
#define FCL_FG_API_H

#ifdef __cplusplus
extern "C" {
#endif

#define FCL_FG_ABI_VERSION 2

/* 模块以 -fvisibility=hidden 编译，只有下列符号对外可见 */
#define FCL_FG_EXPORT __attribute__((visibility("default")))

/* 返回 ABI 版本，主仓用它判断兼容性 */
FCL_FG_EXPORT int fcl_fg_abi_version(void);

/* 初始化。返回 0 = 成功，非 0 = 失败（主仓应静默跳过） */
FCL_FG_EXPORT int fcl_fg_init(void);

/* 释放全部 GPU 资源。窗口销毁 / context lost / 退出游戏 必须调用 */
FCL_FG_EXPORT void fcl_fg_shutdown(void);

/*
 * 在真正的 eglSwapBuffers 之前调用。
 * 此时默认帧缓冲里是完整的一帧，颜色和深度都可读。
 * width/height 为 surface 尺寸（FCL 通过 setBuffersGeometry 设置，
 * 所以它就是内部渲染分辨率）。
 */
FCL_FG_EXPORT void fcl_fg_on_frame_ready(int width, int height);

/*
 * 在一次 eglSwapBuffers 之后调用。
 * 返回 1 表示请求调用方额外再 swap 一次（M0 阶段用于测开销）。
 * 返回 0 表示不需要。
 */
FCL_FG_EXPORT int fcl_fg_present_extra(int width, int height);

/*
 * ---- M1（ABI 2 新增，可选）----
 *
 * 呈现时序变了：每来一帧游戏画面，输出两屏帧：
 *
 *     采集 cur -> present_m1（画插值帧 mid）-> swap
 *              -> present_real（画真实帧 cur）-> swap
 *
 * 调用顺序必须是：
 *     on_frame_ready() -> [present_m1() 返回 1 ? swap : 走 present_extra 路径]
 *                      -> [present_real() 返回 1 ? swap]
 *
 * 两个函数都返回 1 表示「已经把画面画进了默认帧缓冲」，调用方需要再 swap 一次。
 * 返回 0 表示没画，调用方应当走 M0 的 present_extra 路径或什么都不做。
 *
 * 未编 M1（FG_M1_ENABLE=0）或能力不足时，两者恒返回 0。
 */
FCL_FG_EXPORT int fcl_fg_present_m1(int width, int height);
FCL_FG_EXPORT int fcl_fg_present_real(int width, int height);

/* 能力探测结果（人类可读），用于 logcat / 调试页展示 */
FCL_FG_EXPORT const char *fcl_fg_probe(void);

/* 打印累计耗时统计 */
FCL_FG_EXPORT void fcl_fg_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* FCL_FG_API_H */
