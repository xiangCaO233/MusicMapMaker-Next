-- IVM 独立皮肤。
-- 自维护资源包含字体、物件纹理、判定区与打击特效纹理；通用语言、音效、
-- 光标、Logo 和 Shader 复用随软件一同分发的 mmm-default 皮肤资源。
-- 皮肤表中的路径在声明时即选择本地或默认资源根，不执行后置递归改写。
-- 颜色、尺寸和布局保持经典工具软件的高对比、紧凑视觉层级。
local skin_dir = __SKINLUA_DIR__
-- resource_root 只指向 IVM 自维护内容。
local resource_root = skin_dir .. "resources/"
-- default_resource_root 用于明确复用随程序分发的公共内容。
local default_resource_root = skin_dir .. "../mmm-default/resources/"

--- @brief 把 IVM 内部相对路径解析为皮肤资源路径。
--- @param path resources 目录下的相对路径。
--- @return 可由 SkinLoader 直接打开的完整路径。
local function resource(path)
	return resource_root .. path
end

--- @brief 把公共相对路径解析到 mmm-default 资源目录。
--- @param path 默认皮肤 resources 目录下的相对路径。
--- @return 跨皮肤复用资源的完整路径。
local function default_resource(path)
	return default_resource_root .. path
end

-- IVM 拍头线使用纯红色，其余分拍线不区分分母并固定使用同一灰色。
local beat_head_red = { 1.0, 0.0, 0.0, 1.0 }
local beat_line_gray = { 0.68, 0.68, 0.68, 1.0 }

