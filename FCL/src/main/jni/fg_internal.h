/* 仅供 framegen 内部跨文件使用，不导出 */
#ifndef FG_INTERNAL_H
#define FG_INTERNAL_H

#ifdef __cplusplus
extern "C" {
#endif

/* 当前帧颜色纹理（rgba8，由 fg_core 采集）。返回 0 表示尚无有效帧 */
unsigned fcl_fg_current_color_tex(void);

/* 当前帧尺寸。返回 0 表示还未建立 */
int fcl_fg_capture_size(int *w, int *h);

/* ---------------- M1 合成层（fg_m1.c） ---------------- */

/* 把「插值帧」画进默认帧缓冲。返回 1 = 已画，调用方需再 swap 一次 */
int  fcl_fg_m1_present(int w, int h);

/* 把「真实当前帧」画进默认帧缓冲。返回 1 = 已画，调用方需再 swap 一次 */
int  fcl_fg_m1_present_real(int w, int h);

/* 初始化 / 释放（由 fg_core 的生命周期函数调用） */
int  fcl_fg_m1_init(void);
void fcl_fg_m1_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* FG_INTERNAL_H */
