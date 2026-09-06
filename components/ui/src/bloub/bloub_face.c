// bloub_face.c —— TS face.ts 的 C 移植：球面眼位姿 + 静止生活（眨眼/呼吸/漂移）
//
// 眼睛画在球面上：视频实测离边最近的眼睛宽度是另一只的 0.69 倍，恰好是球面
// 点在 z=0.669 处的透视因子 —— 所以给双眼建真实的头部朝向（正交投影），
// 压缩与倾斜自然成立。常数来自对参考帧的拟合（残差 ~1px / R=190px）。
#include "bloub_internal.h"

#include <math.h>

// 半偏差（双眼总分离 ~31 度）与静止眼尺寸（R 单位）
#define EYE_SPLIT 15.46f
#define EYE_W     0.186f
#define EYE_H     0.412f
// 静止头朝向（实测）
static const bloub_gaze_t REST_GAZE = { 28.49f, 28.62f, -13.0f };

// 眨眼时长（实测 1~2 帧 @10fps）
#define BLINK_DUR 0.18f

static float deg2rad(float d) { return d * (float)M_PI / 180.0f; }

// 两个正交向量在自己的平面内旋转（分量式，与 TS spin 一致）
static void spin3(float u[3], float v[3], float angle, float ou[3], float ov[3])
{
    float c = cosf(angle), s = sinf(angle);
    for (int i = 0; i < 3; i++) {
        ou[i] = u[i] * c + v[i] * s;
        ov[i] = v[i] * c - u[i] * s;
    }
}

void bloub_eye_poses(const bloub_gaze_t *gaze, float scale, float split,
                     bloub_eye_pose_t out[2])
{
    float f[3]     = { 0, 0, 1 };
    float right[3] = { 1, 0, 0 };
    float down[3]  = { 0, 1, 0 };
    float nf[3], nr[3], nd[3];

    // yaw：forward 倒向 right；pitch：forward 倒向上（远离 down）；
    // roll：头在自己的平面里歪
    spin3(f, right, deg2rad(gaze->yaw), nf, nr);
    spin3(down, nf, deg2rad(gaze->pitch), nd, nf);
    spin3(nr, nd, deg2rad(gaze->roll), nr, nd);

    // index 0 = 内侧眼（side=-1），1 = 外侧（+1）
    for (int i = 0; i < 2; i++) {
        float side = (i == 0) ? -1.0f : 1.0f;
        float ef[3], er[3];
        spin3(nf, nr, deg2rad(split * side), ef, er);
        out[i].x = ef[0] * scale;
        out[i].y = ef[1] * scale;
        out[i].a = er[0];
        out[i].b = er[1];
        out[i].c = nd[0];
        out[i].d = nd[1];
        out[i].depth = ef[2];
    }
}

// -------- 眨眼日程：预生成的确定性序列（与 TS 相同 seed） --------------
#define BLINK_TABLE_MAX 512
static float s_blinks[BLINK_TABLE_MAX];
static int   s_blink_n = 0;

static void blinks_build(void)
{
    if (s_blink_n) return;
    uint32_t rng = 0x5eedu;
    float t = 1.4f;
    while (t < 900.0f && s_blink_n < BLINK_TABLE_MAX - 2) {
        s_blinks[s_blink_n++] = t;
        t += 1.9f + bloub_rng_next(&rng) * 2.7f;   // 1.9~4.6s 一眨
        if (bloub_rng_next(&rng) < 0.18f) {        // 偶发双眨
            s_blinks[s_blink_n++] = t;
            t += 0.24f;
        }
    }
}

static float blink_lid(float t)
{
    blinks_build();
    for (int i = 0; i < s_blink_n; i++) {
        float start = s_blinks[i];
        if (t < start) break;
        float k = (t - start) / BLINK_DUR;
        if (k >= 0.0f && k <= 1.0f) {
            // 快闭慢开
            return (k < 0.45f) ? (1.0f - k / 0.45f) : ((k - 0.45f) / 0.55f);
        }
    }
    return 1.0f;
}

void bloub_liveliness(float t, const bloub_life_opt_t *opt, bloub_life_t *out)
{
    float wander = opt->wander;
    out->d_yaw   = (bloub_loop_noise(t, 11.3f, 0.4f) * 5.5f +
                    bloub_loop_noise(t, 3.7f, 2.1f) * 1.6f) * wander;
    out->d_pitch = (bloub_loop_noise(t, 9.1f, 1.3f) * 4.2f +
                    bloub_loop_noise(t, 4.3f, 0.7f) * 1.3f) * wander;
    out->d_roll  = bloub_loop_noise(t, 13.7f, 3.2f) * 2.2f * wander;
    out->lid     = opt->blink ? blink_lid(t) : 1.0f;
    // 静止时视频几乎不动（中心 ±0.003）：只留一点点漂移避免死图
    out->drift_x = opt->float_ ? bloub_loop_noise(t, 7.9f, 1.9f) * 0.006f : 0.0f;
    out->drift_y = opt->float_ ? bloub_loop_noise(t, 5.3f, 0.3f) * 0.007f : 0.0f;
    // 宽度恒定，只有纵向极轻微呼吸
    out->breath  = opt->float_ ? 1.0f + sinf((t / 3.4f) * (float)M_PI * 2.0f) * 0.005f : 1.0f;
}

float bloub_blink_scale(float lid)
{
    // 眨眼是屏幕坐标系下的纵向压扁（bbox 宽度不变，高度掉到 ~0.35）
    return 0.06f + 0.94f * bloub_clampf(lid, 0.0f, 1.0f);
}

// 静止脸基准值（states.c 的 base() 用）
void bloub_face_rest(bloub_gaze_t *gaze, float *split, bloub_eye_cfg_t eyes[2])
{
    gaze->yaw = REST_GAZE.yaw;
    gaze->pitch = REST_GAZE.pitch;
    gaze->roll = REST_GAZE.roll;
    *split = EYE_SPLIT;
    eyes[0].w = EYE_W; eyes[0].h = EYE_H; eyes[0].open = 1.0f; eyes[0].tilt = 0.0f;
    eyes[1] = eyes[0];
}
