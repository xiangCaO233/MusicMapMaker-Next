#pragma once

#include "config/skin/SkinDataTypes.h"
#include "config/skin/SkinTypes.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct ImFont;

namespace MMM::Config
{
/// @brief 皮肤加载结果，所有资源描述和配置按值持有。
/// @details 嵌套类型名保留为别名，旧源码继续使用 SkinData::EffectSequence
/// 等名称。 运行时字体仍是非拥有指针；拆分不改变字体生命周期或数据复制方式。
struct SkinData {
    /// @brief 皮肤路径
    std::filesystem::path skinPath;

    /// @brief 皮肤名称
    std::string themeName;
    /// @brief 皮肤作者。
    std::string themeAuthor;
    /// @brief 皮肤版本。
    std::string themeVersion;

    /// @brief 颜色表
    std::unordered_map<std::string, Color> colors;

    /// @brief 皮肤按语言 ID 声明的可选翻译覆写文件。
    std::unordered_map<std::string, std::filesystem::path> langOverrideLuaPaths;

    /// @brief 皮肤覆写中默认语言字典不存在、启动时需要提示的字段。
    std::vector<std::string> missingTranslationOverrideFields;

    /// @brief 默认语言不可用时切换的语言 ID。
    std::string fallBackLang{ "zh_cn" };

    /// @brief 皮肤为系统亮暗外观分别推荐的 UI 主题。
    using ThemeBinding = SkinThemeBinding;
    /// @brief 按值保存对应配置，保留原成员位置和默认初始化。
    ThemeBinding defaultThemes;

    /// @brief 字体表
    std::unordered_map<std::string, std::filesystem::path> fontPaths;

    /// @brief 字体套装 (用于在 AppConfig 中切换)
    std::vector<std::pair<std::string, std::filesystem::path>> asciiFonts;
    /// @brief 可选中日韩字体套装。
    std::vector<std::pair<std::string, std::filesystem::path>> cjkFonts;

    /// @brief 运行时字体对象表 (Key: 字体配置ID, Value: ImFont*)
    std::unordered_map<std::string, ImFont*> runtimeFonts;

    /// @brief 资产路径表 (Key: 资产ID, Value: 文件路径)
    std::unordered_map<std::string, std::filesystem::path> assetPaths;

    /// @brief 按完整资产键保存独立视觉倍率，未声明的纹理保持原始尺寸。
    /// @note 序列键作用于整个动画，不改变源图片、图集 UV 或播放时序。
    std::unordered_map<std::string, float> textureScales;

    /// @brief 音频路径表 (Key: 音频ID, Value: 文件路径)
    std::unordered_map<std::string, std::filesystem::path> audioPaths;

    /// @brief 音效文件开头到有效出声点的延迟，单位为秒。
    std::unordered_map<std::string, double> audioLeadInSeconds;

    /// @brief 特效序列帧表
    using EffectSequence = SkinEffectSequence;

    /// @brief 按资产键保存独立的序列帧描述。
    std::unordered_map<std::string, EffectSequence> effectSequences;

    /// @brief 加载时展开的序列帧倍率，供渲染按帧 ID 常量时间查询。
    /// @note 仅缓存非默认倍率；随 SkinData 重建，禁止跨皮肤复用旧 ID。
    std::unordered_map<std::uint32_t, float> effectTextureScales;

    /// @brief 特效序列帧的基础播放帧率。
    float effectBaseFps{ DEFAULT_EFFECT_BASE_FPS };


    /// @brief 保留历史嵌套效果配置类型名。
    using EffectsConfig = SkinEffectsConfig;
    /// @brief 按值保存对应配置，保留原成员位置和默认初始化。
    EffectsConfig effects;

    /// @brief 画布配置结构
    using CanvasConfig = SkinCanvasConfig;

    /// @brief 画布表 (Key: 画布名称, Value: 画布配置)
    std::unordered_map<std::string, CanvasConfig> canvas_configs;
    /// @brief 空画布配置
    CanvasConfig null_canvas_config{};

    /// @brief 布局配置表 (Key: 布局配置ID, Value: 值)
    std::unordered_map<std::string, std::string> layoutConfigs;

    /// @brief 数值配置表
    std::unordered_map<std::string, float> values;

    /// @brief 常用分拍列表
    std::vector<int> commonDivisors;
};


}  // namespace MMM::Config
