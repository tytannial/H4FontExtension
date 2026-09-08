# H4 中文显示插件 — 逆向标定与实现总文档

> 最后更新：2026-09-07（代码分模块重写后；原 v1 标定总文档与速查 `doc.md` 已合并为这一份）
> IDB：`heroes4.exe.i64`（HoMM IV Complete 2003 版 exe 的 IDA 数据库，本文所有地址均为该 exe）
> 参考 map：`heroes4_debug.map`（2002-10-28 debug 构建链接映射；外部参考料，**不随仓库分发**）——
> **源码与构建配置在 `src/`，逆向分析与待办文档在 `docs/`。**
> 本文合并了原 hook 分析（v1）与三轮 IDA/map 标定成果；旧文档中的临时命名在 §2.4 别名对照表中保留。
> **未完成项一律见 `TODO.md`**，本文只保留分析结论。

---

## 1. 项目概述

英雄无敌4（HoMM IV）中文显示插件。游戏原版（系统1）的字体渲染/换行/测量都是**逐字节**的
（`t_font::read` 里 glyph_count 只有 1 字节，原版字形表结构上装不下 GBK），插件用内联 Hook
把这 7 个入口替换成 GBK 双字节感知实现，从而显示中文。

### 1.1 加载方式与部署位置

- 游戏目录装有 **Ultimate ASI Loader**（伪装成 `version.dll` 的代理，5.4MB，导出真 version.dll
  的全部函数）。它的配置在游戏目录 `version.ini`：
  `[GlobalSets] LoadPlugins=1`、`LoadFromScriptsOnly=1`、`LoadRecursively=0`。
