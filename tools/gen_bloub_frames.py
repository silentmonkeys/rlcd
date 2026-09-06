#!/usr/bin/env python3
"""
gen_bloub_frames.py —— 把 bloub 机器人（/home/chen/demo/bloub，MIT）的动画预渲染成
1-bit 帧序列，打包进 partitions/fonts/bloub_seq.bin，供 ui_bot.c 在「机器人」页播放。

流水线：
  1. 用 Node 22（--experimental-strip-types + tools/bloub_loader.mjs）直接加载 bloub
     的纯函数引擎 src/bot/，按蒙太奇时间轴 sample(t) 逐帧输出最小 SVG
     （tools/bloub_sampler_entry.ts）。
  2. cairosvg 把每帧 SVG 栅格化到 N×N，合成到白底后按 128 阈值二值化
     （与 tools/gen_weather_icons.py 同一套做法）。
  3. 打包为 BLO1 格式：
       header: 'BLO1' | ver(u16)=1 | w(u16) | h(u16) | fps(u16) | count(u16) | reserved(u16)
       data:   count 帧 1-bit 位图顺序拼接，每帧 ceil(w/8)*h 字节，MSB 先行
     帧定长，无索引表——ui_bot.c 按 frame_no * stride * h 直接 seek。

运行：
  python3 tools/gen_bloub_frames.py                    # 默认 160x160 @8fps
  python3 tools/gen_bloub_frames.py --ref-dir /tmp/ref # 另存 PBM 参考帧（Phase B 比对用）
  python3 tools/gen_bloub_frames.py --preview /tmp/bloub_preview.png
"""

import argparse
import io
import json
import os
import struct
import subprocess
import sys
import tempfile

import cairosvg
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT_PATH = os.path.join(ROOT, "partitions", "fonts", "bloub_seq.bin")
DEFAULT_BLOUB_ROOT = "/home/chen/demo/bloub"

# 蒙太奇：慢状态为主（反射屏全屏刷新，快状态会拖影），首尾 idle 让循环平滑
DEFAULT_BLOCKS = ["idle:2.4", "wink:1.6", "wide:1.8", "thinking:2.6", "sleep:2.4", "idle:1.8"]

# BLO1 头部：magic(4) + ver(2) + w(2) + h(2) + fps(2) + count(2) + reserved(2)
HEADER_SIZE = 16
HEADER_FMT = "<4sHHHHHH"


def run_sampler(bloub_root: str, blocks: list[str], fps: int) -> dict:
    """调 Node 采样入口，返回 {fps,total,count,frames:[{t,state,svg}]}。"""
    with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as tmp:
        json_path = tmp.name
    cmd = [
        "node", "--experimental-strip-types", "--disable-warning=ExperimentalWarning",
        "--import", os.path.join(HERE, "bloub_register.mjs"),
        os.path.join(HERE, "bloub_sampler_entry.ts"),
        "--fps", str(fps), "--out", json_path, "--blocks", *blocks,
    ]
    env = dict(os.environ, BLOUB_ROOT=bloub_root)
    try:
        subprocess.run(cmd, cwd=ROOT, env=env, check=True)
        with open(json_path, encoding="utf-8") as f:
            return json.load(f)
    finally:
        if os.path.exists(json_path):
            os.unlink(json_path)


def rasterize(svg: str, size: int) -> Image.Image:
    """SVG → N×N 二值图（墨=1 黑，底=0 白）。"""
    png = cairosvg.svg2png(bytestring=svg.encode(), output_width=size, output_height=size)
    img = Image.open(io.BytesIO(png)).convert("RGBA")
    bg = Image.new("RGBA", img.size, (255, 255, 255, 255))
    img = Image.alpha_composite(bg, img).convert("L")
    return img.point(lambda p: 1 if p < 128 else 0, "1")


def pack_bin(frames_bits: list[bytes], w: int, h: int, fps: int, out_path: str) -> None:
    stride = (w + 7) // 8
    frame_size = stride * h
    for i, bits in enumerate(frames_bits):
        if len(bits) != frame_size:
            sys.exit(f"帧 {i} 尺寸异常：{len(bits)} != {frame_size}")
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(struct.pack(HEADER_FMT, b"BLO1", 1, w, h, fps, len(frames_bits), 0))
        for bits in frames_bits:
            f.write(bits)
    total = os.path.getsize(out_path)
    print(f"生成 {out_path}")
    print(f"  {len(frames_bits)} 帧 {w}x{h} @ {fps}fps，帧定长 {frame_size}B，共 {total}B "
          f"({total / 1024:.0f}KB)")


def save_refs(imgs: list[Image.Image], ref_dir: str) -> None:
    """PBM P4 参考帧（Phase B golden 比对：C 引擎离线渲染逐像素对照）。"""
    os.makedirs(ref_dir, exist_ok=True)
    for i, img in enumerate(imgs):
        img.save(os.path.join(ref_dir, f"frame_{i:04d}.pbm"))
    print(f"参考帧已写入 {ref_dir}（{len(imgs)} 张 PBM P4）")


def save_preview(imgs: list[Image.Image], step: int, path: str) -> None:
    """缩略拼图，肉眼快速过一遍动画节奏。"""
    cols = 8
    picks = imgs[::step]
    rows = (len(picks) + cols - 1) // cols
    sheet = Image.new("1", (cols * 170, rows * 170), 0)
    for i, img in enumerate(picks):
        sheet.paste(img, ((i % cols) * 170 + 5, (i // cols) * 170 + 5))
    sheet.save(path)
    print(f"预览拼图（每 {step} 帧取 1 张，共 {len(picks)} 张）→ {path}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bloub-root", default=DEFAULT_BLOUB_ROOT)
    ap.add_argument("--size", type=int, default=160)
    ap.add_argument("--fps", type=int, default=8)
    ap.add_argument("--blocks", nargs="*", default=DEFAULT_BLOCKS)
    ap.add_argument("--out", default=OUT_PATH)
    ap.add_argument("--ref-dir", default=None, help="同时导出 PBM 参考帧目录")
    ap.add_argument("--preview", default=None, help="同时导出预览拼图 PNG")
    args = ap.parse_args()

    data = run_sampler(args.bloub_root, args.blocks, args.fps)
    frames = data["frames"]
    print(f"采样 {len(frames)} 帧 / {data['total']:.2f}s @ {args.fps}fps "
          f"（states: {sorted(set(f['state'] for f in frames))}）")

    imgs = [rasterize(f["svg"], args.size) for f in frames]

    stride = (args.size + 7) // 8
    bits = []
    for img in imgs:
        raw = img.tobytes()  # "1" 模式 = MSB 先行，每行 stride 字节
        # PIL 逐行打包，直接就是目标布局
        bits.append(raw)
    pack_bin(bits, args.size, args.size, args.fps, args.out)

    if args.ref_dir:
        save_refs(imgs, args.ref_dir)
    if args.preview:
        save_preview(imgs, max(1, len(imgs) // 24), args.preview)


if __name__ == "__main__":
    main()
