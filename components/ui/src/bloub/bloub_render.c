// bloub_render.c —— 场景 → 1-bit 位图（扫描线填充，偶奇规则）
//
// 合成顺序对齐 BloubBot.vue 的图层（本项目状态子集）：
//   dots_behind=true : 墨点 → 身体（眼洞挖穿，点不会从眼洞里露出来）
//   dots_behind=false: 身体（挖洞）→ 墨点叠在最上（thinking 的三点在前）
// 透明度阈值 0.5：眼睛 alpha<0.5 不画、点 opacity<0.5 不画 —— 与「SVG 真值
// 过 128 阈值二值化」的判定逐像素一致，golden 比对才对得上。
#include "bloub_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// Catmull-Rom 细分倍数：64 点 ×SUBDIV → 平滑折线
#define SUBDIV 4
#define MAX_POLY (BLOUB_SAMPLES * SUBDIV)
#define CAPSULE_PTS 40   // 胶囊轮廓：两个半圆各 21 点

// ---- 位图原语 ---------------------------------------------------------

static inline void bm_set(const bloub_bitmap_t *bm, int x, int y, bool set)
{
    if (x < 0 || x >= bm->w || y < 0 || y >= bm->h) return;
    uint8_t *row = bm->bits + (size_t)y * ((bm->w + 7) / 8);
    uint8_t m = (uint8_t)(0x80u >> (x & 7));
    if (set) row[x >> 3] |= m;
    else     row[x >> 3] &= (uint8_t)~m;
}

// 任意多边形扫描线填充/清除（偶奇规则；pts 为像素坐标，半开区间防顶点重计）
static void fill_polygon(const bloub_bitmap_t *bm, const float (*pts)[2], int n, bool set)
{
    if (n < 3) return;
    int stride = (bm->w + 7) / 8;
    static float xs[MAX_POLY + CAPSULE_PTS + 2];

    for (int y = 0; y < bm->h; y++) {
        float cy = (float)y + 0.5f;
        int nx = 0;
        for (int i = 0; i < n; i++) {
            float y1 = pts[i][1];
            float y2 = pts[(i + 1) % n][1];
            float dy = y2 - y1;
            if (dy == 0.0f) continue;
            if (cy >= fminf(y1, y2) && cy < fmaxf(y1, y2)) {
                float t = (cy - y1) / dy;
                xs[nx++] = pts[i][0] + t * (pts[(i + 1) % n][0] - pts[i][0]);
            }
        }
        if (nx < 2) continue;
        for (int i = 1; i < nx; i++) {         // 插入排序（nx 小）
            float v = xs[i];
            int j = i - 1;
            while (j >= 0 && xs[j] > v) { xs[j + 1] = xs[j]; j--; }
            xs[j + 1] = v;
        }
        uint8_t *row = bm->bits + (size_t)y * stride;
        for (int i = 0; i + 1 < nx; i += 2) {
            int x0 = (int)ceilf(xs[i] - 0.5f);
            int x1 = (int)ceilf(xs[i + 1] - 0.5f);   // 像素中心落在 [x0,x1)
            if (x0 < 0) x0 = 0;
            if (x1 > bm->w) x1 = bm->w;
            for (int x = x0; x < x1; x++) {
                uint8_t m = (uint8_t)(0x80u >> (x & 7));
                if (set) row[x >> 3] |= m;
                else     row[x >> 3] &= (uint8_t)~m;
            }
        }
    }
}

// 实心圆（逐行求弦）
static void fill_circle(const bloub_bitmap_t *bm, float cx, float cy, float r, bool set)
{
    int y0 = (int)floorf(cy - r), y1 = (int)ceilf(cy + r);
    if (y0 < 0) y0 = 0;
    if (y1 > bm->h) y1 = bm->h;
    for (int y = y0; y < y1; y++) {
        float dy = (float)y + 0.5f - cy;
        float d2 = r * r - dy * dy;
        if (d2 <= 0.0f) continue;
        float half = sqrtf(d2);
        int x0 = (int)ceilf(cx - half - 0.5f);
        int x1 = (int)ceilf(cx + half - 0.5f);
        for (int x = x0; x < x1; x++) bm_set(bm, x, y, set);
    }
}

// ---- 几何 ------------------------------------------------------------

// Catmull-Rom 闭合链细分。与 TS closedPath 的 cubic Bezier 几何等价：
// P0=p1, P1=p1+(p2-p0)/6, P2=p2-(p3-p1)/6, P3=p2 换成 Hermite 切线
// m1 = 3(P1-P0) = (p2-p0)/2, m2 = 3(P3-P2) = (p3-p1)/2。
static int catmull_rom_closed(const float (*p)[2], int n, float (*out)[2])
{
    int m = 0;
    for (int i = 0; i < n; i++) {
        const float *p0 = p[(i - 1 + n) % n];
        const float *p1 = p[i];
        const float *p2 = p[(i + 1) % n];
        const float *p3 = p[(i + 2) % n];
        float m1x = (p2[0] - p0[0]) * 0.5f;
        float m1y = (p2[1] - p0[1]) * 0.5f;
        float m2x = (p3[0] - p1[0]) * 0.5f;
        float m2y = (p3[1] - p1[1]) * 0.5f;
        for (int k = 0; k < SUBDIV; k++) {
            float u = (float)k / SUBDIV;
            float u2 = u * u, u3 = u2 * u;
            out[m][0] = (2*p1[0] - 2*p2[0] + m1x + m2x) * u3 +
                        (-3*p1[0] + 3*p2[0] - 2*m1x - m2x) * u2 +
                        m1x * u + p1[0];
            out[m][1] = (2*p1[1] - 2*p2[1] + m1y + m2y) * u3 +
                        (-3*p1[1] + 3*p2[1] - 2*m1y - m2y) * u2 +
                        m1y * u + p1[1];
            m++;
        }
    }
    return m;
}

