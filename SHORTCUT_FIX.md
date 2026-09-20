# Ctrl+Shift+F：版本对比、已修复回归与未决问题

## 2026-09-20 处理结果

用户反馈上一版没有解决应用快捷键冲突，并导致 EmEditor 普通输入重复。
已撤回 `c8c2083` 中的 TSF 按键缓存和保留快捷键改动，恢复该提交之前的前端按键处理。该提交中独立的候选窗口通知修复予以保留。

原因：上一版要求测试回调与正式回调的 `wParam/lParam` 完全相同，才判定按键已经处理。如果宿主对同一次按键提供不同的 `lParam`，就会再次调用 `_ProcessKeyEvent`，将同一编码重复发送给 Rime。原测试只检查缓存是否严格匹配，未覆盖这种宿主回调差异。

## 验证范围

`test/test_tsf_key_delivery.py` 提取实际 `OnTestKeyDown/OnKeyDown/OnTestKeyUp/OnKeyUp` 代码，用记录发送次数的 IPC 替身编译测试：

- 撤回前：同一按键的回调参数不同，会发送两次；模拟四个字母的按下和抬起时，发送 16 次，而不是 8 次。
- 撤回后：六组测试均通过，包括不同回调参数、重复测试回调、抬键、连续输入、没有测试回调和真正的长按重复。

这不是 Windows 或 EmEditor 实机验证。当前环境无法编译 Windows DLL，也无法确认所有应用的快捷键处理顺序。

## 功能范围

保留 Rime 层已有的 Ctrl+Shift+F 处理：收到按键且正在中文组字时切换简繁，空闲或英文模式放行。前端恢复普通 TSF 按键路径，没有启用 PreserveKey，也没有新增全局快捷键。

## 与用户测试正常的 0.17.4 对比

用户确认：官方 **0.17.4** 在相同配置、相同软件和输入状态下正常。这使“应用快捷键优先级导致、无法解决”的先前判断失去依据；最初的快捷键问题仍需定位，不能当作已修复。

- 官方 [0.17.4 标签](https://github.com/rime/weasel/tree/0.17.4) 对应 `9cc96e20dc71b80876b12f689bb5863c76c2a7ed`。
- 本分支基于较新的官方 `d73f6295e8252ed2f7b9c12bae32e9001b1afdaa`。比较范围必须包括 0.17.4 之后的官方改动，不能只检查动态候选功能提交。
- 撤回实验后，`KeyEvent.cpp` 与 0.17.4 无差异；`KeyEventSink.cpp` 的差异只剩 COM 方法声明宏和空白，普通按键处理逻辑一致。
- 发布后仍存在 TSF 组字生命周期、IPC 等差异。它们尚未通过 Windows 实测排除，也没有证据指向某一个提交。

新增 `test/test_shortcut_forwarding.py`，提取实际服务端 `ProcessKeyEvent`、导航重映射及默认导航配置，配合记录调用的 Rime API 替身测试。576 组组合覆盖横/竖排、导航开关、动态选词键包含 F/f、候选数量、大小写、CapsLock、按下/抬起及 Rime 接受/拒绝结果。均确认键值和修饰键正确、只发送一次、没有误选候选，返回 Rime 的处理结果。这只排除了这些输入条件下的服务端分发错误，不覆盖真实 IPC、Windows TSF、宿主软件或候选窗口的副作用。

真实 librime 回归也通过：输入中切换一次并消费按下/抬起，空闲和英文模式放行。测试环境是 macOS 上的 librime，不能替代 Windows DLL 实机验证。

## 应用撤回版本

如果已安装上一版修改后的 DLL，需要在 Windows 按 `INSTALL.md` 的环境要求重新运行 `build.bat weasel` 并安装，或恢复修改前的安装包。完全退出并重新打开 Word / EmEditor，确保旧 DLL 已卸载。只重新部署 Rime 配置无法撤回已安装的前端 DLL。

在项目最外层运行回归测试：

```sh
python3 weasel/test/test_tsf_key_delivery.py
python3 weasel/test/test_shortcut_forwarding.py
python3 Rime/tests/test_tab_modes.py
```
