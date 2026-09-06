// bloub_states.c —— TS states.ts 的 C 移植（圆形身体状态子集）
//
// 每个状态一个 pose(t) 纯函数；形状一律用径向轮廓 r(θ) 表达（64 样本），
// 所以任意两个形状的 morph 就是半径线性插值。常数是视频实测值，勿取整。
#include "bloub_internal.h"

#include <math.h>

// thinking 的三点（decor.ts 实测值，R 单位）
#define DOT_X_0  (-0.557f)
#define DOT_X_1  (-0.013f)
#define DOT_X_2  (0.532f)
#define DOT_R    0.165f
#define DOT_PEAK 1.25f

// -------- 轮廓 / 姿态工具 --------------------------------------------

void bloub_sil_circle(bloub_sil_t *s, float radius)
{
    for (int i = 0; i < BLOUB_SAMPLES; i++) s->radii[i] = radius;
    s->rot = 0.0f;
    s->cx = 0.0f;
    s->cy = 0.0f;
    s->sx = 1.0f;
    s->sy = 1.0f;
}

void bloub_sil_blend(const bloub_sil_t *a, const bloub_sil_t *b, float t, bloub_sil_t *out)
{
    for (int i = 0; i < BLOUB_SAMPLES; i++)
        out->radii[i] = bloub_lerpf(a->radii[i], b->radii[i], t);
    // 旋转走最短弧，避免 +170° → -170° 时绕远路
    float dr = b->rot - a->rot;
    while (dr > (float)M_PI)  dr -= BLOUB_TAU;
    while (dr < -(float)M_PI) dr += BLOUB_TAU;
    out->rot = a->rot + dr * t;
    out->cx = bloub_lerpf(a->cx, b->cx, t);
    out->cy = bloub_lerpf(a->cy, b->cy, t);
    out->sx = bloub_lerpf(a->sx, b->sx, t);
    out->sy = bloub_lerpf(a->sy, b->sy, t);
}

void bloub_pose_default(bloub_pose_t *p)
{
    bloub_sil_circle(&p->sil, 1.0f);
    p->off_x = 0.0f;
    p->off_y = 0.0f;
    float split;
    bloub_face_rest(&p->gaze, &split, p->eye);
    p->split = split;
    p->eye_alpha = 1.0f;
    p->n_dots = 0;
    p->dots_behind = false;
}

void bloub_pose_blend(const bloub_pose_t *a, const bloub_pose_t *b, float t, bloub_pose_t *out)
{
    bloub_sil_blend(&a->sil, &b->sil, t, &out->sil);
    out->off_x = bloub_lerpf(a->off_x, b->off_x, t);
    out->off_y = bloub_lerpf(a->off_y, b->off_y, t);
    out->gaze.yaw   = bloub_lerpf(a->gaze.yaw,   b->gaze.yaw,   t);
    out->gaze.pitch = bloub_lerpf(a->gaze.pitch, b->gaze.pitch, t);
    out->gaze.roll  = bloub_lerpf(a->gaze.roll,  b->gaze.roll,  t);
    out->split = bloub_lerpf(a->split, b->split, t);
    for (int i = 0; i < 2; i++) {
        out->eye[i].w    = bloub_lerpf(a->eye[i].w,    b->eye[i].w,    t);
        out->eye[i].h    = bloub_lerpf(a->eye[i].h,    b->eye[i].h,    t);
        out->eye[i].open = bloub_lerpf(a->eye[i].open, b->eye[i].open, t);
        out->eye[i].tilt = bloub_lerpf(a->eye[i].tilt, b->eye[i].tilt, t);
    }
    out->eye_alpha = bloub_lerpf(a->eye_alpha, b->eye_alpha, t);
    // 装饰点用透明度交叉淡化（离场 ×(1-t)，进场 ×t），几何不混合
    out->n_dots = 0;
    for (int i = 0; i < a->n_dots && out->n_dots < 3; i++) {
        out->dot[out->n_dots] = a->dot[i];
        out->dot[out->n_dots].opacity = a->dot[i].opacity * (1.0f - t);
        out->n_dots++;
    }
    for (int i = 0; i < b->n_dots && out->n_dots < 3; i++) {
        out->dot[out->n_dots] = b->dot[i];
        out->dot[out->n_dots].opacity = b->dot[i].opacity * t;
        out->n_dots++;
    }
    out->dots_behind = (t < 0.5f) ? a->dots_behind : b->dots_behind;
}

void bloub_sil_to_points(const bloub_sil_t *s, float scale, float out[][2])
{
    float cr = cosf(s->rot), sr = sinf(s->rot);
    for (int i = 0; i < BLOUB_SAMPLES; i++) {
        float th = (float)i / BLOUB_SAMPLES * BLOUB_TAU;
        float x = s->radii[i] * cosf(th);
        float y = s->radii[i] * sinf(th);
        float rx = x * cr - y * sr;      // 先旋转
        float ry = x * sr + y * cr;
        out[i][0] = (rx * s->sx + s->cx) * scale;   // 再 squash + 平移，最后缩放
        out[i][1] = (ry * s->sy + s->cy) * scale;
    }
}

float bloub_radius_at_angle(const float *radii, float angle)
{
    // θ=0 朝右、顺时针增大（y 向下）—— 与 TS 一致
    float u = angle / BLOUB_TAU;
    u = u - floorf(u);
    float ft = u * BLOUB_SAMPLES;
    int i = (int)ft;
    if (i < 0) i = 0;
    float frac = ft - (float)i;
    int j = (i + 1) % BLOUB_SAMPLES;
    return bloub_lerpf(radii[i % BLOUB_SAMPLES], radii[j], frac);
}

