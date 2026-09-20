# Ctrl+Shift+F：0.17.4 → c652e4b 回归范围

## 已知边界

用户实测：相同 Rime 用户配置、相同软件和输入状态下，官方 0.17.4 正常；第一次编译成功的 c652e4b 已失效。

- 正常基线：`9cc96e20dc71b80876b12f689bb5863c76c2a7ed`（官方 0.17.4）。
- 自定义功能前的官方基线：`d73f6295e8252ed2f7b9c12bae32e9001b1afdaa`。
- 已知故障上界：`c652e4bf330072f516e802b912cb87c09de8cae3`。
- 范围内共 55 个提交：50 个官方后续提交 + 5 个自定义提交；总计 90 个文件变更。

这证明故障在 c652e4b 或更早已存在，不证明就是 c652e4b 本身引入，也不证明是自定义功能引入。

## 可以排除的后续提交

以下提交均晚于故障上界，不是最初故障的来源：

- `5d0c5e7`：动态布局继承、候选样式同步。
- `d41eae3`：按候选布局重映射导航键。
- `ac3a4b4`、`4df34dc`、`ac2ef66`：动态字母选词及其修饰键修复。
- `c8c2083`：按键缓存、TSF 保留快捷键实验。它引入的重复输入回归是另一个问题，工作区中已撤回。

## 你的 5 个提交究竟改了什么

| 提交 | 实质内容 | 是否修改按键处理 |
|---|---|---|
| `90d7146` | 动态候选横/竖排：读取规则、按 option/候选长度/数量决定布局；会话和 UI 样式同步；补充 UI 重绘、配置和测试 | 没有直接修改按键处理；会改变 UI 更新路径 |
| `be936de` | README 说明 | 否 |
| `2cf25c3` | 空提交，触发 CI | 否 |
| `5aab792` | 格式化（忽略空白后的 diff 为空） | 否 |
| `c652e4b` | 修复 lambda 内数组长度的 MSVC C2131 编译错误：局部声明 `RULE_BUF_SIZE = 255`，替换 4 处缓冲区大小引用 | 否 |

自定义范围涉及 21 个文件，但没有修改 `WeaselTSF/`、`WeaselIPC/` 或 `WeaselIPCServer/`。运行时新增逻辑主要位于 `RimeWithWeasel.cpp`、`DynamicCandidateLayout.h` 和三个 `WeaselUI` 文件。

不能仅凭“只改 UI”完全排除动态布局：它在 `_Respond` 中增加布局解析，并在 `_UpdateUI` 中增加 `_GetContext` 和布局解析。真实上下文求值、通知和 UI 回调的副作用尚未在 Windows 上验证。

## 直接按键链路的核对结果

实际比较 0.17.4 和 c652e4b 的源码，结果如下：

| 代码 | 结果 |
|---|---|
| `WeaselTSF/KeyEvent.cpp` | 内容完全相同 |
| `include/KeyEvent.h` | 内容完全相同 |
| `WeaselIPC/WeaselClientImpl.cpp` | 内容完全相同 |
| `OnTestKeyDown / OnKeyDown / OnTestKeyUp / OnKeyUp / OnPreservedKey` | 规范化空白及 `STDAPI` / `STDMETHODIMP` 声明宏后相同 |
| 服务端 `RimeWithWeaselHandler::ProcessKeyEvent` | 规范化空白后相同 |

因此，不能再把最初故障归因于后来的按键重映射、动态字母选词或缓存。也不能据此排除 IPC 传输内部、组字状态、UI 副作用或引擎差异。

## 官方后续改动中需要优先验证的部分

