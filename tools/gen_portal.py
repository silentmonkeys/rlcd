#!/usr/bin/env python3
"""把 components/net_bsp/portal/ 下的前端源码打包成固件用的 portal_assets.h。

设计动机 —— 上一版把 HTML/CSS/JS 三份大字符串常量手写在 portal_assets.h 里，同时
在 simulator/portal_preview.html 里手抄了一份带 mock 的副本。两份手抄很快就漂移：
固件那份的 HTML 丢了 <script> 标签，于是 app.js 虽然照常被 /app.js 提供，浏览器却
从来没加载过 —— 数据不刷新、标签页点不动、右上角永远停在 "连接中…"。

现在唯一权威源是 portal/ 目录下的普通 .html/.css/.js 文件（可编辑、可 lint），
本脚本负责：
  * 把 CSS 和 app.js 内联进 index.html（占位符 {{STYLE}} / {{APP}}），
    首屏从 3 个请求变 1 个 —— ESP32-S3 的 TCP 窗口只有 5760B，少握手就是快
  * gzip 预压缩，运行时零 CPU 开销地发 Content-Encoding: gzip
  * 生成 portal_assets.h（uint8_t 数组，放 flash rodata，运行时 0 RAM）
  * 生成 simulator/portal_preview.html —— 同一份 HTML/CSS/JS 注入 mock fetch，
    浏览器直接打开就能看全部效果，不必烧录

用法：python3 tools/gen_portal.py
改完 portal/ 下任何文件都要重跑，然后 idf.py build。
"""

import gzip
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "components" / "net_bsp" / "portal"
OUT_H = ROOT / "components" / "net_bsp" / "src" / "portal_assets.h"
OUT_PREVIEW = ROOT / "simulator" / "portal_preview.html"

# Chart.js 单独一个资源：约 70KB(gzip)，只有打开"数据"页才按需下载，
# 不拖慢首屏。其余资源全部内联进 index.html。
CHART = SRC / "vendor" / "chart.umd.min.js"
CHART_URL = "/chart.umd.min.js"


def c_array(name: str, data: bytes, comment: str) -> str:
    """把字节串写成 static const uint8_t 数组（每行 16 字节）。"""
    lines = [f"// {comment}", f"static const uint8_t {name}[] = {{"]
    for i in range(0, len(data), 16):
        chunk = data[i : i + 16]
        lines.append("    " + "".join(f"0x{b:02x}," for b in chunk))
    lines.append("};")
    lines.append(f"#define {name}_LEN  {len(data)}")
    return "\n".join(lines)


def build_index() -> str:
    """读 index.html，把 {{STYLE}} / {{APP}} / {{CHART_URL}} 换成真实内容。"""
    html = (SRC / "index.html").read_text(encoding="utf-8")
    css = (SRC / "style.css").read_text(encoding="utf-8")
    app = (SRC / "app.js").read_text(encoding="utf-8")

    for token in ("{{STYLE}}", "{{APP}}", "{{CHART_URL}}"):
        if token not in html:
            sys.exit(f"错误：index.html 缺少占位符 {token}")

    # 先替 CHART_URL（短），再替两个大块；用 lambda 避免 CSS/JS 里的
    # 反斜杠被 re 当成替换组转义
    html = html.replace("{{CHART_URL}}", CHART_URL)
    html = html.replace("{{STYLE}}", css.strip())
    html = html.replace("{{APP}}", app.strip())

    # 内联后不能再残留 <script src=/app.js> 之类的外链，否则又会漏加载
    stray = re.findall(r"<script[^>]*\bsrc\s*=", html)
    if stray:
        sys.exit(f"错误：内联后仍有外链 script：{stray}")
    # 反过来也要保证 app.js 真的进来了 —— 正是上一版丢掉的那一步
    if "function tick(" not in html:
        sys.exit("错误：app.js 没有被内联进 index.html")
    return html


def main() -> None:
    html = build_index()
    html_gz = gzip.compress(html.encode("utf-8"), 9, mtime=0)

    chart = CHART.read_bytes()
    chart_gz = gzip.compress(chart, 9, mtime=0)

    parts = [
        "// portal_assets.h —— 配网 / 管理门户前端资源（**自动生成，请勿手改**）",
        "//",
        "// 由 tools/gen_portal.py 从 components/net_bsp/portal/ 生成。",
        "// 要改前端就改 portal/{index.html,style.css,app.js}，然后重跑：",
        "//     python3 tools/gen_portal.py",
        "//",
        "// 所有资源都是 gzip 预压缩的字节数组，存 flash rodata，运行时 0 RAM、0 CPU：",
        "// handler 直接把这段字节配上 Content-Encoding: gzip 发出去即可。",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        c_array(
            "PORTAL_INDEX_GZ",
            html_gz,
            f"index.html（已内联 style.css + app.js）：{len(html)} B → gzip {len(html_gz)} B",
        ),
        "",
        c_array(
            "PORTAL_CHART_GZ",
            chart_gz,
            f"Chart.js v4.4.8，按需加载：{len(chart)} B → gzip {len(chart_gz)} B",
        ),
        "",
    ]
    OUT_H.write_text("\n".join(parts), encoding="utf-8")

    write_preview(html)

    print(f"index.html  {len(html):>7} B  → gzip {len(html_gz):>6} B")
    print(f"chart.js    {len(chart):>7} B  → gzip {len(chart_gz):>6} B")
    print(f"flash 合计   {len(html_gz) + len(chart_gz):>7} B")
    print(f"写出 {OUT_H.relative_to(ROOT)}")
    print(f"写出 {OUT_PREVIEW.relative_to(ROOT)}")


