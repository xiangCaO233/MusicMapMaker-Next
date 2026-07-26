# Lua 自定义主题插件接口

## 最小插件

每个入口脚本必须返回一个 Lua 表。`type` 必须为 `"theme"`：

```lua
return {
    type = "theme",
    id = "author.theme_name",
    name = "Theme Name",
    base = "DeepDark",
    style = {
        WindowRounding = 8.0,
        Colors = {
            Text = { 0.95, 0.95, 0.95, 1.0 },
            WindowBg = { r = 0.08, g = 0.09, b = 0.12, a = 1.0 },
        },
    },
}
```

顶层字段：

| 字段 | 类型 | 必需 | 说明 |
| --- | --- | --- | --- |
| `type` | string | 是 | 当前固定为 `"theme"`。 |
| `id` | string | 是 | 持久化稳定 ID，长度 1 到 128，只允许 ASCII 字母、数字、`.`、`_`、`-`；不能为 `Auto`，也不能与内置或其他插件主题重复。发布后不应修改。 |
| `name` | string | 否 | 设置页中的显示名称；省略或为空时使用 `id`。 |
| `base` | string | 否 | 继承的内置主题 ID，默认 `DeepDark`。当前不允许继承插件主题。 |
| `style` | table | 否 | 对基底主题的增量覆盖；省略时完整使用基底样式。未知字段会使该主题加载失败。 |

可用的内置主题 ID：

`DeepDark`、`Dark`、`Light`、`Classic`、`Microsoft`、`Darcula`、
`Photoshop`、`Unreal`、`Gold`、`RoundedVisualStudio`、`SonicRiders`、
`DarkRuda`、`SoftCherry`、`Enemymouse`、`DiscordDark`、`Comfy`、
`PurpleComfy`、`FutureDark`、`CleanDark`、`Moonlight`、`Cecilia`、
`ComfortableLight`、`HazyDark`、`Everforest`、`Windark`、`Rest`、
`ComfortableDarkCyan`、`KazamCherry`。

## 一个文件定义多个主题

使用 `themes` 数组时，`type` 仍放在插件顶层，每个数组元素使用单主题定义的
`id`、`name`、`base` 和 `style`：

```lua
return {
    type = "theme",
    themes = {
        {
            id = "author.ocean_dark",
            name = "Ocean Dark",
            base = "DeepDark",
            style = { Colors = { WindowBg = { 0.04, 0.08, 0.12, 1.0 } } },
        },
        {
            id = "author.ocean_light",
            name = "Ocean Light",
            base = "Light",
            style = { Colors = { WindowBg = { 0.90, 0.96, 1.00, 1.0 } } },
        },
    },
}
```

## `style` 字段

所有数值必须是有限数值。二维向量既可写为 `{ x, y }`，也可写为
`{ x = x, y = y }`。大多数尺寸单位是未缩放的 ImGui 像素，应用主题后仍会
叠加编辑器的 UI 缩放和审美设置；其中 `WindowRounding`、`ChildRounding`、
`FrameRounding`、`ItemSpacing`、`WindowPadding` 会被编辑器对应设置覆盖。

### 浮点字段