| 提交 | 改动 | 与问题的关系及限制 |
|---|---|---|
| `8f2561f` | TSF 组字生命周期重构：移除非行内模式占位文本；变更组字结束、自动上屏后的重建、UI 开始/结束以及 `OnCompositionTerminated` 处理 | 最值得做单独前后对比。现在某些终止回调会只清空 TSF 组字指针，保留 Rime 的组字状态，可能出现宿主与引擎状态不同步。是否导致本问题尚未实测；当前用户配置为行内预编辑，不能单凭移除非行内占位文本就认定根因 |
| `93eec2d` | `_UpdateUIElement()` 从仅在宿主接管 UI 时调用，改为每次都调用 | 增加与宿主 TSF UI 管理器的交互；Word 与 EmEditor 可能不同，尚无故障证据 |
| `773113a`、`4eca2ac` | IPC 管道改为线程局部句柄/缓冲区，消息长度改为实际正文长度 | 即使调用端代码不变，响应和处理结果仍经过这层；需 Windows 真实 IPC 验证 |
| `d636e0a`、`74eb272` | 删除旧式 IMM/WeaselIME，改为只支持 TSF，并清理服务端 IME 分支 | 只有官方版本实际走旧式 IME 时才是有效差异。0.17.4 的旧式 IME 支持是安装选项，不能假定用户启用了它 |
| `da1ef1c`、`6b37e97`、`2d2fdd9`、`f701308`、`d73f629` | 通知窗口、上下文读取、响应生成、托盘刷新等改动 | 主要影响候选显示或响应时序；比直接组字/IPC 改动关联更间接 |

其他官方改动包括维护与日志、候选窗定位与绘制、安装语言配置、构建环境、更新器及资源修复。完整提交清单见后文。

## 源码之外还有一个未固定变量：librime

0.17.4 的发布工作流和 c652e4b 的 CI 都执行 `get-rime.ps1 -use dev`，没有指定 `-tag`。
脚本未指定 tag 时读取 GitHub `releases/latest`，即构建当时的最新正式发布。`-use dev` 表示获取开发所需依赖，**不等于使用 nightly**。

因此，2025 年的官方安装包和 2026 年的自编译包不保证包含相同的 librime / Lua 插件。没有读取两个实际安装包前，不能凭配置文件相同就排除这个变量，也不能断言具体版本不同。

## 下一次 Windows 对照如何最有效

先用与故障版一致的引擎依赖、配置和构建方式编译 **d73f629（自定义动态布局之前）**，再与 c652e4b 对照：

- d73f629 正常、c652e4b 失败：范围落在 5 个自定义提交，实质重点是 `90d7146` 的 UI/上下文路径。
- 两者都失败：优先检查 50 个官方后续提交及引擎版本差异。固定引擎后，先对比 `8f2561f` 和其父提交；若前后均失败，再往更早的 IPC/IME 变更二分。

所有对照应记录并固定 `rime.dll` 及其构建依赖版本；完整安装对应前端，重启宿主以卸载旧 DLL。不能只重新部署配置，也不应把其他版本的残留 DLL 当作对应提交的测试结果。

当前环境为 macOS，本轮完成历史源码核对，没有 Windows 实测，没有新增猜测性的生产代码修补，尚未确认根因。

## 完整官方提交清单（历史顺序）