- `LoadFromScriptsOnly=1` ⇒ **加载器只从游戏目录的 `plugins\` 子目录读 `*.asi`**，
  游戏根目录不放插件。所以产物部署命令是（`<游戏目录>` 为你的安装路径）：
  `./build.ps1 -Config Release -DeployDir '<游戏目录>\plugins'`
- 这也是 `CMakeLists.txt` 里 `SUFFIX ".asi"` + `PREFIX ""` 的原因（加载器按 `.asi` 扩展名扫描）。
- **另一种宿主：`Heroes4GL`**（`ddraw.dll` GL 封装）不读 `plugins\*.asi`，而是扫 `mods\*.mod`。
  同一份产物带 4 个 mod 导出（见 §7.2），故 `-ModDir '<游戏目录>\mods'` 投放为 `H4CN.mod` 即可
  走这条路；与 ASI 宿主**二选一**，同时装会加载两次（第二份 `0/7 installed` 空转，安全但无意义）。
- ⚠ **`heroes4.exe` 同级的 `Mp3dec.asi` 与本项目无关**：它是游戏自带的 Miles Sound System
  MP3 解码插件（由 `Mss32.dll` 按自己的插件机制加载），不是 ASI 加载器的插件，
  也不能作为「.asi 放根目录」的证据。旧文档在这点上写错过，以本节为准。

当前状态：
- 系统1 全部关键函数已按 `heroes4_debug.map` 原始 C++ 符号完成 IDA 标定（含 text_window / button 消费者集群）。
- 7 个 Hook 已实现（§7），代码已按 Google C++ 风格分模块重写（§7.6）；换行乱码（§8.1）已修。
- 系统2（2003 版新增富文本引擎）**无法**用此 map 标定，未 Hook（§6）。
- **重构后的代码尚未在游戏内实测**，全部未完成项（含优先级与验证方法）见 `TODO.md`。


## 2. 版本对应事实与 IDB 约定

### 2.1 map ↔ exe 版本差异（重要）
- map 是 2002-10-28 的 debug 构建（源码路径 `C:\work\game\*.cpp`）；安装 exe 为 2003+ Complete 版。
- **exe 起始 `.text`（≈0x401000–0x401B00，abbreviate_number.obj）两版 VA 几乎逐字节对齐**（0x401000 完全同址）；再往后即发散，高区完全错位（如 font.obj：map 0x7CBF10 ↔ exe 0x71B820）。
- 模块相邻关系仍然成立（如 font.obj 后紧跟 footprint.obj），用作集群定位旁证。
- 数据结构与类布局稳定：t_font=28B、t_font_bitmap=44B、vftable 槽位序一致。
- **系统2 全部函数在 map 中无符号** → 属 2003 版新增，见 §6。
- release 链接器 /OPT:REF 剔除了未被引用的字体函数（§3.4）。

### 2.2 IDA 命名约定
- 与 map 对应的函数用 `t_<类>_<方法>` 蛇形命名；数据保留/采用 MSVC mangled 名（如 `??_7t_window@@6B@`）。
- 每个标定函数的注释首行 `[map 模块名] 原始符号 @dbg_VA`，供后续轮次复核。
- IDB 类型库已有：`t_font`(28B)、`t_font_bitmap`(44B)、`t_string_vector`、`t_vc6_string`、`t_sized_font`、`t_font_cache_obj`。

### 2.3 已知 IDB 陷阱
- MSVC SEH 函数的 far-tail 常被 IDA 拆成未归属块（0x9xxxxx 处 handler 表 + 主代码尾块）。已手工修复多处；新逆工时若发现"函数只有一行 call"，先检查尾部块与陈旧 `__noreturn` 原型（修复方法：重建函数边界 + `set type` 清 noreturn + 强刷反编译缓存）。
- 已修复拆块：`0x71E6D0`、`0x884DE0`、`0x885650`、`0x885E80`、`0x887000`、`0x764CD0`、`0x539170`、`0x5D13A0`、`0x8CE760`。
- 陈旧 noreturn 污染清除点：`sub_5D13A0/539170/8CE760`（修复后整条构造链反编译恢复）。

### 2.4 旧名 → 现名对照（v1 文档/旧脚本兼容）

| 旧名（v1 文档 / 已删除的旧 main.cpp） | 现 IDA 名 | 地址 |
|---|---|---|
| Font_DrawString | `t_font_draw_to_pt` | 0x71BD50 |
| Font_MeasureString | `t_font_get_width` | 0x71BDE0 |
| Font_BlitGlyph | `t_font_bitmap_draw_to` | 0x71B820 |
| BreakLine (sub_71BE40) | `t_font_get_column` | 0x71BE40 |
| WrapCore (sub_71C100) | `t_font_wrap_text` | 0x71C100 |
| WrapText (sub_71C5E0) | `t_font_wrap_text_mm` | 0x71C5E0 |
| sub_71C6E0 | `t_font_draw_to_vector` | 0x71C6E0 |
| RenderWrapped (sub_71C7F0) | `t_font_draw_to_rect` | 0x71C7F0 |
| CalcHeight (sub_71C900) | `t_font_get_wrapped_height` | 0x71C900 |
| Text_TextBoxRender (sub_886C80) | `t_text_window_paint` | 0x886C80 |
| sub_8859F0（主文本布局构建器） | `t_text_window_ctor_rect` | 0x8859F0 |
| sub_885FF0（文本框布局） | `t_text_window_update_layout` | 0x885FF0 |
| sub_886790（带字体设置的文本控件） | `t_text_window_set_font` | 0x886790 |
| sub_886830（文本控件设置, 100+调用） | `t_text_window_set_text` | 0x886830 |
| sub_886B80 | `t_text_window_get_row_start` | 0x886B80 |
| sub_886F70 | `t_text_window_on_size_change` | 0x886F70 |
| sub_885500（鼠标点击定位） | `t_text_edit_window_left_button_up` | 0x885500 |
| sub_885780 | `t_text_edit_window_position_caret` | 0x885780 |
| sub_885620 | `t_text_edit_window_on_size_change` | 0x885620 |
| sub_401660 | `abbreviate_number_font` | 0x401660 |
| H4FontStrc（旧结构体名） | `t_font` | 类型库 |

## 3. 系统1：font.obj 字体引擎（本插件 Hook 目标）

### 3.1 数据结构

**t_font（28 字节，旧称 H4FontStrc）** — 类型库同名，字段：
```
+0x00 res_id        资源ID
+0x04 unk4          字号/属性（dbg ctor 5参之一）
+0x08 first_char    编码起始
+0x0C line_height   行高 ← Hook PatchFontMetrics 改写对象
+0x10 unk10
+0x14 glyph_count   字形数
+0x18 glyphs        t_font_bitmap* （数组，紧随一个计数头，new(44*n+4)）
```
vftable `??_7t_font@@6B@`@0x988790。

`t_font_read`@0x71BEE0 已证实：`first_char`/`line_height`/`unk10`/`unk4`/`glyph_count`
**都是资源里的单字节**（所以 glyph_count ≤ 255）。插件在 `game_types.h` 里把 `unk4` 命名为
`size`（唯一被当作只读输入使用的字段），`unk10` 仍含义不明且不使用。

**t_font_bitmap（44 字节，单个字形位图）**：
```
+0x00 flags
+0x04 width   +0x08 height   +0x0C pitch
+0x10 bits(u8*)          位图数据（0=透明,1=阴影→15,2=描边→-16,其余=前景 alpha）
+0x14 margin_left(原点)  +0x18 margin_right  +0x1C margin_top
+0x20 bits2  +0x24/28 pad
```
vftable@0x988798（t_font_bitmap）/0x9887A0（t_abstract_bitmap<ubyte> 基）。

### 3.2 函数总表（release ↔ map）

| release | IDA 名 | map 符号（dbg VA） | 备注/Hook |
|---|---|---|---|
| 0x71B820 | t_font_bitmap_draw_to | bitmap::draw_to @7CCF84 | RGB565 blit；被 DrawString/Vector 版调用 |
| 0x71BAA0 | t_font_bitmap_read | bitmap::read @7CC16F | convert=0 时 1→15,2→-16 |
| 0x71BC40 | t_font_ctor | t_font() @7CC344 | 仅缓存 do_read 调用 |
| 0x71BC60 | t_font_dtor | ~t_font @7CC46F | |
| 0x71BC90 | t_font_bitmap_deleting_dtor | ??_E @7CCD3D0 | |
| 0x71BCF0 | t_font_bitmap_ctor | bitmap() @7CCD460 | |
| 0x71BD20 | t_font_bitmap_dtor | ~bitmap @7CCD4E0 | |
| 0x71BD50 | t_font_draw_to_pt | draw_to(point,char*) @7CC4E1 | **Hook#1 kFontDrawToPt** |
| 0x71BDE0 | t_font_get_width | get_width @7CC59B | **Hook#2 kFontGetWidth** |
| 0x71BE40 | t_font_get_column | get_column @7CC645 | **Hook#3 kFontGetColumn**（像素→字节列） |
| 0x71BEE0 | t_font_read | read @7CC743 | 读表头+new(44n+4) |
| 0x71C100 | t_font_wrap_text | wrap_text(3参) @7CC9F7 | **Hook#5 kFontWrapText**（7B, SEH 序言） |
| 0x71C5E0 | t_font_wrap_text_mm | wrap_text(4参) @7CCF05 | **Hook#4 kFontWrapTextMm**；release 内联 longest_word_length |
| 0x71C6E0 | t_font_draw_to_vector | draw_to(rect,vector) @7CCFFE | 逐行 vector 渲染（问题1相关） |
| 0x71C7F0 | t_font_draw_to_rect | draw_to(rect,char*) @7CD161 | **Hook#6 kFontDrawToRect**（7B） |
| 0x71C900 | t_font_get_wrapped_height | get_wrapped_height @7CD206 | **Hook#7 kFontGetWrappedHeight**（7B） |

集群边界：0x71B800–0x71CA00（之后即 footprint.obj，与 map 模块次序一致，佐证归属）。

### 3.3 调用链（新版命名）

```
get_font(size)@0x875BC0 ← 55+ UI 调用者 → t_font_cache_get@0x764870
UI 文本控件: t_text_window_set_text@0x886830 / set_font@0x886790 / on_size_change@0x886F70
   └→ t_font_wrap_text@0x71C100 (kFontWrapText) → t_text_window_update_layout@0x885FF0
绘制: t_text_window_paint@0x886C80 → t_font_draw_to_pt@0x71BD50 → t_font_bitmap_draw_to@0x71B820
渲染(rect): t_font_draw_to_rect@0x71C7F0 → wrap_text → t_font_draw_to_vector@0x71C6E0
   └→ 逐行 draw_to_pt/BlitGlyph（任务简报路径, 问题1）
