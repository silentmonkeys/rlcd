#!/usr/bin/env bash
# dev_sim.sh —— 一条命令拉起「桌面模拟器 + 图形化调试台」
#
# 干的事：编译模拟器（增量）→ 后台起 rlcd_sim → 等 TCP 9000 可连 →
# 前台起 rlcd_debug_gui（自动连接）→ GUI 关掉时顺手把模拟器收掉。
# 两个窗口**并排摆好、不重叠**：左边模拟器（800×600），右边调试台，
# 按屏幕分辨率算坐标，屏幕太窄时自动退成上下叠放。
#
# 用法：
#   bash tools/dev_sim.sh                 # 编译 + 起模拟器 + 起 GUI
#   bash tools/dev_sim.sh --page 3        # 启动时切到设备信息页
#   bash tools/dev_sim.sh --no-build      # 跳过编译，直接跑现成 build/rlcd_sim
#   bash tools/dev_sim.sh --rebuild       # 删 build 重新 cmake（改了 CMakeLists 用）
#   bash tools/dev_sim.sh --sim-only      # 只起模拟器（前台，日志直出）
#   bash tools/dev_sim.sh --gui-only      # 只起 GUI（连已在跑的模拟器）
#   bash tools/dev_sim.sh --gui-only --port 9001   # 连别的端口（只对 GUI 有意义，见下）
#   bash tools/dev_sim.sh --no-layout     # 不管窗口位置，交给窗口管理器
#   bash tools/dev_sim.sh --stack         # 强制上下叠放（宽屏也用）
#
# 模拟器 stdout/stderr 落到 _logs/sim-<时间戳>.log（_logs/ 已 gitignore）。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SIM_DIR="$ROOT/simulator"
SIM_BUILD="$SIM_DIR/build"
SIM_BIN="$SIM_BUILD/rlcd_sim"
GUI="$ROOT/tools/rlcd_debug_gui/rlcd_debug_gui.py"
LOG_DIR="$ROOT/_logs"

PORT=9000
DO_BUILD=1
DO_REBUILD=0
SIM_ONLY=0
GUI_ONLY=0
DO_LAYOUT=1
FORCE_STACK=0
SIM_ARGS=()

die() { printf '\033[31m[dev_sim] %s\033[0m\n' "$*" >&2; exit 1; }
info() { printf '\033[36m[dev_sim]\033[0m %s\n' "$*"; }

while [ $# -gt 0 ]; do
  case "$1" in
    --no-build)  DO_BUILD=0 ;;
    --rebuild)   DO_REBUILD=1 ;;
    --sim-only)  SIM_ONLY=1 ;;
    --gui-only)  GUI_ONLY=1; DO_BUILD=0 ;;
    --no-layout) DO_LAYOUT=0 ;;
    --stack)     FORCE_STACK=1 ;;
    --port)      PORT="${2:?--port 缺少参数}"; shift ;;
    --page)      SIM_ARGS+=(--page "${2:?--page 缺少参数}"); shift ;;
    -h|--help)   sed -n '2,24p' "${BASH_SOURCE[0]}"; exit 0 ;;
    # 其余参数原样透传给模拟器（--temp / --humi / --gallery / --capture …）
    *)           SIM_ARGS+=("$1") ;;
  esac
  shift
done

# 模拟器侧端口是 sim_console_init(0) 写死的 9000（simulator/main.c），没有 CLI 开关，
# 所以 --port 只能配 --gui-only 用来连远程/别的实例；本地拉起时换端口是连不上的。
if [ "$PORT" != 9000 ] && [ "$GUI_ONLY" = 0 ]; then
  die "--port 只能配 --gui-only 使用：模拟器的监听端口在 simulator/main.c 里固定为 9000"
fi

# ---------------------------------------------------------------- 窗口排版
# 两个窗口都不会自己躲开对方（SDL 默认居中，Tk 默认也在屏幕中间），
# 所以这里按屏幕尺寸算好坐标，分别用 --pos / --geometry 传进去 ——
# 创建时就定位，比事后 wmctrl 搬窗口可靠（WSLg 下搬窗口经常不生效）。
SIM_W=800 SIM_H=600          # main.c: LCD_W*SCALE x LCD_H*SCALE
GUI_W=1080 GUI_H=760         # rlcd_debug_gui.py 的默认尺寸
GAP=12                       # 窗口间距
MARGIN=40                    # 离屏幕左/上边的余量（躲开任务栏）
SIM_POS="" GUI_GEOM=""

screen_size() {
  # xdpyinfo 拿主屏分辨率；没有就退到 tkinter；都失败则返回空
  local dims
  dims=$(xdpyinfo 2>/dev/null | awk '/dimensions:/ {print $2; exit}')
  if [ -z "$dims" ]; then
    dims=$(python3 -c 'import tkinter as tk;r=tk.Tk();print("%dx%d"%(r.winfo_screenwidth(),r.winfo_screenheight()));r.destroy()' 2>/dev/null)
  fi
  [ -n "$dims" ] && printf '%s' "$dims"
}

