-- RM(old)：从 RM旧面板船长 Stable 0512 的实际游玩资源迁移。
-- 原素材作者为 dreamcat、Acc@9961、Sam_0324、TEN-DcyC；迁移说明见同目录 README.md。
-- 外层游玩脚本是资源映射依据，嵌套编辑器皮肤不参与本皮肤。
local skin_dir = __SKINLUA_DIR__

-- @brief 解析 RM 自有资源，入口移动后仍从自身目录加载。
-- @param path 相对于 resources 的路径。
local function resource(path)
    return skin_dir .. "resources/" .. path
end

-- @brief 复用随软件分发的字体、界面音效和画布着色器。
-- @param path 相对于默认皮肤 resources 的路径。
-- 不执行其他皮肤的 Lua，避免修改全局目录或依赖系统路径编码。
local function common(path)
    return skin_dir .. "../mmm-default/resources/" .. path
end

-- @brief 包装复用的默认音效，允许保留原有提前量。
-- @param path 已解析的音频文件路径。
-- @param lead_in_ms 复用默认音效时的原始提前量，缺省为零。
local function audio(path, lead_in_ms)
    return { path = path, lead_in_ms = lead_in_ms or 0.0 }
end

-- 原图已包含蓝色与绿色，不再叠加默认皮肤的米黄乘色。
-- 若项目指定了独立配色方案，最终乘色仍服从项目设置。
-- 本皮肤不改写用户的项目偏好，恢复皮肤配色后可见完整原色。
-- 独立表仅作为只读颜色值共享，加载器会拷贝为配置值。
local white = { 1.0, 1.0, 1.0, 1.0 }
local gray = { 0.38, 0.43, 0.47, 0.50 }

-- @brief 两个音符画布使用同一组随软件维护的渲染程序。
-- 不复制 SPIR-V，避免着色器接口随软件升级后失配。
local function note_shaders()
    return {
        main = common("shader/canvas/Basic2DCanvas/main"),
        effect = common("shader/canvas/Basic2DCanvas/effect"),
    }
end