测量: t_font_get_width@0x71BDE0 ← abbreviate_number_font@0x401660 / sub_601260 / position_caret 等
弹框: sub_838630(77调用者) → get_wrapped_height@0x71C900; sub_8CAE90 → wrap_text_mm@0x71C5E0
```

### 3.4 release 中不存在的 font.obj 函数（勿再找）
`t_font_bitmap::create`、`t_font_bitmap::write`、`t_font::write`、`t_font(HHHHH)` 构造、`longest_word_length`（内联）、`get_character`（内联为 `this->glyphs[c]`）。

### 3.5 换行容器 `t_string_vector`（已标定类型 + 语义）
- **`t_string_vector`**（16B）= `std::vector<t_vc6_string>`：`+0x0 _x0(alloc)`、`+0x4 _Begin`、`+0x8 _End`、`+0xC _Myend`。→ 现有 `HookFontWrapText` 读 `lines->begin/+4`、`lines->end/+8` **正确**。
- **条目 `t_vc6_string`**（16B，MSVC6 refcount std::string）：`+0x0 _x0`、`+0x4 _Ptr(文本)`、`+0x8 _Size`、`+0xC _Myres`。
- `t_font_wrap_text`@0x71C100（`wrap_text(t_string_vector&, const char*, int)` @dbg 7CC9F7）**逐字节**拆行：按 `width`、空格(0x20)、换行(0x0A) 切分，每行 `t_strvec_insert` 入容器，返回最大行宽。**GBK 双字节会在 width 处被劈开**（根因见 §8.1）。
- `t_font_draw_to_vector`@0x71C6E0（@dbg 7CCFFE）逐字节渲染每行（`glyphs[byte-first_char]` → `t_font_bitmap_draw_to`）；**唯一调用者 = `t_font_draw_to_rect`@0x71C7F0（Hook#6）**，已覆盖。
- 辅助（已命名）：`t_strvec_assign`@0x5ABC70、`t_strvec_insert`@0x440F50、`t_strvec_insert_n`@0x41D400、`t_str_assign`@0x401D30、`t_str_append_n`@0x4106A0、`t_str_split`@0x402030、`t_str_grow`@0x401DF0。

## 4. 字体资源加载与缓存

### 4.1 缓存集群（t_font 模板实例，0x764870–0x765320）
| release | 名称 | map (dbg i-copy) |
|---|---|---|
| 0x764870 | t_font_cache_get | abstract_cache<t_font>::get @8220C0 |
| 0x764C70 | t_font_cache_data_dtor | @821090 |
| 0x764CC0 | t_font_cache_get_prefix → "font" | @8231C0 |
| 0x764CD0 | t_font_cache_do_read | @8231E0；`new t_font(28)+ctor+read` |
| 0x764E30 / 0x764F30 | cache_data deleting dtor ×2 | @8234F0 |
| 0x7651C0 | t_ptr_cache_data_font_ctor | @822FB0 |

资源名 = `<family>.<size>`（`t_resource_traits<t_font>::extension` 在 dbg 为 ".fon" 系；exe 内未找到 ".fon"/"8bpp" 明文资源字串，加载文件名逻辑在 string_table/资源目录层）。

### 4.2 standard_fonts.obj（0x874980–0x875C80）
- `get_font(int size)`@0x875BC0 ↔ dbg @96EE0D：对 `g_fonts[14]`@0xA84798 二分 → 命中缓存实例后 `t_font_cache_get`。
- **本版本「请求字号→资源」映射（与 debug 版不同，勿套旧表）**：
  `9→small fonts5, 11→small fonts6, 12/14/16/18/20/22→prose_antique.{同数}, 23→.22, 25→.24, 27→.26, 29→.28, 30→.30, 33→.32, 34→.34`；另有 `.36` 实例未入表。
- 数据：`g_font_9..36` 8B 实例 @0xACED30–0xACEDA0（t_font_cache_obj：vptr+counted_ptr）；族名字符串对象 0xACEDA8。
- 构造函数 `stdfonts_ctor_g_font_NN`@0x874A80..0x875AC0（16 个，静态 init 群）。
- **⚠ 两套编号不是一回事（2026-09-07 实测厘清）**：上表键是 UI 调 `get_font` 的*请求*字号；
  而插件看到的 `t_font::size`（+0x04，`.fon` 头里的资源名义尺寸，也是 H4CN.toml 的 `[fonts.<size>]`
  键与 FontContext 键）是**另一组值**。插件日志实测已出现 12 种：
  `8, 10, 12, 13, 15, 18, 20, 21, 23, 25, 26, 28`（不保证穷尽——任何界面没走过的 size 会在首次
  绘制时由 `H4CN.log` 的 `game size NN -> ...` 行补录）。两两对应关系（如 14→13?）待专项逆工：
  可在 `t_font_read`@0x71BEE0 处对照 `res_id`+请求 size 做一次运行期 dump。

### 4.3 脚本字体
`script_font_cache_ctor`@0x7616A0 → `g_script_font_cache`@0xABDF18（资源 "script.18"，供 script_display_text 用）。

### 4.4 文本内容/编码（本地化架构，map 已定位）
游戏文本的字节来源与编码入口（当前仍是西文编码，GBK 重编码需落在这条链上）：

- **`t_string_table`**（string_table.obj）：字符串表资源，经缓存加载（同 t_font 缓存机制）。
  - `t_string_table::t_string_table()` @dbg 7F3080、`t_string_table::import(t_table const&)` @dbg 772369（**读取原始表 → 字符串**）、`size()` @dbg 7BF980。
  - 缓存 vftable：`??_7?$t_ptr_cache_data@Vt_string_table@@@@6B@`@0x98A54C / `??_7?$t_pointer_cache@Vt_string_table@@@@6B@`@0x98A568；`get`@dbg 7F27F0、`do_get`@dbg 7F2E30、`get_prefix`@dbg 7F2F70。
- **`t_external_string`**（external_string.obj）：UI 文本对象；数百个 `k_text_*`/`k_*` 全局实例。
  - `load_string()` @dbg 7BF62B（**按 ID 从表解析出文本字节**——编码入口）、`load_all()` @dbg 7BF763、`add_table(t_string_table const&)` @dbg 7BF35C、`c_str()` @dbg 50DD80（adventure_frame.obj）。
- **`format_string`**（format_string.obj）：`format_string`/`format_string_va_list` @dbg 7CDE30..7CDF2C（printf 风格格式化）。
- **关键字替换**：`keyword_replacer.obj`(40)、`hero_keyword_replacer.obj`(32)、`artifact_keyword.obj`——文本中 `%hero_name%` 等占位符展开。
- **脚本文本**：`script_display_text.obj`(71)、`script_set_player_message.obj`——脚本触发文本。
- **`t_string_vector`**（string_vector.obj，见 §3.5）也是资源类型（`t_resource_traits` prefix/extension @dbg AB7508/AB7510）。

> 本地化路径：资源文件 → `t_string_table::import` → `t_external_string::load_string` → `c_str()` → `t_text_window::set_text` → 字体渲染。GBK 化可在 ①重编码资源文件 ②Hook `load_string`/`c_str` 转换 两者选一。

## 5. UI 消费者集群

### 5.1 vftable 总表（COL 均经 RTTI 验证）
| 类 | vftable |
|---|---|
| t_window（基） | `??_7t_window@@6B@` @0x988864 |
| t_text_window | @0x995A2C（slot17=on_size_change, slot18=paint, slot27=on_text_change） |
| t_text_edit_window | @0x9959AC（[6]key_down [7]key_press [9]left_down置空 [10]left_up [16]focus_lost [17]on_size_change [27]on_text_change [28]begin_edit） |
| t_scrolling_text_window | @0x995AAC / 次表 @0x995AA0 |
| t_caret_window | @0x9959A0（2槽） |
| t_button | @0x97E0A4 |
| t_basic_dialog | @0x97CA98（集群 0x676000–0x678500 未标定） |
| t_combat_label | `??_7t_combat_label@@6B@` @0x982780（slot8=draw_to，经 RTTI 验证） |
| t_combat_action_message_displayer | @0x981F8C |
| t_combat_non_blocking_message_displayer | @0x98294C（继承 action_msg_displayer + idle_processor） |
| t_help_balloon | `??_7t_help_balloon@@6B@` @0x988EF4（**t_text_window 子类**，slot18=paint 覆盖） |
| t_string_table 缓存数据 | `t_ptr_cache_data` @0x98A54C / `t_pointer_cache` @0x98A568 |

### 5.2 game_window.obj
- `t_window_ctor_prot(transparency,parent)`@0x71E6D0 ↔ dbg @7CFFE0（窗口 ID 计数 dword_ABAD00，默认父窗 dword_ABACF4）。

### 5.3 text_window.obj（全集群已标定）
| release | 名称 | map (dbg) |
|---|---|---|
| 0x8859F0 | t_text_window_ctor_rect | @9812A8 |
| 0x885C80 | t_text_window_ctor_point | @98150F |
| 0x885E80 | t_text_window_ctor_prot | @9816E4（唯一调用者 0x727490→get_font(17)） |
| 0x885C60 | t_text_window_deleting_dtor | @982EA0 |
| 0x885FF0 | t_text_window_update_layout | ≈init_scrollbar @981818（兼 relayout：ctor/set*/on_size_change 五处调用） |
| 0x886730 | t_text_window_slider_change | @9822C5 |
| 0x886750 / 0x886770 | button_scroll_up / down | @9822FC / @982343 |
| 0x886790 | set_font | @9813A4 |
| 0x886830 | set_text | @981441 |
| 0x8868A0 | set_wrapped_text | @9814DE |
| 0x886AB0 | set_color | @9815A9 |
| 0x886B10 | set_drop_shadow | @9815E5 |
| 0x886B80 | get_row_start | @98162D |
| 0x886C80 | paint | @98278C |
| 0x886F70 | on_size_change | @982B16 |
| 0x886FC0 | get_text_height | @981B91 |
| 0x887000/0x887120/0x887140/0x8872B0 | scrolling ctor/deleting dtor/dtor/on_idle | @981BD7 / — / — / @981D4E |

成员偏移（本版本实测）：+0xC4 对齐模式(0左/1右?/2居中换算) +0xD6 阴影标志 +0xD8/0xDC cached_ptr(font) +0xE0 文本 string +0xF0 色(565) +0xF2 阴影色 +0xF8 lines 数据 +0x108 wrap 结果 +0x118 当前行 +0x11C 最大行；scrolling 计时器 +0x1C4/+0x1CC/+0x1D0/+0x1D4。

### 5.4 text_edit_window.obj（含内嵌 t_caret_window）
| release | 名称 | map (dbg) |
|---|---|---|
| 0x884DE0 | t_text_edit_window_ctor | @980231 |
| 0x884E80 | TE_deleting_dtor | @981080(i) |
| 0x884EA0 | TE_delete_char | @98036B |
| 0x885060 | TE_key_down（8/13/37/39/46/100/102） | @9804BA |
| 0x885230 | TE_key_press（插入/上限回滚/ESC） | @98076B |
| 0x885490 | TE_on_keyboard_focus_lost | @98094C |
| 0x8854B0 | TE_begin_edit | @980991 |
| 0x885500 | TE_left_button_up（点击→get_column 定位） | @980A1D |
| 0x885620 | TE_on_size_change | @980BCB |
| 0x885650 | TE_build_caret（new 0xE8B） | @980C26 |
| 0x885750 | TE_on_text_change | @980D21 |
| 0x885780 | TE_position_caret | @980D9F |
| 0x884BF0 / 0x884C10 | caret on_idle(闪烁)/paint | @9800B8 / @9800F4 |
| 0x884B70 / 0x8858D0 | caret deleting dtor / thunk | @980F80 |
| 0x885900 | text_window_init_scroll_sprite（"control.button_scroll"） | $E 群 |

### 5.5 button.obj（0x5A0010–0x5A1380，部分标定）
| release | 名称 | map (dbg) |
|---|---|---|
| 0x5A1380 | **t_button_add_text**（5 状态各建文本窗+shadow, 0xA81↔0xAE6） | @655570 |
| 0x5A1200 | t_button_update_layers（按状态选文本窗, 89 调用者） | ≈update_size @65513B |
| 0x5A1340 / 0x5A1360 | set_highlighted / set_pressed | @6554DA / @655525 |
| 0x5A0580 / 0x5A05A0 | deleting dtor / dtor | — |
| vftable 槽（is_contained/get_child/key_down/key_press/left/right/dbl/leaving/move/update_transparency） | 已定位未命名（0x5A0A80/5A09A0/5A0C00/5A0DA0/5A0DD0/5A0EE0/5A0F50/5A0F80/5A0FE0/5A0F20/5A1180/5A10F0） | 按 §5.1 槽位表对应 |
| t_button ctor / create_button / set_button_layers | 在未归属块中（0x5A0040 前后 SEH 拆块区） | @653DF0/@65683B/@65623F |

### 5.6 abbreviate_number.obj（exe 起始，VA 与 map 对齐区）
- 0x401000 `abbreviate_number(int,int)`（dbg 同址）、0x401450 `insert_commas`(dbg 401478)、0x401660 `abbreviate_number(int,const t_font&,int)`(dbg 401668, 调 get_width)。
- 数字缩写 "K/M/B" 显示路径——汉化时注意其经 `t_font_get_width`（已 Hook）。

### 5.7 combat 文本消费者（combat_label / help_balloon / 消息显示）
**关键结论：均走已 Hook 字体函数，无渲染旁路。**

| release | 名称 | map (dbg) | 说明 |
|---|---|---|---|
| 0x601260 | `t_combat_label_draw_to` | draw_to @7BB352 | 怪物数量标签绘制；调 `t_font_draw_to_rect`(Hook#6)+`t_font_get_width`(Hook#2)+`abbreviate_number_font` |
| 0x6010F0 | `t_combat_label_draw_status_bar` | draw_status_bar @7BB1DF | 血条填充；纯位图绘制，无文本 |
| 0x7277E0 | `t_help_balloon_paint` | paint @7DB4D3 | 悬停气泡；先调 `t_text_window_paint`@0x886C80→`draw_to_pt`(Hook#1)，再画边框线 |
| — | t_combat_action_message_displayer | 集群 dbg 683D78..683F50 | 战斗动作消息（"X 攻击 Y"） |
| — | t_combat_non_blocking_message_displayer | 集群 dbg 6C33A0..6C3660 | 非阻塞消息（继承上者 + idle_processor） |
| — | t_combat_header_table | set_date @6B92D5 | 战斗表头（回合/日期）；经缓存 `combat_header_table_cache`@0x982720 |

- `t_combat_label` 的 `std::map<double, t_bitmap_array>` 缓存是**健康/图标位图**（资源 "icons.combat_labels.health"），非文本，不影响字体路径。

## 6. 系统2：2003 新增富文本引擎（不在 map 中）

- 特征：68B 全局字形表@0xAA1530（43 字形名索引）、double 宽度度量、断行候选、渲染项；与系统1并行。
- **`heroes4_debug.map`（2002-10）无任何对应符号 → 本 map 不能用于标定系统2。** exe 中亦无 ".fon"/"MS FNT"/"8bpp" 字串（GDI 仅 6 个位图函数，无文本 API）——`Font_LoadDLL` 等 v1 猜测名待系统2独立逆工时复核。
- 与系统1衔接点：`TextLayout_BuildLines`@0x5A67A0 会调用 `get_font`@0x875BC0 与 `t_text_window_ctor_rect`@0x8859F0（嵌入系统1窗口渲染片段）。
- 现有暂定名保留：Text_MeasureWidth@519950、Text_GetCharWidth@483250、TextLayout_ComplexLayout@47F780、TextLayout_LinearLayout@482C90、Text_BuildRenderItems@4869C0、Text_Invalidate@47C870、TextLayout_ClassifyBreaks@5A6670、Font_GetGlyphEntry@59FBB0、Font_InitGlyphTable@59FBE0、Font_LoadDLL@88D500、Font_ProcessDLLData@88DB60、Font_BuildBitArrays@88FDE0、Font_Init@8934B0、Font_HasGlyph@893900、Font_IsMonospace@894260、Char_GetBreakType@894020、Char_IsLineBreak@894090、Char_IsBreakCandidate@894B10、TextWidget_Render/SetText/Draw@44BD30/44BFE0/483C40、Render_Resize/GetSurface@59F370/59F4F0。
- 未 Hook；中文若走系统2则不受插件影响（问题3）。

## 7. 插件实现

源码按 Google C++ 风格拆分为 `game_types.h` / `game_addrs.h` / `inline_hook.*` / `diagnostics.*` /
`font_cache.*` / `blit.*` / `wrap.*` / `hooks.*` / `main.cc`（职责划分见 §7.6）。

### 7.1 Hook 总表（`game_addrs.h` 的 `HookTarget`：地址 + 长度 + 期望序言）

| # | 地址 | 常量 | 目标函数 | 长度 | 序言 | 作用 |
|---|---|---|---|---|---|---|
| 1 | 0x71BD50 | `kFontDrawToPt` | t_font_draw_to_pt | 7 | `51 53 55 8B 6C 24 1C` | GBK 单行渲染，替换逐字节 blit |
| 2 | 0x71BDE0 | `kFontGetWidth` | t_font_get_width | 6 | `53 56 8B 74 24 0C` | GBK 宽度（advance 累加，不光栅化） |
| 3 | 0x71BE40 | `kFontGetColumn` | t_font_get_column | 8 | `51 53 55 56 8B 74 24 14` | GBK 断行，半字形溢出容差 |
| 4 | 0x71C5E0 | `kFontWrapTextMm` | t_font_wrap_text_mm | 5 | `8B 54 24 08 53` | 全量替换：`WrapTextOptimal`（GBK 量最长词，复刻 min..max 增长搜索） |
| 5 | 0x71C100 | `kFontWrapText` | t_font_wrap_text | 7 | `6A FF 68 A0 60 94 00` | GBK 感知重写，重建 t_string_vector |
| 6 | 0x71C7F0 | `kFontDrawToRect` | t_font_draw_to_rect | 7 | `6A FF 68 B8 60 94 00` | 替换 wrap+draw_to_vector 逐字节渲染 |
| 7 | 0x71C900 | `kFontGetWrappedHeight` | t_font_get_wrapped_height | 7 | `6A FF 68 D8 60 94 00` | GBK 行数 × 实测行高 |

### 7.2 安装与 trampoline

- 长度取「≥5 字节的第一个真实指令边界」（1+1+1+4=7、1+1+4=6、1×4+4=8、SEH 2+5=7、4+1=5）。
  **旧版把 1/2/3 号目标按 5 字节覆盖，trampoline 复制的是半条 `mov r32, [esp+x]`**——一旦真的
  调用 `g_Orig*` 就会把后面的 `E9` 当操作数执行；因为当时只有 4 号（序言恰好对齐）委托原函数，
  问题没暴露。
- `InstallInlineHook` 先比对 `prologue`，不匹配就**拒绝打补丁**（换了 exe 版本时只是没有中文，
  不会破坏代码）；`orig == nullptr` 时不分配 trampoline。7 个 hook 现在全量替换、全传
  `nullptr`（`kFontWrapTextMm` 的委托已换成 `WrapTextOptimal`）。
- 补丁：≥7 字节用 `B8 imm32 / FF E0`（多出的长度 NOP 填充），5/6 字节用 `E9 rel32`（+NOP）。
  写完 `FlushInstructionCache`；返回 `HookInstallResult` 枚举，失败原因由 `InstallHooks` 汇总诊断。
- 初始化是一次性 `Initialize()`（`InitOnceExecuteOnce`）：`DllMain(DLL_PROCESS_ATTACH)` 调用；同时
  导出无参无返回的 `zk`（`extern "C" __declspec(dllexport)`，x86 导出表名即 `zk`）供其他注入工具在
  LoadLibrary/手动映射后 `GetProcAddress("zk")` 调用。两条路径只生效一次，后到者为空操作；
  DllMain 没跑过时 `zk` 用 `GetModuleHandleExW` 自行找回模块句柄（定位 `plugins\` 下的 log/toml）。
- `DLL_PROCESS_DETACH` 先 `UninstallAllHooks()` 还原原字节，再释放 GDI/缓存。
- **`Heroes4GL` 模组兼容（`mods\*.mod`）**：Heroes4GL（`ddraw.dll` GL 封装）在自己的 `DllMain` 里
  `FindFirstFile "<game>\mods\*.mod"` → `LoadLibrary`，再 `GetProcAddress` 取 `GetName`/`GetMenu`/
  `SetHWND`/`LoadPackages` 四个 `__stdcall` 导出，**四者全非空才保留**、少一个即 `FreeLibrary`
  （见 HeroesGL 的 `Mods::Load`）。本插件这四个都是纯 ABI 桩：`GetName` 回静态串 `"H4CN Chinese
  font"`，`GetMenu` 回 `nullptr`（无自制菜单，全员 `nullptr` 时 H4GL 连「Mods」弹出项都不建），
  `SetHWND` 空（不子类化窗口），`LoadPackages` 不调回调（不注入 `.erps`）。真正的装钩仍由
  `LoadLibrary` 触发的 `DllMain`→`Initialize()` 完成，与 `.asi` 路径逐字节相同。`GetMenu` 与
  `user32` 同名，故用 `#pragma comment(linker, "/export:GetMenu=_H4cnModGetMenu@4")` 别名导出，
  不用 `__declspec`。同一镜像若 `.asi` 与 `.mod` 各装一份会加载两次，第二份序言比对失败 →
  记 `0/7 installed` 空转、不重复打补丁（**二选一部署**）。构建 `-ModDir '<game>\mods'` 直接投放。

