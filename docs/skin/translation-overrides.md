# 皮肤翻译覆写

应用的默认翻译不再属于任何皮肤，而是随资源包独立存放：

```text
assets/
├── translations/
│   ├── en_us.lua
│   └── zh_cn.lua
└── skins/
    └── <skin-id>/
        └── skin.lua
```

皮肤无需提供完整语言文件。需要改变少量皮肤相关文案时，可以在 `skin.lua`
中声明 `lang_overrides`，只覆写默认字典中已经存在的字段。

## 配置格式

```lua
return {
	meta = {
		name = "Example Skin",
		author = "Example Author",
		version = "1.0",
	},

	lang_overrides = {
		zh_cn = "translations/zh_cn.lua",
		en_us = "translations/en_us.lua",
	},

	-- 其他皮肤配置……
}
```

路径相对于当前皮肤的根目录，也就是 `skin.lua` 所在目录。覆写文件可以放在
皮肤包内任意子路径，不要求位于 `resources/`；指向皮肤目录外部的路径会被
拒绝。建议使用相对路径，确保皮肤包移动和导入后仍可加载。

每份覆写文件必须返回一个由字符串键和字符串值组成的 Lua table：

```lua
return {
	["ui.settings.title"] = "皮肤专属设置",
	["ui.common.confirm"] = "好耶",
}
```

加载顺序如下：

1. 应用从 `assets/translations` 加载完整默认字典。
2. 当前皮肤按语言 ID 加载 `lang_overrides`。
3. 默认字典中已有的同名字段被皮肤文本替换。
4. 未覆写的字段继续使用默认翻译。

`lang_overrides` 是可选配置。不需要自定义文案的皮肤应直接省略该字段。

## 未知字段提示

皮肤不能新增只存在于自身的翻译键。如果覆写文件包含默认字典中不存在的
字段，该字段会被忽略，启动时会弹窗列出完整的语言 ID 和字段名，例如：

```text
zh_cn:ui.example.removed_field
```

弹窗中的“继续”会使用其余有效覆写正常启动。修复时应检查字段拼写，或删除
已经不再由应用使用的字段。

## 旧版配置迁移

旧版皮肤中的 `langs` 配置不再参与翻译加载：

```lua
-- 旧格式，现已忽略
langs = {
	en_us = "lang/en_us.lua",
	zh_cn = "lang/zh_cn.lua",
}
```

升级后的首次启动会在新的两份默认翻译就位后，遍历现有用户皮肤并无条件删除
以下两个旧版固定路径文件，即使用户曾经修改过其内容：

```text
resources/lang/en_us.lua
resources/lang/zh_cn.lua
```

清理完成后会写入一次性迁移标记，后续启动不会重复删除。现有皮肤若要保留
自定义翻译，应将需要的字段整理为较小的覆写文件，放到皮肤包内其他路径，并
改用 `lang_overrides` 声明。

## 完整示例

```text
my-skin/
├── skin.lua
└── translations/
    ├── en_us.lua
    └── zh_cn.lua
```

`skin.lua`：

```lua
return {
	meta = {
		name = "My Skin",
		author = "Skin Author",
		version = "1.0",
	},
	lang_overrides = {
		en_us = "translations/en_us.lua",
		zh_cn = "translations/zh_cn.lua",
	},
	fonts = {},
	assets = {},
	audios = {},
	layout = {},
}
```

`translations/zh_cn.lua`：

```lua
return {
	["ui.settings.title"] = "我的皮肤设置",
}
```
