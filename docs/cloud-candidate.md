# 云候选：自动查询与主动快捷键

已接入原生 SogouProvider、Ctrl+Shift+C 主动查询、满 6 个输入字母自动查询、后台 WinHTTP、内存 LRU/TTL、Rime 候选插入和原生 userdb 学习。Google/LLM 仅保留抽象接口。自动查询可用 `cloud_candidate/auto_trigger: false` 关闭。

## 文件

新增：

- `weasel/include/CloudCandidate.h`：数据类型、协议、Provider 接口、Manager、Cache、PrivacyPolicy。
- `weasel/RimeWithWeasel/CloudCandidate.cpp`：Windows WinHTTP 传输。
- `weasel/test/TestCloudCandidate/core.cpp`、`CMakeLists.txt`：跨平台核心回归；Windows 构建同时编译实际 WinHTTP 传输。
- `weasel/.github/workflows/cloud-candidate.yml`：Windows Debug/Release 核心检查；尚未推送或执行远端 CI。
- `Rime/lua/cloud/cloud_native.lua`：Rime 按键、读音、候选和生命周期桥接。
- `Rime/cloud_candidate_quanpin.schema.yaml`：内部只读全拼棱镜，不加入用户方案列表。
- `tests/cloud_native/engine.c`、`run_engine.py`：隔离公开词典的真实 librime/Lua 回归。
- `tests/cloud_native/live_sogou.py`：公开测试拼音的可重复 HTTPS 探测工具。
- 本文。

修改：

- `weasel/include/RimeWithWeasel.h`、`weasel/RimeWithWeasel/RimeWithWeasel.cpp`：会话所有权、请求桥接、结果领取、销毁与失焦取消。
- `weasel/RimeWithWeasel/RimeWithWeasel.vcxproj`、`weasel/WeaselServer/xmake.lua`：编译/链接接入。
- `Rime/double_pinyin_flypy.schema.yaml`、`Rime/rime_mint.schema.yaml`：cloud_candidate 配置、组件、内部棱镜依赖。
- `weasel/test/test_shortcut_forwarding.py`、`weasel/test/test_tsf_key_delivery.py`、`tests/test_tab_mode_switches.lua`：更新与当前接口不匹配的测试桩。

仓库原有未提交修改保留。没有编辑词库或生成目录；没有提交、推送或上传个人词库。旧 cloud_pinyin 模块保持原有关闭状态，新功能不调用它的外部进程网络实现。

## 架构与线程

- **CloudCandidateManager**：注册 Provider、单个后台 worker、有界队列（64 个会话，每会话仅保留最新待执行任务）、截止时间、结果邮箱、取消和 stale 检查。Provider 同步 Query 只在该 worker 调用，输入路径不等待 future 或 HTTP。
- **SogouProvider**：BuildSogouRequest、QuerySogou、ParseSogouResponse、Utf16LeToUtf8；最多返回 5 个完整匹配候选。
- **CloudCandidateCache**：`provider:full_pinyin` 键，steady_clock TTL，真正的 LRU，默认 3600 秒/1000 条。命中立即发布，不调用网络；失败和空候选不缓存。
- **CloudPrivacyPolicy**：查询许可、功能模式和密码标记检查。Lua 先检查 segment tags、tab_mode、ASCII、辅码状态及完整输入范围，C++ 再检查。

后台 WinHTTP 使用异步句柄与完成事件，worker 按单一绝对截止时间等待各阶段；超时关闭 request，并等待 HANDLE_CLOSING 清理，防止回调访问失效内存。严格 HTTPS、禁止重定向、最多读取 64KiB。句柄和回调状态有作用域清理。

结果不直接调用 Rime/UI。现有 TSF 80ms polling 请求异步读写编辑会话，发送内部 F34；Weasel 在受 `g_api_mutex` 串行保护的现有 IPC 派发路径领取结果，Lua 调用 `refresh_non_confirmed_composition`，通过既有 IPC 响应更新 TSF/candidate window。不同 IPC 请求可以来自不同 pipe worker；这里并不假设固定的单个服务线程。新网络 worker 完全不访问 Rime 对象。

每次查询包含 query_id、composition_revision、input_snapshot。Lua 指纹还包括光标、片段范围、模式属性；输入变更、提交、失焦、方案切换和 session 销毁使请求失效。Manager 和桥接层检查身份，Lua 在刷新前再次验证指纹。返回旧输入内容即使后来又重新输入相同字符，也不能恢复旧请求。