### 7.3 GBK 判定、断行与测量

```cpp
IsGbkLead(b):  0x81<=b<=0xFE
IsGbkTrail(b): 0x40<=b<=0x7E || 0x80<=b<=0xFE
```
- 断行只有 `wrap.cc` 的 **`WrapText`** 一处实现，#5 / #6 / #7 全部走它：ASCII 段按空格断词并裁剪
  行尾空格（与原版 `t_font_wrap_text`@0x71C100 的按词回退一致），CJK 逐字断行，`\n` 强制断行，
  返回最大行宽。行数若与 #7 的返回值不一致，原版 `t_text_window_update_layout` 会把滚动条范围
  算成 0 后除零崩溃。
- 空串/纯空格尾不产行，`\n` 产空行 —— 与原版行数语义一致。
- 测量与光栅化分离：`FontContext::Advance` 只查 advance 表（ASCII 在建上下文时用
  `GetTextExtentPoint32W` 一次性预热），`GetGlyph` 才走 GDI 输出。旧版 `LookupGlyph` 为了取
  advance 也做 FillRect+TextOut+GetDIBits，而 `get_width` 有 9 处调用、`wrap_text` 有 13 处。
- `Line` 是 `{ptr, len}` 字节区间，`DrawLine` 按长度绘制；旧版把 `*lineEnd = 0` 写进游戏缓冲再
  恢复，对 `.data`/`.rdata` 里的字符串（如 `Path`@0x97036c）是危险的。
