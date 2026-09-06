#!/usr/bin/env python3
"""
rlcd_debug_gui —— RLCD 模拟器图形化调试客户端

通过 TCP 连接模拟器（默认 127.0.0.1:9000，与 rlcd_debug 同一协议），
用一屏表单远程读写 ui_model 字段、切页、跑场景预设、标注日历。

布局（左右分栏，分割线可拖）：
  ┌ 顶栏：连接状态 + 地址 + [读取全部] [清 override] ────────────┐
  │ 左：可滚动的全字段表单（按组分节，一屏看全，不用切 tab）      │
  │ 右：页面导航 / 场景预设 / 日历 / 日志 + 手动命令              │
  └───────────────────────────────────────────────────────────────┘

交互约定：
  - 数值/文本框：改动即标黄（未提交）→ 回车发该字段，或点底部「应用改动」批量发
  - 布尔/下拉：切换即发
  - 字段右侧 ● 表示该字段已被 override（sim_tick_data 不再自动填充）
  - 数值框右键：塞"无数据"哨兵值（NaN / -1 / -999），专门测无数据渲染分支

协议（文本行，\\n 分隔，响应后跟一个空行作为帧结束标记）：
  set <field> <value>    写字段并设 override 位
  get <field>            读字段 → "val <field> <value>"
  dump                   一次性回全部字段（省掉 N 次串行 get）
  ovr                    回当前被 override 的字段名
  page N / next / prev   切页
  mark MM-DD / unmark / marks CSV / events SPEC / labels SPEC
  clear / ping / help / quit

依赖：Python 3.8+（仅标准库 —— tkinter + socket + threading）
用法：python3 rlcd_debug_gui.py [host[:port]] [--connect] [--geometry WxH+X+Y]
      --connect：启动即自动连接，不用手点「连接」（tools/dev_sim.sh 走这条）
      --geometry：Tk 几何字符串，指定窗口大小/位置，避免和模拟器窗口叠在一起
"""

import math
import queue
import re
import socket
import sys
import threading
import tkinter as tk
from dataclasses import dataclass, field as dc_field
from tkinter import ttk, scrolledtext, messagebox

# ---------------------------------------------------------------------------
# 协议常量
# ---------------------------------------------------------------------------
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 9000
CONN_TIMEOUT = 3.0    # 连接超时（秒）
RECV_TIMEOUT = 2.0    # 读响应兜底超时；正常靠空行帧终止符，不会走到这里
RESP_POLL_MS = 40     # 主线程轮询响应队列的间隔

DIRTY_BG = "#fff3bf"  # 未提交字段的底色

# ---------------------------------------------------------------------------
# 字段元数据
#   kind: "f"=浮点 "i"=整数 "b"=布尔 "s"=字符串 "code"=天气代码下拉
#   na  : "无数据"哨兵值（右键菜单塞入）。ui_model.h 约定：
#         float → NaN；百分比/指数类整型 → -1(UI_INT_NA)；摄氏温度 → -999(UI_TEMP_NA)
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class F:
    name: str
    label: str
    kind: str = "f"
    lo: float = 0
    hi: float = 100
    step: float = 1
    unit: str = ""
    na: object = None
    choices: object = None   # kind="enum"：((值, 标签), ...) → 数字值下拉选


NAN = float("nan")

