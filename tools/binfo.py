#!/usr/bin/env python3
"""
binfo.py —— 快速读出 .bin 里的版本信息，不用烧板子也不用翻 build/ 里的 json。

认得四种文件，自动判类型（也可以用 --type 强制）：

  app      纯应用镜像（build/rlcd_home.bin、OTA 上传的那个）
           读 0x20 处的 esp_app_desc_t：版本 / 项目名 / 编译时间 / IDF 版本
           / ELF SHA256，并按镜像格式走完各段校验尾部 SHA256。
  full     整片镜像（build/rlcd_home_full.bin，Release 里的 *_full.bin）
           0x8000 解分区表 → 每个 app 槽各读一份 app_desc → spiffs 分区当
           fonts 镜像继续往下拆。
  spiffs   fonts 分区镜像（build/fonts.bin）
           扫 SPIFFS 索引页列出文件；对 ui_font_*.bin 读 LVGL binfont 的 head
           段（字号 / bpp / 覆盖码点数），对 ui_font_weather_40.bin 读 WETH 索引
           （图标数 / 代码范围）—— 判断"这版字库是不是我以为的那版"用。
  otadata  otadata 分区（build/ota_data_initial.bin 或从设备 dump 下来的）
           两份 ota_seq 谁大谁生效，顺带看 OTA 自检状态（PENDING_VERIFY 等）。

用法：
  python3 tools/binfo.py build/rlcd_home.bin
  python3 tools/binfo.py build/*.bin              # 一次看多个
  python3 tools/binfo.py -j build/rlcd_home.bin   # JSON，给脚本用
  python3 tools/binfo.py --diff a_full.bin b_full.bin   # 两版对比
"""

import argparse
import hashlib
import json
import struct
import sys
import zlib
from datetime import datetime

# ── 常量 ────────────────────────────────────────────────────────────
IMG_MAGIC       = 0xE9
APP_DESC_MAGIC  = 0xABCD5432
APP_DESC_OFF    = 0x20          # esp_image_header(24) + 首段头(8)
PT_OFFSET       = 0x8000        # CONFIG_PARTITION_TABLE_OFFSET
PT_MAGIC        = 0x50AA        # 字节序列 AA 50，按小端读是 0x50AA
PT_MD5_MAGIC    = 0xEBEB
SPIFFS_PAGE     = 256           # CONFIG_SPIFFS_PAGE_SIZE

CHIPS = {0: "ESP32", 2: "ESP32-S2", 5: "ESP32-C3", 9: "ESP32-S3",
         12: "ESP32-C2", 13: "ESP32-C6", 16: "ESP32-H2", 18: "ESP32-P4"}

PART_TYPES = {0: "app", 1: "data"}
APP_SUBTYPES  = {0x00: "factory", 0x10: "ota_0", 0x11: "ota_1", 0x12: "ota_2",
                 0x13: "ota_3", 0x20: "test"}
DATA_SUBTYPES = {0x00: "otadata", 0x01: "phy", 0x02: "nvs", 0x03: "coredump",
                 0x04: "nvs_keys", 0x81: "fat", 0x82: "spiffs",
                 0x83: "littlefs"}

OTA_STATES = {0: "NEW", 1: "PENDING_VERIFY", 2: "VALID", 3: "INVALID",
              4: "ABORTED", 0xFFFFFFFF: "UNDEFINED(已确认/非OTA)"}


def human(n):
    for unit in ("B", "KB", "MB"):
        if n < 1024 or unit == "MB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024.0


def cstr(b):
    return b.split(b"\0")[0].decode("utf-8", "replace")


# ── app 镜像 ────────────────────────────────────────────────────────
def parse_app_desc(d, base=0):
    """解 esp_app_desc_t（256 字节，位于镜像 +0x20）。"""
    a = d[base + APP_DESC_OFF: base + APP_DESC_OFF + 256]
    if len(a) < 256 or struct.unpack_from("<I", a, 0)[0] != APP_DESC_MAGIC:
        return None
    return {
        "secure_version": struct.unpack_from("<I", a, 4)[0],
        "version":        cstr(a[16:48]),
        "project_name":   cstr(a[48:80]),
        "time":           cstr(a[80:96]),
        "date":           cstr(a[96:112]),
        "idf_ver":        cstr(a[112:144]),
        "elf_sha256":     a[144:176].hex(),
        "mmu_page_size":  (1 << a[180]) if a[180] else None,
    }


