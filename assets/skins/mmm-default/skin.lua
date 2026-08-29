-- assets/skins/mmm-default/skin.lua
local ressPath = __SKINLUA_DIR__

local f_ascii_reg = "font/ComicShannsMonoNerdFontPropo-Regular.otf"
local f_ascii_bold = "font/ComicShannsMonoNerdFontPropo-Bold.otf"

local f0x_ascii_reg = "font/0xProtoNerdFontPropo-Regular.ttf"
local f0x_ascii_italic = "font/0xProtoNerdFontPropo-italic.ttf"
local f0x_ascii_bold = "font/0xProtoNerdFontPropo-Bold.ttf"

local f_cjk_reg = "font/NotoSansMonoCJKsc-Regular.otf"
local f_cjk_bold = "font/NotoSansMonoCJKsc-Bold.otf"

-- 拍线配色与线宽变量化复用
local c_head = { 1.0, 1.0, 1.0, 1.0 } -- 白色：1分音 (拍头线)
local c_half = { 1.0, 0.0, 0.0, 1.0 } -- 红色：2分音
local c_third = { 0.5, 0.0, 0.5, 1.0 } -- 紫色：3分音
local c_quarter = { 0.0, 1.0, 1.0, 1.0 } -- 青色：4分音
local c_sixth = { 0.0, 1.0, 0.0, 1.0 } -- 绿色：6分音
local c_eighth = { 1.0, 0.647, 0.0, 1.0 } -- 橙色：8分音
local c_twelfth = { 0.0, 0.0, 1.0, 1.0 } -- 蓝色：12分音
local c_sixteenth = { 1.0, 1.0, 0.0, 1.0 } -- 黄色：16分音
local c_default = { 0.5, 0.5, 0.5, 1.0 } -- 灰色：默认/其他分拍

-- 常用分拍定义列表
local divisor_list = { 1, 2, 3, 4, 6, 8, 12, 16 }

