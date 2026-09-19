# Ctrl+Shift+F 简繁切换修复

本次修改同时涉及 Rime 配置和小狼毫 TSF 前端。

- `../Rime/lua/transcription_hotkey.lua` 在中文输入过程中处理 Ctrl+Shift+F，忽略 Caps Lock 对字母大小写的影响，消费按下和对应抬键，长按只切换一次。空闲和英文模式仍交给应用处理。
- `WeaselTSF/KeyEventSink.cpp` 注册并注销 TSF 保留快捷键，将回调送到同一个 Rime 处理器，刷新候选窗口。测试按键缓存按键值和事件参数匹配，防止宿主省略某次正式回调后误吞后续按键。

## 安装

1. 将更新后的 Rime 配置（包括新增 Lua 文件）放到实际使用的用户目录，重新部署。
2. 按 `INSTALL.md` 配置 Windows Visual Studio 编译环境，运行 `build.bat weasel`，通过生成的安装文件更新小狼毫。需要更新应用使用的各架构 TSF DLL；只更新配置不会改变前端的快捷键路由。
3. 重新启动 Word、EmEditor，确保应用加载新的 DLL。

## 验证

已在 macOS 上用真实 librime 测试大小写、Caps Lock、重复按下、修饰键先松开、候选简繁刷新和原有模式；已编译运行独立 C++ 按键缓存测试。Windows DLL 尚未在此环境编译，Word / EmEditor 实测仍需在 Windows 完成。

两款应用分别检查：

1. 输入 `uiuia`，按 Ctrl+Shift+F，候选中的「事实」变为「事實」，应用自身的快捷键命令不触发。
2. 松开后再按一次，恢复简体；长按 F 不反复切换。
3. 开启 Caps Lock 后重复；再检查先松开 Ctrl/Shift、后松开 F。
4. 取消输入或切到英文模式后，应用快捷键仍正常。

本地测试命令：

```sh
python3 Rime/tests/test_tab_modes.py  # 在项目最外层运行
c++ -std=c++17 -Iweasel/include weasel/test/TestKeyEventCache/TestKeyEventCache.cpp -o /tmp/key-event-cache-test
/tmp/key-event-cache-test
```