FIELD_GROUPS = [
    ("时间", [
        F("hour",    "时",   "i", 0, 23, 1),
        F("minute",  "分",   "i", 0, 59, 1),
        F("year",    "年",   "i", 2000, 2099, 1),
        F("month",   "月",   "i", 1, 12, 1),
        F("day",     "日",   "i", 1, 31, 1),
        F("weekday", "星期", "i", 0, 6, 1, "0=周日"),
    ]),
    ("室内环境", [
        F("indoor_temp", "室内温度", "f", -10, 50, 0.1, "°C", NAN),
        F("indoor_humi", "室内湿度", "f", 0, 100, 0.1, "%", NAN),
    ]),
    ("天气", [
        F("weather_code",    "天气代码", "code"),
        F("weather_text",    "天气文本", "s", unit="晴/多云/小雨"),
        F("city",            "城市",     "s"),
        F("weather_update",  "更新时间", "s", unit="HH:MM"),
        F("outdoor_temp",    "室外温度", "f", -30, 55, 0.1, "°C", NAN),
        F("outdoor_humi",    "室外湿度", "f", 0, 100, 0.1, "%", NAN),
        F("feels_like_temp", "体感温度", "f", -30, 55, 0.1, "°C", NAN),
        F("temp_min",        "最低温",   "i", -40, 55, 1, "°C", -999),
        F("temp_max",        "最高温",   "i", -40, 55, 1, "°C", -999),
        F("pressure",        "气压",     "i", 870, 1100, 1, "hPa", 0),
        F("visibility",      "能见度",   "i", 0, 50, 1, "km", 0),
        F("uv",              "紫外线",   "i", 0, 15, 1, "UVI", -1),
        F("cloud",           "云量",     "i", 0, 100, 1, "%", -1),
        F("sunrise",         "日出",     "s", unit="HH:MM"),
        F("sunset",          "日落",     "s", unit="HH:MM"),
        F("wind_speed",      "风速",     "f", 0, 60, 0.1, "km/h", NAN),
        F("wind_dir",        "风向",     "s", unit="东北风/无持续风向"),
    ]),
    ("状态栏", [
        F("wifi_connected",   "WiFi 已连接", "b"),
        F("wifi_rssi",        "WiFi 信号",   "i", -100, 0, 1, "dBm"),
        F("battery_percent",  "电池电量",    "i", 0, 100, 1, "%"),
    ]),
    ("设备信息页", [
        F("ip",            "IP",       "s"),
        F("mac",           "MAC",      "s"),
        F("ssid",          "SSID",     "s"),
        F("chip_model",    "芯片",     "s"),
        F("cpu_cores",     "核心数",   "i", 1, 4, 1),
        F("free_heap_kb",  "空闲堆",   "i", 0, 8192, 1, "KB"),
        F("flash_size_mb", "Flash",    "i", 1, 64, 1, "MB"),
        F("uptime_sec",    "运行时长", "i", 0, 999999, 60, "s"),
        F("idf_ver",       "IDF 版本", "s"),
        F("app_ver",       "App 版本", "s"),
    ]),
    ("SD / Flash", [
        F("sd_mounted",    "SD 已挂载", "b"),
        F("sd_total_mb",   "SD 总容量", "i", 0, 131072, 1024, "MB"),
        F("sd_used_mb",    "SD 已用",   "i", 0, 131072, 512, "MB"),
        F("flash_used_kb", "Flash 已用", "i", 0, 65536, 512, "KB"),
        F("flash_free_kb", "Flash 剩余", "i", 0, 65536, 512, "KB"),
    ]),
    ("配网页", [
        F("ap_active",       "SoftAP 广播中", "b"),
        F("ap_ssid",         "AP SSID",       "s"),
        F("ap_ip",           "AP IP",         "s"),
        F("setup_dismissed", "已退出配网页",  "b"),
    ]),
    ("BOT 机器人", [
        # 枚举值与 ui_model.h 的 UI_BOT_ST_* / UI_BOT_EMO_* 一一对应
        F("bot_state", "设备状态", "enum",
          choices=((0, "离线"), (1, "空闲"), (2, "连接中"), (3, "聆听"),
                   (4, "思考"), (5, "播报"), (6, "等待激活"), (7, "错误"))),
        F("bot_emotion", "LLM 情绪", "enum",
          choices=((0, "中性"), (1, "开心"), (2, "难过"), (3, "生气"),
                   (4, "惊讶"), (5, "瞌睡"), (6, "思考"), (7, "喜爱"),
                   (8, "困惑"), (9, "得意"))),
        F("bot_chat_reply", "AI 回答", "s", unit="显示在页面底部"),
    ]),
]

ALL_FIELDS = {f.name: f for _, fs in FIELD_GROUPS for f in fs}

# 页面（与 ui_pages.h 的 ui_page_id_t 一一对应）
PAGES = [
    (0, "HOME 主页"),
    (1, "BOT 机器人"),
    (2, "WEATHER 天气"),
    (3, "CALENDAR 日历"),
    (4, "DEVICE 设备"),
    (5, "SETUP 配网"),
]

# 和风天气 icon 代码 → 中文描述（实况 API 返回的子集）
# 999 是 ui_weather_icon.c 的兜底代码（找不到图标时回落）——必须能测到
WEATHER_CODES = [
    ("100", "晴"), ("101", "多云"), ("102", "少云"), ("103", "晴间多云"), ("104", "阴"),
    ("150", "晴（夜）"), ("151", "多云（夜）"), ("152", "少云（夜）"), ("153", "晴间多云（夜）"),
    ("300", "阵雨"), ("301", "强阵雨"), ("302", "雷阵雨"), ("303", "强雷阵雨"),
    ("304", "雷阵雨+冰雹"), ("305", "小雨"), ("306", "中雨"), ("307", "大雨"),
    ("308", "极端降雨"), ("309", "毛毛雨"), ("310", "暴雨"), ("311", "大暴雨"),
    ("312", "特大暴雨"), ("313", "冻雨"), ("314", "小到中雨"), ("315", "中到大雨"),
    ("316", "大到暴雨"), ("317", "暴到大暴雨"), ("318", "大暴到特大暴雨"),
    ("350", "阵雨（夜）"), ("351", "强阵雨（夜）"), ("399", "雨"),
    ("400", "小雪"), ("401", "中雪"), ("402", "大雪"), ("403", "暴雪"),
    ("404", "雨夹雪"), ("405", "雨雪天气"), ("406", "阵雨夹雪"), ("407", "阵雪"),
    ("408", "小到中雪"), ("409", "中到大雪"), ("410", "大到暴雪"),
    ("456", "阵雨夹雪（夜）"), ("457", "阵雪（夜）"), ("499", "雪"),
    ("500", "薄雾"), ("501", "雾"), ("502", "霾"), ("503", "扬沙"), ("504", "浮尘"),
    ("507", "沙尘暴"), ("508", "强沙尘暴"), ("509", "浓雾"), ("510", "强浓雾"),
    ("511", "中度霾"), ("512", "重度霾"), ("513", "严重霾"), ("514", "大雾"),
    ("515", "特强浓雾"),
    ("900", "热"), ("901", "冷"), ("999", "未知（图标兜底）"),
]

CODE_LABELS = [f"{c}  {t}" for c, t in WEATHER_CODES]
CODE_BY_LABEL = {f"{c}  {t}": c for c, t in WEATHER_CODES}
LABEL_BY_CODE = {c: f"{c}  {t}" for c, t in WEATHER_CODES}

