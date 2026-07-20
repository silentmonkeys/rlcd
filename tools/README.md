# tools

- gen_font.sh — 用 lv_font_conv（npm 包）从系统 TTF 生成 LVGL 字模 C 文件到 `components/ui/src/`
  - ui_font_cjk_16_gen.c（16px CJK）：编辑 CJK_RANGES 追加 `-r 0xNNNN`
  - ui_font_digit_big_gen.c（96px 数字，时钟 HH:MM）
  - ui_font_digit_mid_gen.c（28px 数字，卡片 24℃ / 68%）
  - 运行：`export PATH=$HOME/.npm-global/bin:$PATH && bash tools/gen_font.sh`

新增汉字后必须重跑此脚本，否则会报 glyph not found 警告（月、年曾因此缺字）。
