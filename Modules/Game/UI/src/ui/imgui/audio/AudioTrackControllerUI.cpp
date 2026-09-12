#include "ui/imgui/audio/AudioTrackControllerUI.h"
#include "audio/AudioManager.h"
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "imgui.h"
#include "logic/EditorEngine.h"
#include "ui/UIManager.h"
#include "ui/utils/UIWidgetUtils.h"
#include <cfloat>
#include <cmath>

namespace MMM::UI
{

/// @brief 构造音轨控制器使用的稳定 ImGui 视图名称。
/// @param trackId 音频资源或音效池的稳定标识。
/// @return 带控制器命名前缀的内部窗口 ID。
///
/// 返回值用于 ### 后缀，不面向用户显示；轨道改名不会改变窗口停靠身份。
std::string AudioTrackControllerUI::makeViewName(const std::string& trackId)
{
    return "TrackController_" + trackId;
}

/// @brief 将轨道类型转换为工作区持久化名称。
/// @param type 控制器轨道类型。
/// @return 稳定的英文工作区枚举文本。
const char* AudioTrackControllerUI::trackTypeToWorkspaceName(TrackType type)
{
    return type == TrackType::Effect ? "Effect" : "Main";
}

/// @brief 将工作区持久化名称还原为轨道类型。
/// @param name 工作区记录的英文类型名称。
/// @return Effect 精确匹配音效轨，其余值兼容回退为主轨。
AudioTrackControllerUI::TrackType
AudioTrackControllerUI::workspaceNameToTrackType(const std::string& name)
{
    return name == "Effect" ? TrackType::Effect : TrackType::Main;
}

/// @brief 创建指定音频资源的控制器视图。
/// @param trackId 稳定资源标识，用于配置查找和窗口 ID。
/// @param trackName 面向用户显示的轨道名称。
/// @param type 主轨或音效轨类型。
AudioTrackControllerUI::AudioTrackControllerUI(const std::string& trackId,
                                               const std::string& trackName,
                                               TrackType          type)
    : IUIView(trackName)
    , m_trackId(trackId)
    , m_trackName(trackName)
    , m_type(type)
{
}

/// @brief 请求下一次显示时停靠到指定 Dock 节点。
/// @param dockId 目标 ImGui Dock 节点 ID，0 表示不改变停靠位置。
/// @warning UI 状态写入；实际 Docking 只在后续 update 开始时提交。
void AudioTrackControllerUI::requestDockTo(ImGuiID dockId)
{
    // 保留请求直到 ImGui 确认窗口已进入某个 Dock 节点。
    m_pendingDockId = dockId;
}

/// @brief 请求下一次更新时将音轨控制器窗口聚焦到前台。
/// @warning UI 状态写入；请求在下一次 update 中消费一次。
void AudioTrackControllerUI::requestFocus()
{
    m_shouldFocusNextFrame = true;
}

/// @brief 绘制音轨控制器并把用户修改提交到逻辑或音效池。
/// @param sourceManager UI 管理器，用于工程切换占位和分析工具入口。
///
/// 项目音频先复制 AudioTrackConfig 作为本帧编辑草稿，控件完成后仅在 changed
/// 为真时提交整份配置。皮肤音效没有项目配置时直接更新 AudioManager 音效池，
/// 并根据皮肤资源表决定设置是否持久。
///
/// 轨道类型差异：
/// - 主轨提供音量、静音、速度、音调、分析工具和 EQ；
/// - 项目音效轨使用项目 AudioTrackConfig，但不显示主轨专属控件；
/// - 皮肤音效可能没有项目配置，音量和静音直接来自 SFX Pool；
/// - 关闭音效控制器会暂停对应预览，关闭主轨控制器不改变播放状态。
///
/// 状态边界：
/// - pendingDockId 只在 ImGui 报告已停靠后清除；
/// - focus 请求提交一次后立即清除；
/// - 工程切换时只绘制占位，不读取项目资源；
/// - 本地配置草稿只在当前帧有效；
/// - 用户变更最终经逻辑命令或 AudioManager 专用接口写入。
/// - 子区域只修改引用参数和 changed 标志，不直接持久化配置；
/// - Clay 描述树在本帧构建并立即渲染，不跨帧保存控件引用；
/// - EQ 在 Clay 内容之后使用 ImGui/ImPlot 单独绘制；
/// - 无项目配置的主轨以禁用态展示 EQ，避免伪造可编辑状态；
/// - 未发生变更时不产生逻辑命令或音频池写入。
/// @warning UI 热路径：窗口打开时每帧执行；不得扫描文件、解码音频或等待逻辑
/// 命令完成。项目资源遍历限于当前轻量音频资源列表。
void AudioTrackControllerUI::update(UIManager* sourceManager)
{
    if ( !m_isOpen ) {
        // 关闭音效轨窗口时停止预览，防止不可见控制器继续发声。
        if ( m_type == TrackType::Effect ) {
            Audio::AudioManager::instance().pauseSoundEffect(m_trackId);
        }
        return;
    }

    // 布局指标按当前 DPI 缓存解析，窗口移动到新显示器后可立即适配。
    float dpiScale = Config::AppConfig::instance().getWindowContentScale();
    const auto& layoutMetrics = getLayoutMetrics(dpiScale);

    if ( m_pendingDockId != 0 ) {
        // Always 条件保持请求有效，直到窗口实际报告为已停靠。
        ImGui::SetNextWindowDockID(m_pendingDockId, ImGuiCond_Always);
    }
    if ( m_shouldFocusNextFrame ) {
        // 聚焦请求只消费一次，避免持续抢夺其他窗口焦点。
        ImGui::SetNextWindowFocus();
        m_shouldFocusNextFrame = false;
    }
    ImGui::SetNextWindowSizeConstraints(getMinWindowSize(dpiScale),
                                        ImVec2(FLT_MAX, FLT_MAX));
    // 默认尺寸只在首次使用生效，之后尊重用户保存的窗口布局。
    ImGui::SetNextWindowSize(ImVec2(400, 500), ImGuiCond_FirstUseEver);
    // 可见标题允许改名，### 后缀以 trackId 维持内部窗口身份。
    std::string windowTitle =
        m_trackName + "###" + AudioTrackControllerUI::makeViewName(m_trackId);
    const bool wasOpenBeforeBegin = m_isOpen;
    const bool opened = ImGui::Begin(windowTitle.c_str(), &m_isOpen);
    FeedbackCurrentWindowCloseButton(wasOpenBeforeBegin, &m_isOpen);
    if ( opened ) {
        // 工程切换时不读取可能正在替换的项目和音频资源容器。
        if ( sourceManager && sourceManager->isProjectTransitionInProgress() ) {
            Utils::renderProjectTransitionPlaceholder();
            ImGui::End();
            return;
        }

        // 下列服务引用只在本帧使用，不延长工程或音频对象生命周期。
        auto& audio   = Audio::AudioManager::instance();
        auto& engine  = Logic::EditorEngine::instance();
        auto* project = engine.getCurrentProject();

        if ( m_pendingDockId != 0 && ImGui::IsWindowDocked() ) {
            // ImGui 已接受停靠后清除请求，允许用户随后自由拖出窗口。
            m_pendingDockId = 0;
        }
        // Clay 当前上下文切换到本控制器持有的布局上下文。
        CLayWrapperCore::instance().makeCurrent(m_layoutCtx.context);
        // 默认值保证资源暂不可用时控件仍能以中性状态呈现。
        float volume = 0.5f;
        float speed  = 1.0f;
        float pitch  = 0.0f;
        bool  muted  = false;

        AudioTrackConfig  editedConfig;
        AudioTrackConfig* config = nullptr;
        if ( project ) {
            // 以稳定 ID 查找项目资源；编辑草稿按值复制，不直接改项目容器。
            for ( const auto& res : project->m_audioResources ) {
                if ( res.m_id == m_trackId ) {
                    // 首个 ID 匹配即为目标；资源 ID 在项目内应保持唯一。
                    editedConfig = res.m_config;
                    config       = &editedConfig;
                    break;
                }
            }
        }

        if ( config ) {
            // 项目配置是主轨和已导入音效轨的权威初值。
            volume = config->volume;
            speed  = config->playbackSpeed;
            pitch  = config->playbackPitch;
            muted  = config->muted;
            if ( m_type == TrackType::Main ) {
                // eqEnabled 为 false 时强制显示 None，避免旧 preset
                // 被误认为生效。
                m_currentPreset =
                    config->eqEnabled
                        ? static_cast<Audio::EQPreset>(config->eqPreset)
                        : Audio::EQPreset::None;
            }
        } else {
            if ( m_type == TrackType::Main ) {
                // 主轨缺少项目资源时没有可编辑 EQ，恢复无预设表现。
                m_currentPreset = Audio::EQPreset::None;
            } else {
                // 皮肤音效不属于项目资源，直接从运行时 SFX 池读取当前值。
                volume = audio.getSFXPoolVolume(m_trackId);
                muted  = audio.getSFXPoolMute(m_trackId);
            }
        }

        // 各子区域共享变更标志，本帧末尾只提交一次配置更新。
        bool changed = false;

        // 每帧清空并重建轻量 Clay 描述树，不保留指向上一帧控件的引用。
        m_contentVBox.clear();
        m_contentVBox
            .setSpacing(
                static_cast<uint16_t>(std::ceil(layoutMetrics.contentSpacing)))
            .setPadding(
                static_cast<uint16_t>(std::ceil(layoutMetrics.contentPadding)),
                static_cast<uint16_t>(std::ceil(layoutMetrics.contentPadding)),
                static_cast<uint16_t>(std::ceil(layoutMetrics.contentPadding)),
                static_cast<uint16_t>(std::ceil(layoutMetrics.contentPadding)));
        size_t rowIndex = 0;

        // 所有控制行共享标签宽度，使滑块和输入框在纵向对齐。
        float maxLabelW = layoutMetrics.labelWidth;

        buildVolumeSection(
            m_contentVBox, rowIndex, maxLabelW, volume, muted, changed);

        if ( m_type == TrackType::Main ) {
            // 主轨额外提供速度、音调和分析入口；可用宽度扣除全部行装饰。
            float availWidgetW = ImGui::GetContentRegionAvail().x - maxLabelW -
                                 layoutMetrics.contentPadding * 2.0f -
                                 layoutMetrics.rowPaddingX * 2.0f -
                                 layoutMetrics.rowSpacing;
            buildSpeedAndPitchSection(m_contentVBox,
                                      rowIndex,
                                      maxLabelW,
                                      availWidgetW,
                                      speed,
                                      pitch,
                                      changed);
            buildAnalysisButtons(m_contentVBox, rowIndex, sourceManager);
        }

        if ( m_type == TrackType::Effect ) {
            // 音效轨以独立预览区代替主轨的速度、音调和分析工具。
            buildEffectPreviewSection(m_contentVBox, rowIndex, maxLabelW);
        }

        // 预留微量顶部空间，防止某些停靠布局下内容盖住 Tab。
        ImGui::Dummy(ImVec2(0, 2 * dpiScale));

        // Clay 返回实际高度；推进 ImGui 游标后，后续 EQ 区从其下方开始。
        ImVec2 startPos = ImGui::GetCursorScreenPos();
        ImVec2 sz       = m_contentVBox.renderInCurrent(
            startPos, { ImGui::GetContentRegionAvail().x, 0 });
        ImGui::SetCursorScreenPos({ startPos.x, startPos.y + sz.y });

        // EQ 使用 ImPlot/ImGui 原生布局，不嵌入 Clay 的测量与裁剪流程。
        if ( m_type == TrackType::Main ) {
            // 没有项目配置时仍绘制禁用界面，保持窗口结构和功能提示稳定。
            AudioTrackConfig unavailableConfig;
            if ( !config ) ImGui::BeginDisabled();
            renderEQSection(config ? *config : unavailableConfig, changed);
            if ( !config ) ImGui::EndDisabled();
        }

        // 没有控件变更时不写音频池，也不向逻辑线程发送冗余命令。
        if ( changed ) {
            if ( config ) {
                // 把控件结果写回本地草稿，随后整份配置随命令按值发送。
                config->volume        = volume;
                config->muted         = muted;
                config->playbackSpeed = speed;
                config->playbackPitch = pitch;
            }

            if ( m_type == TrackType::Effect && !config ) {
                // 皮肤声明的音效为永久池资源，动态音效则采用非永久更新。
                bool  isPermanent = true;
                auto& skinData    = Config::SkinManager::instance().getData();
                // audioPaths 是皮肤静态资源清单，不在这里访问文件系统确认路径。
                if ( skinData.audioPaths.count(m_trackId) == 0 ) {
                    isPermanent = false;
                }
                audio.setSFXPoolVolume(m_trackId, volume, isPermanent);
                audio.setSFXPoolMute(m_trackId, muted, isPermanent);
            }

            if ( config ) {
                // 项目配置只能经逻辑命令修改，避免 UI 与逻辑线程并发写容器。
                engine.pushCommand(Logic::CmdUpdateAudioResourceConfig{
                    .id     = m_trackId,
                    .config = *config,
                });
            }
        }
    }
    // Begin 无论是否展开内容都必须配对 End。
    ImGui::End();
}

}  // namespace MMM::UI