// 胶囊（stadium）轮廓：局部中心对称，宽 2·hw 高 2·hh，端头圆半径 min(hw,hh)。
// 先在 R 单位生成并套 SVG matrix(a,b,c,d,e,f)，再映射到像素（sx==sy 时等价于
// viewBox 的等比缩放）。
static int capsule_polygon(float hw, float hh,
                           float a, float b, float c, float d, float e, float f,
                           float sx, float sy, float (*out)[2])
{
    if (hw < 0.01f) hw = 0.01f;
    if (hh < 0.01f) hh = 0.01f;
    float r = (hw < hh) ? hw : hh;
    float ext = (hw > hh) ? (hw - r) : 0.0f;    // 横向端头圆心偏移
    float exty = (hh > hw) ? (hh - r) : 0.0f;   // 纵向端头圆心偏移
    float local[CAPSULE_PTS + 2][2];            // 两个半圆各 (N/2+1) 点 = N+2
    int n = 0;

    if (hw >= hh) {
        for (int i = 0; i <= CAPSULE_PTS / 2; i++) {   // 右端半圆
            float ang = -M_PI_2 + (float)M_PI * (float)i / (CAPSULE_PTS / 2);
            local[n][0] = ext + cosf(ang) * r;
            local[n][1] = sinf(ang) * r;
            n++;
        }
        for (int i = 0; i <= CAPSULE_PTS / 2; i++) {   // 左端半圆
            float ang = M_PI_2 + (float)M_PI * (float)i / (CAPSULE_PTS / 2);
            local[n][0] = -ext + cosf(ang) * r;
            local[n][1] = sinf(ang) * r;
            n++;
        }
    } else {
        for (int i = 0; i <= CAPSULE_PTS / 2; i++) {   // 下端半圆
            float ang = (float)M_PI * (float)i / (CAPSULE_PTS / 2);
            local[n][0] = cosf(ang) * r;
            local[n][1] = exty + sinf(ang) * r;
            n++;
        }
        for (int i = 0; i <= CAPSULE_PTS / 2; i++) {   // 上端半圆
            float ang = (float)M_PI + (float)M_PI * (float)i / (CAPSULE_PTS / 2);
            local[n][0] = cosf(ang) * r;
            local[n][1] = -exty + sinf(ang) * r;
            n++;
        }
    }

    for (int i = 0; i < n; i++) {
        float lx = local[i][0], ly = local[i][1];
        out[i][0] = (a * lx + c * ly + e + BLOUB_VB) * sx;
        out[i][1] = (b * lx + d * ly + f + BLOUB_VB) * sy;
    }
    return n;
}

// ---- 入口 ------------------------------------------------------------

void bloub_render(const bloub_scene_t *sc, bloub_bitmap_t *bm)
{
    int stride = (bm->w + 7) / 8;
    memset(bm->bits, 0, (size_t)stride * bm->h);

    // R 单位 → 像素：viewBox [-VB,VB] → [0,w)
    float sx = (float)bm->w / (2.0f * BLOUB_VB);
    float sy = (float)bm->h / (2.0f * BLOUB_VB);

    static float poly[MAX_POLY][2];        // 细分后的 R 单位轮廓
    static float mapped[MAX_POLY][2];      // 像素坐标
    static float caps[CAPSULE_PTS + 2][2];
    static uint8_t body_mask[6250];        // 身体掩膜（260×192 足够；位图 1-bit）

    int cap = stride * bm->h;
    if (cap > (int)sizeof(body_mask)) return;   // 超尺寸直接放弃（调用方限制）
    memset(body_mask, 0, (size_t)cap);

    int n = catmull_rom_closed(sc->pts, BLOUB_SAMPLES, poly);
    for (int i = 0; i < n; i++) {
        mapped[i][0] = (poly[i][0] + BLOUB_VB) * sx;
        mapped[i][1] = (poly[i][1] + BLOUB_VB) * sy;
    }
    bloub_bitmap_t body = { bm->w, bm->h, body_mask };
    fill_polygon(&body, mapped, n, true);

    if (sc->dots_behind) {
        // 背后墨点：只在身体轮廓外可见（先画点再让身体盖上去）
        for (int i = 0; i < sc->n_dots; i++) {
            if (sc->dot[i].opacity < 0.5f) continue;
            fill_circle(bm, (sc->dot[i].x + BLOUB_VB) * sx,
                        (sc->dot[i].y + BLOUB_VB) * sy, sc->dot[i].r * sx, true);
        }
    }

    // 身体盖上来（覆盖 behind 点），再挖眼洞
    for (size_t i = 0; i < (size_t)cap; i++) bm->bits[i] |= body_mask[i];
    for (int i = 0; i < sc->n_eyes; i++) {
        if (sc->eye[i].alpha < 0.5f) continue;
        int cn = capsule_polygon(sc->eye[i].w * 0.5f, sc->eye[i].h * 0.5f,
                                 sc->eye[i].a, sc->eye[i].b,
                                 sc->eye[i].c, sc->eye[i].d,
                                 sc->eye[i].e, sc->eye[i].f, sx, sy, caps);
        fill_polygon(bm, caps, cn, false);
    }

    if (!sc->dots_behind) {
        // 前置墨点：叠在身体（和眼洞）之上 —— 与 SVG 图层一致
        for (int i = 0; i < sc->n_dots; i++) {
            if (sc->dot[i].opacity < 0.5f) continue;
            fill_circle(bm, (sc->dot[i].x + BLOUB_VB) * sx,
                        (sc->dot[i].y + BLOUB_VB) * sy, sc->dot[i].r * sx, true);
        }
    }
}
