/*
 * fg_shaders.h - GLSL ES 3.00 源码（内嵌，避免 assets / 文件依赖）
 *
 * 全部使用「全屏三角形 + gl_VertexID」，不需要 VBO/VAO/属性。
 * 所有纹理坐标与像素坐标同向（左下为原点），采集纹理自带这个朝向，不必翻转。
 */

#ifndef FG_SHADERS_H
#define FG_SHADERS_H

/* ---------------------------------------------------------------- */
/* 顶点着色器：全屏三角形                                            */
/* ---------------------------------------------------------------- */
static const char *FG_VS_FULL =
"#version 300 es\n"
"out vec2 vUV;\n"
"void main() {\n"
"    vec2 p = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));\n"
"    vUV = p * 2.0;\n"
"    gl_Position = vec4(p * 4.0 - 1.0, 0.0, 1.0);\n"
"}\n";

/* ---------------------------------------------------------------- */
/* 亮度降采样（RGBA 颜色 -> 单通道亮度，尺寸减半）                    */
/* ---------------------------------------------------------------- */
static const char *FG_FS_LUMA =
"#version 300 es\n"
"precision highp float;\n"
"uniform sampler2D uSrc;\n"
"uniform vec2 uSrcTexel;\n"
"in vec2 vUV;\n"
"out vec4 oColor;\n"
"void main() {\n"
"    vec3 c = texture(uSrc, vUV + vec2(-uSrcTexel.x, -uSrcTexel.y)).rgb\n"
"           + texture(uSrc, vUV + vec2( uSrcTexel.x, -uSrcTexel.y)).rgb\n"
"           + texture(uSrc, vUV + vec2(-uSrcTexel.x,  uSrcTexel.y)).rgb\n"
"           + texture(uSrc, vUV + vec2( uSrcTexel.x,  uSrcTexel.y)).rgb;\n"
"    c *= 0.25;\n"
"    oColor = vec4(dot(c, vec3(0.299, 0.587, 0.114)), 0.0, 0.0, 1.0);\n"
"}\n";

/* ---------------------------------------------------------------- */
/* Lucas-Kanade 一步迭代                                             */
/*                                                                   */
/* flow 定义：f(p) = 让 prev 与 cur 对齐的位移（uv 空间）              */
/*            prev(p + f) ~= cur(p)                                  */
/*                                                                   */
/* 输出：f + df，df 由 5x5 加权窗口解 2x2 线性系统得到。              */
/* uStep 用于限幅（不同金字塔层用的步长不同）。                       */
/*                                                                   */
/* 符号推导（别凭直觉改，这里极易搞反，症状是光流朝反方向跑）：        */
/*   残差 e(df) = prev(p+f+df) - cur(p)                              */
/*             ~= It + (Ix, Iy)·df,  It = prev(p+f) - cur(p)         */
/*   最小化 sum w·e²  =>  A·df = -b,                                 */
/*     A = [[gxx,gxy],[gxy,gyy]],  b = (sum w·Ix·It, sum w·Iy·It)    */
/*   本 shader 里为了少一次减法写成 it = cur - prev = -It，           */
/*   于是 gxt = -b.x、gyt = -b.y，代回 -A⁻¹b 后负号抵消：             */
/*     df = ( (gyy·gxt - gxy·gyt), (gxx·gyt - gxy·gxt) ) / det        */
/* ---------------------------------------------------------------- */
static const char *FG_FS_LK =
"#version 300 es\n"
"precision highp float;\n"
"uniform sampler2D uPrev;\n"
"uniform sampler2D uCur;\n"
"uniform sampler2D uFlow;\n"
"uniform vec2  uTexel;\n"
"uniform float uStep;\n"
"in vec2 vUV;\n"
"out vec4 oColor;\n"
"void main() {\n"
"    vec2 f = texture(uFlow, vUV).rg;\n"
"    float gxx = 0.0, gxy = 0.0, gyy = 0.0, gxt = 0.0, gyt = 0.0;\n"
"    for (int dy = -2; dy <= 2; ++dy) {\n"
"        for (int dx = -2; dx <= 2; ++dx) {\n"
"            vec2 off = vec2(float(dx), float(dy)) * uTexel;\n"
"            float w = exp(-float(dx * dx + dy * dy) * 0.15);\n"
"            float l  = texture(uCur, vUV + off - vec2(uTexel.x, 0.0)).r;\n"
"            float r  = texture(uCur, vUV + off + vec2(uTexel.x, 0.0)).r;\n"
"            float u  = texture(uCur, vUV + off - vec2(0.0, uTexel.y)).r;\n"
"            float d  = texture(uCur, vUV + off + vec2(0.0, uTexel.y)).r;\n"
"            float ix = (r - l) * 0.5;\n"
"            float iy = (d - u) * 0.5;\n"
"            float it = texture(uCur,  vUV + off).r\n"
"                     - texture(uPrev, vUV + off + f).r;\n"
"            gxx += w * ix * ix;\n"
"            gxy += w * ix * iy;\n"
"            gyy += w * iy * iy;\n"
"            gxt += w * ix * it;\n"
"            gyt += w * iy * it;\n"
"        }\n"
"    }\n"
"    float det = gxx * gyy - gxy * gxy;\n"
"    vec2 df = vec2(0.0);\n"
"    if (abs(det) > 1e-7) {\n"
"        df = vec2(gyy * gxt - gxy * gyt, gxx * gyt - gxy * gxt) / det;\n"
"    }\n"
"    df = clamp(df, vec2(-uStep), vec2(uStep));\n"
"    oColor = vec4(f + df, 0.0, 1.0);\n"
"}\n";