## 读音与学习

先查询当前原生 script_translator，取得完整候选的 DictEntry.code，用 Memory.decode 恢复词典音节并去声调。不再维护双拼键位表。

全拼兼容使用内部原生全拼 translator。纯首拼从当前本地候选的词典反查恢复读音；反查库仅有单字时，枚举最多 32 个读音组合，再用原生 translator 验证整词。无法验证时不联网。首拼读音仍受本地候选和多音字歧义影响，不是云端直接解码首字母。

云词使用真实 Phrase + 同一词典音节编码，由原有 script_translator 的选择/提交学习机制记入原有 userdb。无第二套永久云词库。已实测选择“小何”后，再次双拼输入本地第 1 候选出现“小何”。纯首拼表词典与主词典独立，学习后的词首先在主词典的双拼/全拼输入中出现，不修改原有首拼词库。

配置 `insert_position: 2` 表示通常保留两个本地候选后插入，最多 5 条。默认 `promote_when_no_local: true`：当前段没有覆盖完整输入的本地词条时，云候选前移至首位。临时拼接 Sentence、未输入部分的 completion 和部分范围候选不算完整词条；原 userdb 学习词算本地词条。为了限制输入线程开销，只检查前 64 个本地候选是否存在完整词条；列表后面尚有单字或部分范围候选，不再阻止前移。这是当前候选窗口的有界判断，不是穷举整个词库。可关闭该选项恢复固定位置。与本地相同文本、相同范围的云项去重。comment 仅为 `☁`；上屏仅正文。特殊模式处理器排在云触发前，保留其 Tab 入口；Ctrl+Shift+C 主动查询不受 6 字母门槛限制。

## 自动触发配置（2026-10-03 更新）

```yaml
cloud_candidate:
  enabled: true
  trigger_key: Control+Shift+C
  auto_trigger: true
  auto_min_length: 6
  auto_debounce_ms: 200
  auto_min_interval_ms: 500
  insert_position: 2
  promote_when_no_local: true
  freeze_after_navigation: true
```

长度按当前未确认输入段的编码字母计数，不按展开后的全拼长度或候选汉字数计数；分隔符和首拼模式前缀不计入。5 个字母不自动查询；6 个及以上字母在输入改变后自动调度。快捷键在不足 6 字母时仍可主动触发，也能在自动结果已显示后再次触发。

输入更新仅标记待查询，复用 TSF 空闲 F34 通道读取稳定 composition 后提交原生异步请求。使用宿主提供的单调时钟 `cloud_clock_ms`，停止输入至少 200ms 才自动调度；每次继续输入重置等待，同一会话两次查询调度至少间隔 500ms（手动查询计入下一次自动等待，但自身不等待）。80ms 轮询会使实际触发稍晚于门槛，网络延迟另计；500ms 约束的是每会话查询调度，不是全局网络配额。持续快速输入不会每键请求，间歇输入可对新状态再次查询，缓存命中不联网。相同输入的菜单刷新和重复 F34 不重复请求。快捷键会接管当前待自动查询，避免同一轮重复提交。退格低于门槛、模式变化、提交、失焦仍会取消过期结果。自动触发关闭后，仅保留快捷键查询。默认开始上下移动、翻页或 Tab 导航时取消尚未完成的云更新，防止选词过程中候选序号跳动；快捷键可再主动发起查询。

本次改动位于 Rime Lua/源配置，已有上一阶段原生云功能 Windows 构建时只需重新部署配置。未修改 C++ 主程序；未在本机运行 Windows TSF 界面。

新增真实引擎验证：200ms 防抖边界、持续输入重置等待、500ms 间隔边界、无完整本地词时首位展示、学习后恢复通常位置、前移开关、超过 64 个本地候选时“兰香如故”前移且保留后续候选、选词导航取消迟到更新、5/6 字母边界、自动结果展示且注释精确等于 `☁`、自动完成后快捷键重查、旧自动响应丢弃、低于门槛取消、自动与手动合并、特殊模式/密码标记阻止自动查询、功能总开关和自动开关。

## 协议

