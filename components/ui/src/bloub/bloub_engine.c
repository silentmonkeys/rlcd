// bloub_engine.c —— TS engine.ts 的 C 移植（无时钟状态机）
//
// sample(now) 是纯时间函数：暂停/恢复/跳时都给出确定的图像。只保留一格
// 历史（prev）：状态切换时把被离开的状态按其自身时间评估后做淡入混合；
// 「淡入途中再切换」则把当前合成姿态冻结为起点（setState 的 depart_fige），
// 否则会出现 26~43px 的跳变（bloub 实测），故原样移植。
//
// 相对 TS 裁剪：无 setShape / setExpression / setLook / eyefit 偏移
// （本子集全部是圆形身体，eyefit 恒为零偏移），无 decor 圆弧与 notify。
#include "bloub_internal.h"

#include <math.h>
#include <stdlib.h>

struct bloub_engine {
    bloub_state_t cur;
    int           prev;          // -1 = 无历史
    int           depart_fige;   // true = 冻结的起点姿态有效
    bloub_pose_t  fige;
    float         t_cur;
    float         t_prev;
    float         blink_at;
};

bloub_engine_t *bloub_engine_create(void)
{
    bloub_engine_t *e = calloc(1, sizeof(bloub_engine_t));
    if (!e) return NULL;
    e->cur = BLOUB_IDLE;
    e->prev = -1;
    e->depart_fige = 0;
    e->t_cur = 0.0f;
    e->t_prev = 0.0f;
    e->blink_at = -10.0f;
    return e;
}

void bloub_engine_free(bloub_engine_t *e)
{
    free(e);
}

bloub_state_t bloub_engine_state(const bloub_engine_t *e)
{
    return e->cur;
}

// 当前状态的姿态（不含淡入混合与生活层）
static void posed(const bloub_engine_t *e, bloub_state_t id, float local_t, bloub_pose_t *out)
{
    bloub_state_def(id)->pose(local_t, out);
}

// 淡入起点：冻结姿态优先，否则是被离开状态按其自身时间的评估（仍在动画中）
static void origine(const bloub_engine_t *e, float now, bloub_pose_t *out)
{
    if (e->depart_fige) {
        *out = e->fige;
        return;
    }
    if (e->prev < 0) {
        posed(e, e->cur, 0.0f, out);
        return;
    }
    float local = now - e->t_prev;
    if (local < 0.0f) local = 0.0f;
    posed(e, (bloub_state_t)e->prev, local, out);
}

// 合成姿态 = 当前状态姿态 ⊕ 淡入混合（不含生活层）。
// 与 TS 一致：没有历史且无冻结起点时不混合（origine 返回 null 的情形）。
// 注意 out 与 b 可能是同一对象（见调用处），混合必须落进独立临时量。
static void pose_composee(const bloub_engine_t *e, float now, bloub_pose_t *out)
{
    static bloub_pose_t mixed;
    const bloub_state_def_t *def = bloub_state_def(e->cur);
    float since = now - e->t_cur;
    if (since < 0.0f) since = 0.0f;
    posed(e, e->cur, since, out);
    if (since >= def->morph) return;
    if (e->prev < 0 && !e->depart_fige) return;

    bloub_pose_t orig;
    origine(e, now, &orig);
    float ratio = bloub_ease_out_quint(bloub_clampf(since / def->morph, 0.0f, 1.0f));
    bloub_pose_blend(&orig, out, ratio, &mixed);
    *out = mixed;
}

void bloub_engine_reset(bloub_engine_t *e, bloub_state_t st, float now)
{
    e->cur = st;
    e->prev = -1;
    e->depart_fige = 0;
    e->t_cur = now;
    e->t_prev = now;
    e->blink_at = -10.0f;
}

void bloub_engine_set_state(bloub_engine_t *e, bloub_state_t st, float now)
{
    if (st == e->cur) return;
    // 淡入进行中再切换：冻结当前合成姿态作为新混合的起点
    const bloub_state_def_t *def = bloub_state_def(e->cur);
    int en_plein = (e->prev >= 0) && (now - e->t_cur < def->morph);
    if (en_plein) {
        pose_composee(e, now, &e->fige);
        e->depart_fige = 1;
    } else {
        e->depart_fige = 0;
    }
    e->prev = e->cur;
    e->t_prev = e->t_cur;
    e->cur = st;
    e->t_cur = now;
    // 视频里每次换状态都被眨眼掩盖
    if (bloub_state_def(st)->blink_in) e->blink_at = now;
}

