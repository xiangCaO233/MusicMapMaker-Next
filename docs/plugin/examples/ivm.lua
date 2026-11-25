-- 内置 IVM（IMD Visual Maker）主题的 Lua 插件接口移植示例。
-- 设计目标：浅灰 Windows 界面、白色编辑区域、细直角边框和紧凑控件，
-- 并用原软件画布中的荧光绿、青色与淡紫色作为交互强调色。
--
-- 为了最接近原图，建议在“设置 -> 软件 -> UI 审美”中同时设置：
--   窗口圆角 = 0
--   组件圆角 = 0
-- 这些项目级审美参数会在主题应用后覆盖 Lua 中的对应圆角字段。
return {
    type = "theme",
    id = "example.ivm",
    name = "IVM（Lua 示例）",
    -- 基底先由宿主应用，随后覆盖 style 中出现的字段。
    -- 新增字段可以先使用基底默认值，不需要复制完整的主题表。
    -- id 用来识别主题，改显示名称不应随手调整该标识。
    base = "Light",

    style = {
        Alpha = 1.0,
        -- 保持全局不透明度，禁用控件再独立降低透明度。
        -- 这样会叠加控件自身颜色，避免整份窗口层级被禁用状态改变。
        DisabledAlpha = 0.55,

        -- 经典桌面工具软件使用直角、单像素边框和紧凑留白。
        WindowPadding = { 7.0, 6.0 },
        WindowMinSize = { 32.0, 32.0 },
        WindowRounding = 0.0,
        WindowBorderSize = 1.0,
        -- 直边宽度与缩放命中范围是两套参数。窄边不代表命中带也只有一个像素，
        -- 拖窗口边缘时仍需要比视觉边线更宽的有效区域。
        WindowBorderHoverPadding = 3.0,
        WindowTitleAlign = { 0.0, 0.5 },
        WindowMenuButtonPosition = "Left",
        ChildRounding = 0.0,
        ChildBorderSize = 1.0,
        PopupRounding = 0.0,
        PopupBorderSize = 1.0,

        -- 向量值使用水平、垂直两个元素。字段全体按同一逻辑尺寸定义，
        -- 修改字号后优先重新核对垂直留白，而非在示例中把所有像素同时放大。
        FramePadding = { 6.0, 3.0 },
        FrameRounding = 0.0,
        FrameBorderSize = 1.0,
        ItemSpacing = { 7.0, 5.0 },
        ItemInnerSpacing = { 5.0, 4.0 },
        CellPadding = { 5.0, 3.0 },
        IndentSpacing = 18.0,

        -- 滑块最小尺寸影响短列表和长表格的拖拽可读性。
        -- 滚动条与其他抓取件分别调整，避免将滑块留白误写成窗口边距。
        ScrollbarSize = 16.0,
        ScrollbarRounding = 0.0,
        ScrollbarPadding = 1.0,
        GrabMinSize = 12.0,
        GrabRounding = 0.0,

        -- 标签顶部描边显示活动标签身份，与窗口圆角无关。
        -- 停靠组中的背景、选中底色、顶线分别控制，避免仅靠高度区别焦点。
        TabRounding = 0.0,
        TabBorderSize = 1.0,
        TabBarBorderSize = 1.0,
        TabBarOverlineSize = 2.0,
        MenuItemRounding = 0.0,
        SelectableRounding = 0.0,
        ButtonTextAlign = { 0.5, 0.5 },
        SelectableTextAlign = { 0.0, 0.0 },
        SeparatorSize = 1.0,
        SeparatorTextBorderSize = 1.0,
        DockingSeparatorSize = 1.0,

        -- 窗口外框虽然使用直角，线条与填充仍保留抗锯齿。
        -- 该组参数也影响画布斜线与角标，调整时应同时观察高 DPI 下的清晰度。
        AntiAliasedLines = true,
        AntiAliasedLinesUseTex = true,
        AntiAliasedFill = true,

        -- 颜色元素依次为红、绿、蓝、不透明度，全部使用 0 到 1 的分量。
        -- 半透明颜色会与控件所在背景叠加，调色时要在原使用场景检查对比。
        Colors = {
            -- 黑字、浅灰窗口和白色内容区构成原软件的主体观感。
            Text = { 0.05, 0.05, 0.05, 1.00 },
            TextDisabled = { 0.43, 0.43, 0.43, 1.00 },
            WindowBg = { 0.941, 0.941, 0.941, 1.00 },
            ChildBg = { 1.00, 1.00, 1.00, 1.00 },
            PopupBg = { 0.965, 0.965, 0.965, 1.00 },
            Border = { 0.60, 0.60, 0.60, 1.00 },
            BorderShadow = { 1.00, 1.00, 1.00, 0.55 },

            -- 输入框常态背景和悬浮、活动状态分别给值。
            -- 把三种状态设成同色会降低键盘焦点及拖动反馈的辨识度。
            FrameBg = { 1.00, 1.00, 1.00, 1.00 },
            FrameBgHovered = { 0.82, 0.97, 0.95, 1.00 },
            FrameBgActive = { 0.66, 0.92, 0.89, 1.00 },

            TitleBg = { 0.925, 0.949, 0.957, 1.00 },
            TitleBgActive = { 0.925, 0.949, 0.957, 1.00 },
            TitleBgCollapsed = { 0.90, 0.92, 0.93, 1.00 },
            MenuBarBg = { 0.949, 0.949, 0.949, 1.00 },

            ScrollbarBg = { 0.91, 0.91, 0.91, 1.00 },
            ScrollbarGrab = { 0.72, 0.72, 0.72, 1.00 },
            ScrollbarGrabHovered = { 0.61, 0.61, 0.61, 1.00 },
            ScrollbarGrabActive = { 0.50, 0.50, 0.50, 1.00 },

            -- 荧光绿来自原画布的物件与编号强调。
            CheckMark = { 0.00, 0.82, 0.12, 1.00 },
            SliderGrab = { 0.08, 0.75, 0.70, 1.00 },
            SliderGrabActive = { 0.00, 0.88, 0.16, 1.00 },

            -- 按钮空闲色靠近窗口，悬浮和按下才提升色相变化。
            -- 这一组不控制文本选择色，也不应替换复选框的勾选标识。
            Button = { 0.91, 0.91, 0.91, 1.00 },
            ButtonHovered = { 0.82, 0.97, 0.95, 1.00 },
            ButtonActive = { 0.67, 0.91, 0.88, 1.00 },

            -- 青色悬浮与淡紫选中呼应原软件轨道块配色。
            Header = { 0.91, 0.82, 0.92, 1.00 },
            HeaderHovered = { 0.77, 0.95, 0.93, 1.00 },
            HeaderActive = { 0.56, 0.88, 0.84, 1.00 },

            Separator = { 0.63, 0.63, 0.63, 1.00 },
            SeparatorHovered = { 0.08, 0.75, 0.70, 1.00 },
            SeparatorActive = { 0.00, 0.82, 0.12, 1.00 },
            -- 调节手柄只在悬浮或拖动时强化颜色；常态透明度保留边角的视觉留白。
            -- 与窗口边线一起调色时仍应独立检查活动态。
            ResizeGrip = { 0.55, 0.55, 0.55, 0.20 },
            ResizeGripHovered = { 0.08, 0.75, 0.70, 0.65 },
            ResizeGripActive = { 0.00, 0.82, 0.12, 0.90 },
            InputTextCursor = { 0.05, 0.05, 0.05, 1.00 },

            TabHovered = { 0.77, 0.95, 0.93, 1.00 },
            Tab = { 0.88, 0.88, 0.88, 1.00 },
            TabSelected = { 1.00, 1.00, 1.00, 1.00 },
            TabSelectedOverline = { 0.00, 0.82, 0.12, 1.00 },
            TabDimmed = { 0.86, 0.86, 0.86, 1.00 },
            TabDimmedSelected = { 0.92, 0.92, 0.92, 1.00 },
            TabDimmedSelectedOverline = { 0.55, 0.55, 0.55, 1.00 },

            -- 预览层铺在目标内容上，透明度应允许辨认目标。
            -- 空停靠区没有下层内容，单独给实色防止浅底下区域界限模糊。
            DockingPreview = { 0.08, 0.75, 0.70, 0.55 },
            DockingEmptyBg = { 0.86, 0.86, 0.86, 1.00 },

            -- 曲线和直方图使用独立色槽。悬浮态只标记当前数据，
            -- 不能以导航或窗口活动色替代常态的图表可读性。
            PlotLines = { 0.15, 0.15, 0.15, 1.00 },
            PlotLinesHovered = { 1.00, 0.10, 0.10, 1.00 },
            PlotHistogram = { 0.00, 0.82, 0.12, 1.00 },
            PlotHistogramHovered = { 0.08, 0.75, 0.70, 1.00 },

            TableHeaderBg = { 0.93, 0.93, 0.93, 1.00 },
            TableBorderStrong = { 0.58, 0.58, 0.58, 1.00 },
            TableBorderLight = { 0.76, 0.76, 0.76, 1.00 },
            TableRowBg = { 1.00, 1.00, 1.00, 1.00 },
            -- 备用行颜色叠加在常规白色内容区之上。
            -- 较浅的交替行仍应让选中状态及文本高亮保持可辨。
            TableRowBgAlt = { 0.90, 0.96, 0.96, 0.55 },

            -- 文字链接保留足够深的前景色；选择背景则采用部分透明，
            -- 避免选择区域中已有的语义色和正文失去清晰边界。
            TextLink = { 0.00, 0.40, 0.38, 1.00 },
            TextSelectedBg = { 0.08, 0.75, 0.70, 0.35 },
            TreeLines = { 0.62, 0.62, 0.62, 1.00 },
            -- 拖放轮廓指明接受区域，背景只给轻量填充。
            -- 轮廓不透明且填充较淡，使拖起资源时仍可看清下面的表格和轨道内容。
            -- 拖放反馈与键盘导航共用强调色，但导航还保留独立的窗口高亮槽。
            DragDropTarget = { 0.00, 0.82, 0.12, 1.00 },
            DragDropTargetBg = { 0.00, 0.82, 0.12, 0.16 },
            UnsavedMarker = { 1.00, 0.12, 0.12, 1.00 },
            NavCursor = { 0.00, 0.82, 0.12, 1.00 },
            NavWindowingHighlight = { 0.08, 0.75, 0.70, 0.70 },
            NavWindowingDimBg = { 0.80, 0.80, 0.80, 0.30 },
            -- 模态遮罩弱化背景工作区，不改变弹窗内控件颜色。
            -- 它与窗口切换遮罩分别配置，以保留两种状态的区别。
            ModalWindowDimBg = { 0.78, 0.78, 0.78, 0.55 },
        },
    },
}