- 每个 (face, cell) 一个 `FontContext`（HFONT + 兼容 DC + 32bpp DIB section + 字形表）：游戏同屏
  混用十几个字号，旧版单例在每次换字号时 Delete/Create 字体并清空整个缓存。face 与 cell 由
  H4CN.toml 按 `t_font::size` 解析（§7.7），默认「size + bias」且每 size 一个，故与旧的每字号一个等价。
- `Rasterize` 直接读 `CreateDIBSection` 返回的 `bits` 指针（不再 `GetDIBits`），转换时算出
  **墨迹包围盒**（含烘焙的投影），`BlitGlyph` 只遍历这块区域并跳过零覆盖像素。
- **超采样**（全局 `[render].supersample`，本分支默认 2，可被 `[fonts.<size>].supersample` 逐字号
  覆盖——小字点对点更锐利；见 §7.7）：借用 Heroes4GL 在**显示端**做
  Hermite/Lanczos/xBRZ 的思路，但挪到我们能控制的**源字形这一级**——DIB 与第二把 HFONT 放大
  N×，字形画进去后把每 N×N 子像素覆盖**盒式降采样**回 1× 再量化到 4bit nibble，于是细笔画能落在
  半像素位置（GDI 1× 网格给不出的中间覆盖）。`supersample=1` 时 `Rasterize` 逐位回到 v3.1.0。
  缓存键相应从 (face, cell) 扩成 **(face, cell, supersample)** 三元组。
  只放大位图：`Measure`/`advance`/`line_height` 仍用 1× 字体（`Rasterize` 结束后在共享 DC 上还原
  `font_`），测宽与换行完全不变，故 #5/#6/#7 行数一致、滚动条不会除零。
