#!/usr/bin/env python3
"""
gen_weather_icons.py —— 把 QWeather 线稿 SVG 天气图标转成 1-bit 位图存进
partitions/fonts/weather/，供 ui_weather_icon.c 在运行时按 weather_code 加载。

处理流程：
  1. 扫描 NEEDS/weather_icons/QWeather-Icons-1.8.0/icons/ 下所有 {code}.svg
     （排除 *-fill.svg 填充版），只保留 ICON.md 里列出的"实况 API 会返回的代码"。
  2. 用 cairosvg 在 4× 分辨率下栅格化（抗锯齿），再 LANCZOS 降到目标尺寸，
     128 阈值二值化 → 干净的 1-bit 线稿。
  3. 写出 {code}.bin：
       uint16 width  (LE)
       uint16 height (LE)
       uint8[] bits  —— 1 位 / 像素，MSB 先行，每行 ceil(w/8) 字节

运行：
  python3 tools/gen_weather_icons.py
"""

import io
import os
import re
import struct
import sys

import cairosvg
from PIL import Image

# ── 目标尺寸：主页图标槽 40×40 ─────────────────────────────────────
SIZE = 40

# ── 路径 ─────────────────────────────────────────────────────────────
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SVG_DIR = os.path.join(ROOT, "NEEDS", "weather_icons",
                       "QWeather-Icons-1.8.0", "icons")
# 直接放在 partitions/fonts/ 根，与字库文件同级，命名对齐 ui_font_<name>_<size>.bin
OUT_DIR = os.path.join(ROOT, "partitions", "fonts")
OUT_NAME = f"ui_font_weather_{SIZE}.bin"
SCALE = 4  # 超采样倍率：先按 4× 栅格化再降采样，得到更干净的线条

# ── ICON.md 列出的实况图标代码（now.icon 会返回的）─────────────────
# 900/901/999 也包含；月相 800-807 明确不入库，这里一并排除。
WEATHER_CODES = set(
    list(range(100, 105)) +   # 晴/少云/晴间多云/阴
    list(range(150, 154)) +   # 夜晚 150-153
    list(range(300, 319)) +   # 雨 300-318
    [350, 351, 399] +         # 夜雨 / 雨
    list(range(400, 411)) +   # 雪 400-410
    [456, 457, 499] +         # 夜雪 / 雪
    list(range(500, 516)) +   # 雾/霾/沙尘 500-515
    [900, 901, 999]           # 热/冷/未知
)


def rasterize(path: str, size: int) -> Image.Image:
    """SVG → 二值化灰度图（黑线 = 0，白底 = 255）。"""
    big = size * SCALE
    png = cairosvg.svg2png(url=path, output_width=big, output_height=big)
    img = Image.open(io.BytesIO(png)).convert("RGBA")
    # RGBA → 先合成到白底（透明区域 = 背景白），再转灰度
    bg = Image.new("RGBA", img.size, (255, 255, 255, 255))
    img = Image.alpha_composite(bg, img).convert("L")
    if SCALE != 1:
        img = img.resize((size, size), Image.LANCZOS)  # 抗锯齿降采样
    # 二值化：线稿（暗）= 1（墨），背景（亮）= 0（纸）
    return img.point(lambda p: 1 if p < 128 else 0, "1")


def pack_bits(img: Image.Image) -> bytes:
    """PIL 1-bit 图 → MSB 先行的字节流，每行 ceil(w/8) 字节。"""
    w, h = img.size
    stride = (w + 7) // 8
    # img.tobytes() 在 "1" 模式下就是 MSB 先行
    raw = img.tobytes()
    out = bytearray()
    for y in range(h):
        out += raw[y * stride:(y + 1) * stride]
    return bytes(out)


def main():
    if not os.path.isdir(SVG_DIR):
        sys.exit(f"找不到 SVG 目录: {SVG_DIR}")
    os.makedirs(OUT_DIR, exist_ok=True)

    # ── 1. 栅格化所有实况图标，暂存内存 ──────────────────────────────
    pat = re.compile(r"^(\d+)\.svg$")
    icons = []   # [(code, bytes)]
    skipped = 0
    for name in sorted(os.listdir(SVG_DIR)):
        m = pat.match(name)
        if not m:
            continue
        code = int(m.group(1))
        if code not in WEATHER_CODES:
            skipped += 1
            continue
        img = rasterize(os.path.join(SVG_DIR, name), SIZE)
        data = struct.pack("<HH", SIZE, SIZE) + pack_bits(img)
        icons.append((code, data))

    icons.sort(key=lambda x: x[0])

    # ── 2. 写合并文件 ─────────────────────────────────────────────────
    # 布局：header | index[count] | data[]
    # header: <4sHHH = 4 + 2 + 2 + 2 = 10 bytes
    # 索引项：uint16 code, uint32 offset, uint16 size  (stride = 8, 4 字节对齐)
    HEADER_SIZE = 10
    INDEX_STRIDE = 8
    index_size = len(icons) * INDEX_STRIDE
    data_offset = HEADER_SIZE + index_size

    out_path = os.path.join(OUT_DIR, OUT_NAME)
    with open(out_path, "wb") as f:
        # header: magic 'WETH' + version(u16) + count(u16) + reserved(u16)
        f.write(struct.pack("<4sHHH", b"WETH", 1, len(icons), 0))
        # index
        off = data_offset
        for code, data in icons:
            f.write(struct.pack("<HIH", code, off, len(data)))
            off += len(data)
        # data
        for _, data in icons:
            f.write(data)

    total = os.path.getsize(out_path)
    print(f"生成合并字库 {out_path}")
    print(f"  图标数：{len(icons)}  跳过非实况：{skipped}")
    print(f"  头部 {HEADER_SIZE}B + 索引 {index_size}B + 数据 {total - data_offset}B = {total}B")

    # ── 3. 清理旧的零散文件 ──────────────────────────────────────────
    # 删除 partitions/fonts/ 下旧的 ui_font_weather_*.bin（除当前输出）
    old_files = [f for f in os.listdir(OUT_DIR)
                 if f.startswith("ui_font_weather_") and f != OUT_NAME]
    for f in old_files:
        os.remove(os.path.join(OUT_DIR, f))
    # 删除旧的 weather/ 子目录（整体迁移到 fonts 根）
    old_dir = os.path.join(OUT_DIR, "weather")
    if os.path.isdir(old_dir):
        import shutil
        shutil.rmtree(old_dir)
        print(f"  已删除旧目录 weather/")
    if old_files:
        print(f"  已删除旧版本字库 {len(old_files)} 个")


if __name__ == "__main__":
    main()