| 字段 | 含义 |
| --- | --- |
| `Alpha` | 全局不透明度，通常为 `0.0` 到 `1.0`。 |
| `DisabledAlpha` | 禁用控件在全局 Alpha 上继续相乘的透明度。 |
| `WindowRounding` | 窗口圆角半径。 |
| `WindowBorderSize` | 窗口边框宽度，常用 `0.0` 或 `1.0`。 |
| `WindowBorderHoverPadding` | 鼠标命中可调整窗口边框区域时向内扩展的距离。 |
| `ChildRounding` | 子窗口圆角半径。 |
| `ChildBorderSize` | 子窗口边框宽度。 |
| `PopupRounding` | 弹出窗口圆角半径。 |
| `PopupBorderSize` | 弹出窗口边框宽度。 |
| `FrameRounding` | 输入框、滑条、按钮等框体圆角半径。 |
| `FrameBorderSize` | 框体边框宽度。 |
| `IndentSpacing` | 树节点等层级内容的缩进宽度。 |
| `ColumnsMinSpacing` | 旧 Columns API 两列之间的最小距离。 |
| `ScrollbarSize` | 滚动条主轴宽度。 |
| `ScrollbarRounding` | 滚动条背景圆角半径。 |
| `ScrollbarPadding` | 滚动条与窗口边缘之间的内边距。 |
| `GrabMinSize` | 滑条抓手和滚动条抓手的最小长度。 |
| `GrabRounding` | 抓手圆角半径。 |
| `LogSliderDeadzone` | 对数滑条中心零点附近的死区宽度。 |
| `ImageRounding` | 通过 ImGui 图片控件绘制图像时的圆角半径。 |
| `ImageBorderSize` | 图片控件边框宽度。 |
| `TabRounding` | 标签页圆角半径。 |
| `TabBorderSize` | 标签页轮廓边框宽度。 |
| `TabMinWidthBase` | 标签页在缩宽前使用的最小基础宽度。 |
| `TabMinWidthShrink` | 标签栏空间不足时标签可缩小到的最小宽度。 |
| `TabCloseButtonMinWidthSelected` | 已选中标签显示关闭按钮所需的最小宽度；负值可禁用。 |
| `TabCloseButtonMinWidthUnselected` | 未选中标签显示关闭按钮所需的最小宽度；负值可禁用。 |
| `TabBarBorderSize` | 标签栏与内容之间的分隔线宽度。 |
| `TabBarOverlineSize` | 已选中标签顶部强调线宽度。 |
| `TableAngledHeadersAngle` | 表格斜表头角度，单位为弧度。 |
| `TreeLinesSize` | 树形层级连接线宽度。 |
| `TreeLinesRounding` | 树形层级连接线圆角半径。 |
| `MenuItemRounding` | 菜单项高亮背景圆角半径。 |
| `SelectableRounding` | Selectable 高亮背景圆角半径。 |
| `DragDropTargetRounding` | 拖放目标高亮框圆角半径。 |
| `DragDropTargetBorderSize` | 拖放目标高亮框线宽。 |
| `DragDropTargetPadding` | 拖放目标高亮框相对目标边界的内缩距离。 |
| `ColorMarkerSize` | 颜色编辑器标记的尺寸。 |
| `InputTextCursorSize` | 文本输入光标宽度。 |
| `SeparatorSize` | 分隔线宽度。 |
| `SeparatorTextBorderSize` | 带标题分隔区域的边框宽度。 |
| `DockingSeparatorSize` | Docking 分隔条宽度。 |
| `MouseCursorScale` | ImGui 软件鼠标光标缩放倍率。 |
| `CurveTessellationTol` | 贝塞尔曲线细分容差；越小越平滑、顶点越多。 |
| `CircleTessellationMaxError` | 圆形自动细分允许的最大误差；越小越平滑、顶点越多。 |

### 二维向量字段

| 字段 | 含义 |
| --- | --- |
| `WindowPadding` | 窗口内容区水平、垂直内边距。 |
| `WindowMinSize` | 普通窗口可调整到的最小宽度、高度。 |
| `WindowTitleAlign` | 标题文字对齐，`{0,0}` 为左上，`{0.5,0.5}` 为居中。 |
| `FramePadding` | 框体内容的水平、垂直内边距。 |
| `ItemSpacing` | 相邻控件的水平、垂直间距。 |
| `ItemInnerSpacing` | 一个组合控件内部元素的水平、垂直间距。 |
| `CellPadding` | 表格单元格的水平、垂直内边距。 |
| `TouchExtraPadding` | 为触摸命中框额外扩展的水平、垂直距离。 |
| `TableAngledHeadersTextAlign` | 表格斜表头文字的归一化对齐位置。 |
| `ButtonTextAlign` | 按钮文字的归一化对齐位置。 |
| `SelectableTextAlign` | Selectable 文字的归一化对齐位置。 |
| `SeparatorTextAlign` | 带标题分隔区域文字的归一化对齐位置。 |
| `SeparatorTextPadding` | 带标题分隔区域文字的水平、垂直内边距。 |
| `DisplayWindowPadding` | 窗口保持在可见显示区域内的安全边距。 |
| `DisplaySafeAreaPadding` | 电视等非矩形安全显示区使用的额外边距。 |

