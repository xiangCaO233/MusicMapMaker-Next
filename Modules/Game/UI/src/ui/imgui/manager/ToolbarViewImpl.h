#pragma once

#include "common/NoteColor.h"
#include "config/ColorPaletteScheme.h"
#include "config/EditorConfig.h"
#include "ui/imgui/manager/BeatLineDisplayModeHistory.h"
#include "ui/imgui/manager/ToolbarView.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace MMM::UI
{
/// @brief 调色盘弹窗当前显示的编辑页。
enum class ToolbarView::PaletteTab {
    Note,
    BeatLine,
};

/// @brief 工具栏各编辑会话的私有实现，所有会话均由 UI 线程独占。
struct ToolbarView::Impl {
    /// @brief 音效面板使用的跨帧配置视图与连续滑动草稿。
    struct SoundEffectDraft {
        /// @brief 是否显示音效分区与逐轨混音控制工具。
        bool m_open{ false };
        /// @brief 本帧音效工具按钮的屏幕左上角坐标，用于定位弹层。
        glm::vec2 m_anchor{ 0.0f, 0.0f };
        /// @brief 音效工具复用的配置值，只有配置修订变化时复制完整对象。
        Config::EditorConfig m_cachedConfig;
        /// @brief 配置缓存的修订号；最大值保证首次显示时获取权威值。
        std::uint64_t m_configRevision{
            std::numeric_limits<std::uint64_t>::max()
        };
        /// @brief 绑定分类增益草稿是否已从配置初始化。
        bool m_gainDraftInitialized{ false };
        /// @brief 未绑定音效文件的实时增益草稿。
        float m_unboundGainDraft{ 1.0f };
        /// @brief 已绑定音效文件的实时增益草稿。
        float m_boundGainDraft{ 1.0f };
        /// @brief 节拍器增益拖动草稿，避免异步配置回读打断手势。
        float m_metronomeGainDraft{ 1.0f };

        /// @brief 使旧应用服务的修订基线失效，下一次绘制重新读配置。
        /// @warning 每帧绑定服务的路径只在指针变化时调用，无配置读取。
        void invalidateConfig()
        {
            m_configRevision = std::numeric_limits<std::uint64_t>::max();
        }

        /// @brief 丢弃上次手势留下的草稿，下一次绘制重新同步权威值。
        void restartEditing()
        {
            m_gainDraftInitialized = false;
            invalidateConfig();
        }

        /// @brief 初始化或更新未被手势占用的草稿。
        /// @warning 可见音效面板每帧调用，仅同步标量值。
        void syncGains(bool hasActiveControl)
        {
            if ( m_gainDraftInitialized && hasActiveControl ) return;
            // 连续手势期间只使用本地值，避免异步回读使滑条跳回。
            const auto& gains      = m_cachedConfig.settings.sfxConfig;
            m_unboundGainDraft     = gains.unboundHitSfxGain;
            m_boundGainDraft       = gains.boundHitSfxGain;
            m_metronomeGainDraft   = gains.editorMetronomeGain;
            m_gainDraftInitialized = true;
        }
    };

    /// @brief 桌面和移动入口共用配色方案、输入缓冲及导入确认状态。
    struct PaletteEditingSession {
        /// @brief 当前调色盘选择来源。
        enum class PaletteSelectionKind {
            InheritSoftwareDefault,  ///< 继承软件默认调色方案。
            SkinDefault,             ///< 使用当前皮肤默认色盘。
            Custom                   ///< 使用用户保存的自定义色盘。
        };

        /// @brief 高级方案管理与快捷调色分开，重新打开默认回到手指友好页。
        bool m_mobileDetails{};
        /// @brief 是否显示调色盘弹窗。
        bool m_open{ false };
        /// @brief 本帧调色盘按钮的屏幕左上角坐标，用于定位弹窗。
        glm::vec2 m_anchor{ 0.0f, 0.0f };
        /// @brief 调色盘弹窗上一帧宽度，用于防止视口越界。
        float m_colorPopupWidth{ 360.0f };
        /// @brief 调色盘弹窗上一帧高度，用于防止视口越界。
        float m_colorPopupHeight{ 360.0f };
        /// @brief 调色盘弹窗当前显示的标签页。
        PaletteTab m_activePaletteTab{ PaletteTab::Note };
        /// @brief 当前调色盘正在编辑的颜色槽位。
        Logic::NoteColorSlot m_activeColorSlot{ Logic::NoteColorSlot::Tap };
        /// @brief 当前调色盘缓存颜色。
        std::array<glm::vec4, Logic::NOTE_COLOR_SLOT_COUNT> m_paletteColors{};
        /// @brief 当前分拍线调色盘正在编辑的颜色槽位。
        std::size_t m_activeBeatLineColorSlot{ 0 };
        /// @brief 当前分拍线调色盘缓存颜色。
        std::array<glm::vec4, Config::BEAT_LINE_COLOR_PALETTE_SLOT_COUNT>
            m_beatLinePaletteColors{};
        /// @brief 当前方案是否覆盖皮肤分拍线配色。
        bool m_overrideBeatLinePalette{ false };
        /// @brief 调色盘颜色是否已从皮肤初始化。
        bool m_colorPaletteInitialized{ false };
        /// @brief 颜色选择器是否使用 HSV 显示模式。
        bool m_colorPickerUseHsv{ false };
        /// @brief 当前活动颜色的 HEX 输入缓冲区。
        std::array<char, 16> m_colorHexBuffer{};
        /// @brief HEX 输入缓冲区当前对应的标签页。
        PaletteTab m_colorHexBufferTab{ PaletteTab::Note };
        /// @brief HEX 输入缓冲区当前对应的颜色槽位索引。
        std::size_t m_colorHexBufferSlot{ 0 };
        /// @brief HEX 输入框是否正处于编辑状态。
        bool m_colorHexInputActive{ false };
        /// @brief 当前选中的持久化调色盘方案索引；-1 表示未使用保存方案。
        int m_activePaletteSchemeIndex{ -1 };
        /// @brief 当前调色盘选择来源，用于区分内置项和可管理的自定义方案。
        PaletteSelectionKind m_activePaletteSelection{
            Impl::PaletteEditingSession::PaletteSelectionKind::SkinDefault
        };
        /// @brief 等待删除确认的自定义调色盘方案索引。
        std::optional<std::size_t> m_pendingDeletePaletteSchemeIndex;
        /// @brief 当前方案名称校验错误的翻译键；为空表示无错误。
        std::string m_paletteSchemeErrorKey;
        /// @brief 当前配色方案导出结果的翻译键；为空表示尚无结果。
        std::string m_paletteExportStatusKey;
        /// @brief 最近一次配色方案导出是否成功。
        bool m_paletteExportSucceeded{ false };
        /// @brief 等待用户确认名称的导入调色方案。
        std::optional<Config::ColorPaletteScheme>
            m_pendingImportedPaletteScheme;
        /// @brief 导入方案名称编辑缓冲区。
        std::array<char, 96> m_importPaletteSchemeNameBuffer{};
        /// @brief 当前导入流程错误的翻译键；为空表示无错误。
        std::string m_paletteImportErrorKey;
        /// @brief 当前导入结果的翻译键；为空表示尚无结果。
        std::string m_paletteImportStatusKey;
        /// @brief 最近一次配色方案导入是否成功。
        bool m_paletteImportSucceeded{ false };
        /// @brief 当前调色盘方案名称编辑缓冲区。
        std::array<char, 96> m_paletteSchemeNameBuffer{};
        /// @brief 上次已应用项目调色方案的项目与方案组合键。
        std::string m_lastAppliedProjectPaletteKey;
    };

    /// @brief 独立缓存音效配置及正在编辑的值。
    SoundEffectDraft m_soundEffects;
    /// @brief 持有 ImGui 使用的固定输入缓冲，地址在窗口生命周期内稳定。
    PaletteEditingSession m_palette;
    /// @brief 最近使用的两个分拍线显示模式，用于快捷键双向切换。
    BeatLineDisplayModeHistory m_beatLineDisplayModeHistory;
};

}  // namespace MMM::UI