# ---------------------------------------------------------------------------
# 场景预设：一键把一组字段设成典型组合
# 覆盖"手工要点十几次"的常用状态，尤其是边界/无数据分支
# ---------------------------------------------------------------------------
SCENES = [
    ("酷暑晴天", [
        ("weather_code", 100), ("weather_text", "晴"), ("outdoor_temp", 39.5),
        ("feels_like_temp", 43), ("temp_min", 30), ("temp_max", 40),
        ("outdoor_humi", 35), ("uv", 11), ("cloud", 5), ("visibility", 25),
        ("indoor_temp", 32.5), ("indoor_humi", 45),
    ]),
    ("极寒暴雪", [
        ("weather_code", 402), ("weather_text", "大雪"), ("outdoor_temp", -18.0),
        ("feels_like_temp", -25), ("temp_min", -22), ("temp_max", -10),
        ("outdoor_humi", 85), ("uv", 1), ("cloud", 95), ("visibility", 2),
        ("indoor_temp", 16.0), ("indoor_humi", 30),
    ]),
    ("暴雨大风", [
        ("weather_code", 310), ("weather_text", "暴雨"), ("outdoor_temp", 22.0),
        ("feels_like_temp", 24), ("outdoor_humi", 96), ("wind_speed", 58.0),
        ("wind_dir", "东南风"), ("cloud", 100), ("visibility", 1), ("uv", 0),
    ]),
    ("雾霾", [
        ("weather_code", 512), ("weather_text", "重度霾"), ("outdoor_temp", 8.0),
        ("visibility", 0), ("outdoor_humi", 78), ("cloud", 80), ("uv", 2),
    ]),
    ("离线（无 WiFi）", [
        ("wifi_connected", "false"), ("wifi_rssi", 0), ("ip", "0.0.0.0"),
        ("ssid", ""),
    ]),
    ("弱信号", [
        ("wifi_connected", "true"), ("wifi_rssi", -82),
    ]),
    ("满信号", [
        ("wifi_connected", "true"), ("wifi_rssi", -42),
    ]),
    ("低电量告警", [
        ("battery_percent", 8),
    ]),
    ("无数据（哨兵）", [
        ("indoor_temp", "nan"), ("indoor_humi", "nan"), ("outdoor_temp", "nan"),
        ("outdoor_humi", "nan"), ("feels_like_temp", "nan"), ("wind_speed", "nan"),
        ("temp_min", -999), ("temp_max", -999), ("uv", -1), ("cloud", -1),
        ("pressure", 0), ("visibility", 0), ("weather_text", ""),
    ]),
    ("配网模式", [
        ("ap_active", "true"), ("ap_ssid", "RLCD-Setup"), ("ap_ip", "192.168.4.1"),
        ("setup_dismissed", "false"), ("wifi_connected", "false"),
    ]),
    ("无 SD 卡", [
        ("sd_mounted", "false"), ("sd_total_mb", 0), ("sd_used_mb", 0),
    ]),
    ("BOT 离线", [
        ("bot_state", 0), ("bot_emotion", 0), ("bot_chat_reply", ""),
    ]),
    ("BOT 聆听", [
        ("bot_state", 3), ("bot_emotion", 0),
    ]),
    ("BOT 思考", [
        ("bot_state", 4), ("bot_emotion", 6),
    ]),
    ("BOT 播报·开心", [
        ("bot_state", 5), ("bot_emotion", 1),
        ("bot_chat_reply", "今天天气不错，适合出门散步。"),
    ]),
    ("BOT 播报·难过", [
        ("bot_state", 5), ("bot_emotion", 2),
        ("bot_chat_reply", "抱歉，我没有找到相关的结果。"),
    ]),
    ("BOT 播报·瞌睡", [
        ("bot_state", 5), ("bot_emotion", 5),
        ("bot_chat_reply", "夜深了，早点休息吧。"),
    ]),
    ("BOT 等待激活", [
        ("bot_state", 6), ("bot_emotion", 0),
    ]),
    ("BOT 错误", [
        ("bot_state", 7), ("bot_emotion", 2),
    ]),
]


# ---------------------------------------------------------------------------
# 后台 sender 线程：维护一条持久 TCP 连接，循环处理命令队列
# ---------------------------------------------------------------------------
class Sender(threading.Thread):
    """socket 读写全在后台，主线程只通过队列收发 → UI 永不阻塞。"""

    def __init__(self, host: str, port: int, resp_queue: queue.Queue):
        super().__init__(daemon=True)
        self.host = host
        self.port = port
        self.cmd_q: "queue.Queue" = queue.Queue()
        self.resp_q = resp_queue
        self.sock = None
        self._stop = threading.Event()
        # 连接结果用 Event 回报（旧版对常驻循环线程 join(timeout=4)，
        # 线程永不退出 → 每次点「连接」都白等 4 秒）
        self.connected_evt = threading.Event()
        self.connect_err = None

    # ---- 线程主体 ----
    def run(self):
        try:
            self.sock = socket.create_connection((self.host, self.port),
                                                 timeout=CONN_TIMEOUT)
            self.sock.settimeout(RECV_TIMEOUT)
        except OSError as e:
            self.connect_err = str(e)
            self.connected_evt.set()
            return

        self.connected_evt.set()
        self.resp_q.put(("#info", f"已连通 {self.host}:{self.port}"))

        while not self._stop.is_set():
            try:
                item = self.cmd_q.get(timeout=0.1)
            except queue.Empty:
                continue
            if item is None:          # 毒丸 → 退出
                break
            cmd, tag = item
            lines, dead = self._roundtrip(cmd)
            self.resp_q.put(("#resp", cmd, lines, tag))
            if dead:
                self.resp_q.put(("#dead", "连接已断开"))
                break

        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass

    def _roundtrip(self, cmd: str):
        """发一条命令，读到空行（帧结束标记）为止。返回 (响应行列表, 是否断开)。"""
        try:
            self.sock.sendall((cmd + "\n").encode())
        except OSError as e:
            return [f"error 发送失败: {e}"], True

        lines, buf = [], b""
        while True:
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                # 正常不该走到这里：服务端每条响应后都发空行帧终止符。
                # 走到说明服务端是旧版或命令无响应。
                return (lines or ["error 响应超时"]), False
            except OSError as e:
                return (lines or [f"error {e}"]), True
            if not chunk:
                return (lines or ["error 连接被对端关闭"]), True
            buf += chunk
            done = False
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode(errors="replace").strip()
                if line == "":        # 空行 = 帧结束
                    done = True
                    break
                lines.append(line)
            if done:
                return lines, False

    # ---- 主线程调用 ----
    def enqueue(self, cmd: str, tag: str = ""):
        self.cmd_q.put((cmd, tag))

    def shutdown(self):
        self._stop.set()
        self.cmd_q.put(None)


