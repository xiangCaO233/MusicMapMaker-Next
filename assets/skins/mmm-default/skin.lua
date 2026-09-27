-- assets/skins/mmm-default/skin.lua
-- 默认皮肤完整声明颜色、音频、字体、纹理、Shader 和初始布局契约。
-- 所有相对资产路径在返回配置前以当前皮肤 resources 目录为根解析。
local ressPath = __SKINLUA_DIR__

-- Comic Shanns 只作为可选 ASCII 字体，不承担 CJK 或图标字形回退。
local f_ascii_reg = "font/ComicShannsMonoNerdFontPropo-Regular.otf"
local f_ascii_bold = "font/ComicShannsMonoNerdFontPropo-Bold.otf"

-- 0xProto Nerd Font 同时提供默认 ASCII 和界面图标字形。
local f0x_ascii_reg = "font/0xProtoNerdFontPropo-Regular.ttf"
local f0x_ascii_italic = "font/0xProtoNerdFontPropo-italic.ttf"
local f0x_ascii_bold = "font/0xProtoNerdFontPropo-Bold.ttf"

-- Noto Sans Mono CJK SC 提供简体中文等宽字形及粗体选项。
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
	-- 元数据用于皮肤列表展示；effectbasefps 定义序列帧的基准播放速率。
	meta = {
		name = "Cecilia",
		author = "xiang",
		version = "1.0",
		effectbasefps = 120,
	},

	-- 路径必须以斜杠结束，resolve_paths 直接执行字符串拼接。
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
				-- 半透明填充显示当前主画布可见判定范围。
				fill = { 1.0, 182.0 / 255.0, 193.0 / 255.0, 160.0 / 255.0 }, -- FFB6C1A0
				-- 不透明边框在浅色背景上保持范围边界可辨识。
				border = { 1.0, 1.0, 1.0, 1.0 },
			},
			-- 判定线色
			judgeline = { 0.0, 1.0, 1.0, 1.0 },
		},

		-- 音符配色 塞西莉娅配色
		-- RGBA 值作为白色音符贴图的统一顶点着色，不修改源纹理。
		note_tap = { 0.8902, 0.8588, 0.7608, 1.0 },
		note_head = { 0.7333, 0.7608, 0.6000, 1.0 },
		note_hold = { 0.7333, 0.7608, 0.6000, 1.0 },
		note_end = { 0.7333, 0.7608, 0.6000, 1.0 },
		note_node = { 0.9922, 0.9255, 0.5608, 1.0 },
		note_flick_arrow = { 0.9922, 0.9255, 0.5608, 1.0 },

		-- 草稿物件使用暖珊瑚与古金色系，与正式物件清晰区分
		draft_notes = {
			-- Tap、Hold 主体和端点保持同一暖色层级，节点与箭头更亮。
			note_tap = { 0.96, 0.69, 0.52, 0.92 },
			note_head = { 0.82, 0.48, 0.36, 0.92 },
			note_hold = { 0.74, 0.40, 0.32, 0.88 },
			note_end = { 0.88, 0.55, 0.40, 0.92 },
			note_node = { 1.0, 0.79, 0.43, 0.96 },
			note_flick_arrow = { 1.0, 0.79, 0.43, 0.96 },
		},

		-- 草稿轨道底板、边框、判定区与标题配色
		draft_tracks = {
			-- texture_tint 保留原轨道纹理颜色，overlay 再压暗背景。
			texture_tint = { 1.0, 1.0, 1.0, 1.0 },
			overlay = { 0.08, 0.12, 0.18, 0.48 },
			border = { 0.35, 0.55, 0.75, 1.0 },
			judgment_tint = { 1.0, 1.0, 1.0, 1.0 },
			label = { 0.96, 0.69, 0.52, 0.92 },
		},

		-- BGM 轨道与自动采样配色
		bgm_tracks = {
			-- alternate 用于相邻轨道交替底色，separator 标记轨道边界。
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
			-- gutter 与 connector 使用同一蓝色族，marker_hover 提高交互反馈。
			gutter_background = { 0.025, 0.035, 0.050, 0.94 },
			gutter_border = { 0.28, 0.78, 0.94, 0.75 },
			marker = { 0.42, 0.72, 0.96, 0.98 },
			marker_hover = { 0.68, 0.86, 1.0, 1.0 },
			marker_text = { 0.04, 0.08, 0.12, 1.0 },
			connector = { 0.42, 0.72, 0.96, 0.86 },
		},

		-- 拍线配色与线宽配置
		beat_lines = {
			-- 已知分母映射到预声明颜色，未知分母统一回退灰色。
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
		-- 拍头线比普通细分线更宽，以突出整拍边界。
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
		-- 名称必须与内置 UI 主题注册表中的稳定 ID 一致。
		light = "Cecilia",
		dark = "Moonlight",
	},

	-- 音频配置
	audios = {
		-- lead_in_ms 修正采样文件开头静音，使听感与判定时刻对齐。
		hiteffect = {
			note = { path = "audio/note.wav", lead_in_ms = 0.023 },
			flick = { path = "audio/flick.wav", lead_in_ms = 0.0 },
		},
		ui = {
			-- UI按钮反馈使用专用短音频，通过音效池混入总线，避免重复加载和削波
			-- hover、按下和抬起保持独立资源，允许控件状态精确反馈。
			hover = { path = "audio/ui/hover.wav", lead_in_ms = 0.0 },
			click = { path = "audio/ui/click.wav", lead_in_ms = 0.0 },
			click_down = { path = "audio/ui/click_down.wav", lead_in_ms = 0.0 },
			click_up = { path = "audio/ui/click_up.wav", lead_in_ms = 0.0 },
			slider = { path = "audio/ui/slider.wav", lead_in_ms = 0.0 },
			notice = { path = "audio/ui/notice.wav", lead_in_ms = 0.0 },
		},
		metronome = {
			-- 普通拍和小节首拍使用不同采样，二者共享相同前导修正。
			beat_low = { path = "audio/metronome/metronome_light.wav", lead_in_ms = 0.023 },
			downbeat_high = { path = "audio/metronome/metronome_accent.wav", lead_in_ms = 0.023 },
		},
	},

	-- 效果配置
	effects = {
		hit_effect = {
			-- fixed 保留判定线中心的固定尺寸序列帧；也可设为 track_fill 填满单轨。
			layout = "fixed",
			-- 爆炸光只增加背景亮度；矩形判定反馈仍使用普通透明覆盖。
			blend = { ["note.effect.flick"] = "additive" },
		},
		glow = {
			-- passes 控制可分离模糊次数，intensity 控制最终合成亮度。
			passes = 6,
			intensity = 0.5,
		},
	},

	-- 字体文件定义
	fonts = {
		-- 三个槽位分别服务西文、CJK 与图标，不依赖系统字体路径。
		ascii = f0x_ascii_reg,
		cjk = f_cjk_reg,
		icons = f0x_ascii_reg,
	},

	-- 可选 ASCII 字体列表
	ascii_fonts = {
		-- 显示名称是设置页持久化值，路径在加载后统一绝对化。
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
		-- CJK 列表只暴露覆盖完整中文字符集的 Noto 字体。
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
		-- 该尺寸用于紧凑设置控件，独立于侧边栏大图标。
		setting_internal = 14,
	},

	-- 资产文件映射
	assets = {
		-- 顶层 UI 资产可由各视图按稳定键名直接请求。
		logo = "image/logo.png",
		cursor = "image/cursor/cursor.png",
		cursortrail = "image/cursor/cursortrail.png",
		cursor_smoke = "image/cursor/cursor_smoke.png",
		btn_play = "image/buttons/play.png",
		btn_pause = "image/buttons/pause.png",
		bg_main = "image/backgrounds/main_menu.jpg",
		panel = {
			-- 轨道背景沿时间方向平铺，判定区贴图覆盖底部交互区域。
			track = {
				background = "image/panel/track.png",
				judgearea = "image/panel/judgearea.png",
			},
		},
		note = {
			-- 音符键名与 Canvas 渲染器使用的纹理语义一一对应。
			note = "image/note/note.png",
			node = "image/note/node.png",
			holdend = "image/note/holdend.png",
			holdbodyvertical = "image/note/holdbodyvertical.png",
			holdbodyhorizontal = "image/note/holdbodyhorizontal.png",
			arrowleft = "image/note/arrowleft.png",
			arrowright = "image/note/arrowright.png",
			effect = {
				-- 方括号范围由皮肤加载器展开为连续序列帧路径。
				note = "image/note/effect/note/[1 .. 6].png",
				-- Hold 独立配置，初始复制单键帧序列；共享原文件但不共享序列身份。
				hold = "image/note/effect/note/[1 .. 6].png",
				flick = "image/note/effect/flick/[1 .. 16].png",
			},
		},
	},

	-- 2d绘制画布配置
	canvases_2d = {
		-- name 是 C++ 画布工厂的类型标识，不能随显示文本本地化。
		basic_2d_canvas = {
			name = "Basic2DCanvas",
			shader_modules = {
				-- main 绘制常规批次，effect 执行发光模糊和合成。
				main = "shader/canvas/Basic2DCanvas/main",
				effect = "shader/canvas/Basic2DCanvas/effect",
			},
		},
		preview_window = {
			-- 预览与主画布复用相同 Shader 接口和皮肤效果参数。
			name = "PreviewWindow",
			shader_modules = {
				main = "shader/canvas/Basic2DCanvas/main",
				effect = "shader/canvas/Basic2DCanvas/effect",
			},
		},
		audio_spectrum_view = {
			-- 频谱画布只有直接纹理绘制主阶段，不使用 glow effect。
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
			-- window1 是首次布局种子，持久化 ImGui 状态会在后续覆盖它。
			window1 = {
				initial_title = "title.FileManager",
				initial_side = "left",
				initial_ratio = 0.26,
			},
		},
	},
}