// -------- 状态定义 ----------------------------------------------------

// thinking：脉冲波从左向右扫过三个点
static float dot_pulse(float t, int index)
{
    float p = (t - index * 0.5f) / 1.5f;
    p = p - floorf(p);
    float k = (p < 0.5f) ? (0.5f - 0.5f * cosf(p * BLOUB_TAU)) : 0.0f;
    return bloub_clampf(k * 2.0f, 0.0f, 1.0f);
}

static void pose_idle(float t, bloub_pose_t *out)
{
    (void)t;
    bloub_pose_default(out);
}

static void pose_wink(float t, bloub_pose_t *out)
{
    (void)t;
    bloub_pose_default(out);
    // 闭上的眼不是「睁眼压扁」：是比睁眼更宽的横杠（0.447 vs 0.236）
    out->gaze.yaw = -5.37f;
    out->gaze.pitch = 4.55f;
    out->gaze.roll = 6.7f;
    out->split = 16.25f;
    out->eye[0].w = 0.236f; out->eye[0].h = 0.464f; out->eye[0].open = 1.0f;
    out->eye[1].w = 0.447f; out->eye[1].h = 0.089f; out->eye[1].open = 1.0f;
}

static void pose_wide(float t, bloub_pose_t *out)
{
    (void)t;
    bloub_pose_default(out);
    out->gaze.yaw = 6.92f;
    out->gaze.pitch = -21.96f;
    out->gaze.roll = 11.6f;
    out->split = 18.43f;
    out->eye[0].w = out->eye[1].w = 0.356f;
    out->eye[0].h = out->eye[1].h = 0.875f;
}

static void pose_thinking(float t, bloub_pose_t *out)
{
    bloub_pose_default(out);
    // 球变成中间那个点：morph 保持连续
    float mid = dot_pulse(t, 1);
    bloub_sil_circle(&out->sil, DOT_R * (1.0f + (DOT_PEAK - 1.0f) * mid));
    out->sil.cx = DOT_X_1;
    out->eye_alpha = 0.0f;
    // 两侧的点从身体侧面「长」出来：先融合 1-2 帧再脱离（实测）
    float emerge = 0.3f + 0.7f * bloub_ease_out_cubic(bloub_clampf(t / 0.3f, 0.0f, 1.0f));
    out->n_dots = 2;
    for (int i = 0; i < 2; i++) {
        int xi = (i == 0) ? 0 : 2;
        float x = (xi == 0) ? DOT_X_0 : DOT_X_2;
        float k = dot_pulse(t, xi);
        out->dot[i].x = x * emerge;
        out->dot[i].y = 0.0f;
        out->dot[i].r = DOT_R * (1.0f + (DOT_PEAK - 1.0f) * k);
        out->dot[i].opacity = 0.55f + 0.45f * k;
    }
}

static void pose_sleep(float t, bloub_pose_t *out)
{
    bloub_pose_default(out);
    // 竖直弹跳实测：±0.19 围绕 +0.11，周期 0.6s
    bloub_sil_circle(&out->sil, 0.1585f);
    out->sil.cy = 0.11f + sinf(t * (BLOUB_TAU / 0.6f)) * 0.19f;
    out->eye_alpha = 0.0f;
}

static const bloub_state_def_t STATES[BLOUB_STATE_COUNT] = {
    { BLOUB_IDLE,     2.4f, 0.45f, false, pose_idle },
    { BLOUB_WINK,     1.6f, 0.30f, true,  pose_wink },
    { BLOUB_WIDE,     1.8f, 0.55f, true,  pose_wide },
    { BLOUB_THINKING, 2.6f, 0.40f, true,  pose_thinking },
    { BLOUB_SLEEP,    2.4f, 0.50f, false, pose_sleep },
};

const bloub_state_def_t *bloub_state_def(bloub_state_t id)
{
    if (id < 0 || id >= BLOUB_STATE_COUNT) return &STATES[BLOUB_IDLE];
    return &STATES[id];
}

// -------- 蒙太奇（与 tools/bloub_sampler_entry.ts DEFAULT_BLOCKS 一致）----
const bloub_block_t BLOUB_MONTAGE[] = {
    { BLOUB_IDLE,     2.4f },
    { BLOUB_WINK,     1.6f },
    { BLOUB_WIDE,     1.8f },
    { BLOUB_THINKING, 2.6f },
    { BLOUB_SLEEP,    2.4f },
    { BLOUB_IDLE,     1.8f },
};
const int BLOUB_MONTAGE_N = (int)(sizeof(BLOUB_MONTAGE) / sizeof(BLOUB_MONTAGE[0]));

int bloub_montage_block_at(float t, float *elapsed)
{
    float total = 0.0f;
    for (int i = 0; i < BLOUB_MONTAGE_N; i++) total += BLOUB_MONTAGE[i].duration;
    float wrapped = t;
    if (t < 0.0f || t >= total) {
        wrapped = t - floorf(t / total) * total;
        if (wrapped >= total) wrapped = 0.0f;
    }
    float acc = 0.0f;
    for (int i = 0; i < BLOUB_MONTAGE_N; i++) {
        if (wrapped < acc + BLOUB_MONTAGE[i].duration) {
            if (elapsed) *elapsed = wrapped - acc;
            return i;
        }
        acc += BLOUB_MONTAGE[i].duration;
    }
    if (elapsed) *elapsed = 0.0f;
    return BLOUB_MONTAGE_N - 1;
}