def parse_image(d, base=0):
    """走 esp_image_header + 各段，算出镜像真实长度并校验尾部 SHA256。"""
    if base >= len(d) or d[base] != IMG_MAGIC:
        return None
    seg_cnt = d[base + 1]
    chip_id = struct.unpack_from("<H", d, base + 12)[0]
    entry   = struct.unpack_from("<I", d, base + 4)[0]
    hash_appended = d[base + 23] == 1

    off, segs = base + 24, []
    for _ in range(seg_cnt):
        if off + 8 > len(d):
            return None
        load, ln = struct.unpack_from("<II", d, off)
        off += 8 + ln
        segs.append({"load_addr": load, "len": ln})

    # 填充到 (len+1) % 16 == 0，再 1 字节校验和
    pad = 15 - ((off - base) % 16)
    off += pad + 1
    img_len = off - base

    info = {
        "chip": CHIPS.get(chip_id, f"unknown(0x{chip_id:02x})"),
        "entry_addr": entry,
        "segments": segs,
        "image_len": img_len,
        "sha256_ok": None,
        "sha256": None,
    }
    if hash_appended:
        want = d[base + img_len: base + img_len + 32]
        if len(want) == 32:
            got = hashlib.sha256(d[base: base + img_len]).digest()
            info["sha256"] = want.hex()
            info["sha256_ok"] = (got == want)
        info["image_len"] = img_len + 32
    return info


# ── 分区表 ──────────────────────────────────────────────────────────
def parse_partitions(d, off=PT_OFFSET):
    parts = []
    for i in range(off, min(off + 0x1000, len(d)), 32):
        e = d[i:i + 32]
        if len(e) < 32:
            break
        magic = struct.unpack_from("<H", e, 0)[0]
        if magic == PT_MD5_MAGIC:
            continue
        if magic != PT_MAGIC:
            break
        ptype, subtype = e[2], e[3]
        p_off, p_size = struct.unpack_from("<II", e, 4)
        tname = PART_TYPES.get(ptype, f"0x{ptype:02x}")
        subs = APP_SUBTYPES if ptype == 0 else DATA_SUBTYPES
        parts.append({
            "name": cstr(e[12:28]),
            "type": tname,
            "subtype": subs.get(subtype, f"0x{subtype:02x}"),
            "offset": p_off,
            "size": p_size,
            "flags": struct.unpack_from("<I", e, 28)[0],
        })
    return parts


# ── otadata ─────────────────────────────────────────────────────────
def parse_otadata(d):
    """
    两个 sector 各一份 esp_ota_select_entry_t：
      ota_seq u32 | seq_label[20] | ota_state u32 | crc u32
    crc = crc32_le(0xFFFFFFFF, &ota_seq, 4)（bootloader_common_ota_select_crc）。
    seq 大的那份生效，槽号 = (seq - 1) % app槽数。
    """
    out = []
    for i, base in enumerate((0, 0x1000)):
        if base + 32 > len(d):
            break
        seq   = struct.unpack_from("<I", d, base)[0]
        label = d[base + 4: base + 24]
        state = struct.unpack_from("<I", d, base + 24)[0]
        crc   = struct.unpack_from("<I", d, base + 28)[0]
        blank = d[base:base + 32] == b"\xff" * 32
        out.append({"slot": i, "ota_seq": seq,
                    "label": label.hex() if label.strip(b"\xff") else None,
                    "state": OTA_STATES.get(state, f"0x{state:08x}"),
                    "crc": crc,
                    "crc_ok": blank or crc == ota_seq_crc(seq),
                    "blank": blank})
    valid = [e for e in out
             if e["crc_ok"] and not e["blank"]
             and e["ota_seq"] not in (0, 0xFFFFFFFF)]
    boot = None
    if valid:
        best = max(valid, key=lambda e: e["ota_seq"])
        boot = f"ota_{(best['ota_seq'] - 1) % 2}"
    return {"entries": out, "boot_slot": boot}