- GBK→UTF-16 显式用 CP 936，渲染不依赖系统 ANSI 代码页。

### 7.4 GetFontContext / PatchFontMetrics
`size` 即 t_font+0x04（`t_font_read`@0x71BEE0 证实是资源里的单字节），是游戏位图字体的身份
（`get_font@0x875BC0` 按它二分 `g_fonts`）。`GetFontContext` 用 `Config::Resolve` 把 size 映射到
`{face, cell}`：默认 `cell = size + [fonts].default_bias`（**4**，与配置出现前一致），可被
`[fonts.<size>]` 的 `size`/`bias` 覆盖（见 §7.7）。每次 Hook 进入时把 **`TEXTMETRIC.tmHeight`**
（GDI 实测单元高度）写入 `font->line_height`(+0x0C)，使未 Hook 路径（`t_text_window_paint`、
`t_text_window_update_layout` 等）的行距与我们实际绘制用的高度一致；绘制/换行的推进量同样用
`tmHeight`。这一写回受 `[general].patch_line_height` 开关控制（默认开）。


### 7.5 RGB565
混合掩码（blit 用）：M0@0xAAF0D8=0x000F81F0，M1@0xAAF0DC=0x0000F81F，M2@0xAAF0F4=0x00007E00，M3@0xAAF104=0x000007E0；
4bit alpha（高4=前景/低4=阴影）：
```
nMask=(byte>>7)+(byte>>4)
clr=(M0&(16*(M1&clr)+nMask*((M1&fg)-(M1&clr))))+(M2&(16*(M3&clr)+nMask*((M3&fg)-(M3&clr)))); clr>>=4
```
色打包位移常量（set_color/ctor 用）：dword_AAF100/104(ppvBits)/108/10C/110。

