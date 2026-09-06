// bloub_math.c —— TS math.ts 的 C 移植（纯函数，浮点版）
#include "bloub_internal.h"

#include <math.h>

float bloub_clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

float bloub_lerpf(float a, float b, float t)
{
    return a + (b - a) * t;
}

// TS easings.ts：实测过渡是指数 ease-out，身体不过冲
float bloub_ease_out_cubic(float t)
{
    float u = 1.0f - t;
    return 1.0f - u * u * u;
}

float bloub_ease_in_out_cubic(float t)
{
    if (t < 0.5f) return 4.0f * t * t * t;
    float u = -2.0f * t + 2.0f;
    return 1.0f - (u * u * u) / 2.0f;
}

float bloub_ease_out_quint(float t)
{
    float u = 1.0f - t;
    return 1.0f - u * u * u * u * u;
}

float bloub_loop_noise(float t, float period, float seed)
{
    float p = (t / period) * BLOUB_TAU;
    // 基频互质的三个正弦叠加，肉眼不重复
    return 0.55f * sinf(p + seed) +
           0.30f * sinf(2.0f * p + seed * 1.7f + 1.1f) +
           0.15f * sinf(3.0f * p + seed * 2.3f + 2.4f);
}

float bloub_rng_next(uint32_t *state)
{
    // mulberry32：Math.imul 只保留低 32 位，等价于 uint32 溢出乘法。
    // 两个第二操作数都是按位或（1|a、61|t），不是乘法 —— 写错会改掉整个
    // 眨眼日程的确定性序列。
    uint32_t s = *state = *state + 0x6d2b79f5u;
    uint32_t t = (s ^ (s >> 15)) * (s | 1u);
    t = (t + ((t ^ (t >> 7)) * (61u | t))) ^ t;
    return (float)((t ^ (t >> 14))) / 4294967296.0f;
}
