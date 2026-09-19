# Dynamic Candidate Layout（动态候选布局）使用文档

## 概述

Weasel 提供了通用的 **Dynamic Candidate Layout（动态候选布局）** 能力。在连续输入过程中，输入法可根据 Rime Context 中的 Option 状态、候选词长度、可见候选词数量等特征，自动在横排（Horizontal）与纵排（Vertical）布局之间无缝切换。

> **注意**：Dynamic Layout 是纯前端候选窗口排版与渲染布局功能，完全独立于 Rime 核心的候选词生成、过滤和排序逻辑。

---

## 配置方法

在 `weasel.custom.yaml` 或输入方案配置中开启动态布局：

```yaml
patch:
  style/layout/type: auto

  dynamic_layout:
    default: horizontal  # 未命中任何规则时的默认布局: horizontal | vertical

    rules:
      # 1. Option 规则：当特定 context option 为指定值时生效
      - option: vertical_layout
        value: true
        layout: vertical

      # 2. 候选词最长文本长度规则：当当前页候选词中最长 Unicode 字符数超过阈值时生效
      - candidate_max_text_length_gt: 20
        layout: vertical

      # 3. 候选词数量规则：当当前页实际可见候选词数量超过阈值时生效
      - candidate_count_gt: 6
        layout: vertical
```

### 规则执行逻辑

- **按需启用**：仅当 `style/layout/type: auto` 时启用动态布局；若配置为传统的 `horizontal: true/false` 或静态 `type: horizontal/vertical`，则保持原有静态布局，向下完全兼容。
- **First match wins（优先匹配原则）**：`rules` 列表按书写顺序自上而下匹配，首个命中的规则决定最终布局。
- **优雅回退**：若所有规则均未命中，或配置发生缺省，自动回退到 `dynamic_layout/default`（缺省默认为 `horizontal`）。

---

## 与 Lua 插件联动示例

Rime Lua 插件可以通过设置或清除 context option 来控制候选窗的横竖排形态：

```lua
-- 进入工具模式或长文本模式，切换为竖排展示
env.engine.context:set_option("vertical_layout", true)

-- 退出模式，恢复默认横排展示
env.engine.context:set_option("vertical_layout", false)
```

Weasel 在下一次候选词刷新时会自动根据 Option 状态调整候选窗排版，直接复用当前候选窗口，窗口无闪烁、无黑屏、不关闭重建、无需切换 Schema。

## 布局感知的候选导航

动态布局会按当前候选窗的实际排版调整方向键语义：

| 当前布局 | 上一个候选 | 下一个候选 | 上一页 | 下一页 |
| --- | --- | --- | --- | --- |
| 横排 | `Ctrl+Left` | `Ctrl+Right` | `Up` | `Down` |
| 竖排 | `Up` | `Down` | `Ctrl+Left` | `Ctrl+Right` |

该映射仅在候选菜单存在时生效；没有候选菜单时，`Ctrl+Left` 和 `Ctrl+Right` 仍交还应用程序处理。

可在 `dynamic_layout/navigation` 中启用并调整全部按键：

```yaml
dynamic_layout:
  navigation:
    enabled: true
    horizontal:
      previous_candidate: Control+Left
      next_candidate: Control+Right
      previous_page: Up
      next_page: Down
    vertical:
      previous_candidate: Up
      next_candidate: Down
      previous_page: Control+Left
      next_page: Control+Right
```

- `enabled: false` 或省略 `navigation` 时，完全保留 Rime 原有按键逻辑。
- 支持 `Control`（或 `Ctrl`）、`Shift`、`Alt`、`Super`（或 `Win`）修饰键。
- 支持 `Left`、`Right`、`Up`、`Down`、`Page_Up`、`Page_Down`、`Home`、`End`。
- 将某项设置为 `none` 可单独禁用该动作；无效按键名称会写入日志并禁用该项。