我们的 `BlitGlyph` 与该逐位一致（阴影/前景两遍、`nMask` 取法、`>>4` 都相同），差别只在：
掩码与 `m&fg`/`m&shadow` 的乘积**提到像素循环外**、`byte==0` 直接跳过不回写、
且只遍历字形的墨迹包围盒（`ink_x/ink_y/ink_w/ink_h`，含烘焙的投影）。

### 7.6 代码结构（`src/`）

| 文件 | 职责 |
|---|---|
| `game_types.h` | `game::Font/Bitmap/Rect/Vc6String/StringVector`，字段名对齐 IDB，`static_assert` 锁死布局 |
| `game_addrs.h` | 全部绝对地址 + `HookTarget`（地址 / 长度 / 期望序言），注释里记每个目标的指令长度拆解 |
| `inline_hook.*` | 序言比对 → 打补丁 → trampoline → `UninstallAllHooks()` 还原原字节；返回失败原因枚举 |
| `diagnostics.*` | `DiagLog`：每条都 ODS + 追加 `plugins\H4CN.log`（带本地时间戳）；启动小结每次运行写一行（版本 + hook 计数），字体替换/上下文失败也报 |
| `config.*` | `H4CN.toml`（`deps/toml.hpp` = toml++ 单头，仅此 TU）：`[general]` 通道开关 + `[render].supersample`（全局，可被 `[fonts.<size>].supersample` 逐字号覆盖）+ `[fonts]`/`[fonts.<size>]` face/cell；纯解析 `ParseConfig`（host 可单测）+ `EnsureConfigLoaded`（首帧懒加载，不在 `DllMain`） |
| `font_cache.*` | 按 (face, cell) 的 `FontContext`（HFONT + 兼容 DC + 32bpp DIB section，实现 `Advancer`）、advance 与像素两级缓存、`GetFontContext`（解析配置并建/缓存）、`PatchFontMetrics` |
| `blit.*` | 4bit alpha → RGB565 混合（只走墨迹盒）+ `DrawLine`（按字节区间绘制） |
| `wrap.*` | `WrapText`（**唯一**断行实现）+ `WrapTextOptimal`（复刻 0x71C5E0 的 min..max 增长搜索）+ `DecodeChar`；经 `Advancer` 测量，不碰游戏内存/GDI，host 可单测 |
| `hooks.*` | 7 个 hook 函数 + `InstallHooks()` |
| `main.cc` | `InitOnceExecuteOnce` 一次性 `Initialize()`：`DllMain` ATTACH 调用；另导出无参无返回的 `zk`（其他注入工具 `GetProcAddress("zk")` 手动加载后调用，重复调用/已初始化则空操作）；DETACH 先还原钩子再释放 GDI/缓存。再加 4 个 `__stdcall` 空实现 `GetName`/`GetMenu`/`SetHWND`/`LoadPackages` 供 `Heroes4GL` 以 `mods\*.mod` 加载（见 §7.2）|
| `tests/wrap_test.cc` `tests/config_test.cc` | host 控制台单测（`./build.ps1 -Config Debug -Test`，默认不建）：断行行数/宽度/断词规则；`ParseConfig` 的类型/范围/优先级/坏值回落 |

Hook 之间的分工要点：#1–#7 全部全量替换原实现（`orig` 传 `nullptr`，安装器不建 trampoline）；
#4 `kFontWrapTextMm` 现走 `WrapTextOptimal`，是唯一仍复刻原版控制流（min..max 增长搜索）的 hook。
`game::Bitmap` 前 20 字节与 IDB `t_bitmap` 完全一致（旧 `H4BitmapStrc` 的 margin/pad 字段是虚构的，
`TempBitmap` 每字形 memset 40 字节也已删除）。

### 7.7 H4CN.toml 配置（`config.*`）

- 解析库 `deps/toml.hpp`（toml++ v3.4，单头 MIT）；异常模式（`/EHsc`），仅 `config.cc` 一个 TU
  include，外层包 `#pragma warning(push,0)` 让 /W4 看不到第三方告警。
- **懒加载**：`GetFontContext` 首行 `EnsureConfigLoaded()`（`std::call_once`），即首次被 Hook 的
  绘制/测量调用时读一次，绝不在 `DllMain`（loader lock）读盘。文件缺失/超 64KB/语法错/类型错
  一律「报一行 + 用默认」，坏值逐项忽略、不污染邻居。
- 通道：`diagnostics.*` 在配置读入前把已产生的行（装钩小结）带时间戳缓存，`EnsureConfigLoaded`
  解析到 `[general]` 后 `DiagApplyChannels` 冲刷——于是 `log_file`/`log_debug_view` 对整会话一致。
- schema（全部可选，缺省即复刻配置功能之前的行为）：见 `H4CN.example.toml` 与 `config.h` 头注释。
  主键 `[fonts.<size>]` 的 size = `t_font::size`（**资源名义尺寸，≠ §4.2 的 get_font 请求字号**；
  实测已见 8 10 12 13 15 18 20 21 23 25 26 28，见 §4.2 ⚠）。每表内优先级：
  `size` > `bias` > `[fonts].default_bias`。未列出的 size 自动走 fallback + default_bias，
  并在 `H4CN.log` 留下 `game size NN -> ...` 行供补配置。`H4CN.example.toml` 既是 schema，也随
  本分支携带调优后的逐 size 默认（大字号 LiSu、小字号 SimSun、逐 size bias 2..7，小字号
  `supersample = 1` 点对点）；复制为 `plugins\H4CN.toml` 即生效（仓库不再另存一份 `H4CN.toml`）。
