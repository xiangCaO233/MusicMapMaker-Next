-- assets/skins/mmm-nightly/skin.lua
local ressPath = __SKINLUA_DIR__

local f_ascii_reg  = "font/ComicShannsMonoNerdFontPropo-Regular.otf"
local f_ascii_bold = "font/ComicShannsMonoNerdFontPropo-Bold.otf"
local f_cjk_reg    = "font/NotoSansMonoCJKsc-Regular.otf"
local f_cjk_bold   = "font/NotoSansMonoCJKsc-Bold.otf"

local Skin = {
	meta = {
		name = "MMM Nightly",
		author = "xiang",
		version = "1.0",
		effectbasefps = 60,
	},

	basePath = ressPath .. "resources/",

	-- 颜色配置 (R, G, B, A)
	colors = {
		-- 预览区配色
		preview = {
			-- 主画布范围 示意包围框背景色
			boundingbox = { 0.7, 0.7, 0.7, 0.5 },
			-- 判定线色
			judgeline = { 0.0, 1.0, 1.0, 1.0 },
		},

		-- 音符配色 塞西莉娅配色
		note_tap = { 0.8902, 0.8588, 0.7608, 1.0 },
		note_hold = { 0.7333, 0.7608, 0.6000, 1.0 },
		note_node = { 0.9843, 0.8667, 0.8118, 1.0 },
		note_flick_arrow = { 0.9922, 0.9255, 0.5608, 1.0 },

		-- 拍线配色与线宽配置
		beat_lines = {
			beat_1 = { 1.0, 1.0, 1.0, 1.0 }, -- 拍头线
			beat_2 = { 1.0, 0.0, 0.0, 1.0 }, -- 2分音
			beat_3 = { 0.5, 0.0, 0.5, 1.0 }, -- 3分音
			beat_4 = { 0.0, 1.0, 1.0, 1.0 }, -- 4分音
			beat_5 = { 0.5, 0.0, 0.0, 1.0 }, -- 5分音 (暗红色)
			beat_6 = { 0.0, 1.0, 0.0, 1.0 }, -- 6分音
			beat_8 = { 1.0, 0.647, 0.0, 1.0 }, -- 8分音
			beat_12 = { 0.0, 0.0, 1.0, 1.0 }, -- 12分音
			beat_16 = { 1.0, 1.0, 0.0, 1.0 }, -- 16分音
			default = { 0.5, 0.5, 0.5, 1.0 } -- 默认/其他分拍
		},
	},

	-- 其他数值配置
	values = {
		beat_lines_width = {
			beat_1 = 4.0,
			default = 2.0
		}
	},

    -- UI 默认主题 (DeepDark, Dark, Light, Classic)
    theme = "DeepDark",

	-- 音频配置
	audios = {
		hiteffect = {
			note = "audio/note.wav",
			flick = "audio/flick.wav",
		},
	},

	-- 效果配置
	effects = {
		glow = {
			passes = 6,
			intensity = 0.5 
		}
	},

	-- 翻译配置
	langs = {
		en_us = "lang/en_us.lua",
		zh_cn = "lang/zh_cn.lua",
	},

	-- 字体文件定义
	fonts = {
		ascii = f_ascii_reg,
		cjk = f_cjk_reg,
	},

	-- 可选 ASCII 字体列表
	ascii_fonts = {
		["Comic Sans (Regular)"] = f_ascii_reg,
		["Comic Sans (Bold)"] = f_ascii_bold,
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
		content = 14,

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
				initial_ratio = 0.275,
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