def ota_seq_crc(seq):
    return zlib.crc32(struct.pack("<I", seq)) & 0xFFFFFFFF


def looks_like_otadata(d):
    """
    otadata 必须是 1~2 个 4KB sector，且至少一份 entry 可信（全 0xFF 的出厂
    初始，或 CRC 对得上）。要求"全部可信"会把写入途中掉电的坏盘判成无法识别 ——
    那恰恰是最需要看的场景。CRC 是 32 位，随机数据撞上的概率可以忽略。
    """
    if len(d) not in (0x1000, 0x2000):
        return False
    o = parse_otadata(d)
    return any(e["crc_ok"] for e in o["entries"])


# ── LVGL binfont / 天气图标 ─────────────────────────────────────────
def parse_binfont(b):
    """
    LVGL binfont（lv_font_conv 输出）：
      head 段 48 字节 —— +14 font_size / +16 ascent / +18 descent
                        / +37 bpp / +41 compression_id
      cmap 段 —— u32 subtable_count，之后每 16 字节一项：
                 data_offset u32 | range_start u32 | range_length u16
                 | glyph_id_start u16 | data_entries u16 | format u8 | pad u8
    码点数按格式取：稀疏格式（1/3）用 data_entries，FORMAT0（0/2）是连续区间
    用 range_length —— 拿 range_length 当稀疏表的数量会把 13 个数字算成 8415。
    """
    if len(b) < 48 or b[4:8] != b"head":
        return None
    h = {
        "font_size":  struct.unpack_from("<H", b, 14)[0],
        "ascent":     struct.unpack_from("<h", b, 16)[0],
        "descent":    struct.unpack_from("<h", b, 18)[0],
        "bpp":        b[37],
        "compressed": b[41] != 0,
        "codepoints": None,
        "ranges":     None,
        "cp_min":     None,
        "cp_max":     None,
        "truncated":  False,
    }
    # 顺序扫段表找 cmap
    off = 0
    while off + 8 <= len(b):
        ln = struct.unpack_from("<I", b, off)[0]
        tag = b[off + 4: off + 8]
        if ln < 8:
            break
        if tag == b"cmap":
            n = struct.unpack_from("<I", b, off + 8)[0]
            total, ranges, lo, hi = 0, 0, None, None
            for k in range(n):
                s = off + 12 + k * 16
                if s + 16 > len(b):
                    h["truncated"] = True     # 只读了文件前若干 KB
                    break
                start = struct.unpack_from("<I", b, s + 4)[0]
                rng_len, _gid, entries = struct.unpack_from("<HHH", b, s + 8)
                fmt = b[s + 14]
                total += entries if fmt in (1, 3) else rng_len
                ranges += 1
                lo = start if lo is None else min(lo, start)
                hi = max(hi or 0, start + rng_len - 1)
            h["codepoints"], h["ranges"] = total, ranges
            h["cp_min"], h["cp_max"] = lo, hi
            break
        off += ln
    return h


def parse_weather_pack(b):
    """tools/gen_weather_icons.py 的 WETH 包：header + index[code,offset,size]。"""
    if len(b) < 10 or b[:4] != b"WETH":
        return None
    ver, cnt = struct.unpack_from("<HH", b, 4)
    codes = []
    for i in range(cnt):
        s = 10 + i * 8
        if s + 8 > len(b):
            break
        codes.append(struct.unpack_from("<H", b, s)[0])
    return {"version": ver, "count": cnt,
            "code_min": min(codes) if codes else None,
            "code_max": max(codes) if codes else None,
            "codes": codes}


