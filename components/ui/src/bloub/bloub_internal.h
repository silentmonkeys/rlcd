// bloub_internal —— C 移植内部声明（bloub_math/face/states/engine 共享）
// 对外 API 只在 include/bloub_bot.h。
#pragma once

#include "bloub_bot.h"

// ------------ bloub_math.c（对应 TS math.ts） -------------------------
#define BLOUB_TAU 6.28318530717958647692f

float bloub_clampf(float v, float lo, float hi);
float bloub_lerpf(float a, float b, float t);
float bloub_ease_out_cubic(float t);
float bloub_ease_in_out_cubic(float t);
float bloub_ease_out_quint(float t);
// 一维周期噪声：在 period 上无缝循环（视线漂移用），seed 是弧度偏置
float bloub_loop_noise(float t, float period, float seed);
// mulberry32（与 TS createRng 逐位一致的确定性序列）
float bloub_rng_next(uint32_t *state);

// ------------ bloub_face.c（对应 TS face.ts） -------------------------
typedef struct { float yaw, pitch, roll; } bloub_gaze_t;   // 度

typedef struct {
    float w, h;       // R 单位
    float open;       // 1=睁 0=闭
    float tilt;       // 度
} bloub_eye_cfg_t;

typedef struct {
    float x, y;          // 眼心（R 单位，已乘 scale）
    float a, b, c, d;    // 切向 2x2（SVG matrix 前四元）
    float depth;         // >0 才可见（法向 z 分量）
} bloub_eye_pose_t;

// 双眼位姿：视线 gaze（度）、split 半偏差（度）。index 0=内侧 1=外侧。
void bloub_eye_poses(const bloub_gaze_t *gaze, float scale, float split,
                     bloub_eye_pose_t out[2]);
// 静止生活：视线漂移 / 眨眼 / 漂浮 / 呼吸
typedef struct {
    float d_yaw, d_pitch, d_roll;   // 度
    float lid;                      // 1=睁开 0=闭合
    float drift_x, drift_y;         // R 单位
    float breath;                   // 纵向呼吸系数
} bloub_life_t;

typedef struct {
    float wander;   // 0..1 漂移幅度
    bool  blink;    // 是否眨眼
    bool  float_;   // 是否漂浮/呼吸
} bloub_life_opt_t;

void bloub_liveliness(float t, const bloub_life_opt_t *opt, bloub_life_t *out);
float bloub_blink_scale(float lid);   // 眨眼 → 纵向压扁系数
// 静止基准：REST_GAZE / EYE_SPLIT / EYE_W·H（states.c 的 base() 用）
void bloub_face_rest(bloub_gaze_t *gaze, float *split, bloub_eye_cfg_t eyes[2]);

// ------------ bloub_states.c（对应 TS states.ts 子集） ----------------
typedef struct {
    float radii[BLOUB_SAMPLES];
    float rot;        // 弧度
    float cx, cy;     // R 单位
    float sx, sy;
} bloub_sil_t;

typedef struct {
    bloub_sil_t sil;
    float off_x, off_y;
    bloub_gaze_t gaze;
    float split;
    bloub_eye_cfg_t eye[2];
    float eye_alpha;
    int n_dots;
    struct { float x, y, r, opacity; } dot[3];
    bool dots_behind;
} bloub_pose_t;

typedef struct {
    bloub_state_t id;
    float duration;                    // 视频实测保持时长
    float morph;                       // 入场淡入时长
    bool  blink_in;                    // 入场被眨眼掩盖
    void (*pose)(float t, bloub_pose_t *out);   // t = 块内时间
} bloub_state_def_t;

const bloub_state_def_t *bloub_state_def(bloub_state_t id);
void bloub_sil_circle(bloub_sil_t *s, float radius);
void bloub_sil_blend(const bloub_sil_t *a, const bloub_sil_t *b, float t, bloub_sil_t *out);
void bloub_pose_blend(const bloub_pose_t *a, const bloub_pose_t *b, float t, bloub_pose_t *out);
void bloub_pose_default(bloub_pose_t *p);          // base()：圆 + 静止脸
void bloub_sil_to_points(const bloub_sil_t *s, float scale, float out[][2]);
float bloub_radius_at_angle(const float *radii, float angle);