参考并核对：[librime-cloud sougou.lua](https://github.com/hchunhui/librime-cloud/blob/master/scripts/lua/sougou.lua)、[rime-wenyun wenyun.lua](https://github.com/xing133/rime-wenyun/blob/main/lua/wenyun.lua)。原参考代码使用 HTTP；本次同一路径 HTTPS 实测可用。

Endpoint：

```text
https://shouji.sogou.com/web_ime/mobile.php?durtot=0&h=000000000000000&r=store_mf_wandoujia&v=3.7
```

POST，`Content-Type: application/octet-stream`，无需 cookie/私密 token。`h` 是参考实现中的公开固定占位值。

请求：一字节总长度、固定七字节 `00 05 00 00 00 00 01`、一字节拼音长度、无分隔 ASCII 全拼、前述所有字节 XOR 校验。长度上限 245 字符。

响应：候选数量为偏移 0x12 的 LE uint16；记录从 0x14 起，每条为长度前缀 UTF-16LE 文本、两个长度前缀元数据区和尾随一字节标记。第二元数据区的 LE uint16 边界序列用于检查完整消费范围。未知字段按 legacy 跳过并注明来源。拒绝截断、异常数量、非法 UTF-16/代理对/控制字符；只显示覆盖完整查询的候选。

## 实测

2026-10-03，本机真实 HTTPS POST，Python 标准 HTTP 客户端测量，尚非 Windows WinHTTP 实测。未携带用户上下文，只有下列公开拼音。

| input | latency | candidate list |
|---|---:|---|
| zhongguo | 110ms | 中国、中过、中果 |
| zhonghuarenmingongheguo | 114ms | 中华人民共和国 |
| beijing | 102ms | 北京、背景、倍镜 |
| xiaohe | 116ms | 小何、小河、小盒 |
| zhangyiming | 85ms | 张一鸣、张益铭、张一明 |
| zijietiaodong | 90ms | 字节跳动 |
| heishenhuawukong | 93ms | 黑神话悟空 |

服务有时只返回 1–3 条，5 是上限。实际小鹤方案“xiao he”的按键是 `xnhe`，不是题目示例的 `xnhc`；实现跟随 Rime 实际解析，不强行改映射。

## 验证与运行

已通过：

- macOS 可移植云核心 Debug/Release CMake build + CTest。
- 固定 byte fixture、请求校验、UTF-16、截断/无效响应、HTTP 500、timeout。
- 缓存命中、TTL、LRU 淘汰、Provider 隔离。
- 模拟 A 请求后提交 B，A 迟到丢弃；截止时间失败。
- 真实 librime 部署及 Lua：双拼/全拼/纯首拼、主动 Ctrl+Shift+C、候选第 3 位、纯正文上屏、userdb 学习、stale、所有列出的模式属性/密码标记、失败保留本地、功能关闭。
- 576 组快捷键转发、TSF 按键去重、原 cloud_pinyin 关闭回归、Tab 模式开关。
- clang-format **18.1.8** 对本次新增/修改的 C++ 文件 dry-run 检查。

**Windows 完整 Weasel Debug/Release build：本机无法执行，未验证。Windows WinHTTP 运行与真实 TSF UI 行为也未验证。新增 Windows CI 未执行，不能视为构建成功。**

```sh
cmake -S weasel/test/TestCloudCandidate -B /tmp/cloud-tests -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/cloud-tests
ctest --test-dir /tmp/cloud-tests --output-on-failure
python3 tests/cloud_native/run_engine.py
```

Windows 可使用同一 CMake 测试目录，分别 `--config Debug` / `Release` 构建并用 `ctest -C` 检查。MSBuild 与 xmake 已接入生产源文件/WinHTTP；安装匹配的 Windows Weasel 构建后，再用既有 Rime 部署流程部署源配置。不要手改 Rime/build。

## 已知风险

- 搜狗私有接口没有稳定性保证，参数和二进制格式可能改变；失败静默保留本地候选。
- 本实现只使用 HTTPS；没有 HTTP 明文降级。云端会收到自动门槛满足或快捷键主动触发的标准全拼；特殊模式仍禁止查询。
- TSF keyboard-disabled 路径会阻止常规禁用输入上下文，但**尚未把 IS_PASSWORD/InputScope/secure-input 状态显式传到服务端**，代码保留 TODO。`cloud_password_field` 标记的拦截已测，不能声称所有密码控件自动检测完成。Windows 验证应覆盖不同宿主控件。
- Rime userdb 学习和刷新依赖当前 librime-lua 的 Memory/DictEntry/Phrase 能力及现有 IPC 串行约束；后台不能调用这些接口。
- 两个本地候选后插入会移动后续候选序号；结果处理受 composition 身份约束，仍需 Windows 实际选词/翻页验收。

下一阶段仅建议：GoogleProvider、LLMProvider、上下文候选。均未在本轮实现。
