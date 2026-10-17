#pragma once

#include "mmm/timing/Timing.h"

#include <cstddef>
#include <string_view>

namespace MMM::Canvas
{
/// @brief 时间线持久化能力描述；只决定警告，不限制编辑权限。
/// @note 字符串仅引用静态字面量，排队提示不依赖路径缓冲区生命周期。
/// @note 此描述不是导出转换策略，保存行为仍由对应格式导出器负责。
struct TimelineFormatSupport {
    /// @brief 对应文件扩展名；空值表示本次需求未限制的格式。
    std::string_view extension;
    /// @brief 提示中列出的可保存时间点类型。
    std::string_view supportedTypes;
    /// @brief 是否可保存 Scroll；BPM 在所有已列格式中均受支持。
    bool supportsScroll{ true };
    /// @brief 是否可保存 Jump 和 HS，两者共享当前格式能力。
    bool supportsJumpAndHs{ true };
    /// @brief 是否可持久化冻结音频的 STOP 等待。
    // 原生 MMM 与 BMS 保留等待；其余格式不得把 STOP 冒充成 SV。
    bool supportsStop{ true };

    /// @brief 查询类型能否随原格式保存，禁止据此阻止用户提交。
    /// @param effect 本次实际修改的时间点类型。
    /// @return 格式未列入限制时保留已有编辑行为。
    /// @note 判断采用类型而非 UI 标签，Scroll 显示为 SV 时结果仍一致。
    [[nodiscard]] constexpr bool supports(TimingEffect effect) const
    {
        // 显式匹配枚举，避免将新增类型误归为某个已知类型。
        switch ( effect ) {
        case TimingEffect::BPM: return true;
        case TimingEffect::STOP: return supportsStop;
        case TimingEffect::SCROLL: return supportsScroll;
        case TimingEffect::JUMP:
        case TimingEffect::HS: return supportsJumpAndHs;
        }
        return true;
    }
};

/// @brief 从快照路径识别格式能力，不构造文件系统路径也不访问磁盘。
/// @param path 当前谱面路径；支持大小写不同的扩展名。
/// @return IMD、osu、mc 的明确能力；mmm 与其他格式保持无限制。
/// @warning UI 提交路径：仅扫描路径字符串，禁止改为文件系统查询。
/// @note 使用当前打开文件的格式，而非全局保存偏好，提示才具有明确来源。
/// @note 扩展名不存在时不报错，也不把未保存的新谱面视为受限格式。
[[nodiscard]] inline constexpr TimelineFormatSupport timelineFormatSupport(
    std::string_view path)
{
    // 只比较文件名最后一个点之后的内容，目录中的点不是扩展名。
    const auto dot   = path.find_last_of('.');
    const auto slash = path.find_last_of("/\\");
    if ( dot == std::string_view::npos ||
         (slash != std::string_view::npos && dot < slash) )
        return {};
    const auto extension = path.substr(dot + 1);
    // 扩展名属于 ASCII，不使用可能分配或依赖全局 locale 的转换。
    const auto equals = [extension](std::string_view expected) {
        if ( extension.size() != expected.size() ) return false;
        for ( std::size_t i = 0; i < extension.size(); ++i ) {
            const char value = extension[i];
            const char lower = value >= 'A' && value <= 'Z'
                                   ? static_cast<char>(value + ('a' - 'A'))
                                   : value;
            if ( lower != expected[i] ) return false;
        }
        return true;
    };
    // 保存能力仅覆盖用户指定的三种格式，不推测其他导出器的能力。
    if ( equals("imd") ) return { ".imd", "BPM", false, false, false };
    if ( equals("osu") ) return { ".osu", "BPM、SCROLL", true, false, false };
    if ( equals("mc") )
        return { ".mc", "BPM、SCROLL、JUMP、HS", true, true, false };
    if ( equals("bms") || equals("bme") || equals("bml") || equals("pms") )
        return { ".bms", "BPM、STOP", false, false, true };
    return {};
}
}  // namespace MMM::Canvas
