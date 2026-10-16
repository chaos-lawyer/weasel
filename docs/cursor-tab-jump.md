# 动态词库光标定位与 Tab 跳转

本次修复需在 Windows 重新构建并安装 Weasel；仅重新部署 Rime 配置不会更新前端代码。保留原来的 `style/cursor_tab_jump: true` 配置即可，不需要修改词库语法。

## 修复内容

- 在 `OnKeyDown` 执行 Tab 跳转，兼容一次或多次 `OnTestKeyDown`，也兼容直接调用 `OnKeyDown` 的应用。跳转成功后同时消费 Tab 的释放事件。
- 填空中普通文字上屏时保留剩余跳转目标。
- 把首次定位推迟到 `EndComposition` 之后。持久文本上下文使用 TSF 范围；临时文本上下文使用提交后的方向键定位，避免依赖提交后失效的范围。持久上下文拒绝首次选区设置时也尝试方向键方式。
- 方向键定位事件绕过 Rime 处理，并使用私有结束标记收尾；兼容 TSF 回调丢失 `dwExtraInfo` 的情况。
- Esc、手动移动光标、鼠标操作、切换上下文或窗口以及修改键快捷键退出跳转。拼音仍在候选状态时，Tab 交给原有方案处理；应先选词上屏再跳转。

Chromium 的提交路径会以 `kMoveCursorAfterText` 插入文本，且将上下文标记为 `TS_SS_TRANSITORY`，因此提交前设置选区、长期保存范围不能作为跨应用定位的共同前提。参考 [Chromium TSFTextStore 实现](https://github.com/chromium/chromium/blob/main/ui/base/ime/win/tsf_text_store.cc)。

## 本地回归

在项目根目录运行：

```sh
python3 tests/test_cursor_tab_jump.py
python3 tests/test_frontend_reopen.py
python3 tests/test_frontend_undo.py
```

测试编译实际 C++ 方法并在模拟文本存储与按键回调中运行，覆盖持久/临时上下文、中文填空、连续字段、未填字段跳过、重复测试回调、无测试回调、按键释放、选区拒绝、注入失败和焦点变化。它们不能代替 Windows/MSVC 构建及实际应用测试。

## Windows 验收

在 Word、记事本、Obsidian 编辑区、EmEditor、Chrome 普通文本输入框/多行输入框、微信聊天编辑框逐项测试：

| 词条示例 | 操作与预期 |
| --- | --- |
| `csdw=1,（本页无正文，为《$|》签署页）` | 上屏停在《》之间；输入并上屏“测试合同”；按 Tab 停在最后的“）”之后。 |
| `cstg=1,联系人：$|，电话：$|` | 上屏选中 `$1`；输入“张三”并上屏；Tab 选中 `$2`；输入数字（含小键盘输入）；Tab 到末尾。 |
| 同上 | 不输入内容直接按 Tab，依次跳过选中的标记，最后一次 Tab 折叠到末尾。 |
| 同上 | 填空过程中按 Esc、用鼠标重新定位、按方向键或切换窗口后，Tab 恢复应用原有行为。 |
| 普通无占位符词条 | 正常上屏到末尾，Tab 保持原有行为。 |

方向键兼容方式按模板中未改动的后缀计算距离，合并 UTF-16 代理对和 CRLF；组合附加符、ZWJ 表情序列、双向文本以及会自动改写模板后缀的编辑器插件，仍需单独实测。一次移动超过 4096 步、按住 Ctrl/Alt/Shift 选择模板、系统拒绝注入时会停止跳转。当前未在上述 Windows 应用中验证。