# ── SPIFFS ──────────────────────────────────────────────────────────
def parse_spiffs(d):
    """
    扫索引页列文件。页布局（对齐后，META_LENGTH=4 / OBJ_NAME_LEN=32）：
      +0 obj_id u16 | +2 span_ix u16 | +4 flags u8 | +8 size u32
      | +12 type u8 | +13 name[32] | +45 meta[4]
    只认 span_ix==0 的索引头（obj_id 最高位置 1），文件数据页按 obj_id 收集。
    """
    files, seen = [], set()
    for i in range(0, len(d) - SPIFFS_PAGE + 1, SPIFFS_PAGE):
        p = d[i:i + SPIFFS_PAGE]
        obj_id, span = struct.unpack_from("<HH", p, 0)
        if obj_id == 0xFFFF or not (obj_id & 0x8000) or span != 0:
            continue
        size = struct.unpack_from("<I", p, 8)[0]
        ftype = p[12]
        name = cstr(p[13:45])
        if ftype != 1 or not name or size == 0xFFFFFFFF:
            continue
        if not all(32 <= ord(c) < 127 for c in name):
            continue
        key = (obj_id, name)
        if key in seen:
            continue
        seen.add(key)
        files.append({"name": name.lstrip("/"), "size": size,
                      "obj_id": obj_id & 0x7FFF, "index_page": i})
    files.sort(key=lambda f: f["name"])
    return files


def spiffs_read_file(d, f, want=4096):
    """
    按 obj_id 收集数据页、按 span_ix 排序拼出文件前 want 字节（够读 header）。
    数据页 obj_id 不带最高位；一旦 span 0..k 连续覆盖到 want 就停，
    但不能"数够页数就 break" —— 页在镜像里未必严格升序，缺一页会错位。
    """
    oid = f["obj_id"]
    per = SPIFFS_PAGE - 5
    need = min(want, f["size"])
    chunks = {}
    for i in range(0, len(d) - SPIFFS_PAGE + 1, SPIFFS_PAGE):
        o, span = struct.unpack_from("<HH", d, i)
        if o != oid:
            continue
        chunks[span] = d[i + 5: i + SPIFFS_PAGE]
        # span 0..k 连续且长度已够，就不用继续扫了
        k = 0
        while k in chunks:
            k += 1
        if k * per >= need:
            break
    out = b"".join(chunks[k] for k in sorted(chunks))
    return out[:need]


# ── 顶层：判类型 + 汇总 ─────────────────────────────────────────────
def sniff(d):
    if len(d) > PT_OFFSET + 32 and \
       struct.unpack_from("<H", d, PT_OFFSET)[0] == PT_MAGIC:
        return "full"
    if d[:1] == bytes([IMG_MAGIC]) and parse_app_desc(d):
        return "app"
    if parse_spiffs(d):
        return "spiffs"
    if looks_like_otadata(d):
        return "otadata"
    return "unknown"


def describe_font_file(d, f):
    # cjk 字库的 cmap 段有 13 KB，要读够才能把 43 个子表全统进去
    head = spiffs_read_file(d, f, 64 * 1024)
    if f["name"].startswith("ui_font_weather"):
        return ("weather", parse_weather_pack(head))
    bf = parse_binfont(head)
    if bf:
        return ("binfont", bf)
    if head[:4] == b"WETH":
        return ("weather", parse_weather_pack(head))
    return (None, None)


def analyze(path, force_type=None):
    with open(path, "rb") as fp:
        d = fp.read()
    kind = force_type or sniff(d)
    r = {"path": path, "type": kind, "file_size": len(d),
         "file_sha256": hashlib.sha256(d).hexdigest(),
         "head_hex": d[:16].hex(" ")}

    if kind == "app":
        r["image"] = parse_image(d)
        r["app"] = parse_app_desc(d)
    elif kind == "full":
        r["partitions"] = parse_partitions(d)
        r["apps"], r["fonts"] = [], []
        for p in r["partitions"]:
            if p["type"] == "app" and p["offset"] < len(d):
                desc = parse_app_desc(d, p["offset"])
                img = parse_image(d, p["offset"])
                if desc or img:
                    r["apps"].append({"partition": p["name"],
                                      "offset": p["offset"],
                                      "app": desc, "image": img})
            elif p["subtype"] == "spiffs" and p["offset"] < len(d):
                sub = d[p["offset"]: p["offset"] + p["size"]]
                files = parse_spiffs(sub)
                for f in files:
                    kind2, meta = describe_font_file(sub, f)
                    f["kind"], f["meta"] = kind2, meta
                r["fonts"].append({"partition": p["name"], "files": files})
            elif p["subtype"] == "otadata" and p["offset"] < len(d):
                r["otadata"] = parse_otadata(d[p["offset"]:
                                               p["offset"] + p["size"]])
        if d[:1] == bytes([IMG_MAGIC]):
            r["bootloader"] = parse_image(d, 0)
    elif kind == "spiffs":
        files = parse_spiffs(d)
        for f in files:
            f["kind"], f["meta"] = describe_font_file(d, f)
        r["fonts"] = [{"partition": "(镜像)", "files": files}]
    elif kind == "otadata":
        r["otadata"] = parse_otadata(d)
    return r