# ---------------------------------------------------------------------------
# 一行字段控件：负责"值 ↔ 控件"、脏标记、override 标记
# ---------------------------------------------------------------------------
class FieldRow:
    def __init__(self, app: "App", parent, row: int, spec: F):
        self.app = app
        self.spec = spec
        self.dirty = False
        self.applied = ""      # 最近一次已提交/已读回的值（用于判断是否变脏）

        ttk.Label(parent, text=spec.label).grid(
            row=row, column=0, sticky="w", padx=(2, 6), pady=1)

        self.var = tk.StringVar()
        self.widget = None

        if spec.kind == "b":
            # 布尔：切换即发（不用标脏 —— 复选框状态本身就是意图）
            self.bvar = tk.BooleanVar(value=False)
            self.widget = ttk.Checkbutton(
                parent, variable=self.bvar,
                command=lambda: app.send_field(spec.name,
                                               "true" if self.bvar.get() else "false"))
            self.widget.grid(row=row, column=1, sticky="w", pady=1)

        elif spec.kind == "code":
            # 天气代码：可键入过滤（62 项 readonly 下拉翻找太慢），选中即发
            self.widget = ttk.Combobox(parent, textvariable=self.var,
                                       values=CODE_LABELS, width=17)
            self.widget.grid(row=row, column=1, sticky="ew", pady=1)
            self.widget.bind("<<ComboboxSelected>>", lambda _e: self._send_code())
            self.widget.bind("<Return>", lambda _e: self._send_code())

        elif spec.kind == "enum":
            # 枚举下拉：标签 = "值 中文"，选中即发数字值（readonly，杜绝手滑）
            self.enum_by_label = {f"{v} {t}": v for v, t in spec.choices}
            self.widget = ttk.Combobox(parent, textvariable=self.var,
                                       values=list(self.enum_by_label),
                                       state="readonly", width=17)
            self.widget.grid(row=row, column=1, sticky="ew", pady=1)
            self.widget.bind("<<ComboboxSelected>>", lambda _e: self._send_enum())

        else:
            width = 17 if spec.kind == "s" else 10
            if spec.kind == "s":
                self.widget = ttk.Entry(parent, textvariable=self.var, width=width)
            else:
                self.widget = ttk.Spinbox(parent, textvariable=self.var, width=width,
                                          from_=spec.lo, to=spec.hi,
                                          increment=spec.step)
            self.widget.grid(row=row, column=1, sticky="ew", pady=1)
            self.widget.bind("<Return>", lambda _e: self.submit())
            self.var.trace_add("write", self._on_edit)
            # 右键塞哨兵值："无数据"渲染分支最该测，手打 nan/-999 很别扭
            if spec.na is not None:
                self._attach_na_menu()

        col = 2
        if spec.unit:
            ttk.Label(parent, text=spec.unit, foreground="#666").grid(
                row=row, column=col, sticky="w", padx=(4, 0), pady=1)
        col += 1

        # override 标记
        self.lbl_ovr = ttk.Label(parent, text=" ", width=2, foreground="#c0392b")
        self.lbl_ovr.grid(row=row, column=col, sticky="w")

    # ---- 哨兵值右键菜单 ----
    def _attach_na_menu(self):
        na = self.spec.na
        shown = "NaN" if isinstance(na, float) and math.isnan(na) else str(na)
        menu = tk.Menu(self.widget, tearoff=0)
        menu.add_command(label=f"设为无数据（{shown}）",
                         command=lambda: self._set_na(shown))
        self.widget.bind("<Button-3>",
                         lambda e: menu.tk_popup(e.x_root, e.y_root))

    def _set_na(self, shown: str):
        self.var.set("nan" if shown == "NaN" else shown)
        self.submit()

    def _send_code(self):
        label = self.var.get()
        code = CODE_BY_LABEL.get(label)
        if code is None:
            # 允许直接键入裸代码
            m = re.match(r"\s*(\d{3})", label)
            code = m.group(1) if m else None
        if code is None:
            self.app.log(f"[!] 无法识别天气代码: {label}", "err")
            return
        self.app.send_field(self.spec.name, code)

    def _send_enum(self):
        value = self.enum_by_label.get(self.var.get())
        if value is None:
            self.app.log(f"[!] 无法识别枚举: {self.var.get()}", "err")
            return
        self.applied = self.var.get()   # 下拉本身即意图，不进脏状态
        self.app.send_field(self.spec.name, value)

    # ---- 脏标记 ----
    def _on_edit(self, *_):
        now_dirty = self.var.get() != self.applied
        if now_dirty != self.dirty:
            self.dirty = now_dirty
            self._paint()
            self.app.refresh_apply_button()

    def _paint(self):
        if self.spec.kind in ("b", "code", "enum"):
            return
        try:
            if self.dirty:
                self.widget.configure(background=DIRTY_BG, foreground="black")
            else:
                # ttk 主题下清掉自定义色：交回主题默认
                self.widget.configure(background="white", foreground="black")
        except tk.TclError:
            pass   # 某些主题不支持直接设色，脏状态仍由「应用改动 (N)」计数体现

    # ---- 提交 / 回填 ----
    def value_for_send(self):
        return self.var.get().strip()

    def submit(self):
        if self.spec.kind == "b":
            return
        val = self.value_for_send()
        if val == "":
            self.app.log(f"[!] {self.spec.label} 为空，跳过", "err")
            return
        self.app.send_field(self.spec.name, val)

    def set_value(self, raw: str):
        """服务端回来的值 → 控件（并清脏标记）。"""
        if self.spec.kind == "b":
            self.bvar.set(raw.lower() in ("1", "true", "on", "yes"))
            self.applied = raw
            return
        if self.spec.kind == "code":
            self.var.set(LABEL_BY_CODE.get(raw, raw))
            self.applied = self.var.get()
            return
        if self.spec.kind == "enum":
            # 数字值 → "值 中文" 标签（回读 / 场景后 read_all 都走这里）
            self.var.set(next((f"{v} {t}" for v, t in self.spec.choices
                               if str(v) == str(raw).strip()), raw))
            self.applied = self.var.get()
            return
        self.applied = raw
        self.var.set(raw)        # trace 会把 dirty 复位（值与 applied 相等）
        self.dirty = False
        self._paint()

    def confirm_applied(self):
        """set 已被设备确认 → 当前控件值即为设备值，清脏标记。"""
        if self.spec.kind == "b":
            return
        self.applied = self.var.get()
        if self.dirty:
            self.dirty = False
            self._paint()

    def mark_override(self, on: bool):
        self.lbl_ovr.configure(text="●" if on else " ")


