// bloub_sampler_entry.ts —— 在 Node 里驱动 bloub 引擎走一段蒙太奇，逐帧输出最小 SVG
//
// bloub 的引擎（src/bot/）是无 DOM / 无时钟的纯函数：engine.sample(t) 给出
// 几何（身体 64 点径向轮廓 + 胶囊眼 + 粒子），本文件复刻 BloubBot.vue 里
// 「单色渲染」需要的最小 SVG 组装——1-bit 下身体=黑实心、眼睛=白洞（clipPath
// 裁进身体）、粒子=黑圆点，透明度按 0.5 阈值取舍，渐变圆弧（orbit 等）不参与。
//
// 输出 JSON：{ fps, total, count, frames: [{ t, state, svg }] }
// 由 tools/gen_bloub_frames.py 消费（cairosvg 栅格化 + 二值化 + 打包 .bin）。
//
// 用法（见 gen_bloub_frames.py）：
//   node --experimental-strip-types --import tools/bloub_register.mjs \
//        tools/bloub_sampler_entry.ts --fps 8 --out /tmp/frames.json \
//        --blocks idle:2.4 wink:1.6 ...
//
// bloub 源码路径用 BLOUB_ROOT 环境变量覆盖（默认 /home/chen/demo/bloub）。
// bloub 本体 MIT 许可，常数均为对参考视频的实测值，勿取整——详见其 CLAUDE.md。

const BLOUB = process.env.BLOUB_ROOT ?? '/home/chen/demo/bloub'

const { BotEngine } = await import(`${BLOUB}/src/bot/engine`)
const { blockAt, offsetOf } = await import(`${BLOUB}/src/bot/cycles`)
const { RAYON } = await import(`${BLOUB}/src/bot/repere`)

/** 半边 viewBox：引擎输出坐标的定义域（decor 的环最多到 1.4×RAYON）。 */
const VB = 158

/** 命令行 / 默认蒙太奇：慢状态 + 首尾 idle，眨眼呼吸是引擎自带的。 */
const DEFAULT_BLOCKS = 'idle:2.4 wink:1.6 wide:1.8 thinking:2.6 sleep:2.4 idle:1.8'

function parseArgs(argv) {
  const opts = { fps: 8, blocks: DEFAULT_BLOCKS, out: '' }
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === '--fps') opts.fps = Number(argv[++i])
    else if (argv[i] === '--out') opts.out = argv[++i]
    else if (argv[i] === '--blocks') {
      const rest = []
      // --blocks 后面的所有 state:dur 都是它的参数
      while (i + 1 < argv.length && !argv[i + 1].startsWith('--')) rest.push(argv[++i])
      opts.blocks = rest.join(' ')
    }
  }
  return opts
}

/** "idle:2.4 wink:1.6" → Block[] */
function parseBlocks(spec) {
  return spec
    .trim()
    .split(/\s+/)
    .filter(Boolean)
    .map((tok) => {
      const [state, dur] = tok.split(':')
      return { state, duration: Number(dur) }
    })
}

const r2 = (v) => Math.round(v * 100) / 100

/** 粒子/墨点：bloub 里 opacity 最低 0.55（thinking），1-bit 一律按实心黑处理。 */
function dotsSvg(dots, R) {
  return dots
    .filter((d) => d.opacity > 0.5 && d.r > 0.0005)
    .map((d) =>
      d.d
        ? `<path d="${d.d}" transform="translate(${r2(d.x)} ${r2(d.y)}) rotate(${r2(d.rot ?? 0)}) scale(${R})" fill="#000"/>`
        : `<circle cx="${r2(d.x)}" cy="${r2(d.y)}" r="${r2(d.r)}" fill="#000"/>`
    )
    .join('')
}

/**
 * BotFrame → 单色 SVG。
 * 对应 BloubBot.vue 的图层顺序（我们的状态集没有 orbit 圆弧）：
 *   背后粒子 → 身体（clip 到自身路径的黑矩形，眼睛/notch 是白洞）→ 前面粒子。
 */
function frameSvg(frame, R) {
  const behind = frame.dotsBehind ? dotsSvg(frame.dots, R) : ''
  const front = frame.dotsBehind ? '' : dotsSvg(frame.dots, R)

  // 眼睛 alpha < 0.5 的帧直接不画（淡入淡出在 1-bit 下表现为出现/消失）
  const eyes = frame.eyes
    .filter((e) => e.alpha >= 0.5)
    .map((e) => `<path d="${e.d}" transform="${e.matrix}" fill="#fff"/>`)
    .join('')

  const notch = frame.notch
    ? `<circle cx="${r2(frame.notch.x)}" cy="${r2(frame.notch.y)}" r="${r2(frame.notch.r)}" fill="#fff"/>`
    : ''
  const notif = frame.notif
    ? `<circle cx="${r2(frame.notif.x)}" cy="${r2(frame.notif.y)}" r="${r2(frame.notif.r)}" fill="#000"/>`
    : ''

  return (
    `<svg xmlns="http://www.w3.org/2000/svg" viewBox="${-VB} ${-VB} ${VB * 2} ${VB * 2}">` +
    `<defs><clipPath id="bc"><path d="${frame.bodyPath}"/></clipPath></defs>` +
    behind +
    `<g clip-path="url(#bc)">` +
    `<rect x="${-VB}" y="${-VB}" width="${VB * 2}" height="${VB * 2}" fill="#000"/>` +
    eyes +
    notch +
    `</g>` +
    front +
    notif +
    `</svg>`
  )
}

const opts = parseArgs(process.argv.slice(2))
const blocks = parseBlocks(opts.blocks)
if (!blocks.length) throw new Error('空蒙太奇')

const fps = opts.fps
const total = blocks.reduce((s, b) => s + b.duration, 0)
const count = Math.floor(total * fps) // 最后一帧停在 total 之前，回绕点不采样

// 复刻 BloubBot.vue 的 rendAt：块倒退时 reset（无历史），否则按绝对偏移 setState
const engine = new BotEngine(RAYON, blocks[0].state)
let dernierBloc = -1
const frames = []
for (let i = 0; i < count; i++) {
  const t = i / fps
  const { index } = blockAt(blocks, t)
  if (index !== dernierBloc) {
    if (index < dernierBloc) engine.reset(blocks[index].state, offsetOf(blocks, index))
    else engine.setState(blocks[index].state, offsetOf(blocks, index))
    dernierBloc = index
  }
  const frame = engine.sample(t)
  frames.push({ t, state: frame.bodyPath ? blockState(blocks, index) : '', svg: frameSvg(frame, RAYON) })
}

function blockState(blocks, index) {
  return blocks[index]?.state ?? ''
}

const payload = { fps, total, count, blocks: opts.blocks, frames }
if (opts.out) {
  const fs = await import('node:fs')
  fs.writeFileSync(opts.out, JSON.stringify(payload))
  console.error(`采样完成：${count} 帧 / ${total}s @${fps}fps → ${opts.out}`)
} else {
  process.stdout.write(JSON.stringify(payload))
}
