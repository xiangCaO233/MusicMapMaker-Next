-- MusicMapMaker-Next Lua 主题插件完整示例。
-- 将本文件复制到 <配置根目录>/plugins/themes/theme-example.lua，
-- 然后启动应用或点击“工具 -> 重载插件”。
return {
    -- 插件类型；当前主题插件必须固定为 "theme"。
    type = "theme",

    -- 持久化稳定 ID；只允许字母、数字、点、下划线和连字符，发布后不要修改。
    id = "example.twilight",

    -- 设置页显示名称；省略时使用 id。
    name = "Example Twilight",

    -- 内置基底主题 ID；未覆盖的变量和颜色从该主题继承。
    base = "DeepDark",

    -- ImGuiStyle 增量覆盖表；未使用的字段可以直接删除。
    style = {
        -- 全局不透明度，通常使用 0.0 到 1.0。
        Alpha = 1.0,
        -- 禁用控件在 Alpha 上继续相乘的透明度。
        DisabledAlpha = 0.60,
        -- 窗口内容区水平、垂直内边距。
        WindowPadding = { 8.0, 8.0 },
        -- 普通窗口可缩放到的最小宽度、高度。
        WindowMinSize = { 32.0, 32.0 },
        -- 窗口圆角半径；最终会被编辑器“窗口圆角”设置覆盖。
        WindowRounding = 8.0,
        -- 窗口边框宽度，常用 0.0 或 1.0。
        WindowBorderSize = 1.0,
        -- 鼠标命中可调整窗口边框区域时向内扩展的距离。
        WindowBorderHoverPadding = 4.0,
        -- 标题文字归一化对齐位置，{ 0.0, 0.5 } 表示左侧垂直居中。
        WindowTitleAlign = { 0.0, 0.5 },
        -- 标题栏折叠菜单按钮的位置：None、Left、Right、Up 或 Down。
        WindowMenuButtonPosition = "Left",

        -- 子窗口圆角半径；最终会跟随编辑器“窗口圆角”设置。
        ChildRounding = 8.0,
        -- 子窗口边框宽度。
        ChildBorderSize = 1.0,
        -- 弹出窗口圆角半径。
        PopupRounding = 6.0,
        -- 弹出窗口边框宽度。
        PopupBorderSize = 1.0,

        -- 输入框、滑条、按钮等框体的水平、垂直内边距。
        FramePadding = { 6.0, 4.0 },
        -- 框体圆角半径；最终会被编辑器“组件圆角”设置覆盖。
        FrameRounding = 6.0,
        -- 框体边框宽度。
        FrameBorderSize = 0.0,
        -- 相邻控件水平、垂直间距；最终会被编辑器“组件间距”设置覆盖。
        ItemSpacing = { 8.0, 6.0 },
        -- 一个组合控件内部元素的水平、垂直间距。
        ItemInnerSpacing = { 4.0, 4.0 },
        -- 表格单元格的水平、垂直内边距。
        CellPadding = { 4.0, 2.0 },
        -- 为触摸命中框额外扩展的水平、垂直距离。
        TouchExtraPadding = { 0.0, 0.0 },
        -- 树节点等层级内容的缩进宽度。
        IndentSpacing = 21.0,
        -- 旧 Columns API 两列之间的最小距离。
        ColumnsMinSpacing = 6.0,

        -- 滚动条主轴宽度。
        ScrollbarSize = 14.0,
        -- 滚动条背景圆角半径。
        ScrollbarRounding = 9.0,
        -- 滚动条与窗口边缘之间的内边距。
        ScrollbarPadding = 2.0,
        -- 滑条抓手和滚动条抓手的最小长度。
        GrabMinSize = 10.0,
        -- 抓手圆角半径。
        GrabRounding = 4.0,
        -- 对数滑条中心零点附近的死区宽度。
        LogSliderDeadzone = 4.0,

        -- 通过 ImGui 图片控件绘制图像时的圆角半径。
        ImageRounding = 0.0,
        -- 图片控件边框宽度。
        ImageBorderSize = 0.0,

        -- 标签页圆角半径。
        TabRounding = 6.0,
        -- 标签页轮廓边框宽度。
        TabBorderSize = 0.0,
        -- 标签页在缩宽前使用的最小基础宽度。
        TabMinWidthBase = 20.0,
        -- 标签栏空间不足时标签可缩小到的最小宽度。
        TabMinWidthShrink = 8.0,
        -- 已选中标签显示关闭按钮所需的最小宽度；负值可禁用。
        TabCloseButtonMinWidthSelected = 0.0,
        -- 未选中标签显示关闭按钮所需的最小宽度；负值可禁用。
        TabCloseButtonMinWidthUnselected = 0.0,
        -- 标签栏与内容之间的分隔线宽度。
        TabBarBorderSize = 1.0,
        -- 已选中标签顶部强调线宽度。
        TabBarOverlineSize = 2.0,

        -- 表格斜表头角度，单位为弧度。
        TableAngledHeadersAngle = 0.61,
        -- 表格斜表头文字的归一化对齐位置。
        TableAngledHeadersTextAlign = { 0.5, 0.0 },

        -- 树形层级连接线宽度。
        TreeLinesSize = 1.0,
        -- 树形层级连接线圆角半径。
        TreeLinesRounding = 0.0,
        -- 菜单项高亮背景圆角半径。
        MenuItemRounding = 4.0,
        -- Selectable 高亮背景圆角半径。
        SelectableRounding = 4.0,

        -- 颜色编辑器预览按钮的位置。
        ColorButtonPosition = "Right",
        -- 按钮文字的归一化对齐位置。
        ButtonTextAlign = { 0.5, 0.5 },
        -- Selectable 文字的归一化对齐位置。
        SelectableTextAlign = { 0.0, 0.0 },
        -- 带标题分隔区域文字的归一化对齐位置。
        SeparatorTextAlign = { 0.0, 0.5 },
        -- 带标题分隔区域文字的水平、垂直内边距。
        SeparatorTextPadding = { 20.0, 3.0 },
        -- 带标题分隔区域的边框宽度。
        SeparatorTextBorderSize = 3.0,
        -- 普通分隔线宽度。
        SeparatorSize = 1.0,

        -- 拖放目标高亮框圆角半径。
        DragDropTargetRounding = 0.0,
        -- 拖放目标高亮框线宽。
        DragDropTargetBorderSize = 2.0,
        -- 拖放目标高亮框相对目标边界的内缩距离。
        DragDropTargetPadding = 3.5,
        -- 颜色编辑器标记的尺寸。
        ColorMarkerSize = 4.0,
        -- 文本输入光标宽度。
        InputTextCursorSize = 1.0,
        -- Docking 节点是否显示关闭按钮。
        DockingNodeHasCloseButton = true,
        -- Docking 分隔条宽度。
        DockingSeparatorSize = 2.0,

        -- ImGui 软件鼠标光标缩放倍率。
        MouseCursorScale = 1.0,
        -- 贝塞尔曲线细分容差；越小越平滑、顶点越多。
        CurveTessellationTol = 1.25,
        -- 圆形自动细分最大误差；越小越平滑、顶点越多。
        CircleTessellationMaxError = 0.30,
        -- 是否抗锯齿绘制折线和边框。
        AntiAliasedLines = true,
        -- 是否优先使用纹理方式抗锯齿绘制线条。
        AntiAliasedLinesUseTex = true,
        -- 是否抗锯齿绘制填充图形边缘。
        AntiAliasedFill = true,

        -- 窗口保持在可见显示区域内的安全边距。
        DisplayWindowPadding = { 19.0, 19.0 },
        -- 电视等非矩形安全显示区使用的额外边距。
        DisplaySafeAreaPadding = { 3.0, 3.0 },

        -- 颜色键严格使用 ImGui 样式颜色名；RGBA 分量范围均为 0.0 到 1.0。
        Colors = {
            -- 普通文字颜色。
            Text = { 0.92, 0.94, 1.00, 1.00 },
            -- 禁用文字颜色。
            TextDisabled = { 0.48, 0.50, 0.58, 1.00 },
            -- 主窗口背景颜色。
            WindowBg = { 0.055, 0.060, 0.090, 1.00 },
            -- 子窗口背景颜色；透明时显示父级背景。
            ChildBg = { 0.055, 0.060, 0.090, 0.00 },
            -- 弹出窗口和下拉菜单背景颜色。
            PopupBg = { 0.080, 0.085, 0.120, 0.98 },
            -- 控件和窗口边框颜色。
            Border = { 0.26, 0.28, 0.38, 0.55 },
            -- 边框阴影颜色。
            BorderShadow = { 0.00, 0.00, 0.00, 0.00 },
            -- 输入框、滑条等框体默认背景。
            FrameBg = { 0.12, 0.13, 0.19, 1.00 },
            -- 框体悬浮背景。
            FrameBgHovered = { 0.20, 0.22, 0.34, 1.00 },
            -- 框体按下或编辑中的背景。
            FrameBgActive = { 0.25, 0.27, 0.42, 1.00 },
            -- 非活动窗口标题栏背景。
            TitleBg = { 0.055, 0.060, 0.090, 1.00 },
            -- 活动窗口标题栏背景。
            TitleBgActive = { 0.10, 0.11, 0.17, 1.00 },
            -- 折叠窗口标题栏背景。
            TitleBgCollapsed = { 0.055, 0.060, 0.090, 0.75 },
            -- 菜单栏背景。
            MenuBarBg = { 0.08, 0.09, 0.13, 1.00 },
            -- 滚动条槽背景。
            ScrollbarBg = { 0.04, 0.04, 0.06, 0.55 },
            -- 滚动条抓手默认颜色。
            ScrollbarGrab = { 0.25, 0.27, 0.36, 1.00 },
            -- 滚动条抓手悬浮颜色。
            ScrollbarGrabHovered = { 0.35, 0.38, 0.51, 1.00 },
            -- 滚动条抓手激活颜色。
            ScrollbarGrabActive = { 0.46, 0.49, 0.68, 1.00 },
            -- 复选框勾选标记颜色。
            CheckMark = { 0.67, 0.48, 1.00, 1.00 },
            -- 滑条抓手默认颜色。
            SliderGrab = { 0.60, 0.43, 0.92, 1.00 },
            -- 滑条抓手激活颜色。
            SliderGrabActive = { 0.76, 0.61, 1.00, 1.00 },
            -- 按钮默认背景。
            Button = { 0.20, 0.17, 0.31, 1.00 },
            -- 按钮悬浮背景。
            ButtonHovered = { 0.38, 0.28, 0.58, 1.00 },
            -- 按钮按下背景。
            ButtonActive = { 0.48, 0.34, 0.72, 1.00 },
            -- Header、TreeNode、Selectable 的默认背景。
            Header = { 0.22, 0.18, 0.34, 1.00 },
            -- Header、TreeNode、Selectable 的悬浮背景。
            HeaderHovered = { 0.40, 0.30, 0.62, 1.00 },
            -- Header、TreeNode、Selectable 的激活背景。
            HeaderActive = { 0.48, 0.36, 0.72, 1.00 },
            -- 普通分隔线颜色。
            Separator = { 0.25, 0.27, 0.36, 1.00 },
            -- 分隔线悬浮颜色。
            SeparatorHovered = { 0.45, 0.34, 0.70, 1.00 },
            -- 分隔线拖动中的颜色。
            SeparatorActive = { 0.58, 0.44, 0.88, 1.00 },
            -- 窗口缩放抓手默认颜色。
            ResizeGrip = { 0.38, 0.28, 0.58, 0.25 },
            -- 窗口缩放抓手悬浮颜色。
            ResizeGripHovered = { 0.48, 0.36, 0.72, 0.67 },
            -- 窗口缩放抓手激活颜色。
            ResizeGripActive = { 0.58, 0.44, 0.88, 0.95 },
            -- 输入框文本光标颜色。
            InputTextCursor = { 0.90, 0.84, 1.00, 1.00 },
            -- 标签页悬浮背景。
            TabHovered = { 0.40, 0.30, 0.62, 1.00 },
            -- 普通标签页背景。
            Tab = { 0.12, 0.11, 0.18, 1.00 },
            -- 已选中标签页背景。
            TabSelected = { 0.24, 0.19, 0.36, 1.00 },
            -- 已选中标签页顶部强调线颜色。
            TabSelectedOverline = { 0.70, 0.51, 1.00, 1.00 },
            -- 非活动 Docking 区域标签页背景。
            TabDimmed = { 0.08, 0.08, 0.12, 1.00 },
            -- 非活动 Docking 区域已选中标签页背景。
            TabDimmedSelected = { 0.16, 0.14, 0.23, 1.00 },
            -- 非活动 Docking 区域已选中标签页强调线颜色。
            TabDimmedSelectedOverline = { 0.48, 0.36, 0.72, 1.00 },
            -- Docking 目标预览颜色。
            DockingPreview = { 0.67, 0.48, 1.00, 0.70 },
            -- 空 Docking 节点背景颜色。
            DockingEmptyBg = { 0.04, 0.04, 0.06, 1.00 },
            -- 折线图默认颜色。
            PlotLines = { 0.64, 0.66, 0.72, 1.00 },
            -- 折线图悬浮颜色。
            PlotLinesHovered = { 1.00, 0.43, 0.35, 1.00 },
            -- 柱状图默认颜色。
            PlotHistogram = { 0.90, 0.70, 0.20, 1.00 },
            -- 柱状图悬浮颜色。
            PlotHistogramHovered = { 1.00, 0.60, 0.20, 1.00 },
            -- 表格表头背景。
            TableHeaderBg = { 0.14, 0.15, 0.21, 1.00 },
            -- 表格外框和表头分隔线颜色。
            TableBorderStrong = { 0.29, 0.31, 0.42, 1.00 },
            -- 表格内部分隔线颜色。
            TableBorderLight = { 0.21, 0.23, 0.31, 1.00 },
            -- 表格普通行背景；Alpha 为 0 时使用窗口背景。
            TableRowBg = { 0.00, 0.00, 0.00, 0.00 },
            -- 表格交替行背景。
            TableRowBgAlt = { 1.00, 1.00, 1.00, 0.035 },
            -- 可点击文本链接颜色。
            TextLink = { 0.55, 0.72, 1.00, 1.00 },
            -- 文本选区背景。
            TextSelectedBg = { 0.49, 0.35, 0.75, 0.45 },
            -- 树形层级连接线颜色。
            TreeLines = { 0.32, 0.34, 0.46, 1.00 },
            -- 拖放目标边框颜色。
            DragDropTarget = { 0.95, 0.80, 0.25, 1.00 },
            -- 拖放目标背景颜色。
            DragDropTargetBg = { 0.95, 0.80, 0.25, 0.18 },
            -- 未保存文档标记颜色。
            UnsavedMarker = { 0.95, 0.70, 0.20, 1.00 },
            -- 键盘或手柄导航光标颜色。
            NavCursor = { 0.72, 0.54, 1.00, 1.00 },
            -- 导航窗口切换时的高亮颜色。
            NavWindowingHighlight = { 1.00, 1.00, 1.00, 0.70 },
            -- 导航窗口切换时其他窗口的遮罩颜色。
            NavWindowingDimBg = { 0.05, 0.05, 0.08, 0.45 },
            -- 模态窗口后方的遮罩颜色。
            ModalWindowDimBg = { 0.05, 0.05, 0.08, 0.55 },
        },
    },
}
