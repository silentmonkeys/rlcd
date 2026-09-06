#!/usr/bin/env python3
"""
bloub_golden.py —— C 引擎移植 vs Node 真值 的逐帧像素比对。

真值：tools/gen_bloub_frames.py 的采样+栅格化链路（bloub 原始引擎 → SVG →
cairosvg → 128 阈值二值化）。
待测：simulator/build/rlcd_sim --bot-frame <t> --bot-out <pbm>（纯 C 引擎
离线渲染，不经 LVGL/SDL）。

判定：逐帧比较 1-bit 位图的差异像素数，阈值以内算通过（边缘抗锯齿阈值
抖动会产生 1px 级差异，几何错误才会产生大块差异）。

运行：
  python3 tools/bloub_golden.py            # 默认比对 24 帧
  python3 tools/bloub_golden.py --frames 48
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SIM = os.path.join(ROOT, "simulator", "build", "rlcd_sim")

SIZE = 160          # 与 simulator/main.c 的 BOT_OUT_SIZE 一致
STRIDE = (SIZE + 7) // 8
DIFF_LIMIT = 0.025  # 差异像素 / 真值墨像素 上限（抗锯齿边缘抖动余量）


def read_pbm_p4(path):
    with open(path, "rb") as f:
        data = f.read()
    # 解析头：P4\nW H\n（字段间可有空白）
    if data[:2] != b"P4":
        raise ValueError(f"{path}: 不是 P4 PBM")
    pos, fields = 2, []
    while len(fields) < 2:
        while pos < len(data) and data[pos:pos+1].isspace():
            pos += 1
        if data[pos:pos+1] == b"#":
            while data[pos:pos+1] not in (b"\n", b""):
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos+1].isspace():
            pos += 1
        fields.append(int(data[start:pos]))
    pos += 1  # 单个空白分隔符
    w, h = fields
    expected = ((w + 7) // 8) * h
    bits = data[pos:pos + expected]
    if len(bits) != expected:
        raise ValueError(f"{path}: 位图数据不足")
    return w, h, bits


def popcount(b):
    return bin(b).count("1")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=24)
    ap.add_argument("--fps", type=int, default=8)
    args = ap.parse_args()

    # 1. Node 真值：采样 → SVG → 1-bit
    sys.path.insert(0, HERE)
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "gen_bloub_frames", os.path.join(HERE, "gen_bloub_frames.py"))
    g = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(g)

    from PIL import Image

    data = g.run_sampler(g.DEFAULT_BLOUB_ROOT, g.DEFAULT_BLOCKS, args.fps)
    frames = data["frames"][:args.frames]
    print(f"真值：{len(frames)} 帧 @ {args.fps}fps（蒙太奇 {data['total']:.2f}s）")

    worst = (0.0, -1)
    total_set = 0
    total_diff = 0
    with tempfile.TemporaryDirectory() as tmp:
        for i, fr in enumerate(frames):
            # 真值 PBM —— 手写 P4 头 + 原始位流（PIL 的 .save 对 mode "1"
            # 会按「1=白」的视觉语义反转，与 C 侧「1=墨」的约定相反）
            truth_path = os.path.join(tmp, f"truth_{i:03d}.pbm")
            img = g.rasterize(fr["svg"], SIZE)
            with open(truth_path, "wb") as f:
                f.write(f"P4\n{SIZE} {SIZE}\n".encode())
                f.write(img.tobytes())
            _, _, tb = read_pbm_p4(truth_path)

            # C 引擎 PBM
            c_path = os.path.join(tmp, f"c_{i:03d}.pbm")
            r = subprocess.run([SIM, "--bot-frame", f"{fr['t']:.6f}",
                                "--bot-out", c_path],
                               capture_output=True, text=True)
            if r.returncode != 0:
                sys.exit(f"帧 {i}（t={fr['t']}）：rlcd_sim --bot-frame 失败\n{r.stderr}")
            _, _, cb = read_pbm_p4(c_path)

            n_set = sum(popcount(b) for b in tb)
            diff = sum(popcount(a ^ b) for a, b in zip(tb, cb))
            total_set += n_set
            total_diff += diff
            ratio = diff / max(n_set, 1)
            mark = ""
            if ratio > DIFF_LIMIT:
                mark = "  ← 超阈值"
                if ratio > worst[0]:
                    worst = (ratio, i)
            print(f"  帧 {i:3d} t={fr['t']:6.3f}s [{fr['state']:9s}] "
                  f"墨 {n_set:5d} 差异 {diff:4d} ({ratio*100:5.2f}%){mark}")

    overall = total_diff / max(total_set, 1)
    print(f"\n总体：{total_diff}/{total_set} = {overall*100:.2f}% "
          f"（阈值 {DIFF_LIMIT*100:.1f}%）")
    if worst[1] >= 0:
        print(f"最差帧：#{worst[1]}（{worst[0]*100:.1f}%）")
    sys.exit(0 if overall <= DIFF_LIMIT else 1)


if __name__ == "__main__":
    main()