plan_layout() {
  local dims sw sh
  dims=$(screen_size)
  if [ -z "$dims" ]; then
    info "拿不到屏幕分辨率，跳过窗口排版"
    return
  fi
  sw=${dims%x*}
  sh=${dims#*x}

  local need_w=$((SIM_W + GAP + GUI_W + MARGIN * 2))
  if [ "$FORCE_STACK" = 0 ] && [ "$sw" -ge "$need_w" ]; then
    # 并排：模拟器在左，调试台在右，垂直方向都靠上对齐
    local y=$MARGIN
    SIM_POS="$MARGIN,$y"
    GUI_GEOM="${GUI_W}x${GUI_H}+$((MARGIN + SIM_W + GAP))+${y}"
    info "并排布局（屏幕 ${sw}x${sh}）：模拟器左 / 调试台右"
  else
    # 屏幕不够宽：上下叠放。GUI 高度按剩余空间压缩，但不低于 minsize(600)
    local gui_h=$((sh - SIM_H - GAP - MARGIN * 2))
    [ "$gui_h" -lt 600 ] && gui_h=600
    SIM_POS="$MARGIN,$MARGIN"
    GUI_GEOM="${GUI_W}x${gui_h}+${MARGIN}+$((MARGIN + SIM_H + GAP))"
    if [ "$FORCE_STACK" = 1 ]; then
      info "上下布局（--stack）：模拟器上 / 调试台下"
    else
      info "屏幕宽 ${sw} < 并排所需 ${need_w}，改上下布局"
    fi
  fi
}

if [ "$DO_LAYOUT" = 1 ]; then
  plan_layout
  # --pos 只在本脚本拉起模拟器时加；--gui-only 时模拟器不是我们起的
  [ -n "$SIM_POS" ] && [ "$GUI_ONLY" = 0 ] && SIM_ARGS+=(--pos "$SIM_POS")
fi

# ---------------------------------------------------------------- 编译
if [ "$DO_REBUILD" = 1 ]; then
  info "删除 $SIM_BUILD 重新配置"
  rm -rf "$SIM_BUILD"
fi
if [ "$DO_BUILD" = 1 ]; then
  command -v cmake >/dev/null || die "找不到 cmake：sudo apt install cmake build-essential libsdl2-dev"
  info "编译模拟器…"
  cmake -S "$SIM_DIR" -B "$SIM_BUILD" >/dev/null || die "cmake 配置失败"
  cmake --build "$SIM_BUILD" -j"$(nproc)" || die "编译失败"
fi
[ "$GUI_ONLY" = 1 ] || [ -x "$SIM_BIN" ] || die "$SIM_BIN 不存在，去掉 --no-build 先编译"

# ---------------------------------------------------------------- 端口占用
port_busy() { ss -ltn "sport = :$PORT" 2>/dev/null | grep -q LISTEN; }

if [ "$GUI_ONLY" = 0 ] && port_busy; then
  info "端口 $PORT 已被占用（可能有旧的 rlcd_sim 在跑），先收掉"
  pkill -f "$SIM_BIN" 2>/dev/null
  for _ in $(seq 20); do port_busy || break; sleep 0.1; done
  port_busy && die "端口 $PORT 仍被占用，手动查一下：ss -ltnp \"sport = :$PORT\""
fi

# ---------------------------------------------------------------- 只起模拟器
if [ "$SIM_ONLY" = 1 ]; then
  info "前台运行：$SIM_BIN ${SIM_ARGS[*]-}"
  exec "$SIM_BIN" "${SIM_ARGS[@]}"
fi

# ---------------------------------------------------------------- 起模拟器（后台）
SIM_PID=""
cleanup() {
  if [ -n "$SIM_PID" ] && kill -0 "$SIM_PID" 2>/dev/null; then
    info "收掉模拟器 (pid $SIM_PID)"
    kill "$SIM_PID" 2>/dev/null
    wait "$SIM_PID" 2>/dev/null
  fi
}
trap cleanup EXIT INT TERM

if [ "$GUI_ONLY" = 0 ]; then
  [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] || die "没有 DISPLAY —— WSL 里需要 WSLg 或 X server"
  mkdir -p "$LOG_DIR"
  # 时间戳由 date 取，脚本自身不产生 build 目录以外的东西
  SIM_LOG="$LOG_DIR/sim-$(date +%Y%m%d-%H%M%S).log"
  "$SIM_BIN" "${SIM_ARGS[@]}" >"$SIM_LOG" 2>&1 &
  SIM_PID=$!
  info "模拟器 pid $SIM_PID，日志 $SIM_LOG"

  # 等它把 TCP server 起起来再拉 GUI，否则 GUI 会连失败要手点重连
  for _ in $(seq 50); do            # 最多 5s
    kill -0 "$SIM_PID" 2>/dev/null || { info "模拟器启动即退出，日志尾部："; tail -20 "$SIM_LOG"; exit 1; }
    port_busy && break
    sleep 0.1
  done
  port_busy || { info "等不到 $PORT 监听，日志尾部："; tail -20 "$SIM_LOG"; exit 1; }
  info "调试端口 $PORT 就绪"
fi

# ---------------------------------------------------------------- 起 GUI（前台）
command -v python3 >/dev/null || die "找不到 python3"
python3 -c 'import tkinter' 2>/dev/null || die "缺 tkinter：sudo apt install python3-tk"

info "启动调试台（关掉窗口即退出）"
GUI_ARGS=("127.0.0.1:$PORT" --connect)
[ -n "$GUI_GEOM" ] && GUI_ARGS+=(--geometry "$GUI_GEOM")
python3 "$GUI" "${GUI_ARGS[@]}"