local Skin = {
	meta = {
		name = "Cecilia",
		author = "xiang",
		version = "1.0",
		effectbasefps = 120,
	},

	basePath = ressPath .. "resources/",

	-- 常用分拍列表
	beat_divisors = divisor_list,

	-- 颜色配置 (R, G, B, A)
	colors = {
		-- 预览区配色
		preview = {
			-- 主画布范围 示意包围框背景色
			boundingbox = { 0.7, 0.7, 0.7, 0.5 },
			-- 时间线/预览判定框
			judgment_guide = {
				fill = { 1.0, 182.0 / 255.0, 193.0 / 255.0, 160.0 / 255.0 }, -- FFB6C1A0
				border = { 1.0, 1.0, 1.0, 1.0 },
			},
			-- 判定线色
			judgeline = { 0.0, 1.0, 1.0, 1.0 },
		},

		-- 音符配色 塞西莉娅配色
		note_tap = { 0.8902, 0.8588, 0.7608, 1.0 },
		note_head = { 0.7333, 0.7608, 0.6000, 1.0 },
		note_hold = { 0.7333, 0.7608, 0.6000, 1.0 },
		note_end = { 0.7333, 0.7608, 0.6000, 1.0 },
		note_node = { 0.9922, 0.9255, 0.5608, 1.0 },
		note_flick_arrow = { 0.9922, 0.9255, 0.5608, 1.0 },

		-- BGM 轨道与自动采样配色
		bgm_tracks = {
			background = { 0.035, 0.055, 0.075, 0.92 },
			alternate = { 0.055, 0.080, 0.105, 0.92 },
			border = { 0.32, 0.48, 0.62, 0.55 },
			separator = { 0.28, 0.78, 0.94, 0.95 },
			label = { 0.72, 0.88, 0.96, 0.92 },
			sample = { 0.36, 0.72, 0.92, 0.96 },
			offset = { 0.96, 0.56, 0.28, 0.92 },
			text = { 0.90, 0.96, 1.0, 0.96 },
		},

		-- 批注时间戳标记区配色
		annotations = {
			gutter_background = { 0.025, 0.035, 0.050, 0.94 },
			gutter_border = { 0.28, 0.78, 0.94, 0.75 },
			marker = { 0.42, 0.72, 0.96, 0.98 },
			marker_hover = { 0.68, 0.86, 1.0, 1.0 },
			marker_text = { 0.04, 0.08, 0.12, 1.0 },
			connector = { 0.42, 0.72, 0.96, 0.86 },
		},

		-- 拍线配色与线宽配置
		beat_lines = {
			beat_1 = c_head,
			beat_2 = c_half,
			beat_3 = c_third,
			beat_4 = c_quarter,
			beat_6 = c_sixth,
			beat_8 = c_eighth,
			beat_12 = c_twelfth,
			beat_16 = c_sixteenth,
			default = c_default,
		},
	},

	-- 其他数值配置
	values = {
		beat_lines_width = {
			beat_1 = 4.0,
			default = 2.0,
		},
		glow = {
			-- 发光后处理分辨率倍率，低于 1 可降低 hover 光效的 GPU 片元开销
			resolution_scale = 0.5,
		},
	},

	-- UI 推荐主题：自动模式下跟随系统亮暗外观切换
	theme = {
		light = "Cecilia",
		dark = "Moonlight",
	},

	-- 音频配置
	audios = {
		hiteffect = {
			note = { path = "audio/note.wav", lead_in_ms = 0.023 },
			flick = { path = "audio/flick.wav", lead_in_ms = 0.0 },
		},
		ui = {
			-- UI按钮反馈使用专用短音频，通过音效池混入总线，避免重复加载和削波
			hover = { path = "audio/ui/hover.wav", lead_in_ms = 0.0 },
			click = { path = "audio/ui/click.wav", lead_in_ms = 0.0 },
			click_down = { path = "audio/ui/click_down.wav", lead_in_ms = 0.0 },
			click_up = { path = "audio/ui/click_up.wav", lead_in_ms = 0.0 },
			slider = { path = "audio/ui/slider.wav", lead_in_ms = 0.0 },
			notice = { path = "audio/ui/notice.wav", lead_in_ms = 0.0 },
		},
		metronome = {
			beat_low = { path = "audio/metronome/metronome_light.wav", lead_in_ms = 0.023 },
			downbeat_high = { path = "audio/metronome/metronome_accent.wav", lead_in_ms = 0.023 },
		},
	},

	-- 效果配置
	effects = {
		hit_effect = {
			-- fixed 保留判定线中心的固定尺寸序列帧；也可设为 track_fill 填满单轨。
			layout = "fixed",
		},
		glow = {
			passes = 6,
			intensity = 0.5,
		},
	},

	-- 字体文件定义
	fonts = {
		ascii = f0x_ascii_reg,
		cjk = f_cjk_reg,
		icons = f0x_ascii_reg,
	},

	-- 可选 ASCII 字体列表
	ascii_fonts = {
		["Comic Sans (Regular)"] = f_ascii_reg,
		["Comic Sans (Bold)"] = f_ascii_bold,
		["0xProto (Regular)"] = f0x_ascii_reg,
		["0xProto (Italic)"] = f0x_ascii_italic,
		["0xProto (Bold)"] = f0x_ascii_bold,
		["Noto Sans CJK (Regular)"] = f_cjk_reg,
		["Noto Sans CJK (Bold)"] = f_cjk_bold,
	},

	-- 可选 CJK 字体列表
	cjk_fonts = {
		["Noto Sans CJK (Regular)"] = f_cjk_reg,
		["Noto Sans CJK (Bold)"] = f_cjk_bold,
	},

	-- 字体尺寸配置
	fontsize = {
		-- 标题字体大小/主要是imgui的窗口标题
		title = 20,
		-- 菜单字体大小/主要是菜单栏的和内部菜单项的字体大小
		menu = 18,
		-- 文件管理器字体大小/主要是资源管理器音频管理器谱面管理器等里面浏览的文件的字体大小
		filemanager = 16,
		-- 内容字体大小/主要是设置项，文本编辑器等字体的大小
		content = 15,

		-- 侧边栏字体图标的尺寸
		side_bar = 24,

		-- 设置内部的字体图标的尺寸
		setting_internal = 14,
	},

	-- 资产文件映射
	assets = {
		logo = "image/logo.png",
		cursor = "image/cursor/cursor.png",
		cursortrail = "image/cursor/cursortrail.png",
		cursor_smoke = "image/cursor/cursor_smoke.png",
		btn_play = "image/buttons/play.png",
		btn_pause = "image/buttons/pause.png",
		bg_main = "image/backgrounds/main_menu.jpg",
		panel = {
			track = {
				background = "image/panel/track.png",
				judgearea = "image/panel/judgearea.png",
			},
		},
		note = {
			note = "image/note/note.png",
			node = "image/note/node.png",
			holdend = "image/note/holdend.png",
			holdbodyvertical = "image/note/holdbodyvertical.png",
			holdbodyhorizontal = "image/note/holdbodyhorizontal.png",
			arrowleft = "image/note/arrowleft.png",
			arrowright = "image/note/arrowright.png",
			effect = {
				note = "image/note/effect/note/[1 .. 6].png",
				flick = "image/note/effect/flick/[1 .. 16].png",
			},
		},
	},

	-- 2d绘制画布配置
	canvases_2d = {
		basic_2d_canvas = {
			name = "Basic2DCanvas",
			shader_modules = {
				main = "shader/canvas/Basic2DCanvas/main",
				effect = "shader/canvas/Basic2DCanvas/effect",
			},
		},
		preview_window = {
			name = "PreviewWindow",
			shader_modules = {
				main = "shader/canvas/Basic2DCanvas/main",
				effect = "shader/canvas/Basic2DCanvas/effect",
			},
		},
		audio_spectrum_view = {
			name = "AudioSpectrumView",
			shader_modules = {
				main = "shader/canvas/AudioSpectrumView/main",
			},
		},
	},

	-- 布局参数
	layout = {
		-- 侧边栏配置
		side_bar = {
			width = 32,
		},
		-- 悬浮窗初始配置
		floating_windows = {
			window1 = {
				initial_title = "title.FileManager",
				initial_side = "left",
				initial_ratio = 0.26,
			},
		},
	},
}

-- 自动拼接路径 (支持无限嵌套)
function Skin:resolve_paths(current_assets)
	-- 如果没传参数，默认处理 self.assets
	local assets_to_process = current_assets or self.assets

	for k, v in pairs(assets_to_process) do
		if type(v) == "table" then
			-- 如果是表，递归处理子表
			self:resolve_paths(v)
		elseif type(v) == "string" then
			-- 如果是字符串，执行拼接
			assets_to_process[k] = self.basePath .. v
		end
	end
end

Skin:resolve_paths()

return Skin