| 提交 | 说明 |
|---|---|
| 4e264d9 | ci: update runner windows-2019 to windows-latest |
| 7bd62cd | ci: incomplete edit |
| 35cd09c | ci: fix ci actions caused by runner updating (#1645) |
| dc99071 | revert: 0d20317 and #1281 |
| c127fa7 | fix: localtime warnings |
| 8b88df2 | ci: change runner back to 2022 |
| 49811d3 | fix(UI): hang by unexpected nullptr in StandardLayout calculation |
| ef9c8b3 | fix(WeaselTSF): client dump caused by dereferencing nullptr during CEndCompositionEditSession::DoEditSession |
| 2d2fdd9 | refactor(RimeWithWeasel): Reduce redundant _GetCandidateInfo() calls by computing candidates once per context. (#1750) |
| 4210a7a | fix(WeaselUI): fix#1672, The candidate box is in the wrong position |
| b77f4bd | fix(RimeWithWeasel): Make static variable access thread-safe with std::lock_guard |
| 4783148 | fix(WeaselSetup): typo in if condiction |
| ce29333 | build(xmake): remove .xmake-cache (#1762) |
| 6164dbf | build: handle xmake runtime warnings |
| e573a0a | build: version info define for rc files only |
| a10c058 | build: WeaselServer link with libs not dependent objs |
| da1ef1c | fix(RimeWithWeasel): tip not shown correctly when margin set nagative, and show_notifications not work correctly |
| 773113a | fix(WeaselIPC): resources of PipeChannelBase concurrent modification |
| d13f74e | build(xmake): disable full path for __FILE__ macro |
| f84af93 | chore: fix warnings for x64 compilation |
| 6b37e97 | fix(RimeWithWeasel): tip not shown correctly when margin set nagative, incomplete edit |
| 4eca2ac | perf(WeaselIPC): optimize message transmission with dynamic sizing |
| b0d0e86 | fix(WeaselTSF): couldnot open directory with default file manager but only explorer |
| fb2998e | refactor(RimeWithWeasel): improve readability and _RimeGetColor performance |
| 31e741c | refactor(RimeWithWeasel): less memory copy by reference for some funcs |
| c4c2aaf | refactor(RimeWithWeasel): use static global rime_api |
| b2b4a38 | refactor(RimeWithWeasel): _RimeParseStringOptWithFallback with std::array to enhance performance, with constexpr reference input |
| f701308 | perf(RimeWithWeasel): improve performance of RimeWithWeaselHandler::_Respond |
| d636e0a | refactor: drop WeaselIME (#1775) |
| 47ed6b1 | fix(WeaselTSF): multi WeaselServer might be started |
| 5f96fae | fix(RimeWithWeasel): join_maintenance_thread to avoid hanging |
| 74eb272 | refactor(RimeWithWeasel): remove useless code for ime |
| bccdff6 | fix(WeaselServer): WinSparkle will always open ReleaseNoteUrl in default browser, bundled it with patch |
| 7da19f2 | ci: merge ci ymls |
| 193a9ad | fix(WeaselUI): mark metrics of fullscreenlayout |
| cbe3aab | perf(WeaselUI): GdiplusBlur with openmp |
| b3d161e | fix(WeaselUI): hover is triggered when mouse not moved |
| 5ffbae1 | fix(WeaselUI): fix clipboard memory issue |
| 9b2943c | fix(WeaselUI): candies.size() might larger then MAX_CANDIDATE_COUNT |
| 022f749 | fix(WeaselUI): hover will be triggered, when panel is first created with mouse on some candidate, event mouse not moved, incomplete edit of b3d161e |
| 93eec2d | fix(WeaselTFS): Always update UIElements. |
| f9203ca | fix(WeaselTSF): remove incorrect double quotes from ShellExecuteW path |
| c681ea8 | fix: define version macros for C++ builds |
| 8f2561f | fix(WeaselTSF): handle non-inline auto-commit without placeholder text |
| 829b07e | fix (WeaselTSF): Use STDMETHODIMP instead of STDAPI for non-static member functions (#1835) |
| 1468b88 | refactor(WeaselSetup): customizable installing profile |
| ee0767a | refactor(WeaselSetup): add params for command line to install profile |
| 9d45db1 | fix(WeaselSetup): persist installing profile and fix ARM32 uninstall |
| 287ce64 | fix(WeaselSetup): dir and custom button not enable after select custom dir |
| d73f629 | fix(WeaselServer): avoid tray refresh blocking IPC pipe |

## 完整自定义提交清单（历史顺序）

| 提交 | 说明 |
|---|---|
| 90d7146 | feat: implement dynamic candidate layout |
| be936de | docs: add dynamic candidate layout section to README.md |
| 2cf25c3 | ci: trigger github actions build |
| 5aab792 | style: format code according to clang-format rules |
| c652e4b | fix(DynamicLayout): fix MSVC error C2131 in lambda by defining RULE_BUF_SIZE |
