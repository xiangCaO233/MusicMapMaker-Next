#pragma once

#include "graphic/imguivk/VKContext.h"
#include "graphic/system/SystemTheme.h"
#include "graphic/theme/ImGuiThemeRegistry.h"
#include <atomic>
#include <chrono>
#include <string_view>

namespace MMM::Graphic
{
/// @brief 正式界面与启动窗口共用的字体 atlas、主题注册表及动态外观。
/// @details 除字体重建标志外，外观状态只由持有 ImGui 上下文的线程访问。
/// 字体和主题操作只借用渲染器已准备好的上下文，无权销毁设备或窗口。
struct VKContext::Appearance {
    /// @brief 加载启动期系统首选字体。
    /// @warning 启动低频路径：会查询系统字体并读取字体文件，禁止放入每帧
    /// 渲染路径。
    void setupBootstrapFonts();

    /// @brief 应用不依赖皮肤资源的启动期最小样式。
    /// @warning 启动低频路径：只修改 ImGui 样式状态。
    void applyBootstrapTheme();

    /// @brief 将全部编译期主题注册为独立实例。
    void registerBuiltInThemes();

    /// @brief 加载正式皮肤字体并记录本 atlas 的固定缩放比例。
    void setupFonts();
    /// @brief 在调用方提供的设备达到空闲后重新建立 atlas。
    /// @warning 低频资源更新入口，会等待 GPU 并执行字体文件读取。
    void rebuildFonts(vk::Device device);
    /// @brief 用 atlas 现有缩放恢复字体设置，不触发纹理重新打包。
    void updateFontScales();
    /// @brief 发布字体重建脏位，实际重建由渲染线程负责。
    void requestFontRebuild();
    /// @brief 在 NewFrame 之前消费字体重建脏位。
    /// @warning 每帧检查一次；仅收到请求时允许阻塞等待 GPU。
    void checkAndRebuildFonts(vk::Device device);
    /// @brief 自动主题只在外观变化时重新应用。
    /// @warning 每帧可调用，平台状态检测每秒至多一次。
    void checkAndApplySystemTheme();
    /// @brief 按设置选择并应用已注册的主题。
    void applyTheme();
    /// @brief 重载 Lua 插件和内置主题并应用当前选择。
    ThemePluginReloadResult reloadPlugins();
    /// @brief 保存插件启用状态并重新加载。
    bool setPluginEnabled(std::string_view pluginId, bool enabled);
    /// @brief 借用长生命周期的主题注册表。
    const ImGuiThemeRegistry& getThemeRegistry() const;

private:
    /// @brief 字体重建请求标志
    /// @warning
    /// 热路径/原子：每帧检查、设置页写入；只作为脏位，禁止在此承载字体资源生命周期同步。
    std::atomic<bool> m_fontRebuildRequested{ false };

    /// @brief 最近一次自动模式实际应用的系统主题。
    SystemTheme m_appliedSystemTheme{};

    /// @brief 下一次允许刷新平台主题状态的单调时钟时间点。
    /// @warning 渲染热路径每帧读取，用于把平台查询限制为每秒一次。
    std::chrono::steady_clock::time_point m_nextSystemThemeCheck{};

    /// @brief 当前图形上下文持有的内置与 Lua 插件主题实例。
    ImGuiThemeRegistry m_themeRegistry;

    /// @brief 当前 ImGui 字体 atlas 生命周期内固定的主字体倍率。
    /// @warning 运行时设置变更不得直接修改该值，否则 ImGui 1.92
    /// dynamic atlas 可能在渲染中重新打包字体纹理。
    float m_fontAtlasScaleMain{ 1.0f };

    /// @brief 当前 ImGui 字体 atlas 生命周期内固定的字体 UI 缩放。
    /// @warning 运行时设置变更不得直接修改该值，避免已有 ImFont
    /// 在渲染中切换到新的动态烘焙尺寸。
    float m_fontAtlasBaseScale{ 1.0f };
};
}  // namespace MMM::Graphic