# ── 打印 ────────────────────────────────────────────────────────────
def build_date(app):
    """把 "Aug  8 2026" + "17:16:10" 拼成好读的形式，解析失败就原样返回。"""
    raw = f"{app.get('date','')} {app.get('time','')}".strip()
    try:
        return datetime.strptime(raw, "%b %d %Y %H:%M:%S") \
                       .strftime("%Y-%m-%d %H:%M:%S")
    except ValueError:
        return raw


def kv(k, v, indent=2):
    print(f"{' ' * indent}{k:<12}{v}")


def print_app(app, img, indent=2):
    if app:
        kv("版本", app["version"], indent)
        kv("项目", app["project_name"], indent)
        kv("编译时间", build_date(app), indent)
        kv("IDF", app["idf_ver"], indent)
        if app["secure_version"]:
            kv("secure_ver", app["secure_version"], indent)
        kv("ELF SHA", app["elf_sha256"], indent)
    else:
        kv("版本", "（无 app_desc，可能不是应用镜像）", indent)
    if img:
        ok = {True: "✓ 通过", False: "✗ 不匹配", None: "（未附加）"}[img["sha256_ok"]]
        kv("芯片", img["chip"], indent)
        kv("镜像", f"{human(img['image_len'])} / {img['image_len']} B, "
                   f"{len(img['segments'])} 段, SHA256 {ok}", indent)


def print_fonts(groups):
    for g in groups:
        total = sum(f["size"] for f in g["files"])
        print(f"  fonts 分区 {g['partition']} —— {len(g['files'])} 个文件, "
              f"实占 {human(total)}")
        for f in g["files"]:
            line = f"    {f['name']:<26} {f['size']:>7} B"
            m = f.get("meta")
            if f.get("kind") == "binfont" and m:
                cp = f"{m['codepoints']} 字" if m["codepoints"] else "? 字"
                if m.get("truncated"):
                    cp += "+"
                line += f"  binfont {m['font_size']}px {m['bpp']}bpp, {cp}"
                if m["compressed"]:
                    line += ", 压缩"
            elif f.get("kind") == "weather" and m:
                line += (f"  WETH v{m['version']}, {m['count']} 图标 "
                         f"({m['code_min']}–{m['code_max']})")
            print(line)


def print_report(r):
    label = {"app": "应用镜像", "full": "整片镜像", "spiffs": "SPIFFS 镜像",
             "otadata": "otadata 分区", "unknown": "无法识别"}[r["type"]]
    print(f"\n{r['path']}  —— {label}  ({human(r['file_size'])} / "
          f"{r['file_size']} B)")

    if r["type"] == "app":
        print_app(r.get("app"), r.get("image"))
    elif r["type"] == "full":
        if r.get("bootloader"):
            kv("bootloader", f"{r['bootloader']['chip']}, "
                             f"{human(r['bootloader']['image_len'])}")
        for a in r["apps"]:
            print(f"  [{a['partition']} @ 0x{a['offset']:x}]")
            print_app(a["app"], a["image"], indent=4)
        if r.get("otadata"):
            o = r["otadata"]
            states = " / ".join(f"seq{e['ota_seq']}:{e['state']}"
                                for e in o["entries"])
            kv("otadata", f"启动槽 {o['boot_slot'] or '（未写，走首个 app）'}"
                          f"  [{states}]")
        print_fonts(r.get("fonts", []))
        if not r["apps"]:
            print("  （分区表里的 app 槽在本文件中没有数据）")
    elif r["type"] == "spiffs":
        print_fonts(r["fonts"])
    elif r["type"] == "otadata":
        o = r["otadata"]
        kv("启动槽", o["boot_slot"] or "（未写，走首个 app）")
        for e in o["entries"]:
            if e["blank"]:
                kv(f"槽{e['slot']}", "空白（0xFF，出厂初始）")
            else:
                bad = "" if e["crc_ok"] else "  ⚠ CRC 不符"
                kv(f"槽{e['slot']}",
                   f"ota_seq={e['ota_seq']}  状态={e['state']}{bad}")
    else:
        kv("头部字节", r["head_hex"])
        print("  提示：可用 -t app|full|spiffs|otadata 强制解析")
    kv("文件 SHA", r["file_sha256"])