return {
    -- 显示名与文件夹名分离，设置界面显示精确的 RM。
    -- 版本号描述本次迁移，不冒充原游戏版本或官方发行版本。
    -- 原包序列帧按 30 FPS 播放，保持短促的蓝绿打击闪光。
    meta = {
        name = "RM(old)",
        author = "dreamcat, Acc@9961, Sam_0324, TEN-DcyC / MusicMapMaker-Next 迁移",
        version = "1.0",
        effectbasefps = 30,
    },

    -- 分拍线是编辑辅助，保留常用细分但降低其视觉权重。
    beat_divisors = { 1, 2, 3, 4, 6, 8, 12, 16 },
    colors = {
        -- 蓝色 Tap 和绿色长条头分别使用原图，不依赖换色模拟。
        note_tap = white,
        note_head = white,
        note_hold = white,
        note_end = white,
        note_node = white,
        note_flick_arrow = white,
        -- 预览的青色判定线呼应原版按键底部的青色装饰。
        preview = {
            boundingbox = { 0.18, 0.75, 0.86, 0.28 },
            hoverbox = { 0.42, 0.90, 1.0, 0.40 },
            judgeline = { 0.18, 0.90, 1.0, 1.0 },
            judgment_guide = {
                fill = { 0.12, 0.66, 0.76, 0.22 },
                border = { 0.28, 0.90, 1.0, 0.90 },
            },
        },
        -- 拍头保持可辨识，其余分拍线统一灰色以免干扰原图颜色。
        beat_lines = { beat_1 = { 0.72, 0.80, 0.86, 0.75 }, default = gray },
        -- 草稿使用半透明乘色，与正式音符区分，同时保留纹理细节。
        draft_notes = {
            note_tap = { 0.85, 0.65, 0.65, 0.65 },
            note_head = { 1.0, 0.65, 0.65, 0.65 },
            note_hold = { 1.0, 0.65, 0.65, 0.55 },
            note_end = { 1.0, 0.65, 0.65, 0.65 },
            note_node = { 1.0, 0.65, 0.65, 0.65 },
            note_flick_arrow = { 1.0, 0.65, 0.65, 0.65 },
        },
    },
    -- 深色轨道上只需要细辅助线；悬浮光效沿用软件现有管线。
    -- 拍线宽度按画布像素配置，与图片分辨率及原包轨道数无关。
    values = {
        beat_lines_width = { beat_1 = 1.5, default = 1.0 },
        glow = { resolution_scale = 0.5 },
    },
    -- 游戏资源不提供桌面主题，使用现有深色主题搭配深色轨道。
    theme = { light = "Moonlight", dark = "Moonlight" },
    -- 固定判定点序列帧不会像轨道填充效果一样拉伸整幅爆闪。
    -- 光效帧只承担视觉表现，判定时机与音效事件继续由软件管理。
    effects = {
        -- 两组打击光使用加法叠加，贴图 RGB 已反预乘，渲染时只乘一次 Alpha。
        hit_effect = {
            layout = "fixed",
            blend = { ["note.effect.note"] = "additive", ["note.effect.flick"] = "additive" },
        },
        glow = { passes = 6, intensity = 0.5 },
    },

    audios = {
        -- 原包不含音频，打击音复用默认皮肤，不借用新版 RM 的素材。
        -- 默认单键保留既有提前量，滑键使用默认零提前量。
        hiteffect = {
            note = audio(common("audio/note.wav"), 0.023),
            flick = audio(common("audio/flick.wav")),
        },
        -- 界面反馈音效与游戏按键音分开，保持软件按钮反馈一致。
        ui = {
            hover = audio(common("audio/ui/hover.wav")),
            click = audio(common("audio/ui/click.wav")),
            click_down = audio(common("audio/ui/click_down.wav")),
            click_up = audio(common("audio/ui/click_up.wav")),
            slider = audio(common("audio/ui/slider.wav")),
            notice = audio(common("audio/ui/notice.wav")),
        },
        -- 节拍器继续使用软件专用音，不把结算音或判定音当作节拍音。
        metronome = {
            beat_low = audio(common("audio/metronome/metronome_light.wav")),
            downbeat_high = audio(common("audio/metronome/metronome_accent.wav")),
        },
    },

    -- 字体不来自游戏截图；共用字体保证中文和软件图标完整。
    -- 所有公共字体引用同级 mmm-default，RM 随内置资源一同分发。
    fonts = {
        ascii = common("font/0xProtoNerdFontPropo-Regular.ttf"),
        cjk = common("font/NotoSansMonoCJKsc-Regular.otf"),
        icons = common("font/0xProtoNerdFontPropo-Regular.ttf"),
    },
    -- 字体候选表显式使用完整路径，导出时可解析到真实资源。
    ascii_fonts = { ["0xProto"] = common("font/0xProtoNerdFontPropo-Regular.ttf") },
    cjk_fonts = { ["Noto Sans CJK SC"] = common("font/NotoSansMonoCJKsc-Regular.otf") },
    -- 字号沿用日常编辑尺度，游戏素材的缩放只由纹理尺寸控制。
    fontsize = { title = 20, menu = 18, filemanager = 16, content = 15,
                 side_bar = 24, setting_internal = 14 },

    assets = {
        -- 软件导航控件保留现有图标，不挪用游戏结算页或编辑器素材。
        logo = common("image/logo.png"),
        cursor = common("image/cursor/cursor.png"),
        cursortrail = common("image/cursor/cursortrail.png"),
        cursor_smoke = common("image/cursor/cursor_smoke.png"),
        -- 单轨底板取自游玩轨道内部，避免每条轨道重复出现整屏透视边框。
        -- 判定区只取五轨中央的单个按键，不烘焙固定轨道数量。
        -- 底板与判定区各自拉伸，适配四轨、六轨及用户自定义轨道布局。
        panel = { track = {
            background = resource("image/panel/track.png"),
            judgearea = resource("image/panel/judgearea.png"),
        } },
        note = {
            -- 最终音符组件存档于同一图集；两张正俯视按键使用 281 × 123 的等比画布。
            -- Note 是其他部件的相对尺寸基准，替换时须保持两张图同尺寸。
            -- HoldHead 也供单滑键和折线起点使用，保证起点统一为绿色。
            note = resource("image/note/note.png"),
            holdhead = resource("image/note/holdhead.png"),
            -- 圆节点与矩形尾端在同一图集中生成，共用绿色边缘与光晕参数。
            -- 发光组件从模型图集统一缩小为 50%，原版实体素材保持原尺寸。
            -- PNG 留白参与软件的相对尺寸计算，不能统一紧裁所有部件。
            node = resource("image/note/node.png"),
            holdend = resource("image/note/holdend.png"),
            -- 横向与纵向连接体分别裁自图集中段，交给软件按实际跨度拉伸。
            -- 光带沿连接方向颜色恒定，拉长时不复制节点或引入重复接缝。
            holdbodyvertical = resource("image/note/holdbodyvertical.png"),
            holdbodyhorizontal = resource("image/note/holdbodyhorizontal.png"),
            -- 图集里的两个箭头仅含三角头与光晕，不包含多余的短杆。
            -- 左右箭头分别按固定坐标裁切，连接体仍由软件绘制到箭头中间。
            arrowleft = resource("image/note/arrowleft.png"),
            arrowright = resource("image/note/arrowright.png"),
            effect = {
                -- 原帧序保持不变，黑底转换为透明以适配 Alpha 混合。
                -- RGB 同时反预乘，防止透明度转换后发光亮度被计算两次。
                -- 连续序列范围必须与落盘帧数一致，缺帧不能静默跳过。
                note = resource("image/note/effect/note/[1 .. 9].png"),
                flick = resource("image/note/effect/flick/[1 .. 16].png"),
            },
        },
    },

    -- 主画布与预览均由软件渲染，原包的 Malody 游玩脚本不会被执行。
    canvases_2d = {
        basic_2d_canvas = { name = "Basic2DCanvas", shader_modules = note_shaders() },
        preview_window = { name = "PreviewWindow", shader_modules = note_shaders() },
        audio_spectrum_view = { name = "AudioSpectrumView", shader_modules = {
            main = common("shader/canvas/AudioSpectrumView/main"),
        } },
    },
    -- 保留标准工具侧栏与文件管理器的初始停靠位置。
    layout = {
        side_bar = { width = 32 },
        floating_windows = { window1 = {
            initial_title = "title.FileManager", initial_side = "left", initial_ratio = 0.26,
        } },
    },
}
