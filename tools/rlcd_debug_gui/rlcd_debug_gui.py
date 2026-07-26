#!/usr/bin/env python3
"""
rlcd_debug_gui —— RLCD 模拟器图形化调试客户端

通过 TCP 连接模拟器（默认 127.0.0.1:9000，与 rlcd_debug 同一协议），
用精确数值输入 + 按钮远程读写 ui_model 字段、切页、标注日历。
解决"开两个终端太麻烦"的问题：一个 GUI 窗口完成全部调试。

实现要点：
  - 单持久 TCP 连接：连一次，全部命令复用（匹配服务端 one-client 模型）。
  - 后台 sender 线程：socket 读写在后台，主线程通过"命令事件 + 响应队列"
    拿回响应 → UI 永不阻塞。
  - 数值字段用 Spinbox（可键盘直输 + 按钮步进），精确到 0.1。

协议（文本行，\n 分隔，响应以空行结束）：
  set <field> <value>        写 ui_model 字段，并设 override 位
  get <field>                读字段当前值
  page N / next / prev       切页
  mark MM-DD / unmark        日历标注
  marks CSV / events SPEC / labels SPEC
  clear                      清除所有 override（恢复 sim_tick_data 自动填充）
  ping / help / quit

依赖：Python 3.8+（仅标准库 —— tkinter + socket + threading）
用法：python3 rlcd_debug_gui.py
"""

import queue
import re
import socket
import threading
import tkinter as tk
from dataclasses import dataclass, field
from tkinter import ttk, scrolledtext, messagebox

# ---------------------------------------------------------------------------
# 协议常量
# ---------------------------------------------------------------------------
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 9000
CONN_TIMEOUT = 3.0    # 连接超时（秒）
RECV_TIMEOUT = 2.0    # 读响应超时（秒）
RESP_POLL_MS = 50     # 主线程轮询响应队列的间隔

# ---------------------------------------------------------------------------
# 字段元数据
# 分组 → [(字段名, 标签, 类型, 范围, 单位)]
#   类型 "num" : 数值 (lo, hi, step)  —— 用 Spinbox
#   类型 "code": 天气代码               —— 用下拉
#   类型 "bool": 布尔                  —— 用大按钮切换
#   类型 "text": 字符串                —— 用 Entry
# ---------------------------------------------------------------------------
FIELD_GROUPS = [
    ("环境", [
        ("indoor_temp",     "室内温度",     "num",  (-10.0, 50.0, 0.1),  "°C"),
        ("indoor_humi",     "室内湿度",     "num",  (0.0, 100.0, 0.1),   "%"),
        ("outdoor_temp",    "室外温度",     "num",  (-30.0, 55.0, 0.1),  "°C"),
        ("outdoor_humi",    "室外湿度",     "num",  (0.0, 100.0, 0.1),   "%"),
        ("feels_like_temp", "体感温度",     "num",  (-30.0, 55.0, 0.1),  "°C"),
        ("pressure",        "气压",         "num",  (900, 1100, 1),      "hPa"),
        ("visibility",      "能见度",       "num",  (0, 50, 1),          "km"),
    ]),
    ("天气", [
        ("weather_code", "天气代码",   "code", None,              "QWeather icon"),
        ("weather_text", "天气文本",   "text", None,              "晴/多云/小雨…"),
        ("city",         "城市",       "text", None,              "城市名"),
        ("temp_min",     "最低温度",   "num",  (-30.0, 55.0, 0.1),"°C"),
        ("temp_max",     "最高温度",   "num",  (-30.0, 55.0, 0.1),"°C"),
        ("sunrise",      "日出",       "text", None,              "HH:MM"),
        ("sunset",       "日落",       "text", None,              "HH:MM"),
        ("uv",           "紫外线指数", "num",  (0, 15, 1),         "UVI"),
        ("cloud",        "云量",       "num",  (0, 100, 1),       "%"),
    ]),
    ("风", [
        ("wind_speed", "风速",   "num", (0.0, 60.0, 0.1), "km/h"),
        ("wind_dir",   "风向",   "text", None,             "北风/东南…"),
    ]),
    ("状态", [
        ("wifi_connected",  "WiFi 已连接", "bool", None, ""),
        ("wifi_rssi",       "WiFi 信号",   "num",  (-100, 0, 1),    "dBm"),
        ("battery_percent", "电池电量",    "num",  (0, 100, 1),     "%"),
        ("battery_charging","充电中",      "bool", None,           ""),
    ]),
]