# ── diff ────────────────────────────────────────────────────────────
def flatten(r):
    """抽出适合逐行对比的键值，够看出"两版差在哪"。"""
    out = {}
    def put_app(prefix, app, img):
        if app:
            out[prefix + "版本"] = app["version"]
            out[prefix + "编译时间"] = build_date(app)
            out[prefix + "IDF"] = app["idf_ver"]
            out[prefix + "ELF SHA"] = app["elf_sha256"][:16] + "…"
        if img:
            out[prefix + "镜像大小"] = img["image_len"]

    if r["type"] == "app":
        put_app("app.", r.get("app"), r.get("image"))
    for a in r.get("apps", []):
        put_app(f"{a['partition']}.", a["app"], a["image"])
    for g in r.get("fonts", []):
        for f in g["files"]:
            m = f.get("meta") or {}
            extra = ""
            if f.get("kind") == "binfont":
                extra = f" [{m.get('font_size')}px {m.get('codepoints')}字]"
            elif f.get("kind") == "weather":
                extra = f" [{m.get('count')}图标]"
            out["fonts." + f["name"]] = f"{f['size']} B{extra}"
    if r.get("otadata"):
        out["otadata.启动槽"] = r["otadata"]["boot_slot"]
    out["文件大小"] = r["file_size"]
    return out


def print_diff(ra, rb):
    a, b = flatten(ra), flatten(rb)
    print(f"\nA = {ra['path']}\nB = {rb['path']}\n")
    keys = list(dict.fromkeys(list(a) + list(b)))
    width = max(len(k) for k in keys) + 2
    same = 0
    for k in keys:
        va, vb = a.get(k, "—"), b.get(k, "—")
        if va == vb:
            same += 1
            continue
        print(f"  ~ {k:<{width}} A: {va}\n    {'':<{width}} B: {vb}")
    print(f"\n  （另有 {same} 项相同）")


def main():
    ap = argparse.ArgumentParser(
        description="读 .bin 里的版本信息（app / full / fonts / otadata）")
    ap.add_argument("files", nargs="+", help="bin 文件，可多个")
    ap.add_argument("-j", "--json", action="store_true", help="输出 JSON")
    ap.add_argument("-t", "--type", choices=["app", "full", "spiffs",
                                             "otadata"],
                    help="强制按某类型解析（自动判错时用）")
    ap.add_argument("--diff", action="store_true",
                    help="对比两个 bin（需正好传 2 个文件）")
    args = ap.parse_args()

    if args.diff and len(args.files) != 2:
        ap.error("--diff 需要正好 2 个文件")

    reports = []
    for p in args.files:
        try:
            reports.append(analyze(p, args.type))
        except FileNotFoundError:
            print(f"找不到文件：{p}", file=sys.stderr)
            return 1
        except (struct.error, ValueError) as e:
            print(f"解析失败 {p}：{e}", file=sys.stderr)
            return 1

    if args.diff:
        print_diff(*reports)
    elif args.json:
        print(json.dumps(reports if len(reports) > 1 else reports[0],
                         ensure_ascii=False, indent=2))
    else:
        for r in reports:
            print_report(r)
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
