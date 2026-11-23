#pragma once

#include "config/BeatLinePalette.h"
#include "config/NotePalette.h"

#include <nlohmann/json_fwd.hpp>
#include <string>

namespace MMM::Config
{
/// @brief 编辑器设置与独立分享文件共用的完整配色方案。
/// @details 颜色按值保存，应用一次配置或复制分享方案时不借用上层设置对象。
struct ColorPaletteScheme {
    /// @brief 方案显示名称。
    std::string name{ "Palette" };

    /// @brief 完整物件颜色，顺序与工具栏物件颜色槽位一致。
    NoteColorPalette noteColors{};

    /// @brief 完整分拍线颜色。
    BeatLineColorPalette beatLineColors{};
};

/// @brief 将调色盘方案序列化为 JSON。
void to_json(nlohmann::json& json, const ColorPaletteScheme& scheme);
/// @brief 从 JSON 读取调色盘方案。
void from_json(const nlohmann::json& json, ColorPaletteScheme& scheme);

}  // namespace MMM::Config
