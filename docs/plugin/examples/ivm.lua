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
    base = "Light",

    style = {
        Alpha = 1.0,
        DisabledAlpha = 0.55,

        -- 经典桌面工具软件使用直角、单像素边框和紧凑留白。
        WindowPadding = { 7.0, 6.0 },
        WindowMinSize = { 32.0, 32.0 },
        WindowRounding = 0.0,
        WindowBorderSize = 1.0,
        WindowBorderHoverPadding = 3.0,
        WindowTitleAlign = { 0.0, 0.5 },
        WindowMenuButtonPosition = "Left",
        ChildRounding = 0.0,
        ChildBorderSize = 1.0,
        PopupRounding = 0.0,
        PopupBorderSize = 1.0,

        FramePadding = { 6.0, 3.0 },
        FrameRounding = 0.0,
        FrameBorderSize = 1.0,
        ItemSpacing = { 7.0, 5.0 },
        ItemInnerSpacing = { 5.0, 4.0 },
        CellPadding = { 5.0, 3.0 },
        IndentSpacing = 18.0,

        ScrollbarSize = 16.0,
        ScrollbarRounding = 0.0,
        ScrollbarPadding = 1.0,
        GrabMinSize = 12.0,
        GrabRounding = 0.0,

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

        AntiAliasedLines = true,
        AntiAliasedLinesUseTex = true,
        AntiAliasedFill = true,

        Colors = {
            -- 黑字、浅灰窗口和白色内容区构成原软件的主体观感。
            Text = { 0.05, 0.05, 0.05, 1.00 },
            TextDisabled = { 0.43, 0.43, 0.43, 1.00 },
            WindowBg = { 0.941, 0.941, 0.941, 1.00 },
            ChildBg = { 1.00, 1.00, 1.00, 1.00 },
            PopupBg = { 0.965, 0.965, 0.965, 1.00 },
            Border = { 0.60, 0.60, 0.60, 1.00 },
            BorderShadow = { 1.00, 1.00, 1.00, 0.55 },

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

            DockingPreview = { 0.08, 0.75, 0.70, 0.55 },
            DockingEmptyBg = { 0.86, 0.86, 0.86, 1.00 },

            PlotLines = { 0.15, 0.15, 0.15, 1.00 },
            PlotLinesHovered = { 1.00, 0.10, 0.10, 1.00 },
            PlotHistogram = { 0.00, 0.82, 0.12, 1.00 },
            PlotHistogramHovered = { 0.08, 0.75, 0.70, 1.00 },

            TableHeaderBg = { 0.93, 0.93, 0.93, 1.00 },
            TableBorderStrong = { 0.58, 0.58, 0.58, 1.00 },
            TableBorderLight = { 0.76, 0.76, 0.76, 1.00 },
            TableRowBg = { 1.00, 1.00, 1.00, 1.00 },
            TableRowBgAlt = { 0.90, 0.96, 0.96, 0.55 },

            TextLink = { 0.00, 0.40, 0.38, 1.00 },
            TextSelectedBg = { 0.08, 0.75, 0.70, 0.35 },
            TreeLines = { 0.62, 0.62, 0.62, 1.00 },
            DragDropTarget = { 0.00, 0.82, 0.12, 1.00 },
            DragDropTargetBg = { 0.00, 0.82, 0.12, 0.16 },
            UnsavedMarker = { 1.00, 0.12, 0.12, 1.00 },
            NavCursor = { 0.00, 0.82, 0.12, 1.00 },
            NavWindowingHighlight = { 0.08, 0.75, 0.70, 0.70 },
            NavWindowingDimBg = { 0.80, 0.80, 0.80, 0.30 },
            ModalWindowDimBg = { 0.78, 0.78, 0.78, 0.55 },
        },
    },
}