return {
	-- 元数据用于皮肤列表展示；基准帧率控制特效序列播放节奏。
	meta = {
		name = "IVM",
		author = "MusicMapMaker-Next",
		version = "1.0",
		effectbasefps = 120,
	},

	-- 分拍列表与 beat_lines 键保持一致，覆盖常用编辑网格。
	beat_divisors = { 1, 2, 3, 4, 6, 8, 12, 16 },

	-- 所有数组固定使用零到一范围的 RGBA 顺序。
	colors = {
		-- 预览范围使用中性灰，判定线保留 IVM 标志性红色。
		preview = {
			-- boundingbox 表示主画布视野，hoverbox 表示鼠标反馈区域。
			boundingbox = { 0.58, 0.58, 0.58, 0.42 },
			hoverbox = { 0.74, 0.74, 0.74, 0.48 },
			judgment_guide = {
				-- 低透明填充不遮挡压缩后的音符预览。
				fill = { 0.72, 0.72, 0.72, 0.30 },
				border = { 0.42, 0.42, 0.42, 1.0 },
			},
			judgeline = { 0.92, 0.08, 0.08, 1.0 },
		},

		-- 白色纹理作为 Alpha 遮罩；以下颜色决定最终纯色物件外观。
		-- Tap 使用青色，Hold 全链路与 Flick 箭头统一为亮绿色。
		note_tap = { 0.078, 0.784, 0.769, 1.0 },
		note_head = { 0.0, 0.918, 0.078, 1.0 },
		note_hold = { 0.0, 0.918, 0.078, 1.0 },
		note_end = { 0.0, 0.918, 0.078, 1.0 },
		note_node = { 0.0, 0.918, 0.078, 1.0 },
		note_flick_arrow = { 0.0, 0.918, 0.078, 1.0 },

		-- 草稿物件使用工业警示红与琥珀色，避开正式物件的青绿主色
		draft_notes = {
			-- 主体使用红色警示层级，节点和箭头使用更亮琥珀色。
			note_tap = { 0.92, 0.20, 0.18, 0.92 },
			note_head = { 0.78, 0.10, 0.08, 0.92 },
			note_hold = { 0.70, 0.08, 0.07, 0.88 },
			note_end = { 0.86, 0.15, 0.12, 0.92 },
			note_node = { 0.96, 0.56, 0.28, 0.96 },
			note_flick_arrow = { 0.96, 0.56, 0.28, 0.96 },
		},

		-- 草稿轨道底板、边框、判定区与标题配色
		draft_tracks = {
			-- 轨道 tint 降低亮度，overlay 再提供红黑背景分区。
			texture_tint = { 0.82, 0.82, 0.82, 1.0 },
			overlay = { 0.18, 0.04, 0.035, 0.52 },
			border = { 0.92, 0.20, 0.18, 0.96 },
			judgment_tint = { 0.96, 0.56, 0.28, 1.0 },
			label = { 0.96, 0.56, 0.28, 0.96 },
		},

		-- BGM 轨道与自动采样配色
		bgm_tracks = {
			-- 交替底色帮助区分相邻自动采样轨道。
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
			-- 批注沿用红色主题，但通过亮度与 Alpha 区分状态。
			gutter_background = { 0.030, 0.030, 0.035, 0.96 },
			gutter_border = { 0.92, 0.08, 0.08, 0.78 },
			marker = { 0.92, 0.26, 0.30, 0.98 },
			marker_hover = { 1.0, 0.58, 0.60, 1.0 },
			marker_text = { 0.08, 0.02, 0.02, 1.0 },
			connector = { 0.92, 0.26, 0.30, 0.88 },
		},

		-- 除拍头外不按分母区分颜色，复现 IVM 的统一灰色网格。
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

	-- 数值效果参数与颜色资源分离，便于渲染器按类型读取。
	values = {
		-- 拍头和普通线使用相同宽度，差异仅由红灰颜色表达。
		beat_lines_width = {
			beat_1 = 1.5,
			default = 1.5,
		},
		glow = {
			-- 半分辨率后处理降低连续悬浮光效的片元成本。
			resolution_scale = 0.5,
		},
	},

	-- 自动主题模式下无论系统偏亮或偏暗，都固定使用内置 IVM 主题。
	theme = {
		-- 名称必须与内置 IVM UI 主题的稳定注册 ID 一致。
		light = "IVM",
		dark = "IVM",
	},

	-- IVM 不复制公共音效，所有音频均显式指向默认皮肤。
	audios = {
		-- 打击音前导值校正采样开头静音与判定时刻。
		hiteffect = {
			note = {
				-- 普通音符使用默认短采样并保留既有 23 微秒修正值。
				path = default_resource("audio/note.wav"),
				lead_in_ms = 0.023,
			},
			flick = {
				-- Flick 独立采样无需额外前导偏移。
				path = default_resource("audio/flick.wav"),
				lead_in_ms = 0.0,
			},
		},
		ui = {
			-- 每种控件状态使用独立文件，避免运行时动态切片。
			hover = {
				-- 悬浮反馈应轻量且允许高频复用音效池。
				path = default_resource("audio/ui/hover.wav"),
				lead_in_ms = 0.0,
			},
			click = {
				-- 完整点击采样供不区分按下与抬起的控件使用。
				path = default_resource("audio/ui/click.wav"),
				lead_in_ms = 0.0,
			},
			click_down = {
				-- 按下状态在鼠标或键盘激活瞬间播放。
				path = default_resource("audio/ui/click_down.wav"),
				lead_in_ms = 0.0,
			},
			click_up = {
				-- 抬起状态只在成功提交交互时播放。
				path = default_resource("audio/ui/click_up.wav"),
				lead_in_ms = 0.0,
			},
			slider = {
				-- 滑块反馈由限频逻辑控制，本表只声明采样路径。
				path = default_resource("audio/ui/slider.wav"),
				lead_in_ms = 0.0,
			},
			notice = {
				-- 通知音与普通点击分离，保留更高提示优先级。
				path = default_resource("audio/ui/notice.wav"),
				lead_in_ms = 0.0,
			},
		},
		metronome = {
			-- 普通拍和小节首拍使用不同音色但相同前导校准。
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
			-- 六次模糊与半强度合成平衡边缘平滑和 GPU 成本。
			passes = 6,
			intensity = 0.5,
		},
	},

	-- Liberation Sans 与 Windows Arial 指标兼容，提供经典 Windows 工具软件观感。
	fonts = {
		-- ASCII 使用皮肤自带字体，CJK 和图标复用默认完整字库。
		ascii = resource("font/LiberationSans-Regular.ttf"),
		cjk = default_resource("font/NotoSansMonoCJKsc-Regular.otf"),
		icons = default_resource("font/0xProtoNerdFontPropo-Regular.ttf"),
	},

	-- 可选列表的显示名属于用户配置持久化值。
	ascii_fonts = {
		{ "IVM Windows Sans", resource("font/LiberationSans-Regular.ttf") },
	},

	-- CJK 仅暴露默认皮肤中覆盖完整简体中文字形的字体。
	cjk_fonts = {
		{
			"Noto Sans Mono CJK SC",
			default_resource("font/NotoSansMonoCJKsc-Regular.otf"),
		},
	},

	-- 紧凑字号层级模拟传统 Windows 工具界面密度。
	fontsize = {
		-- 标题略大于菜单，突出浮动窗口层级。
		title = 18,
		-- 菜单与管理器文本依次减小，内容保持可读下限。
		menu = 16,
		filemanager = 15,
		content = 15,
		side_bar = 22,
		-- 设置内部图标使用最小字号以适配紧凑行高。
		setting_internal = 14,
	},

	-- 资产映射明确标记哪些内容由 IVM 自维护、哪些复用默认皮肤。
	assets = {
		-- Logo、光标、按钮和背景不参与 IVM 物件视觉替换。
		logo = default_resource("image/logo.png"),
		cursor = default_resource("image/cursor/cursor.png"),
		cursortrail = default_resource("image/cursor/cursortrail.png"),
		cursor_smoke = default_resource("image/cursor/cursor_smoke.png"),
		btn_play = default_resource("image/buttons/play.png"),
		btn_pause = default_resource("image/buttons/pause.png"),
		bg_main = default_resource("image/backgrounds/main_menu.jpg"),
		panel = {
			-- 轨道背景复用默认资源，判定区使用 IVM 自有贴图。
			track = {
				background = default_resource("image/panel/track.png"),
				judgearea = resource("image/panel/judgearea.png"),
			},
		},
		note = {
			-- 全部玩家物件贴图来自 IVM，自身为白色 Alpha 遮罩。
			note = resource("image/note/note.png"),
			node = resource("image/note/node.png"),
			holdend = resource("image/note/holdend.png"),
			holdbodyvertical = resource("image/note/holdbodyvertical.png"),
			holdbodyhorizontal = resource("image/note/holdbodyhorizontal.png"),
			arrowleft = resource("image/note/arrowleft.png"),
			arrowright = resource("image/note/arrowright.png"),
			effect = {
				-- 方括号范围由加载器展开为连续的 note 与 flick 帧。
				note = resource("image/note/effect/note/[1 .. 6].png"),
				flick = resource("image/note/effect/flick/[1 .. 16].png"),
			},
		},
	},

	-- 画布工厂名称保持默认实现，只替换资源根和皮肤参数。
	canvases_2d = {
		basic_2d_canvas = {
			-- 主画布使用默认皮肤提供的已编译 Shader 模块。
			name = "Basic2DCanvas",
			shader_modules = {
				main = default_resource("shader/canvas/Basic2DCanvas/main"),
				effect = default_resource("shader/canvas/Basic2DCanvas/effect"),
			},
		},
		preview_window = {
			-- 预览窗口与主画布共享相同 main/effect 管线接口。
			name = "PreviewWindow",
			shader_modules = {
				main = default_resource("shader/canvas/Basic2DCanvas/main"),
				effect = default_resource("shader/canvas/Basic2DCanvas/effect"),
			},
		},
		audio_spectrum_view = {
			-- 频谱视图只需要直接纹理绘制 Shader。
			name = "AudioSpectrumView",
			shader_modules = {
				main = default_resource("shader/canvas/AudioSpectrumView/main"),
			},
		},
	},

	-- 初始布局只在没有持久化 ImGui 布局时作为种子使用。
	layout = {
		side_bar = {
			-- 32 像素宽度与 22 像素图标保持紧凑留白。
			width = 32,
		},
		floating_windows = {
			-- 首个文件管理器停靠左侧并占主区域约四分之一。
			window1 = {
				initial_title = "title.FileManager",
				initial_side = "left",
				initial_ratio = 0.26,
			},
		},
	},
}

-- 维护约束：IVM 自有资源必须通过 resource helper 明确引用。
-- 维护约束：公共资源必须通过 default_resource helper 明确引用。
-- 维护约束：不得把调用者工作目录拼入任何皮肤资产路径。
-- 维护约束：资源根相对关系依赖 ivm 与 mmm-default 目录同级。
-- 维护约束：迁移默认皮肤目录时必须同步 default_resource_root。
-- 维护约束：颜色数组顺序必须保持 RGBA，不能改为 ARGB。
-- 维护约束：新增分拍分母需同步 beat_divisors 与 beat_lines。
-- 维护约束：序列帧范围必须与自有 PNG 编号完全一致。
-- 维护约束：Shader 复用要求 IVM 与默认画布 push constant 布局一致。
-- 维护约束：字体显示名称变化会影响用户已保存的选择。
-- 维护约束：画布 name 必须对应 C++ 已注册工厂标识。
-- 维护约束：新增配置键需与 SkinLoader 的嵌套读取路径一致。
-- 维护约束：本文件只返回配置表，不执行文件系统或图像处理。
-- 维护约束：皮肤加载位于启动或重载低频路径，不进入每帧渲染。
-- 维护约束：正式音符与草稿音符必须保持足够的色相和亮度差异。
-- 维护约束：BGM 自动采样颜色不得与玩家青绿音符混淆。
-- 维护约束：批注 marker_hover 必须比普通 marker 更易辨识。
-- 维护约束：track_fill 特效会拉伸纹理，源帧需为可接受的渐变结构。
-- 维护约束：发光 passes 或 intensity 调整需复核低端 GPU 帧耗。
-- 维护约束：resolution_scale 必须保持正值且不应超过全分辨率需求。
-- 维护约束：主题 light 和 dark 固定同名是 IVM 视觉一致性要求。
-- 维护约束：UI 音效复用依赖默认皮肤随程序完整分发。
-- 维护约束：自有 Liberation Sans 许可证文件必须与字体共同保留。
-- 维护约束：默认 CJK 和图标字体路径变化时需同步本文件引用。
-- 维护约束：判定区贴图尺寸需与轨道渲染器的拉伸语义兼容。
-- 维护约束：音符连接体贴图必须保留沿延伸方向的无缝边缘。
-- 维护约束：资源图集重建后应核对所有独立贴图路径仍存在。
-- 维护约束：布局默认值不能覆盖用户已经持久化的窗口状态。
-- 维护约束：返回表之外不得留下依赖全局状态的延迟初始化逻辑。