# 和风天气 icon 代码 → 中文描述（实况 API 返回的子集）
WEATHER_CODES = [
    ("100", "晴"), ("101", "多云"), ("102", "少云"), ("103", "阴"), ("104", "阴"),
    ("150", "晴（夜）"), ("151", "多云（夜）"), ("152", "少云（夜）"), ("153", "阴（夜）"),
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
    ("511", "中度霾"), ("512", "重度霾"), ("513", "严重霾"), ("514", "大雾"), ("515", "特强浓雾"),
    ("900", "未知"),
]


# ---------------------------------------------------------------------------
# 后台 sender 线程：维护一条持久 TCP 连接，循环处理命令队列
# ---------------------------------------------------------------------------
@dataclass
class Cmd:
    text: str
    evt: threading.Event = field(default_factory=threading.Event)
    resp: str = ""


class Sender(threading.Thread):
    def __init__(self, host: str, port: int, resp_queue: queue.Queue):
        super().__init__(daemon=True)
        self.host = host
        self.port = port
        self.cmd_q: queue.Queue[Cmd] = queue.Queue()
        self.resp_q = resp_queue     # 主线程从这里取响应
        self.sock: socket.socket | None = None
        self._stop = threading.Event()
        self.ok = False

    def run(self):
        try:
            self.sock = socket.create_connection((self.host, self.port), timeout=CONN_TIMEOUT)
            self.sock.settimeout(RECV_TIMEOUT)
            self.ok = True
        except OSError as e:
            self.resp_q.put(f"[连接失败] {e}")
            return

        self.resp_q.put(f"[+] 已连通 {self.host}:{self.port}")

        while not self._stop.is_set():
            try:
                cmd = self.cmd_q.get(timeout=0.1)
            except queue.Empty:
                continue
            if cmd is None:  # 毒丸 → 退出
                break
            resp = self._roundtrip(cmd.text)
            cmd.resp = resp
            cmd.evt.set()
            self.resp_q.put((cmd.text, resp))

        if self.sock:
            try: self.sock.close()
            except OSError: pass

    def _roundtrip(self, cmd: str) -> str:
        try:
            self.sock.sendall((cmd + "\n").encode())
            parts, buf = [], b""
            while True:
                try:
                    chunk = self.sock.recv(4096)
                except socket.timeout:
                    break
                if not chunk:
                    return "\n".join(parts) if parts else "(连接断开)"
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    d = line.decode(errors="replace").strip()
                    if d == "":
                        return "\n".join(parts)
                    parts.append(d)
            if buf:
                parts.append(buf.decode(errors="replace").strip())
            return "\n".join(parts) if parts else "(无响应)"
        except OSError as e:
            return f"[连接断开] {e}"

    def enqueue(self, text: str):
        self.cmd_q.put(Cmd(text=text))

    def shutdown(self):
        self._stop.set()
        self.cmd_q.put(None)


