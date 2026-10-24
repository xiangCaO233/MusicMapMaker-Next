#pragma once

#include "config/skin/SkinData.h"
#include "config/skin/SkinTypes.h"
#include "translation/Translation.h"
#include <cstdint>
#include <filesystem>
#include <sol/forward.hpp>
#include <string>
#include <utility>
#include <vector>

struct ImFont;

namespace MMM
{
namespace Config
{
/// @brief 按值持有当前皮肤数据与翻译器，保留热路径内联访问。
class SkinManager
{
public:
    static SkinManager& instance();

    ///@brief 获取常用分拍列表
    inline const std::vector<int>& getCommonDivisors() const
    {
        return m_data.commonDivisors;
    }

    ///@brief 获取翻译器
    inline Translation::Translator& getTranslator() { return m_translator; }

    /// @brief 使用应用默认翻译资源载入皮肤。
    /// @param luaFilePath 皮肤入口 Lua 文件路径。
    /// @return 皮肤及默认翻译均载入成功时返回 true。
    bool loadSkin(const std::string& luaFilePath);

    /// @brief 使用指定默认翻译资源目录载入皮肤。
    /// @param luaFilePath 皮肤入口 Lua 文件路径。
    /// @param translationsRoot 默认翻译文件所在目录。
    /// @return 皮肤及默认翻译均载入成功时返回 true。
    /// @warning 该重载用于测试和受控资源环境，常规调用应使用单参数入口。
    bool loadSkin(const std::string&           luaFilePath,
                  const std::filesystem::path& translationsRoot);

    ///@brief 直接获取皮肤数据
    const SkinData& getData() const { return m_data; }

    /// @brief 获取皮肤兼容旧调用方的默认主题。
    /// @return 亮色分支主题；无法识别系统外观时也使用该分支。
    const std::string& getDefaultTheme() const
    {
        return m_data.defaultThemes.light;
    }

    /// @brief 获取皮肤为指定系统外观推荐的主题。
    /// @param appearance 系统亮暗外观分支。
    /// @return 对应分支的主题名称。
    const std::string& getDefaultTheme(SkinThemeAppearance appearance) const
    {
        return appearance == SkinThemeAppearance::Dark
                   ? m_data.defaultThemes.dark
                   : m_data.defaultThemes.light;
    }

    ///@brief 获取字体路径
    std::filesystem::path getFontPath(const std::string& key);

    ///@brief 获取音频路径
    std::filesystem::path getAudioPath(const std::string& key);

    /// @brief 获取音效文件开头到有效出声点的延迟。
    /// @param key 音频 ID。
    /// @return 延迟，单位为秒；不存在时返回 0。
    double getAudioLeadInSeconds(const std::string& key) const;

    ///@brief 获取资产路径
    std::filesystem::path getAssetPath(const std::string& key);

    /// @brief 获取普通纹理或整个序列的独立视觉倍率。
    /// @param key 与 assets 一致的完整点分键。
    /// @return 正有限倍率；缺失或无效声明返回 1。
    /// @warning 渲染热路径只查询缓存，不进行 Lua 访问或资源加载。
    float getTextureScale(const std::string& key) const;

    /// @brief 通过已分配的序列帧 ID 查询该动画的视觉倍率。
    /// @param textureId 当前皮肤序列中的帧 ID。
    /// @return 未声明倍率或不属于序列的 ID 返回 1。
    /// @warning 逐图元热路径只查数值键，不遍历序列或构造字符串。
    float getEffectTextureScale(std::uint32_t textureId) const;

    ///@brief 获取特效序列帧
    const SkinData::EffectSequence* getEffectSequence(
        const std::string& key) const;

    ///@brief 获取特效基础帧率
    float getEffectBaseFps() const { return m_data.effectBaseFps; }

    /// @brief 获取打击特效纹理布局方式。
    HitEffectLayoutMode getHitEffectLayoutMode() const
    {
        return m_data.effects.hitEffect.layout;
    }

    ///@brief 获取画布配置
    const SkinData::CanvasConfig& getCanvasConfig(
        const std::string& canvasName);

    ///@brief 获取发光特效的渲染轮次
    int   getGlowPasses() const { return m_data.effects.glow.passes; }
    float getGlowIntensity() const { return m_data.effects.glow.intensity; }

    ///@brief 获取布局配置
    std::string getLayoutConfig(const std::string& key);

    ///@brief 获取数值配置
    float getValue(const std::string& key, float defaultValue = 0.0f);

    ///@brief 获取颜色配置
    Color getColor(const std::string& key);

    ///@brief 获取运行时字体
    ImFont* getFont(const std::string& key);

    ///@brief 获取所有可用的 ASCII 字体
    const std::vector<std::pair<std::string, std::filesystem::path>>&
    getAsciiFonts() const
    {
        return m_data.asciiFonts;
    }

    ///@brief 获取所有可用的 CJK 字体
    const std::vector<std::pair<std::string, std::filesystem::path>>&
    getCjkFonts() const
    {
        return m_data.cjkFonts;
    }

    ///@brief 设置运行时字体 (供渲染层初始化时调用)
    void setFont(const std::string& key, ImFont* font);

    /// @brief 清空运行时字体指针缓存。
    void clearRuntimeFonts();

private:
    ///@brief 皮肤数据
    SkinData m_data;

    ///@brief 翻译器
    Translation::Translator m_translator;

    void parseAssetsRecursive(const sol::table&  currentTable,
                              const std::string& prefix);

    /// @brief 递归解析音频配置表。
    /// @param currentTable 当前处理的 Lua 表。
    /// @param prefix 键前缀。
    void parseAudiosRecursive(const sol::table&  currentTable,
                              const std::string& prefix);

    /**
     * @brief 递归解析颜色配置表
     * @param currentTable 当前处理的 Lua 表
     * @param prefix 键前缀（用于处理嵌套，如 "preview"）
     */
    void parseColorsRecursive(const sol::table&  currentTable,
                              const std::string& prefix);

    /**
     * @brief 递归解析布局配置表
     * @param currentTable 当前处理的 Lua 表
     * @param prefix 键前缀（用于处理嵌套，如 "side_bar"）
     */
    void parseLayoutRecursive(const sol::table&  currentTable,
                              const std::string& prefix);

    /**
     * @brief 递归解析数值配置表
     * @param currentTable 当前处理的 Lua 表
     * @param prefix 键前缀
     */
    void parseValuesRecursive(const sol::table&  currentTable,
                              const std::string& prefix);

    SkinManager() = default;
};

}  // namespace Config
}  // namespace MMM