- 纯逻辑（`ParseConfig` + `Config::Resolve`）无 Windows 依赖，`config_test.cc` 直接喂字符串断言。
- `[render].supersample`（全局，整数 1..4，默认 **2**）+ 可选 `[fonts.<size>].supersample`（逐字号
  覆盖，同样 1..4）：字形光栅化的超采样倍数，详见 §7.3「超采样」条。`Config::Resolve` 输出该字号最终
  生效值（per-size > 全局），`GetFontContext` 据此建 (face, cell, ss) 三元组缓存。`1` = 逐位复刻 v3.1.0
  观感（稳定基线，小字号建议点对点更锐利），`2` = 全局实验默认。越界/非整数按坏值忽略回落。合并回
  master 前须决定是否把全局默认改回 1（AGENTS「缺省复刻旧行为」）。


## 8. 已知问题的技术分析

> **状态与待办一律在 `TODO.md`**，本节只留「根因/证据/候选做法」这类不会随进度失效的分析。

### 8.1 换行乱码（任务简报/剧情文本）——根因与修复原理
- 根因：`t_font_wrap_text`@0x71C100 逐字节拆行 + 用空格字形测量 GBK 字节（宽度错误，且会在
  width 处把一个双字节字符劈成两半）。
- 修复原理：`wrap.cc` 的 `WrapText`（§7.3）单点实现，GBK 感知测量、永不拆多字节字符；#5/#6/#7
  三个 hook 共用，行数与高度不可能再对不上。断行为中西文混排（ASCII 按词 + 裁行尾空格还原原版
  语义，CJK 逐字）。
- 涉及的任务简报路径：sub_69EF40（dialog.win_lose.prologue_*）→ sub_58FA40（bitmap group）→
  `t_text_window_ctor_rect`/`set_text` → `wrap_text` → `t_font_draw_to_vector`@0x71C6E0 逐行渲染
  （绕过 draw_to_pt，已由 Hook#6 替换）。

### 8.2 弹出框宽度不正确（两处独立成因）
- `sub_838630`（77 调用者）宽度取 `this[+0xE0]-this[+0xD8]` 父边界，**根本不测文本**；高度走
  已 Hook 的 `get_wrapped_height`。→ 需 Hook `sub_838630` 改用 GBK `get_width`。
- `sub_8CAE90` → `wrap_text_mm@0x71C5E0`：#4 号 hook 只修度量后**委托原版**，而原版最优宽度是用
  游戏自带字形表量的（GBK 被夹到空格字形），结果必错。→ 或整体重写 `0x71C5E0`，或先用我们的
  宽度算 optimal 再让原版填容器。

### 8.3 系统2 未 Hook
2003 新增富文本引擎（§6），`heroes4_debug.map` 无符号，独立逆工后再评估。衔接点
`TextLayout_BuildLines@0x5A67A0` 会回调 `get_font@0x875BC0` 与 `t_text_window_ctor_rect@0x8859F0`。

## 9. 标定侧待办

全部移入 **`TODO.md`**（basic_dialog 集群、button 虚槽落名、编码/本地化 release 地址、
combat 消息渲染路径核实、系统2 专项）。逆工通用注意（tail 拆块 / 陈旧 noreturn）见 §2.3。


## 10. 构建与静态检查

```powershell
./build.ps1 -Config Release -DeployDir '<游戏目录>\plugins'
# 产物: out/build/x86-release/H4CN.asi（-Config Debug → out/build/x86-debug/）
# -Clean 先删该预设的构建树；-Test 额外构建并运行 host 单测（ctest：wrap+config）
# -ModDir '<游戏目录>\mods' 追加把同一产物复制成 H4CN.mod 供 Heroes4GL 加载（与 ASI 二选一）
```

`build.ps1` 定位 VS（vswhere）、进 **x86** 开发者环境（`Enter-VsDevShell -DevCmdArguments
'-arch=x86 -host_arch=x64'`）、把 VS 自带 cmake/ninja 前置到 PATH（PATH 上先命中的
Strawberry Perl 附带的 `cmake.exe` 会以 `0xC000007B` 崩溃），再跑
`cmake --preset x86-<cfg>` + `cmake --build --preset x86-<cfg>`（`CMakePresets.json`，Ninja）。
产物永远是 32 位 PE：`heroes4.exe` 是 32 位，`game_types.h` 的 `static_assert` 故意在 x64 下编译失败。

CMakeLists.txt：C++20、`/W4 /permissive-`、链接 kernel32+gdi32+user32（`GetDC`/`ReleaseDC` 在
user32）、`OUTPUT_NAME H4CN` + `SUFFIX ".asi"` + `PREFIX ""` + `DEBUG_POSTFIX ""`；
可选 `H4CN_DEPLOY_DIR`（→ `H4CN.asi`）与 `H4CN_MOD_DIR`（→ `H4CN.mod`）非空时各 POST_BUILD 拷一份过去。
CRT 为默认 `/MD`（Debug 是 `/MDd`）：**Release 目标机需装 VC++ 2015-2022 x86 运行库；Debug 版依赖
调试版 CRT（`MSVCP140D/VCRUNTIME140D/ucrtbased`），只在装了 VS 的机器上能加载，不可外发。**

```powershell
$vs = 'C:\Program Files\Microsoft Visual Studio\18\Community'   # 用 vswhere 定位
$cf = "$vs\VC\Tools\Llvm\x64\bin\clang-format.exe"
$ct = "$vs\VC\Tools\Llvm\x64\bin\clang-tidy.exe"
& $cf -i --dry-run -Werror --style=file src/*.cc src/*.h src/tests/*.cc   # 必须 0 告警
& $ct -p out/build/x86-debug --extra-arg=-m32 --quiet src/*.cc src/tests/*.cc   # 仓库根目录跑
```

`clang-tidy` 的两个坑：**必须 `-m32`**（compile_commands 只写了 x86 的 cl.exe 路径、没有架构标志，
clang 默认按宿主 x64 解析，`game_types.h` 的布局 `static_assert` 会全部误报），且需要 presets 导出的
`compile_commands.json`（已开 `CMAKE_EXPORT_COMPILE_COMMANDS`）。配置见 `.clang-tidy`
（关掉 `performance-no-int-to-ptr`：把 IDB 校准的绝对地址转指针是本项目的固有做法）。


