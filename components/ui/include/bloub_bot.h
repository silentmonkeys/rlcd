// bloub_bot —— bloub 机器人的 C 运行时引擎 + 1-bit 光栅化
//
// bloub（/home/chen/demo/bloub，MIT）的 src/bot/ 是无 DOM / 无时钟的纯函数
// 引擎，本目录把「圆形身体状态子集」（idle/wink/wide/thinking/sleep）移植为 C：
//   - 引擎：状态机（setState 单历史槽淡入 + reset）、眨眼/呼吸/视线漂移
//   - 渲染：Catmull-Rom 细分折线 → 扫描线填充进 1-bit 位图（1=墨），
//     眼睛=胶囊挖洞（clip 进身体），墨点=实心圆
// 常数全部是对参考视频的实测值，与 TS 侧逐字段一致，勿取整（见 bloub 的
// CLAUDE.md）。跳过的 TS 模块及理由：
//   - eyefit：圆形身体恒返零偏移（eyefit.ts:436 注释明确 circle → NUL）
//   - skins/expressions/look/setShape：定制层，本页用不到
//   - decor 圆弧/notify：1-bit 下渐变无意义，且所选状态不带圆弧
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

// 与 TS 侧 repere.ts / profiles.ts 一致
#define BLOUB_SAMPLES 64
#define BLOUB_R       100.0f    // 静止半径（viewBox 单位）
#define BLOUB_VB      158.0f    // 半边 viewBox：decor 最多到 1.4×R=140 < 158

// 状态子集：全部圆形身体（无 profiles/skins 依赖）
typedef enum {
    BLOUB_IDLE = 0,
    BLOUB_WINK,
    BLOUB_WIDE,
    BLOUB_THINKING,
    BLOUB_SLEEP,
    BLOUB_STATE_COUNT
} bloub_state_t;

// sample() 的输出：像素渲染所需的全部几何（R 单位，y 向下，原点=身体中心）
typedef struct {
    // 身体：BLOUB_SAMPLES 个轮廓点（已含呼吸/漂移/混合后的最终形变）
    float pts[BLOUB_SAMPLES][2];
    // 眼睛：胶囊局部尺寸（R 单位，中心对称）+ SVG matrix(a,b,c,d,e,f)
    // eye <0 阈值由渲染器决定：alpha<0.5 不画（1-bit 下淡入淡出=出现/消失）
    int n_eyes;
    struct {
        float w, h;                // 胶囊未变形宽高（R 单位）
        float a, b, c, d, e, f;    // matrix
        float alpha;
    } eye[2];
    // 墨点/粒子（实心圆；TS 侧带路径 d 的异形点本项目状态集用不到）
    int n_dots;
    struct { float x, y, r, opacity; } dot[3];
    bool dots_behind;              // true=点画在身体后面（只在轮廓外可见）
} bloub_scene_t;

// 引擎句柄（不透明；内部静态分配，单实例足够 UI 用）
typedef struct bloub_engine bloub_engine_t;

bloub_engine_t *bloub_engine_create(void);   // 初始 idle
void bloub_engine_free(bloub_engine_t *e);

// 语义与 TS BotEngine 一致：set_state 保留前一状态淡入（单历史槽），
// reset 丢弃历史直接落位（蒙太奇回绕用）。
void bloub_engine_set_state(bloub_engine_t *e, bloub_state_t st, float now);
void bloub_engine_reset(bloub_engine_t *e, bloub_state_t st, float now);
bloub_state_t bloub_engine_state(const bloub_engine_t *e);

// 采样一帧。now 是引擎时钟秒（调用方自己积累 dt）。
void bloub_engine_sample(bloub_engine_t *e, float now, bloub_scene_t *out);

// -------- 渲染 --------------------------------------------------------
// 1-bit 位图：bits 由调用方提供，stride=(w+7)/8，bit=1 是墨（黑）。
typedef struct {
    int      w, h;
    uint8_t *bits;                 // stride*h 字节，MSB 先行
} bloub_bitmap_t;

// 场景 → 位图。视口映射：x∈[-VB,VB] → [0,w)，y 同理。
// 透明度阈值 0.5：与「SVG 真值 → 128 阈值二值化」逐像素对齐（golden 比对用）。
void bloub_render(const bloub_scene_t *sc, bloub_bitmap_t *bm);

// -------- 蒙太奇（与 tools/bloub_sampler_entry.ts 的 DEFAULT_BLOCKS 一致）----
typedef struct {
    bloub_state_t state;
    float         duration;
} bloub_block_t;
extern const bloub_block_t BLOUB_MONTAGE[];
extern const int           BLOUB_MONTAGE_N;
// t（秒，自动回绕）→ 所在块索引；*elapsed 出参返回块内时间
int bloub_montage_block_at(float t, float *elapsed);

#ifdef __cplusplus
}
#endif