/* ---------------------------------------------------------------- */
/* 合成：双向 warp + 遮挡倾向一侧                                    */
/*                                                                   */
/* t = 0 取 prev，t = 1 取 cur。两帧差异大（遮挡/新露出的区域）时，    */
/* 向时间上更近的一侧靠，避免半透明重影。                             */
/*                                                                   */
/* 采样位置推导（prev(p+f) ~= cur(p)，场景点在 t 时刻位于 p+f·(1−t)）：*/
/*   输出像素 q 处取 prev  ->  q + f·t                                */
/*   输出像素 q 处取 cur   ->  q - f·(1−t)                            */
/* 2× 插帧只用到 t=0.5，此时两种写法等价；写成上面这样是为了 3× 时     */
/* （t=1/3、2/3）也正确 —— 写反的话 3× 会明显像在倒放。               */
/* ---------------------------------------------------------------- */
static const char *FG_FS_COMPOSE =
"#version 300 es\n"
"precision highp float;\n"
"uniform sampler2D uPrev;\n"
"uniform sampler2D uCur;\n"
"uniform sampler2D uFlow;\n"
"uniform float uT;\n"
"in vec2 vUV;\n"
"out vec4 oColor;\n"
"void main() {\n"
"    vec2 f = texture(uFlow, vUV).rg;\n"
"    vec3 a = texture(uPrev, vUV + f * uT).rgb;\n"
"    vec3 b = texture(uCur,  vUV - f * (1.0 - uT)).rgb;\n"
"    float err = length(a - b);\n"
"    float conf = clamp(1.0 - err * 2.0, 0.0, 1.0);\n"
"    vec3 mixed = mix(a, b, uT);\n"
"    vec3 near  = (uT < 0.5) ? a : b;\n"
"    oColor = vec4(mix(near, mixed, conf), 1.0);\n"
"}\n";

/* ---------------------------------------------------------------- */
/* 直通复制（把采集纹理画到默认帧缓冲，M1 的「真实帧」呈现）          */
/* ---------------------------------------------------------------- */
static const char *FG_FS_COPY =
"#version 300 es\n"
"precision highp float;\n"
"uniform sampler2D uSrc;\n"
"in vec2 vUV;\n"
"out vec4 oColor;\n"
"void main() { oColor = vec4(texture(uSrc, vUV).rgb, 1.0); }\n";

#endif /* FG_SHADERS_H */