# ---------------------------------------------------------------------------
# 主应用
# ---------------------------------------------------------------------------
class App(tk.Tk):
    def __init__(self, host=DEFAULT_HOST, port=DEFAULT_PORT, autoconnect=False,
                 geometry=None):
        super().__init__()
        self.title("RLCD 调试台")
        self.geometry(geometry or "1080x760")
        self.minsize(900, 600)

        self.host = tk.StringVar(value=host)
        self.port = tk.StringVar(value=str(port))
        self.resp_queue: queue.Queue = queue.Queue()
        self.sender = None
        self._connected = False

        self.rows: dict = {}          # name -> FieldRow
        self.history: list = []       # 手动命令历史
        self.hist_idx = 0

        self._setup_styles()
        self._build_ui()
        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self.after(RESP_POLL_MS, self._drain)
        # 自动连接：等窗口画完再连，连接失败也只是日志一行，不阻塞界面
        if autoconnect:
            self.after(100, self._toggle_connect)

    def _setup_styles(self):
        s = ttk.Style()
        try:
            s.theme_use("clam")       # clam 支持 Entry/Spinbox 背景色（脏标记要用）
        except tk.TclError:
            pass
        s.configure("TLabelframe.Label", font=("", 10, "bold"))
        s.configure("Accent.TButton", font=("", 10, "bold"))
        s.configure("Scene.TButton", font=("", 9), padding=(2, 4))

    # ==================== UI ====================
    def _build_ui(self):
        self._build_topbar()

        paned = ttk.PanedWindow(self, orient=tk.HORIZONTAL)
        paned.pack(fill=tk.BOTH, expand=True, padx=8, pady=(0, 6))

        left = ttk.Frame(paned)
        right = ttk.Frame(paned)
        paned.add(left, weight=3)
        paned.add(right, weight=2)

        self._build_fields(left)
        self._build_right(right)
        self._set_connected(False)

    def _build_topbar(self):
        top = ttk.Frame(self)
        top.pack(fill=tk.X, padx=8, pady=(8, 4))

        self.btn_connect = ttk.Button(top, text="连接", style="Accent.TButton",
                                      width=6, command=self._toggle_connect)
        self.btn_connect.pack(side=tk.LEFT)

        ttk.Label(top, text="  地址").pack(side=tk.LEFT)
        ttk.Entry(top, textvariable=self.host, width=13).pack(side=tk.LEFT, padx=(3, 4))
        ttk.Label(top, text="端口").pack(side=tk.LEFT)
        ttk.Entry(top, textvariable=self.port, width=6).pack(side=tk.LEFT, padx=(3, 10))

        self.lbl_status = ttk.Label(top, text="● 未连接", foreground="#888")
        self.lbl_status.pack(side=tk.LEFT)

        ttk.Button(top, text="清除 override",
                   command=lambda: self.run("clear", tag="after_clear")).pack(side=tk.RIGHT, padx=3)
        ttk.Button(top, text="读取全部", command=self.read_all).pack(side=tk.RIGHT, padx=3)

    # ---- 左栏：可滚动全字段表单 ----
    def _build_fields(self, parent):
        bar = ttk.Frame(parent)
        bar.pack(fill=tk.X, pady=(0, 4))
        ttk.Label(bar, text="ui_model 字段", font=("", 10, "bold")).pack(side=tk.LEFT)
        ttk.Label(bar, text="  ● = 已 override   右键数值框可设无数据",
                  foreground="#888", font=("", 8)).pack(side=tk.LEFT)

        # Canvas + Scrollbar 实现纵向滚动
        canvas = tk.Canvas(parent, highlightthickness=0, borderwidth=0)
        vsb = ttk.Scrollbar(parent, orient="vertical", command=canvas.yview)
        canvas.configure(yscrollcommand=vsb.set)

        inner = ttk.Frame(canvas)
        win = canvas.create_window((0, 0), window=inner, anchor="nw")

        def _on_inner(_e):
            canvas.configure(scrollregion=canvas.bbox("all"))
        inner.bind("<Configure>", _on_inner)
        # 内层宽度跟随 canvas，避免右侧留白
        canvas.bind("<Configure>", lambda e: canvas.itemconfigure(win, width=e.width))

        # 滚轮：指针在左栏内即生效
        def _wheel(e):
            canvas.yview_scroll(-1 if e.num == 4 else 1, "units")
        def _wheel_win(e):
            canvas.yview_scroll(int(-e.delta / 120), "units")
        for w in (canvas, inner):
            w.bind("<Button-4>", _wheel)
            w.bind("<Button-5>", _wheel)
            w.bind("<MouseWheel>", _wheel_win)

        # 分组
        for gname, specs in FIELD_GROUPS:
            box = ttk.Labelframe(inner, text=gname, padding=(6, 3))
            box.pack(fill=tk.X, expand=False, padx=2, pady=3)
            box.columnconfigure(1, weight=1)
            for i, spec in enumerate(specs):
                self.rows[spec.name] = FieldRow(self, box, i, spec)

        # 底部：批量提交
        act = ttk.Frame(parent)
        act.pack(fill=tk.X, pady=(4, 0))
        self.btn_apply = ttk.Button(act, text="应用改动", style="Accent.TButton",
                                    command=self.apply_dirty, state=tk.DISABLED)
        self.btn_apply.pack(side=tk.LEFT)
        ttk.Button(act, text="放弃改动", command=self.revert_dirty).pack(side=tk.LEFT, padx=4)

        canvas.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        vsb.pack(side=tk.RIGHT, fill=tk.Y)
        # pack 顺序：act 在 canvas 之前 pack 会占底部，这里让它保持在底
        act.pack_configure(side=tk.BOTTOM)
        bar.pack_configure(side=tk.TOP)

    # ---- 右栏 ----
    def _build_right(self, parent):
        # 页面导航
        nav = ttk.Labelframe(parent, text="页面", padding=(6, 4))
        nav.pack(fill=tk.X, pady=(0, 4))
        self.page_var = tk.IntVar(value=-1)
        grid = ttk.Frame(nav)
        grid.pack(fill=tk.X)
        for i, (pid, label) in enumerate(PAGES):
            ttk.Radiobutton(grid, text=label, value=pid, variable=self.page_var,
                            command=lambda p=pid: self.run(f"page {p}")
                            ).grid(row=i // 2, column=i % 2, sticky="w", padx=2)
        row = ttk.Frame(nav)
        row.pack(fill=tk.X, pady=(4, 0))
        ttk.Button(row, text="◀ 上一页", command=lambda: self.run("prev")).pack(side=tk.LEFT)
        ttk.Button(row, text="下一页 ▶", command=lambda: self.run("next")).pack(side=tk.LEFT, padx=4)

        # 场景预设
        sc = ttk.Labelframe(parent, text="场景预设（一键批量设值）", padding=(6, 4))
        sc.pack(fill=tk.X, pady=(0, 4))
        for i, (name, ops) in enumerate(SCENES):
            ttk.Button(sc, text=name, style="Scene.TButton", width=14,
                       command=lambda o=ops, n=name: self.apply_scene(n, o)
                       ).grid(row=i // 3, column=i % 3, sticky="ew", padx=2, pady=1)
        for c in range(3):
            sc.columnconfigure(c, weight=1)

        # 日历
        cal = ttk.Labelframe(parent, text="日历", padding=(6, 4))
        cal.pack(fill=tk.X, pady=(0, 4))
        cal.columnconfigure(1, weight=1)
        self.cal_marks = self._cal_row(cal, 0, "标注日期", "10-01,05-01",
                                      lambda v: f"marks {v}")
        self.cal_events = self._cal_row(cal, 1, "预定内容", "01-01=元旦;02-14=情人节",
                                       lambda v: f"events {v}")
        self.cal_labels = self._cal_row(cal, 2, "底部标签", "好好学习;天天向上",
                                       lambda v: f"labels {v}")
        ttk.Button(cal, text="清除所有标注",
                   command=lambda: self.run("unmark")).grid(row=3, column=1, sticky="e", pady=(3, 0))

        # 日志（可拉伸）
        logf = ttk.Labelframe(parent, text="响应日志", padding=(4, 2))
        logf.pack(fill=tk.BOTH, expand=True)
        self.txt_log = scrolledtext.ScrolledText(logf, wrap=tk.WORD,
                                                font=("monospace", 9), height=8)
        self.txt_log.pack(fill=tk.BOTH, expand=True)
        self.txt_log.tag_config("ok", foreground="#0a7a0a")
        self.txt_log.tag_config("err", foreground="#c00")
        self.txt_log.tag_config("sent", foreground="#777")
        self.txt_log.tag_config("info", foreground="#0a5")

        cmdf = ttk.Frame(logf)
        cmdf.pack(fill=tk.X, pady=(3, 0))
        ttk.Label(cmdf, text="▶").pack(side=tk.LEFT)
        self.cmd_var = tk.StringVar()
        ent = ttk.Entry(cmdf, textvariable=self.cmd_var)
        ent.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=3)
        ent.bind("<Return>", lambda _e: self._run_manual())
        ent.bind("<Up>", self._hist_up)
        ent.bind("<Down>", self._hist_down)
        ttk.Button(cmdf, text="发送", command=self._run_manual).pack(side=tk.LEFT)
        ttk.Button(cmdf, text="清屏",
                   command=lambda: self.txt_log.delete("1.0", tk.END)).pack(side=tk.LEFT, padx=(3, 0))

    def _cal_row(self, parent, row, label, placeholder, build):
        ttk.Label(parent, text=label).grid(row=row, column=0, sticky="w", pady=1)
        var = tk.StringVar()
        ent = ttk.Entry(parent, textvariable=var)
        ent.grid(row=row, column=1, sticky="ew", padx=4, pady=1)
        # 灰色提示文字：聚焦时清掉
        ent.insert(0, placeholder)
        ent.configure(foreground="#999")

        def _focus_in(_e):
            if ent.get() == placeholder:
                ent.delete(0, tk.END)
                ent.configure(foreground="black")

        def _focus_out(_e):
            if not ent.get().strip():
                ent.insert(0, placeholder)
                ent.configure(foreground="#999")

        def _send():
            v = var.get().strip()
            if not v or v == placeholder:
                self.log("[!] 内容为空", "err")
                return
            self.run(build(v))

        ent.bind("<FocusIn>", _focus_in)
        ent.bind("<FocusOut>", _focus_out)
        ent.bind("<Return>", lambda _e: _send())
        ttk.Button(parent, text="发送", width=5, command=_send).grid(row=row, column=2, pady=1)
        return var

    # ==================== 连接 ====================
    def _set_connected(self, ok: bool):
        self._connected = ok
        if ok:
            self.lbl_status.configure(
                text=f"● 已连接 {self.host.get()}:{self.port.get()}", foreground="#0a0")
            self.btn_connect.configure(text="断开")
        else:
            self.lbl_status.configure(text="● 未连接", foreground="#888")
            self.btn_connect.configure(text="连接")

    def _toggle_connect(self):
        if self._connected:
            if self.sender:
                self.sender.shutdown()
            self.sender = None
            self._set_connected(False)
            self.log("[*] 已断开", "info")
            return

        try:
            host, port = self.host.get().strip(), int(self.port.get())
        except ValueError:
            messagebox.showerror("地址错误", "端口必须是整数")
            return

        self.log(f"[*] 连接 {host}:{port} …", "info")
        self.btn_connect.configure(state=tk.DISABLED)
        sender = Sender(host, port, self.resp_queue)
        self.sender = sender
        sender.start()
        # 不 join 常驻线程（旧版的 4 秒卡顿就是这么来的）——轮询 Event
        self._poll_connect(sender, waited=0.0)

    def _poll_connect(self, sender: Sender, waited: float):
        if sender is not self.sender:
            return                          # 已被新的连接请求取代
        if sender.connected_evt.is_set():
            self.btn_connect.configure(state=tk.NORMAL)
            if sender.connect_err:
                self.log(f"[连接失败] {sender.connect_err}", "err")
                self.sender = None
                self._set_connected(False)
            else:
                self._set_connected(True)
                self.read_all()             # 连上立刻拉全量状态
            return
        if waited > CONN_TIMEOUT + 1.0:
            self.btn_connect.configure(state=tk.NORMAL)
            self.log("[连接失败] 超时", "err")
            self.sender = None
            self._set_connected(False)
            return
        self.after(50, lambda: self._poll_connect(sender, waited + 0.05))

    # ==================== 命令 ====================
    def run(self, cmd: str, tag: str = ""):
        if not self._connected or not self.sender:
            self.log(f"[!] 未连接，忽略: {cmd}", "err")
            return
        self.log(f">>> {cmd}", "sent")
        self.sender.enqueue(cmd, tag)

    def send_field(self, name: str, value):
        self.run(f"set {name} {value}", tag=f"set:{name}")

    def read_all(self):
        """一条 dump 拉全部字段 + 一条 ovr 拿 override 状态。"""
        self.run("dump", tag="dump")
        self.run("ovr", tag="ovr")

    def dirty_rows(self):
        return [r for r in self.rows.values() if r.dirty]

    def refresh_apply_button(self):
        n = len(self.dirty_rows())
        if n:
            self.btn_apply.configure(text=f"应用改动 ({n})", state=tk.NORMAL)
        else:
            self.btn_apply.configure(text="应用改动", state=tk.DISABLED)

    def apply_dirty(self):
        rows = self.dirty_rows()
        if not rows:
            return
        self.log(f"[*] 批量提交 {len(rows)} 个字段", "info")
        for r in rows:
            r.submit()

    def revert_dirty(self):
        """放弃未提交改动 → 重新从设备读回真值。"""
        if self.dirty_rows():
            self.log("[*] 放弃未提交改动，重新读取", "info")
        self.read_all()

    def apply_scene(self, name: str, ops):
        self.log(f"[*] 场景「{name}」：{len(ops)} 个字段", "info")
        for fname, val in ops:
            self.send_field(fname, val)
        # 场景发的是原始字面量（nan / -999 / true），设备存下来后的规范形式
        # 可能不同 —— 回读一遍让控件显示真实值。
        # 等一个数据节拍（sim_tick_data 每 500ms）再读，否则读到的可能是
        # 本轮 set 之前的值。
        self.after(650, self.read_all)

    def _run_manual(self):
        cmd = self.cmd_var.get().strip()
        if not cmd:
            return
        self.history.append(cmd)
        self.hist_idx = len(self.history)
        self.run(cmd, tag="manual")
        self.cmd_var.set("")

    def _hist_up(self, _e):
        if not self.history:
            return "break"
        self.hist_idx = max(0, self.hist_idx - 1)
        self.cmd_var.set(self.history[self.hist_idx])
        return "break"

    def _hist_down(self, _e):
        if not self.history:
            return "break"
        self.hist_idx = min(len(self.history), self.hist_idx + 1)
        self.cmd_var.set("" if self.hist_idx >= len(self.history)
                         else self.history[self.hist_idx])
        return "break"

    # ==================== 响应处理 ====================
    def _drain(self):
        try:
            while True:
                item = self.resp_queue.get_nowait()
                kind = item[0]
                if kind == "#info":
                    self.log(f"[+] {item[1]}", "info")
                elif kind == "#dead":
                    self.log(f"[!] {item[1]}", "err")
                    self.sender = None
                    self._set_connected(False)     # 旧版这里不复位，UI 一直显示已连接
                elif kind == "#resp":
                    _, cmd, lines, tag = item
                    self._handle_resp(cmd, lines, tag)
        except queue.Empty:
            pass
        self.after(RESP_POLL_MS, self._drain)

    def _handle_resp(self, cmd: str, lines, tag: str):
        # dump / get 的 val 行 → 回填控件
        filled = 0
        ok_set = False
        for line in lines:
            if line.startswith("val "):
                parts = line[4:].split(" ", 1)
                name = parts[0]
                val = parts[1] if len(parts) > 1 else ""
                row = self.rows.get(name)
                if row:
                    row.set_value(val)
                    filled += 1
                    continue
            if tag == "ovr" and line.startswith("ok "):
                self._apply_ovr(line[3:].strip())
                continue
            if tag.startswith("set:") and line.startswith("ok "):
                ok_set = True
            # 其余照常进日志
            self.log(f"<<< {line}",
                     "err" if line.startswith("error") else "ok")

        # set 成功 → 该字段的值已落到设备，清掉脏标记
        # （否则提交完输入框一直黄着，看起来像没生效）
        if ok_set:
            row = self.rows.get(tag[4:])
            if row:
                row.confirm_applied()

        if filled:
            self.log(f"<<< 回填 {filled} 个字段", "ok")

        self.refresh_apply_button()

        # clear 之后设备恢复自动填充，但要等模拟器的数据节拍跑过一轮才
        # 读得到恢复后的值 —— sim_tick_data() 是每 500ms 一次
        # （main.c: `if (now - last_data > 500)`），所以这里必须等 >500ms，
        # 否则 dump 读回的还是 clear 前的旧值。
        if tag == "after_clear":
            self.after(650, self.read_all)
        # set 之后刷新 override 标记；合并成一次，避免批量提交时
        # 每个字段都跟一条 ovr（13 个字段的场景会发 13 次）
        elif tag.startswith("set:"):
            self._schedule_ovr()

    def _schedule_ovr(self):
        """把连续多次 set 触发的 ovr 刷新合并成一次（去抖 150ms）。"""
        if getattr(self, "_ovr_pending", None):
            self.after_cancel(self._ovr_pending)
        self._ovr_pending = self.after(150, self._do_ovr)

    def _do_ovr(self):
        self._ovr_pending = None
        if self._connected and self.sender:
            self.sender.enqueue("ovr", "ovr")

    def _apply_ovr(self, names: str):
        active = set() if names in ("", "(none)") else set(names.split())
        for name, row in self.rows.items():
            row.mark_override(name in active)

    def log(self, msg: str, tag: str = ""):
        self.txt_log.insert(tk.END, msg + "\n", tag)
        self.txt_log.see(tk.END)

    def _on_close(self):
        if self.sender:
            self.sender.shutdown()
        self.destroy()


def main():
    host, port = DEFAULT_HOST, DEFAULT_PORT
    autoconnect = False
    geometry = None
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        arg = args[i]
        if arg == "--connect":
            autoconnect = True
        elif arg == "--geometry":
            i += 1
            if i >= len(args):
                print("--geometry 缺少参数（形如 1080x760+1400+40）", file=sys.stderr)
                return 1
            geometry = args[i]
        elif ":" in arg:
            h, _, p = arg.partition(":")
            host = h or DEFAULT_HOST
            try:
                port = int(p)
            except ValueError:
                print(f"端口无效: {p}", file=sys.stderr)
                return 1
        else:
            host = arg
        i += 1
    App(host, port, autoconnect, geometry).mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