### 布尔字段

| 字段 | 含义 |
| --- | --- |
| `DockingNodeHasCloseButton` | Docking 节点是否显示关闭按钮。 |
| `AntiAliasedLines` | 是否抗锯齿绘制折线和边框。 |
| `AntiAliasedLinesUseTex` | 是否优先使用纹理方式抗锯齿绘制线条。 |
| `AntiAliasedFill` | 是否抗锯齿绘制填充图形边缘。 |

### 方向字段

方向字段的值必须为 `"None"`、`"Left"`、`"Right"`、`"Up"` 或
`"Down"`。

| 字段 | 含义 |
| --- | --- |
| `WindowMenuButtonPosition` | 窗口标题栏折叠菜单按钮的位置。 |
| `ColorButtonPosition` | 颜色编辑器预览按钮的位置。 |

## `Colors` 字段

颜色值必须包含四个 `0.0` 到 `1.0` 的有限 RGBA 分量，可写为
`{ r, g, b, a }` 或 `{ r = r, g = g, b = b, a = a }`。键名严格使用
当前 Dear ImGui 的样式颜色名：

`Text`、`TextDisabled`、`WindowBg`、`ChildBg`、`PopupBg`、`Border`、
`BorderShadow`、`FrameBg`、`FrameBgHovered`、`FrameBgActive`、
`TitleBg`、`TitleBgActive`、`TitleBgCollapsed`、`MenuBarBg`、
`ScrollbarBg`、`ScrollbarGrab`、`ScrollbarGrabHovered`、
`ScrollbarGrabActive`、`CheckMark`、`SliderGrab`、`SliderGrabActive`、
`Button`、`ButtonHovered`、`ButtonActive`、`Header`、`HeaderHovered`、
`HeaderActive`、`Separator`、`SeparatorHovered`、`SeparatorActive`、
`ResizeGrip`、`ResizeGripHovered`、`ResizeGripActive`、`InputTextCursor`、
`TabHovered`、`Tab`、`TabSelected`、`TabSelectedOverline`、
`TabDimmed`、`TabDimmedSelected`、`TabDimmedSelectedOverline`、
`DockingPreview`、`DockingEmptyBg`、`PlotLines`、`PlotLinesHovered`、
`PlotHistogram`、`PlotHistogramHovered`、`TableHeaderBg`、
`TableBorderStrong`、`TableBorderLight`、`TableRowBg`、`TableRowBgAlt`、
`TextLink`、`TextSelectedBg`、`TreeLines`、`DragDropTarget`、
`DragDropTargetBg`、`UnsavedMarker`、`NavCursor`、`NavWindowingHighlight`、
`NavWindowingDimBg`、`ModalWindowDimBg`。

未写入的颜色和样式字段沿用 `base` 内置主题。完整逐项注释示例见
[`examples/theme-example.lua`](examples/theme-example.lua)。

## 错误处理

以下情况会拒绝对应主题：

- 脚本语法或执行错误；
- 入口没有返回表，或 `type` 不是 `"theme"`；
- 缺少 `id`、ID 非法或重复；
- `base` 不是已注册的内置主题；
- `style`、`themes` 或颜色值类型错误；
- 使用未知 `ImGuiStyle` 或颜色字段。

一个文件的 `themes` 数组中，单个主题失败不会阻止同文件中后续主题继续校验。
修复文件后点击“工具 → 重载插件”即可重新载入，不需要重启应用。
