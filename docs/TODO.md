# TODO — 未完成项

本文件是唯一的待办清单。`PROGRESS.md`（原 `doc.md`，§ 编号不变）只保留分析结论（根因、地址、结构），条目状态一律写在这里。
条目按优先级排列；每条给出「证据 / 影响 / 建议做法 / 如何验证」。

---

## P0 游戏内实测 —— 首次通过（2026-09-07），剩余界面待覆盖

Release 产物部署到 `plugins\` 后实机跑过：**基本显示正常，几处界面中文正确**。
重构后的行为改动（行距用实测 `tmHeight`、`get_width` 用 advance 累加、ASCII 按词断行）
在已看到的界面上无异常。

第二轮（含提交 2~4：Advancer 重构、P1 诊断、P2.2 `WrapTextOptimal`）用户反馈：
**"除了 H4CN.log 没有生成，其他运行基本没问题"** —— 即新 hook 行为未暴露异常；日志缺失
是因为首版只在失败时写文件，已改为每次启动写一行（见 P1），待部署新版后确认。

三处**有意为之**的行为改变，已见界面未暴露问题，但下列点位仍需逐个确认（尤其滚动窗除零、
中英混排断词）：

| 改动 | 位置 | 现象风险 |
|---|---|---|
| 行距改用 GDI 实测 `TEXTMETRIC.tmHeight`（不再 `size+4`） | `font_cache.cc` FontContext ctor / `PrepareContext` | 行间距整体变化，密集界面可能变挤或变疏 |
| `get_width` 返回 advance 累加（不再裁首末 bearing） | `hooks.cc` HookFontGetWidth | 自适应宽度控件比实际墨迹宽 1~2px |
| ASCII 段按词断行（原版行为），CJK 仍逐字 | `wrap.cc` WrapText | 英文段落行数变多；中文不变 |

待覆盖的验证路径（未勾选＝本轮尚未逐个确认）：
- [ ] 主菜单 / 城镇 / 英雄界面（多字号同屏，看行距与基线）
- [ ] 战斗怪物数量标签（走 `draw_to_rect`，含中英混排 + K/M/B 缩写走 `get_width`）
- [ ] 任务简报与胜利/失败文本（`set_text`→wrap 长段落）
- [ ] **可滚动文本窗**：拖滚动条到两端（§7.3 的除零风险点，最关键）
- [ ] 弹框 / 悬停提示气泡（宽度问题见 P2，另计）
- [ ] 中英混排段落的断词位置
- [ ] 启动后 `plugins\H4CN.log` 出现一行 `H4CN: v<版本>: 7/7 font hooks installed`（P1）

```powershell
./build.ps1 -Config Release -DeployDir '<游戏目录>\plugins'
```


---

## P1 装钩失败没有任何提示 —— 代码完成，待复验日志生成（2026-09-07）

原状：`InstallHooks()` 丢弃安装结果，序言比对失败（换 exe 版本）＝游戏正常启动但没中文，
无法区分「地址错」「GDI 失败」「没装对目录」。

落地：新模块 `diagnostics.*`：`DiagLog` 一条消息同时走 ODS + 追加 `plugins\H4CN.log`
（带本地时间戳），全程非阻塞、可在 loader lock 下跑：

- `inline_hook.h`：bool → `HookInstallResult` 枚举（`prologue-mismatch` / `alloc-failed` /
  `bad-args`），比对失败仍然拒写；
- `hooks.cc`：把 7 个结果汇总成一行，**每次启动都写**（首版只在失败时写 → 玩家侧永远看不到
  这个文件，无法证明插件加载）。成功：`v<版本>: 7/7 font hooks installed`；失败追加
  `0x…(原因)` 清单。版本号来自 CMake `PROJECT_VERSION`（`version.h.in` → 生成的 `version.h`）；
- `font_cache.cc`：`GetTextFaceW` 对比 `LiSu`/`隶书`（zh-CN 本地化名，避免误报）；某字号
  GDI 上下文创建失败每字号报一次。

验证：
- [ ] 游戏启动后 `plugins\H4CN.log` 出现 `H4CN: v3.0.0: 7/7 font hooks installed`
      （host 侧已用 `diagnostics.cc` 独立跑通写文件，见 `2026-09-07` 记录）；
- [ ] 故意把某个 `HookTarget::prologue` 改错一字节 → 日志行变成 `6/7 ... failed: 0x…(
      prologue-mismatch)`、游戏不崩（可选）。

---

## P2 弹出框/对话框宽度不正确（PROGRESS.md §8.2）

两个独立成因。2026-09-07：#2 已重写为全量替换（代码完成，待游戏内验证），#1 未动。

1. **未修**：`sub_838630`（77 个调用者）宽度直接取父边界 `this[+0xE0] - this[+0xD8]`，压根不测
   文本；高度走已 Hook 的 `get_wrapped_height`。→ 需 Hook `sub_838630` 改用 `t_font_get_width`。
   IDA 复核（本轮）：确实用 `v5[56]-v5[54]` 建 rect 与调 `get_wrapped_height`，全程无测宽。
   是 77 调用者的 SEH 大函数，风险高 → 等 P0 实测确认观感不可接受，先逆 0xD8/0xE0 的写入方再立项。
2. **已完成**：`sub_8CAE90` → `wrap_text_mm@0x71C5E0`。#4 hook 从「只修度量+委托原版」改为
   全量替换：`WrapTextOptimal`（wrap.cc）复刻原版控制流——`max(min, min(longest_word, max'))`
   起步、×3/2 增长、终止于单行 / 触顶 / `4·lines·line_height ≤ w`，返回最终行宽（调用方据此
   居中）；最长词宽改由 `Advancer` 量（原游戏字形表把 GBK 夹到空格字形必错）。去 trampoline。
   host 单测覆盖增长序列（width 50→75→112→168）与负 max 夹取。

P0 复验新增点位：走 `sub_8CAE90` 的弹框——宽度随内容变、中英混排断词合理、不再窄到竖排。

---

## P3 系统2（2003 新增富文本引擎）未 Hook（PROGRESS.md §6/§8.3）

`heroes4_debug.map`（2002-10）里**没有任何对应符号**，属 2003 版新增，现有 map 无法标定；
暂定名一堆（`Text_MeasureWidth@519950`、`TextLayout_*`、`Font_LoadDLL@88D500`、`Char_*`…），
全局字形表 68B@0xAA1530（43 个字形名索引，double 宽度度量）。

影响面未知：若某些界面（帮助/滚动公告/过场字幕？）走系统2，那部分中文仍乱码。
先决条件：定位「哪些 UI 用系统2」——建议从 `TextLayout_BuildLines@0x5A67A0` 的 xref 反查调用链，
它同时调 `get_font@0x875BC0` 和 `t_text_window_ctor_rect@0x8859F0`（与系统1的衔接点）。

---

## P4 性能优化（分支 feat/perf-optimization，2026-09-10：代码完成，待游戏内 perf 实测）

测量通道已落地：`[general].perf_log`（默认 off）——7 个 Hook 入口与字形光栅化各带一个 QPC
计时槽（`diagnostics.*` 的 `ScopedPerf`/`PerfRecord`），每累计 2048 次向 H4CN.log 出一行
`perf: <名称> n=… avg=…us max=…us total=…ms`。关闭时热路径只多读一个 bool。基线/回归对比
都以此为据，勿凭猜优化。

| 项 | 位置 | 状态 |
|---|---|---|
| 每次 hook 调用重做 string+tuple+map 解析 context | `font_cache.cc` `GetFontContext` | ✅ 已做：`game_size → FontContext*` memo（tagged atomic 指针，release 发布；失败 size 也缓存）。仅 miss 一次走原 Resolve+map；`Init/ShutdownFontCache` 同步清空 |
| 逐字符加全局锁 | `font_cache.*` `blit.*` `hooks.cc` | ✅ 已做：锁改为**每 context** 的 `recursive_mutex`；hook 整体 `lock_guard<FontContext>` 一次，`GlyphRouter(locked=true)` 走免锁变体 `AdvanceLocked`/`GetGlyphLocked`（路由与 advance 值两路完全一致，断行=排版不变） |
| 每次调用现构 `std::vector<Line>` | `hooks.cc` 三处 wrap + `wrap.cc` `WrapTextOptimal` 内层 | ✅ 已做：`thread_local` scratch + `clear()` 复用容量；`WrapTextOptimal` 尾部改 copy-assign（Line 是 `{ptr,len}` 视图，拷贝比让出缓冲再 malloc 便宜） |
| 编译参数 | `src/CMakeLists.txt` | ✅ 已做：Release `/GL`+`/LTCG`（`INTERPROCEDURAL_OPTIMIZATION_RELEASE`，`check_ipo_supported` 守卫）；产物 138240→132608 B，5 导出齐全 |
| 同一段文本被断两次 | `t_text_window_update_layout@0x885FF0` 先 `wrap_text` 再 `get_wrapped_height` | ❌ 决定不做（2026-09-10 数据）：稳态 `wrap_text` avg **3.9µs**、`get_width` avg **0.6µs**，重复断一次整个窗口 ≈8µs；(size,text ptr,width) memo 的失效条件（文本被 `set_text` 就地改）风险远大于收益 |

最终稳态数字（修正单位后的 build，2026-09-10 第二轮）：
`get_width` avg 0.6µs / max 28µs，`wrap_text` avg 3.9µs，`draw_to_pt` avg 6.9~7.3µs / max
81~336µs（首次光栅化新字形）。窗口内 avg>15µs 的时刻与 `game size 20/8 -> ...` 行同秒——
冷 context 创建（CreateFont+DIB+95 次 ASCII advance 预填充）摊进一次 hook，每 size 仅一次。

验证（待办）：
- [x] 首轮 `perf_log=true` 实测（2026-09-10，优化后 build）：数字方向与第二轮一致
- [x] 空日志行根因：`ReportFaceSubstitution` 用 `%S` 排 SimSun 的本地化名，CRT "C" locale 下
  转成空串 → 改为 `WideToUtf8` + `%s`，并把 SimSun/宋体 等 7 对常用中文字体本地化名加入
  `IsSameFace` 别名表（误报消失）；全项目加 `/utf-8` 消除 C4819
- [x] 复跑确认（12:44/12:48 两份日志）：无空 `H4CN: ` 行、无 SimSun 误报，`perf:` 行单位正确
- [x] 显示与优化前一致（三轮实机多界面未见异常），`wrap_tests`/`config_tests` 已绿

---

## P5 已知妥协

- bias 4 仍是经验值（现为 `[fonts].default_bias` 默认，见下条：已可按字号在 H4CN.toml 覆盖）。
  做成数据驱动仍需 `.fon` 资源来对照真实 `size`↔`line_height` 关系，参考文件 `8bpp16.fon` 已删除。
  备选：从 `t_font_bitmap_read@0x71BAA0`（把 bits 的 1→15、2→-16 做转换的那个）继续逆出资源格式。
- ~~字体硬编码 `LiSu`，无配置~~ 已实现（2026-09-07，v3.1.0）：`H4CN.toml` ——
  `[fonts].fallback_face`（默认 LiSu）+ 每字号 `[fonts.<size>]` 的 `face`/`size`/`bias`；
  首帧懒加载不在 DllMain 读盘；坏配置一律「报 `H4CN.log` + 回落默认」。缺隶书时 `CreateFontW`
  的静默替换现在也会被 `GetTextFaceW` 检出并记一行。**待游戏内验证**：不带文件跑一次（行为应与
  v3.0.0 完全一致），再放一份改过 `fallback_face`/某字号 `size` 的文件确认生效 + 日志出现
  `game size NN -> '<face>' cell K px` 映射行。
- 断行只处理空格词边界：连字符、破折号、标点后断词（英文排版）未做。
- `DLL_PROCESS_DETACH` 时假定渲染线程已停：若有线程正在执行 trampoline，`VirtualFree` 会崩
  （`inline_hook.cc` 注释已记）。Ultimate ASI Loader 通常不卸载插件，实测前先不管。

---

## 标定侧待办（PROGRESS.md 原 §9，非插件代码工作）

1. **basic_dialog 集群**（0x676000–0x678500，vftable@0x97CA98）：`set_font`(dbg 5F5CD7) 在
   release 疑似被内联；连同 `add_ok_button`/`add_material` 一起逆。
2. **button 虚槽落名**：is_contained=0x5A0A80、get_child=0x5A09A0、key_down=0x5A0C00、
   key_press=0x5A0DA0、left_down=0x5A0DD0、left_up=0x5A0EE0、right_up=0x5A0F50、
   double_click=0x5A0F80、mouse_leaving=0x5A0FE0、mouse_move=0x5A0F20、
   update_transparency/update_size=0x5A1180/5A10F0；t_button ctor / create_button /
   set_button_layers 在未归属 SEH 拆块区。
3. **编码/本地化 release 地址**（dbg VA 已有）：`t_string_table::import`@7B2369、
   `t_external_string::load_string`@7BF62B、`load_all`@7BF763、`c_str`@50DD80、
   `t_string_table_cache` 的 do_get/get/get_prefix（vftable@0x98A54C 槽位已定）。
   关联：文本必须已是 GBK 字节，本插件不做转码；若资源是别的编码，链路是
    `load_string`/`c_str`（PROGRESS.md §4.4）。
4. **combat 消息显示** `display_action_message`/`erase_action_message`（集群 dbg 683D78..6C3660）
   文本渲染路径待核实后落名。
5. ~~死代码区 0x6F65CE–0x6F7AD0~~（OPT:REF 残留）已确认勿再投入。
6. 通用逆工注意：函数体只有一行 call / 构造不完整 → 按 PROGRESS.md §2.3 修 tail + noreturn。
7. **`get_font` 请求字号 ↔ `t_font::size` 资源名义尺寸的配对**（PROGRESS §4.2 ⚠）：插件/H4CN.toml
   用的是后者，实测已见 12 种 `8 10 12 13 15 18 20 21 23 25 26 28`（不保证穷尽）。在
   `t_font_read`@0x71BEE0 处对 `res_id`+请求 size 做一次运行期 dump 即可落定映射表，并把日志
    `game size NN` 收集到的新 size 补进仓库根 `H4CN.example.toml` 的逐字号调优。验证：dump 出的 size 覆盖
   日志里出现过的全部 size，无遗漏。

---

## 工程性小项

- ~~仓库无 `.gitattributes`~~ 已加：`* text=auto eol=lf` + 二进制扩展名（含 `*.asi`）标
  `binary`，跨机器 checkout 行尾稳定。
- ~~没有测试框架可用~~ 已加纯逻辑单测：`src/tests/wrap_test.cc`（`./build.ps1 -Config Debug -Test`
  跑 ctest）。`WrapText` 经 `Advancer` 接口测量，不依赖游戏内存/GDI，行数/宽度/断词规则全覆盖。
  后续 `wrap_text(min,max)` 的倍增搜索若整体重写（P2.2），也应以可单测的纯函数落地。