void bloub_engine_sample(bloub_engine_t *e, float now, bloub_scene_t *out)
{
    const bloub_state_def_t *def = bloub_state_def(e->cur);
    float since = now - e->t_cur;
    if (since < 0.0f) since = 0.0f;

    bloub_pose_t pose;
    posed(e, e->cur, since, &pose);

    // --- 淡入 -----------------------------------------------------------
    if (since < def->morph && (e->prev >= 0 || e->depart_fige)) {
        bloub_pose_t orig;
        origine(e, now, &orig);
        float ratio = bloub_ease_out_quint(bloub_clampf(since / def->morph, 0.0f, 1.0f));
        bloub_pose_t mixed;
        bloub_pose_blend(&orig, &pose, ratio, &mixed);
        pose = mixed;
    }

    // --- 静止生活 ---------------------------------------------------------
    int alive = pose.eye_alpha > 0.01f;
    bloub_life_opt_t opt = { alive ? 1.0f : 0.0f, alive, alive };
    bloub_life_t life;
    bloub_liveliness(now, &opt, &life);

    bloub_gaze_t gaze;
    gaze.yaw   = pose.gaze.yaw + life.d_yaw;
    gaze.pitch = pose.gaze.pitch + life.d_pitch;
    gaze.roll  = pose.gaze.roll + life.d_roll;

    // 换状态触发的强制眨眼（0.2s 内闭合再张开）
    float forced = bloub_clampf((now - e->blink_at) / 0.2f, 0.0f, 1.0f);
    float forced_lid = (forced < 1.0f) ? fabsf(forced * 2.0f - 1.0f) : 1.0f;
    float lid = life.lid;
    if (forced_lid < lid) lid = forced_lid;

    float off_x = pose.off_x + life.drift_x;
    float off_y = pose.off_y + life.drift_y;

    // --- 身体 -------------------------------------------------------------
    bloub_sil_t sil = pose.sil;
    sil.cx += off_x;
    sil.cy += off_y;
    sil.sy *= life.breath;
    bloub_sil_to_points(&sil, BLOUB_R, out->pts);

    // --- 眼睛 -------------------------------------------------------------
    out->n_eyes = 0;
    if (pose.eye_alpha > 0.01f) {
        bloub_eye_pose_t ep[2];
        bloub_eye_poses(&gaze, BLOUB_R, pose.split, ep);
        for (int i = 0; i < 2; i++) {
            if (ep[i].depth <= 0.02f) continue;
            const bloub_eye_cfg_t *cfg = &pose.eye[i];
            // 圆形身体：fit = 1（bloub_radius_at_angle 对全 1 半径恒 1，
            // 保留计算以贴住 TS 的通用路径）
            float fit = bloub_radius_at_angle(pose.sil.radii,
                                              atan2f(ep[i].y, ep[i].x) - pose.sil.rot);
            // 倾斜：切向基上再绕平面转 phi（我们的状态 tilt=0，保留通用式）
            float phi = cfg->tilt * (float)M_PI / 180.0f;
            float cp = cosf(phi), sp = sinf(phi);
            float ax = ep[i].a * cp + ep[i].c * sp;
            float ay = ep[i].b * cp + ep[i].d * sp;
            float cx2 = -ep[i].a * sp + ep[i].c * cp;
            float cy2 = -ep[i].b * sp + ep[i].d * cp;
            // 眨眼是屏幕系纵向压扁，在矩阵 b/d 分量之后合成
            float k = bloub_blink_scale(lid < cfg->open ? lid : cfg->open);
            float alpha = pose.eye_alpha * bloub_clampf(ep[i].depth / 0.12f, 0.0f, 1.0f);
            if (alpha < 0.01f) continue;

            out->eye[out->n_eyes].w = cfg->w * BLOUB_R;
            out->eye[out->n_eyes].h = cfg->h * BLOUB_R;
            out->eye[out->n_eyes].a = ax;
            out->eye[out->n_eyes].b = ay * k;
            out->eye[out->n_eyes].c = cx2;
            out->eye[out->n_eyes].d = cy2 * k;
            out->eye[out->n_eyes].e = ep[i].x * fit + off_x * BLOUB_R;
            out->eye[out->n_eyes].f = ep[i].y * fit + off_y * BLOUB_R;
            out->eye[out->n_eyes].alpha = alpha;
            out->n_eyes++;
        }
    }

    // --- 墨点 -------------------------------------------------------------
    out->n_dots = 0;
    for (int i = 0; i < pose.n_dots; i++) {
        const float op = pose.dot[i].opacity;
        const float r = pose.dot[i].r;
        if (op <= 0.01f || r <= 0.0005f) continue;
        out->dot[out->n_dots].x = (pose.dot[i].x + off_x) * BLOUB_R;
        out->dot[out->n_dots].y = (pose.dot[i].y + off_y) * BLOUB_R;
        out->dot[out->n_dots].r = r * BLOUB_R;
        out->dot[out->n_dots].opacity = op;
        out->n_dots++;
    }
    out->dots_behind = pose.dots_behind;
}
