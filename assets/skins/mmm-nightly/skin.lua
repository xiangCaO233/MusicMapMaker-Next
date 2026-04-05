-- assets/skins/mmm-nightly/skin.lua
local ressPath = __SKINLUA_DIR__
local Skin = {
	meta = {
		name = "MMM Nightly",
		author = "xiang",
		version = "1.0",
	},

	basePath = ressPath .. "resources/",

	-- 颜色配置 (R, G, B, A)
	colors = {
		background = { 0.1, 0.1, 0.1, 1.0 },
		primary = { 0.0, 0.8, 0.8, 1.0 },
		text = { 1.0, 1.0, 1.0, 1.0 },
		alert = { 1.0, 0.2, 0.2, 1.0 },

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
	},

	-- 翻译配置
	langs = {
		en_us = "lang/en_us.lua",
		zh_cn = "lang/zh_cn.lua",
	},

	-- 字体配置
	fonts = {
		ascii = "font/ComicShannsMonoNerdFontPropo-Regular.otf",
		cjk = "font/NotoSansMonoCJKsc-Regular.otf",
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
		menubar = {
			minimize = "image/menubar/window-minimize.svg",
			maximize = "image/menubar/window-maximize.svg",
			close = "image/menubar/window-close.svg",
		},
		side_bar = {
			file_explorer_icon = "image/sidebar/folder.svg",
			audio_explorer_icon = "image/sidebar/music.svg",
		},
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
				note = "image/note/note/[1 .. 1].png",
				flick = "image/note/flick/[1 .. 16].png",
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
			icon_size = 20,
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