# ---------------------------------------------------------------------------
# 主应用
# ---------------------------------------------------------------------------
class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("RLCD 调试客户端")
        self.geometry("780x720")
        self.minsize(700, 560)

        self.host = tk.StringVar(value=DEFAULT_HOST)
        self.port = tk.StringVar(value=str(DEFAULT_PORT))
        self.resp_queue: queue.Queue = queue.Queue()
        self.sender: Sender | None = None

        self.field_vars: dict[str, tk.Variable] = {}
        self.field_meta: dict[str, dict] = {}

        self._setup_styles()
        self._build_ui()
        self.protocol("WM_DELETE_WINDOW", self._on_close)

    # ---- 样式 ----------------------------------------------------------
    def _setup_styles(self):
        s = ttk.Style()
        s.configure("TLabelframe.Label", font=("", 11, "bold"))
        s.configure("Big.TButton", font=("", 11), padding=(12, 6))
        s.configure("Accent.TButton", font=("", 11, "bold"), padding=(14, 8))
        s.configure("Status.TLabel", font=("", 11))
        s.configure("Group.TLabelframe.Label", font=("", 12, "bold"))

    # ---- UI 构建 -------------------------------------------------------
    def _build_ui(self):
        # ── 顶部：单行连接栏（左:连接+地址+状态  右:动作按钮）─────
        top = ttk.Frame(self)
        top.pack(fill=tk.X, padx=10, pady=(8, 4))

        self.btn_connect = ttk.Button(top, text="连接", style="Accent.TButton", command=self._toggle_connect)
        self.btn_connect.pack(side=tk.LEFT, padx=(0, 6))

        ttk.Label(top, text="地址", font=("", 11)).pack(side=tk.LEFT)
        ttk.Entry(top, textvariable=self.host, width=12, font=("", 11)).pack(side=tk.LEFT, padx=(2, 6))
        ttk.Label(top, text="端口", font=("", 11)).pack(side=tk.LEFT)
        ttk.Entry(top, textvariable=self.port, width=5, font=("", 11)).pack(side=tk.LEFT, padx=(2, 10))

        self.lbl_status = ttk.Label(top, text="● 未连接", foreground="#888")
        self.lbl_status.pack(side=tk.LEFT)

        # 右侧动作按钮
        ttk.Button(top, text="读取全部", style="Big.TButton", command=self._read_all).pack(side=tk.RIGHT, padx=3)
        ttk.Button(top, text="清除 override", style="Big.TButton",
                   command=lambda: self._run_cmd("clear")).pack(side=tk.RIGHT, padx=3)
        ttk.Button(top, text="Ping", style="Big.TButton", command=lambda: self._run_cmd("ping")).pack(side=tk.RIGHT, padx=3)

        # ── 中部：Notebook（4 个分组 tab，grid 对齐）───────────────
        self.notebook = ttk.Notebook(self)
        self.notebook.pack(fill=tk.BOTH, expand=True, padx=10, pady=4)
        self._build_field_tabs()

        # ── 底部：单行控制（手动命令 + 页面 + 日历）───────────────
        bottom = ttk.Frame(self)
        bottom.pack(fill=tk.X, padx=10, pady=(0, 4))

        # 手动命令
        ttk.Label(bottom, text="▶", font=("", 11)).pack(side=tk.LEFT)
        self.cmd_var = tk.StringVar()
        ent = ttk.Entry(bottom, textvariable=self.cmd_var, width=22, font=("", 11))
        ent.pack(side=tk.LEFT, padx=(4, 4))
        ent.bind("<Return>", lambda _e: self._run_manual())
        ttk.Button(bottom, text="发送", style="Accent.TButton", command=self._run_manual).pack(side=tk.LEFT, padx=(0, 10))

        ttk.Separator(bottom, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=4)

        # 页面导航
        ttk.Button(bottom, text="◀", width=3, command=lambda: self._run_cmd("prev")).pack(side=tk.LEFT, padx=2)
        ttk.Button(bottom, text="▶", width=3, command=lambda: self._run_cmd("next")).pack(side=tk.LEFT, padx=2)
        self.page_var = tk.StringVar(value="0")
        ttk.Entry(bottom, textvariable=self.page_var, width=3, font=("", 11)).pack(side=tk.LEFT, padx=(6, 2))
        ttk.Button(bottom, text="跳转", width=4, command=self._goto_page).pack(side=tk.LEFT, padx=(0, 10))

        ttk.Separator(bottom, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=4)

        # 日历标注
        self.mark_var = tk.StringVar()
        ttk.Entry(bottom, textvariable=self.mark_var, width=8, font=("", 11)).pack(side=tk.LEFT, padx=2)
        ttk.Button(bottom, text="标注", width=4, command=self._mark).pack(side=tk.LEFT, padx=2)
        ttk.Button(bottom, text="取消", width=4, command=lambda: self._run_cmd("unmark")).pack(side=tk.LEFT, padx=2)

        # ── 日志（底部固定高度，不被挤压）────────────────────────
        log_frame = ttk.LabelFrame(self, text="响应日志", padding=4)
        log_frame.pack(fill=tk.X, padx=10, pady=(0, 8))
        self.txt_log = scrolledtext.ScrolledText(log_frame, wrap=tk.WORD, font=("monospace", 10), height=6)
        self.txt_log.pack(fill=tk.X)
        self.txt_log.tag_config("ok", foreground="#0a7a0a")
        self.txt_log.tag_config("err", foreground="#c00")
        self.txt_log.tag_config("sent", foreground="#555")

        self._set_connected(False)
        self.after(RESP_POLL_MS, self._drain_responses)

    def _build_field_tabs(self):
        """4 个分组 tab，每 tab 内用 grid 对齐：标签 | 输入控件 | 设置按钮。"""
        for group_name, fields in FIELD_GROUPS:
            tab = ttk.Frame(self.notebook, padding=8)
            self.notebook.add(tab, text=f"  {group_name}  ")
            # grid 列：0=标签, 1=输入控件, 2=单位, 3=设置按钮
            tab.columnconfigure(1, weight=1)   # 输入列可拉伸
            for row_idx, (fname, label, ftype, rng, unit) in enumerate(fields):
                self._add_field(tab, row_idx, fname, label, ftype, rng, unit)

    def _add_field(self, parent, row, fname, label, ftype, rng, unit):
        """grid 单行：标签(0) | 输入控件(1) | 单位(2) | 小设置按钮(3)。"""
        meta = {"type": ftype}

        # 列 0：字段名标签
        ttk.Label(parent, text=label, font=("", 11), anchor="w").grid(
            row=row, column=0, sticky="w", padx=(0, 8), pady=3)

        if ftype == "bool":
            var = tk.BooleanVar(value=False)
            chk = ttk.Checkbutton(parent, text="开启", variable=var,
                                  command=lambda f=fname, v=var: self._run_cmd(
                                      f"set {f} {'true' if v.get() else 'false'}"))
            chk.grid(row=row, column=1, sticky="w", pady=3)
            meta["var"] = var

        elif ftype == "code":
            var = tk.StringVar(value="100")
            code_names = [f"{c}  {t}" for c, t in WEATHER_CODES]
            cb = ttk.Combobox(parent, textvariable=var, values=code_names, width=16,
                              state="readonly", font=("", 11))
            cb.grid(row=row, column=1, sticky="ew", pady=3)
            # 选中不即发，必须点「设」
            ttk.Button(parent, text="设置", style="Accent.TButton",
                       command=lambda f=fname, v=var: self._run_cmd(
                           f"set {f} {v.get().split()[0]}")).grid(
                row=row, column=3, padx=(4, 0), pady=3)
            meta["var"] = var

        elif ftype == "num":
            lo, hi, step = rng
            is_int = isinstance(step, int) and step == 1 and isinstance(lo, int)
            var = tk.DoubleVar(value=float(lo)) if not is_int else tk.IntVar(value=lo)

            fmt = "%d" if is_int else "%.1f"
            sb = ttk.Spinbox(parent, from_=lo, to=hi, increment=step,
                             textvariable=var, width=10, font=("", 12), format=fmt)
            sb.grid(row=row, column=1, sticky="ew", pady=3)

            # 直接回车发送
            sb.bind("<Return>", lambda e, f=fname, v=var, i=is_int:
                    self._run_cmd(f"set {f} {int(v.get()) if i else round(float(v.get()), 1)}"))

            # 校验：只接受数字
            def _validate(s, is_int=is_int):
                if s in ("", "-"):
                    return True
                try:
                    int(s) if is_int else float(s)
                    return True
                except ValueError:
                    return False
            sb.config(validate="key", validatecommand=(self.register(_validate), "%P"))

            # 单位标签（列 2）
            if unit:
                ttk.Label(parent, text=unit, font=("", 11)).grid(
                    row=row, column=2, sticky="w", padx=(4, 4), pady=3)

            ttk.Button(parent, text="设置", style="Accent.TButton",
                       command=lambda f=fname, v=var, i=is_int: self._run_cmd(
                           f"set {f} {int(v.get()) if i else round(float(v.get()), 1)}")).grid(
                row=row, column=3, padx=(4, 0), pady=3)
            meta["var"] = var

        else:  # text
            var = tk.StringVar(value="")
            ent = ttk.Entry(parent, textvariable=var, width=16, font=("", 11))
            ent.grid(row=row, column=1, sticky="ew", pady=3)
            ent.bind("<Return>", lambda e, f=fname, v=var: self._run_cmd(f"set {f} {v.get()}"))
            if unit:
                ttk.Label(parent, text=unit, font=("", 11)).grid(
                    row=row, column=2, sticky="w", padx=(4, 4), pady=3)
            ttk.Button(parent, text="设置", style="Accent.TButton",
                       command=lambda f=fname, v=var: self._run_cmd(
                           f"set {f} {v.get()}")).grid(
                row=row, column=3, padx=(4, 0), pady=3)
            meta["var"] = var

        self.field_vars[fname] = var
        self.field_meta[fname] = meta

    def _read_all(self):
        """全局读取按钮：依次 get 所有字段。"""
        if not self._connected:
            return
        all_fields = [fname for group in FIELD_GROUPS for fname, *_ in group[1]]
        for f in all_fields:
            self._run_cmd(f"get {f}")

    # ------------------------------------------------------------------
    # 连接管理
    # ------------------------------------------------------------------
    def _set_connected(self, ok: bool):
        self._connected = ok
        if ok:
            self.lbl_status.config(text=f"● 已连接 {self.host.get()}:{self.port.get()}", foreground="#0a0")
            self.btn_connect.config(text="断开")
        else:
            self.lbl_status.config(text="● 未连接", foreground="#888")
            self.btn_connect.config(text="连接")

    def _toggle_connect(self):
        if self._connected and self.sender:
            # 断开
            self.sender.shutdown()
            self.sender = None
            self._set_connected(False)
            self._log("[*] 已断开", tag="ok")
            return

        host, port = self._get_addr()
        if host is None:
            return
        self._log(f"[*] 连接 {host}:{port} …")
        self.btn_connect.config(state=tk.DISABLED)

        def worker():
            self.sender = Sender(host, port, self.resp_queue)
            self.sender.start()
            # 等线程报回结果（最多 CONN_TIMEOUT+1 秒）
            self.sender.join(timeout=CONN_TIMEOUT + 1.0)
            if not self.sender.ok:
                self.after(0, lambda: self._set_connected(False))
                self.after(0, lambda: self.btn_connect.config(state=tk.NORMAL))
                self.after(0, lambda: (self.sender.shutdown(), setattr(self, "sender", None)))
            else:
                self.after(0, lambda: self._set_connected(True))
                self.after(0, lambda: self.btn_connect.config(state=tk.NORMAL))
                self.after(0, lambda: self._run_cmd("ping"))
        threading.Thread(target=worker, daemon=True).start()

    def _get_addr(self):
        try:
            return self.host.get().strip(), int(self.port.get())
        except ValueError:
            messagebox.showerror("地址错误", "端口必须是整数")
            return None, None

    # ------------------------------------------------------------------
    # 命令执行（入队到后台 sender）
    # ------------------------------------------------------------------
    def _run_cmd(self, cmd: str):
        if not self._connected or not self.sender:
            self._log(f"[!] 未连接，忽略: {cmd}", tag="err")
            return
        self._log(f">>> {cmd}", tag="sent")
        self.sender.enqueue(cmd)

    def _run_manual(self):
        cmd = self.cmd_var.get().strip()
        if cmd:
            self._run_cmd(cmd)
            self.cmd_var.set("")

    def _goto_page(self):
        try:
            n = int(self.page_var.get())
            self._run_cmd(f"page {n}")
        except ValueError:
            messagebox.showerror("错误", "页码必须是整数")

    def _mark(self):
        txt = self.mark_var.get().strip()
        if not txt:
            return
        if "," in txt:
            self._run_cmd(f"marks {txt}")
        elif re.fullmatch(r"\d{2}-\d{2}", txt):
            self._run_cmd(f"mark {txt}")
        else:
            messagebox.showerror("格式错误", "标注格式：MM-DD 或 MM-DD,MM-DD,...")

    # ------------------------------------------------------------------
    # 响应轮询（主线程定时从队列取响应，写日志）
    # ------------------------------------------------------------------
    def _drain_responses(self):
        try:
            while True:
                item = self.resp_queue.get_nowait()
                if isinstance(item, tuple):
                    cmd, resp = item
                    tag = "err" if resp.lower().startswith(("error", "[连接")) else "ok"
                    for line in resp.split("\n"):
                        self._log(f"<<< {line}", tag=tag)
                else:
                    tag = "err" if item.startswith("[连接失败") else "ok"
                    self._log(item, tag=tag)
        except queue.Empty:
            pass
        self.after(RESP_POLL_MS, self._drain_responses)

    def _log(self, msg: str, tag: str = ""):
        self.txt_log.insert(tk.END, msg + "\n", tag)
        self.txt_log.see(tk.END)

    # ------------------------------------------------------------------
    # 关闭
    # ------------------------------------------------------------------
    def _on_close(self):
        if self.sender:
            self.sender.shutdown()
        self.destroy()


def main():
    app = App()
    app.mainloop()


if __name__ == "__main__":
    main()