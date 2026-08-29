-- IVM 独立皮肤。
-- 自维护资源包含字体、物件纹理、判定区与打击特效纹理；通用语言、音效、
-- 光标、Logo 和 Shader 复用随软件一同分发的 mmm-default 皮肤资源。
local skin_dir = __SKINLUA_DIR__
local resource_root = skin_dir .. "resources/"
local default_resource_root = skin_dir .. "../mmm-default/resources/"

local function resource(path)
	return resource_root .. path
end

local function default_resource(path)
	return default_resource_root .. path
end

-- IVM 拍头线使用纯红色，其余分拍线不区分分母并固定使用同一灰色。
local beat_head_red = { 1.0, 0.0, 0.0, 1.0 }
local beat_line_gray = { 0.68, 0.68, 0.68, 1.0 }

return {
	meta = {
		name = "IVM",
		author = "MusicMapMaker-Next",
		version = "1.0",
		effectbasefps = 120,
	},

	beat_divisors = { 1, 2, 3, 4, 6, 8, 12, 16 },

	colors = {
		preview = {
			boundingbox = { 0.58, 0.58, 0.58, 0.42 },
			hoverbox = { 0.74, 0.74, 0.74, 0.48 },
			judgment_guide = {
				fill = { 0.72, 0.72, 0.72, 0.30 },
				border = { 0.42, 0.42, 0.42, 1.0 },
			},
			judgeline = { 0.92, 0.08, 0.08, 1.0 },
		},

		-- 白色纹理作为 Alpha 遮罩；以下颜色决定最终纯色物件外观。
		note_tap = { 0.078, 0.784, 0.769, 1.0 },
		note_head = { 0.0, 0.918, 0.078, 1.0 },
		note_hold = { 0.0, 0.918, 0.078, 1.0 },
		note_end = { 0.0, 0.918, 0.078, 1.0 },
		note_node = { 0.0, 0.918, 0.078, 1.0 },
		note_flick_arrow = { 0.0, 0.918, 0.078, 1.0 },

		-- 草稿物件使用工业警示红与琥珀色，避开正式物件的青绿主色
		draft_notes = {
			note_tap = { 0.92, 0.20, 0.18, 0.92 },
			note_head = { 0.78, 0.10, 0.08, 0.92 },
			note_hold = { 0.70, 0.08, 0.07, 0.88 },
			note_end = { 0.86, 0.15, 0.12, 0.92 },
			note_node = { 0.96, 0.56, 0.28, 0.96 },
			note_flick_arrow = { 0.96, 0.56, 0.28, 0.96 },
		},

		-- 草稿轨道底板、边框、判定区与标题配色
		draft_tracks = {
			texture_tint = { 0.82, 0.82, 0.82, 1.0 },
			overlay = { 0.18, 0.04, 0.035, 0.52 },
			border = { 0.92, 0.20, 0.18, 0.96 },
			judgment_tint = { 0.96, 0.56, 0.28, 1.0 },
			label = { 0.96, 0.56, 0.28, 0.96 },
		},

		-- BGM 轨道与自动采样配色
		bgm_tracks = {
			background = { 0.035, 0.035, 0.035, 0.94 },
			alternate = { 0.075, 0.075, 0.075, 0.94 },
			border = { 0.40, 0.40, 0.40, 0.72 },
			separator = { 0.92, 0.08, 0.08, 0.96 },
			label = { 0.82, 0.82, 0.82, 0.94 },
			-- 自动采样物件沿用 mmm-default 配色，与 IVM 玩家物件明确区分。
			sample = { 0.36, 0.72, 0.92, 0.96 },
			offset = { 0.96, 0.56, 0.28, 0.92 },
			text = { 0.90, 0.96, 1.0, 0.96 },
		},

		-- 批注时间戳标记区配色
		annotations = {
			gutter_background = { 0.030, 0.030, 0.035, 0.96 },
			gutter_border = { 0.92, 0.08, 0.08, 0.78 },
			marker = { 0.92, 0.26, 0.30, 0.98 },
			marker_hover = { 1.0, 0.58, 0.60, 1.0 },
			marker_text = { 0.08, 0.02, 0.02, 1.0 },
			connector = { 0.92, 0.26, 0.30, 0.88 },
		},

		beat_lines = {
			beat_1 = beat_head_red,
			beat_2 = beat_line_gray,
			beat_3 = beat_line_gray,
			beat_4 = beat_line_gray,
			beat_6 = beat_line_gray,
			beat_8 = beat_line_gray,
			beat_12 = beat_line_gray,
			beat_16 = beat_line_gray,
			default = beat_line_gray,
		},
	},

	values = {
		beat_lines_width = {
			beat_1 = 1.5,
			default = 1.5,
		},
		glow = {
			resolution_scale = 0.5,
		},
	},

	-- 自动主题模式下无论系统偏亮或偏暗，都固定使用内置 IVM 主题。
	theme = {
		light = "IVM",
		dark = "IVM",
	},

	audios = {
		hiteffect = {
			note = {
				path = default_resource("audio/note.wav"),
				lead_in_ms = 0.023,
			},
			flick = {
				path = default_resource("audio/flick.wav"),
				lead_in_ms = 0.0,
			},
		},
		ui = {
			hover = {
				path = default_resource("audio/ui/hover.wav"),
				lead_in_ms = 0.0,
			},
			click = {
				path = default_resource("audio/ui/click.wav"),
				lead_in_ms = 0.0,
			},
			click_down = {
				path = default_resource("audio/ui/click_down.wav"),
				lead_in_ms = 0.0,
			},
			click_up = {
				path = default_resource("audio/ui/click_up.wav"),
				lead_in_ms = 0.0,
			},
			slider = {
				path = default_resource("audio/ui/slider.wav"),
				lead_in_ms = 0.0,
			},
			notice = {
				path = default_resource("audio/ui/notice.wav"),
				lead_in_ms = 0.0,
			},
		},
		metronome = {
			beat_low = {
				path = default_resource("audio/metronome/metronome_light.wav"),
				lead_in_ms = 0.023,
			},
			downbeat_high = {
				path = default_resource("audio/metronome/metronome_accent.wav"),
				lead_in_ms = 0.023,
			},
		},
	},

	-- IVM 物件保持纯色，交互时使用发光标识悬浮或选中；打击时使用独立的轨道渐变。
	effects = {
		hit_effect = {
			-- fixed：在判定线按物件尺寸绘制；track_fill：拉伸到整条可见轨道。
			layout = "track_fill",
		},
		glow = {
			passes = 6,
			intensity = 0.5,
		},
	},

	-- Liberation Sans 与 Windows Arial 指标兼容，提供经典 Windows 工具软件观感。
	fonts = {
		ascii = resource("font/LiberationSans-Regular.ttf"),
		cjk = default_resource("font/NotoSansMonoCJKsc-Regular.otf"),
		icons = default_resource("font/0xProtoNerdFontPropo-Regular.ttf"),
	},

	ascii_fonts = {
		{ "IVM Windows Sans", resource("font/LiberationSans-Regular.ttf") },
	},

	cjk_fonts = {
		{
			"Noto Sans Mono CJK SC",
			default_resource("font/NotoSansMonoCJKsc-Regular.otf"),
		},
	},

	fontsize = {
		title = 18,
		menu = 16,
		filemanager = 15,
		content = 15,
		side_bar = 22,
		setting_internal = 14,
	},

	assets = {
		logo = default_resource("image/logo.png"),
		cursor = default_resource("image/cursor/cursor.png"),
		cursortrail = default_resource("image/cursor/cursortrail.png"),
		cursor_smoke = default_resource("image/cursor/cursor_smoke.png"),
		btn_play = default_resource("image/buttons/play.png"),
		btn_pause = default_resource("image/buttons/pause.png"),
		bg_main = default_resource("image/backgrounds/main_menu.jpg"),
		panel = {
			track = {
				background = default_resource("image/panel/track.png"),
				judgearea = resource("image/panel/judgearea.png"),
			},
		},
		note = {
			note = resource("image/note/note.png"),
			node = resource("image/note/node.png"),
			holdend = resource("image/note/holdend.png"),
			holdbodyvertical = resource("image/note/holdbodyvertical.png"),
			holdbodyhorizontal = resource("image/note/holdbodyhorizontal.png"),
			arrowleft = resource("image/note/arrowleft.png"),
			arrowright = resource("image/note/arrowright.png"),
			effect = {
				note = resource("image/note/effect/note/[1 .. 6].png"),
				flick = resource("image/note/effect/flick/[1 .. 16].png"),
			},
		},
	},

	canvases_2d = {
		basic_2d_canvas = {
			name = "Basic2DCanvas",
			shader_modules = {
				main = default_resource("shader/canvas/Basic2DCanvas/main"),
				effect = default_resource("shader/canvas/Basic2DCanvas/effect"),
			},
		},
		preview_window = {
			name = "PreviewWindow",
			shader_modules = {
				main = default_resource("shader/canvas/Basic2DCanvas/main"),
				effect = default_resource("shader/canvas/Basic2DCanvas/effect"),
			},
		},
		audio_spectrum_view = {
			name = "AudioSpectrumView",
			shader_modules = {
				main = default_resource("shader/canvas/AudioSpectrumView/main"),
			},
		},
	},

	layout = {
		side_bar = {
			width = 32,
		},
		floating_windows = {
			window1 = {
				initial_title = "title.FileManager",
				initial_side = "left",
				initial_ratio = 0.26,
			},
		},
	},
}