--- @brief 递归解析资产表中所有相对字符串路径。
--- @param current_assets 可选子表；缺省时从完整 assets 根开始。
--- @warning 该过程原地修改表且必须只执行一次，否则会重复拼接 basePath。
-- 表值递归下降，字符串叶节点统一拼接当前皮肤资源根。
function Skin:resolve_paths(current_assets)
	-- 如果没传参数，默认处理 self.assets
	local assets_to_process = current_assets or self.assets

	for k, v in pairs(assets_to_process) do
		-- 未知非表、非字符串扩展值保持原样，便于将来增加数值元数据。
		if type(v) == "table" then
			-- 如果是表，递归处理子表
			self:resolve_paths(v)
		elseif type(v) == "string" then
			-- 如果是字符串，执行拼接
			-- Lua 表键保持不变，仅替换路径字符串值。
			assets_to_process[k] = self.basePath .. v
		end
	end
end

-- 返回前解析一次，使 C++ 消费者始终收到可直接访问的完整路径。
Skin:resolve_paths()

-- Skin 表是该 Lua 文件唯一导出值。
return Skin

-- 维护约束：新增相对资源必须放入 assets 表以参与统一路径解析。
-- 维护约束：已经是绝对路径的字符串不得直接加入 assets 表。
-- 维护约束：新增画布名称必须先由 C++ 画布工厂注册。
-- 维护约束：Shader 目录必须同时提供匹配阶段的 SPIR-V 产物。
-- 维护约束：序列帧范围必须与实际连续文件编号完全一致。
-- 维护约束：颜色数组固定采用零到一的 RGBA 顺序。
-- 维护约束：新增配置键应保持与 SkinLoader 的嵌套读取路径一致。
-- 维护约束：字体显示名称变化会影响用户已保存的选择值。
-- 维护约束：音频路径和 lead-in 必须与实际采样文件成套校准。
-- 维护约束：默认皮肤资源由安装和首次启动同步流程共同依赖。
-- 维护约束：皮肤目录版本变化时必须由资源同步流程更新目标副本。
-- 维护约束：默认资源缺失不能通过系统文件或当前目录静默回退。
-- 维护约束：图集重建后必须核对 assets 表中的独立贴图路径。
-- 维护约束：画布 Shader 源码变更时必须重新生成对应 SPIR-V。
-- 维护约束：布局默认值只能影响首次创建，不能覆盖持久化状态。
-- 维护约束：返回 Skin 前不得执行文件 I/O 或依赖用户配置内容。
