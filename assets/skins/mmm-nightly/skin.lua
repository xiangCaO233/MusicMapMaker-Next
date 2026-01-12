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
	},

    fonts = {
        ascii = "fonts/ComicShannsMonoNerdFontPropo-Regular.otf",
        cjk = "fonts/NotoSansMonoCJKsc-Regular.otf",
    },

	-- 资产文件映射
	assets = {
		cursor = "images/cursor/cursor.png",
		btn_play = "images/buttons/play.png",
		btn_pause = "images/buttons/pause.png",
		bg_main = "images/backgrounds/main_menu.jpg",
	},

	-- 布局参数
	layout = {
		windowPadding = 20,
		buttonHeight = 40,
	},
}

-- 自动拼接路径 (在Lua层处理逻辑，减轻C++负担)
function Skin:resolve_paths()
	for k, v in pairs(self.assets) do
		-- 最终: .../mmm-nightly/resources/images/buttons/play.png
		self.assets[k] = self.basePath .. v
	end
end

Skin:resolve_paths()

return Skin