# ---------------------------------------------------------------------------
# 桌面预览：同一份 HTML，把 fetch 换成 mock，浏览器直接打开
# ---------------------------------------------------------------------------
MOCK = r"""
<script>
/* ===== 桌面预览 Mock（仅本文件，不进固件）=====
   自动生成，改 mock 请改 tools/gen_portal.py */
(function(){
  var STATUS={ok:true,wifi:true,rssi:-48,ip:"192.168.1.87",ssid:"MyHome_2.4G",
    temp:24.6,humi:58,batt:92,city:"北京市",wtext:"多云转晴",wupd:"16:42",
    otemp:31.2,ohumi:44,feels:33.5,tmin:25,tmax:32,wind:"东北 12 km/h",
    sd:true,sd_used:124,sd_total:15200,uptime:39472,heap:148,
    chip:"ESP32-S3",app:"RLCD-Home 0.1",idf:"v6.0.1",mac:"84:F7:03:6C:AA:BB",flash_free:2048};
  /* city 留空 = 演示"自动定位"形态（PUBIP.auto=true） */
  var CONFIG={ok:true,ssid:"MyHome_2.4G",city:"",has_pass:true,has_key:true,has_host:true,
    has_uapi:true};
  var PUBIP={ok:true,valid:true,ip:"117.182.103.101",region:"中国 广西 南宁市",
    isp:"China Mobile Communications Group Co., Ltd.",district:"青秀区",
    auto:true,city:"青秀区"};
  var LIMITS={marks:32,events:16,labels:16,text:31};
  var CAL={ok:true,sd:true,marks:["01-01","02-14","10-01","12-25"],
    events:[{date:"02-14",text:"情人节"},{date:"10-01",text:"国庆节"}],
    labels:["今天也要加油","保持好心情","新的一天新的开始"]};
  var SCAN={ok:true,aps:[{ssid:"MyHome_2.4G",rssi:-48,auth:true},
    {ssid:"Neighbor_WiFi",rssi:-72,auth:true},{ssid:"Guest_Free",rssi:-85,auth:false}]};
  /* 接口调用统计：次数随所选窗口缩放，看得出 day < week < month < year */
  function apistat(u){
    var p=(String(u).split("p=")[1]||"month");
    var k={day:1,week:6,month:26,year:300}[p]||26;
    return {ok:true,valid:true,sd:true,
      from:{day:"2026-08-08",week:"2026-08-03",month:"2026-08-01",year:"2026-01-01"}[p],
      items:[{name:"和风天气 · 城市解析",n:1*k,fail:0},
             {name:"和风天气 · 实况",n:6*k,fail:Math.floor(k/9)},
             {name:"和风天气 · 每日预报",n:1*k,fail:0},
             {name:"UAPI · 公网 IP 定位",n:1*k,fail:0}],
      total:9*k};
  }
  function csv(){
    var h="timestamp,indoor_temp,indoor_humi,outdoor_temp,outdoor_humi,weather,city,wifi_rssi\n";
    var rows=[],now=Date.now();
    for(var i=500;i>=0;i--){
      var t=new Date(now-i*12*60*1000),p=function(n){return String(n).padStart(2,"0")};
      var ts=t.getFullYear()+"-"+p(t.getMonth()+1)+"-"+p(t.getDate())+" "+p(t.getHours())+":"+p(t.getMinutes());
      rows.push(ts+","+(23.5+Math.sin(i/11)*6).toFixed(1)+","+(52+Math.cos(i/17)*14).toFixed(0)
                +",30,60,多云,北京市,-48");
    }
    return h+rows.join("\n");
  }
  function J(o){return Promise.resolve({ok:true,status:200,
    text:function(){return Promise.resolve(JSON.stringify(o))},
    json:function(){return Promise.resolve(o)}})}
  var real=window.fetch;
  window.fetch=function(u,o){
    var post=o&&o.method==='POST';
    if(u==='/api/status')   return J(STATUS);
    if(u==='/api/limits')   return J(LIMITS);
    if(u==='/api/scan')     return J(SCAN);
    if(u==='/api/config')   return post?J({ok:true}):J(CONFIG);
    if(u==='/api/pubip')    return J(PUBIP);
    if(String(u).indexOf('/api/apistat')===0) return J(apistat(u));
    if(u==='/api/calendar') return post?J({ok:true}):J(CAL);
    if(u==='/api/data/csv') return Promise.resolve({ok:true,status:200,
      text:function(){return Promise.resolve(csv())},
      blob:function(){return Promise.resolve(new Blob([csv()],{type:'text/csv'}))}});
    if(String(u).indexOf('/api/')===0) return J({ok:true});
    return real.apply(window,arguments);   /* Chart.js 等真实资源照常走网络 */
  };
})();
</script>
"""

# 预览页从 CDN 取 Chart.js —— 本地没有设备来 serve /chart.umd.min.js
PREVIEW_CHART_URL = "https://cdn.jsdelivr.net/npm/chart.js@4.4.8/dist/chart.umd.min.js"


def write_preview(html: str) -> None:
    """把 mock 注在业务 JS 之前，并把 Chart.js 指向 CDN。"""
    out = html.replace(f'"{CHART_URL}"', f'"{PREVIEW_CHART_URL}"')
    marker = "<script>var CHART_URL="
    idx = out.index(marker)
    out = out[:idx] + MOCK.strip() + "\n" + out[idx:]
    OUT_PREVIEW.write_text(out, encoding="utf-8")


if __name__ == "__main__":
    main()
