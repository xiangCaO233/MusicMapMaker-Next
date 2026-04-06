return {
	-- [ Translation Key ] = "翻译后的目标文本"

	-- 普通文本
	["ui.file"] = "文件",
	["ui.file.new_map"] = "新建谱面",
	["ui.file.new_pro"] = "新建项目",
	["ui.file.open_map"] = "打开谱面",
	["ui.file.open_pro"] = "打开项目",
	["ui.file.open_recent"] = "打开最近",
	["ui.file.save"] = "保存",
	["ui.file.save_as"] = "另存为...",

	["ui.file.exit"] = "退出",
	["ui.edit"] = "编辑",
	["ui.edit.copy"] = "复制",
	["ui.edit.paste"] = "粘贴",
	-- ["ui.view"] = "视图",
	-- ["ui.settings"] = "设置",
	-- ["ui.exit"] = "退出",

	["canvas.editor"] = "编辑器",
	["canvas.preview"] = "预览",

	["title.file_manager"] = "文件浏览",
	["title.settings_manager"] = "设置",
	["ui.settings.software"] = "软件配置",
	["ui.settings.software.general"] = "通用设置",
	["ui.settings.software.language"] = "界面语言",
	["ui.settings.software.picker_native"] = "系统原生选择器",
	["ui.settings.software.picker_unified"] = "编辑器统一选择器",
	["ui.settings.software.sync"] = "同步与时钟",
	["ui.settings.software.sync_mode"] = "音频同步模式",
	["ui.settings.software.sync_factor"] = "积分追踪系数",
	["ui.settings.software.sync_buffer"] = "水箱缓冲时间",

	["ui.settings.visual"] = "视觉配置",
	["ui.settings.visual.judgeline"] = "判定线",
	["ui.settings.visual.judgeline_pos"] = "判定线位置",
	["ui.settings.visual.judgeline_width"] = "判定线线宽",
	["ui.settings.visual.note"] = "物件渲染",
	["ui.settings.visual.note_scale_x"] = "物件横向缩放",
	["ui.settings.visual.note_scale_y"] = "物件纵向缩放",
	["ui.settings.visual.background"] = "背景与画布",
	["ui.settings.visual.bg_darken"] = "背景暗化程度",
	["ui.settings.visual.timeline_zoom"] = "时间线缩放",
	["ui.settings.visual.offset"] = "视觉偏移",
	["ui.settings.visual.visual_offset"] = "全局渲染偏移 (秒)",

	["ui.settings.project"] = "项目配置",
	["ui.settings.project.no_project"] = "当前未打开任何项目",
	["ui.settings.project.info"] = "项目基本信息",
	["ui.settings.project.name"] = "项目标题",
	["ui.settings.project.artist"] = "曲作者",
	["ui.settings.project.mapper"] = "谱师",
	["ui.settings.project.path"] = "物理路径",

	["ui.settings.editor"] = "编辑器配置",
	["ui.settings.editor.sfx"] = "音效触发策略",
	["ui.settings.editor.sfx_strategy"] = "折线内部音效",
	["ui.settings.editor.sfx_flick_scale"] = "Flick音量随宽度增益",
	["ui.settings.editor.sfx_flick_mul"] = "每轨道增益倍率",
	["ui.file_manager.initial_hint"] = "暂未打开项目",
	["ui.file_manager.open_directory"] = "打开文件夹",
	["ui.file_manager.path_hint"] = "#路径提示",

	["title.audio_manager"] = "音频管理",
	["ui.audio_manager.initial_hint"] = "暂未打开项目, 无音频资源",
	["ui.audio_manager.visual_offset"] = "渲染视觉偏移",
	["ui.audio_manager.visual_offset_tooltip"] = "调整整体渲染相对于音频的偏移量(ms)，正值代表物件提前显示",
	["ui.audio_manager.audio_tracks"] = "音轨列表",

	["title.beatmap_manager"] = "谱面管理",
	["ui.beatmap_manager.initial_hint"] = "暂未打开项目, 无谱面资源",
	["ui.beatmap_manager.beatmaps"] = "谱面列表",

	-- 带格式化参数的文本 (TR_FMT)
	-- 大括号 {} 的位置和数量须和代码里的逻辑一致
	-- ["title.fps: {}"] = "帧率: {}",

	-- 支持调整参数顺序 (fmt 库特性)
	-- 原文: "Position: {}, {}"
	-- ["info.position: {}, {}"] = "位置: X={}, Y={}",

	-- tips,长文本
	["tips.welcome"] = "欢迎使用MusicMapMaker!",
}
