#include "ui/imgui/manager/SettingsView.h"

#include "config/AppConfig.h"
#include "config/skin/translation/Translation.h"
#include "network/collaboration/CollaborationRoom.h"
#include "ui/UIManager.h"
#include "ui/utils/UIWidgetUtils.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace MMM::UI
{
namespace
{
/// @brief 返回协作目录连接状态对应的本地化文本。
/// @param state 当前目录连接状态。
/// @return 可直接绘制的本地化文本。
///
/// 映射只负责展示，不修改连接状态。未知枚举值与 Idle 共用空闲文案，确保协议
/// 扩展期间设置页仍能显示稳定文本。
/// @warning UI 热路径：协作设置页可见时每帧调用，只有常量分支。
const char* directoryStateText(
    Network::Collaboration::CollaborationDirectoryState state)
{
    // 使用局部别名保持 switch 与网络层枚举的对应关系清晰。
    using State = Network::Collaboration::CollaborationDirectoryState;
    switch ( state ) {
    case State::Connecting:
        return TR("ui.collaboration.directory.connecting").data();
    case State::Connected:
        return TR("ui.collaboration.directory.connected").data();
    case State::Error: return TR("ui.collaboration.directory.error").data();
    case State::Idle:
        // default 防御未来新增状态，不能返回悬空或空文本。
    default: return TR("ui.collaboration.directory.idle").data();
    }
}
}  // namespace

/// @brief 绘制多人协作设置页。
///
/// 页面把服务地址、信令端口和 TLS 作为一组原子端点配置。活动房间期间所有
/// 输入和应用按钮禁用，避免连接中的传输目标被中途替换。应用时先让房间采用
/// 新端点，再写入 AppConfig；持久化失败则同时恢复内存配置和房间端点。
///
/// Clay 只负责设置项的测量和排版，具体输入、折叠标题及反馈卡仍由 ImGui
/// 绘制。折叠状态写入当前 ImGui StateStorage，不进入长期应用配置。
///
/// 输入状态约束：
/// - 地址缓冲区由 SettingsView 持有并保证零结尾；
/// - 信令端口每帧限制在 1 至 65535；
/// - TLS 与地址、端口一起构成不可拆分的服务端点；
/// - 活动房间期间三项输入和应用按钮全部禁用；
/// - 目录状态和错误来自 CollaborationRoom 当前只读状态；
/// - 应用反馈状态保存在视图成员中，直到下一次应用覆盖。
///
/// 提交事务约束：
/// - 点击应用时从 UI 草稿构造独立 endpoint；
/// - 先调用 room->setServerEndpoint 验证并应用运行时端点；
/// - 运行时成功后才更新 AppConfig 内存字段；
/// - AppConfig::save 成功才形成最终提交；
/// - 任一步失败都会恢复旧 AppConfig 字段；
/// - 若房间已经切换，还会用旧值恢复运行时端点。
/// - 回滚不自动重新应用，用户可以修正输入后再次提交；
/// - 端点切换和配置保存都只由明确按钮点击触发；
/// - 设置页不会创建或销毁 CollaborationRoom；
/// - 页面不会在地址编辑过程中发送网络请求；
/// - 活动房间警告覆盖历史成功或失败反馈；
/// - 网络层目录错误优先于所有通用反馈文案。
/// @warning UI 热路径：设置窗口打开且当前页为协作页时每帧执行；仅在
/// 用户确认应用时修改目录连接并持久化配置。
void SettingsView::drawCollaborationSettings()
{
    // AppConfig 是输入草稿初值和最终持久化目标。
    auto& appConfig = Config::AppConfig::instance();
    // 管理器或协作房间可能在启动、关闭阶段不可用，页面仍保持只读布局。
    auto* room =
        m_sourceManager ? m_sourceManager->getCollaborationRoom() : nullptr;
    const bool roomActive = room && room->isActive();

    // 页面布局结构：
    // - 顶部一行是可折叠服务设置标题；
    // - 展开后依次显示说明、地址、端口、TLS 和目录状态；
    // - Clay 树结束后绘制整宽应用按钮；
    // - 按钮下方按需绘制状态或错误反馈卡；
    // - sectionOpen 同时控制设置树和操作反馈区域。
    // Clay 容器每帧重建描述，行和节索引复用 SettingsView 缓存对象。
    m_contentVBox.clear();
    m_contentVBox.setSpacing(6).setPadding(8, 8, 8, 8);
    std::size_t rowIndex     = 0;
    std::size_t sectionIndex = 0;
    const float maxLabelWidth =
        // 同一设置标签页共享标签列宽，输入控件起点保持纵向对齐。
        getCurrentTabLabelWidth(appConfig.getWindowContentScale());

    // headerId 同时包含节、行和翻译文本，避免与其他设置折叠区冲突。
    const char* sectionLabel =
        TR_CACHE("ui.settings.collaboration.server").data();
    std::string   headerId = "COLLAB_S" + std::to_string(sectionIndex) + "_R" +
                             std::to_string(rowIndex) + "_H_" + sectionLabel;
    const ImGuiID headerStorageId = ImGui::GetID(headerId.c_str());
    // 未写入状态时默认展开，用户选择只保留在当前 ImGui 上下文。
    const bool sectionOpen =
        ImGui::GetStateStorage()->GetInt(headerStorageId, 1) != 0;

    // 折叠标题独占一行并填满内容宽度，点击区域与视觉背景一致。
    auto& headerRow = getRow(rowIndex++);
    headerRow.setPadding(0, 0, 0, 0).setSpacing(0);
    const float headerHeight = ImGui::GetFrameHeight();
    headerRow.addElement(
        (headerId + "_el").c_str(),
        Sizing::Grow(),
        Sizing::Fixed(headerHeight),
        [sectionLabel, headerStorageId](Clay_BoundingBox rect, bool) {
            // 折叠标题样式契约：
            // - 默认颜色完全来自当前 ImGui Header 色；
            // - Hovered 对 RGB 和 alpha 做轻微提升；
            // - Active 使用更明显提升表现按下反馈；
            // - 临时 WorkRect 右边界限制点击和底色范围；
            // - StateStorage 以稳定 ImGuiID 保存展开状态。
            // Clay 提供最终屏幕矩形，ImGui 控件游标必须显式移动到该位置。
            ImGui::SetCursorScreenPos({ rect.x, rect.y });
            const ImVec4 headerColor =
                ImGui::GetStyle().Colors[ImGuiCol_Header];
            ImGui::PushStyleColor(ImGuiCol_Header, headerColor);
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                                  { headerColor.x + 0.05F,
                                    headerColor.y + 0.05F,
                                    headerColor.z + 0.05F,
                                    headerColor.w + 0.1F });
            ImGui::PushStyleColor(ImGuiCol_HeaderActive,
                                  { headerColor.x + 0.1F,
                                    headerColor.y + 0.1F,
                                    headerColor.z + 0.1F,
                                    headerColor.w + 0.15F });

            // 临时收窄 WorkRect，使 CollapsingHeader 精确填充 Clay 分配宽度。
            ImGuiWindow* window    = ImGui::GetCurrentWindow();
            const float  savedMaxX = window->WorkRect.Max.x;
            window->WorkRect.Max.x = rect.x + rect.width;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                ImVec2(0.0F, 0.0F));
            const bool nowOpen = ImGui::TreeNodeEx(
                reinterpret_cast<void*>(
                    static_cast<std::intptr_t>(headerStorageId)),
                ImGuiTreeNodeFlags_CollapsingHeader |
                    ImGuiTreeNodeFlags_DefaultOpen,
                "%s",
                sectionLabel);
            // 所有临时样式和 WorkRect 必须在 Lambda 内恢复，避免影响后续元素。
            ImGui::PopStyleVar();
            window->WorkRect.Max.x = savedMaxX;
            ImGui::GetStateStorage()->SetInt(headerStorageId, nowOpen ? 1 : 0);
            ImGui::PopStyleColor(3);
        });
    m_contentVBox.addLayout((headerId + "_layout").c_str(),
                            headerRow,
                            Sizing::Grow(),
                            Sizing::Fixed(headerHeight));

    if ( sectionOpen ) {
        // 只有展开时才把设置行加入 Clay 树，折叠态不创建隐藏输入控件。
        auto& section = getSection(sectionIndex++);
        section.setDecorated(true).setSpacing(6).setPadding(8, 8, 8, 8);

        // 说明文本单独成行并启用换行，宽度随设置窗口变化。
        auto& descriptionRow = getRow(rowIndex++);
        descriptionRow.setPadding(8, 8, 4, 4);
        const float descriptionHeight =
            ImGui::GetTextLineHeightWithSpacing() + 8.0F;
        descriptionRow.addElement(
            "CollaborationServerDescription",
            Sizing::Grow(),
            Sizing::Fixed(descriptionHeight),
            [](Clay_BoundingBox rect, bool) {
                // 换行终点使用 Clay 右边界，防止文本越过装饰卡片。
                ImGui::SetCursorScreenPos({ rect.x, rect.y });
                ImGui::PushTextWrapPos(rect.x + rect.width);
                ImGui::TextUnformatted(
                    TR("ui.settings.collaboration.server_description").data());
                ImGui::PopTextWrapPos();
            });
        section.addLayout("CollaborationServerDescriptionRow",
                          descriptionRow,
                          Sizing::Grow(),
                          Sizing::Fit());

        addSettingItem(
            section,
            rowIndex,
            TR("ui.collaboration.server_address").data(),
            maxLabelWidth,
            [this, roomActive](Clay_BoundingBox rect, bool) {
                // 地址输入约束：
                // - 宽度使用 Clay 分配的值列矩形；
                // - Hint 只在缓冲区为空时提供格式提示；
                // - 不在输入过程中尝试连接或解析 DNS；
                // - 活动房间下仍展示当前草稿但禁止编辑。
                // 活动房间锁定端点地址，避免连接对象与编辑草稿分歧。
                ImGui::BeginDisabled(roomActive);
                ImGui::SetNextItemWidth(rect.width);
                ImGui::InputTextWithHint(
                    "##SettingsCollaborationServerAddress",
                    TR("ui.collaboration.server_address_hint").data(),
                    m_collaborationServerAddressInputBuffer.data(),
                    m_collaborationServerAddressInputBuffer.size());
                // 缓冲区由 SettingsView 持有，InputText 不分配或替换其地址。
                ImGui::EndDisabled();
            });

        addSettingItem(
            section,
            rowIndex,
            TR("ui.collaboration.signaling_port").data(),
            maxLabelWidth,
            [this, roomActive](Clay_BoundingBox rect, bool) {
                // 端口输入约束：
                // - InputInt 不提供步进按钮，避免滚轮或按钮意外改值；
                // - 编辑结果立即限制在 uint16_t 的非零端口范围；
                // - 真正窄化转换只发生在应用按钮回调内；
                // - 输入阶段不触发网络重连。
                // 端口输入禁用步进按钮，只接受明确数值编辑。
                ImGui::BeginDisabled(roomActive);
                ImGui::SetNextItemWidth(rect.width);
                ImGui::InputInt("##SettingsCollaborationSignalingPort",
                                &m_collaborationSignalingPortInput,
                                0,
                                0);
                m_collaborationSignalingPortInput =
                    // 每帧钳制到 TCP/UDP 有效端口范围，避免构造时窄化溢出。
                    std::clamp(m_collaborationSignalingPortInput, 1, 65535);
                ImGui::EndDisabled();
            });

        addSettingItem(section,
                       rowIndex,
                       TR("ui.collaboration.use_tls").data(),
                       maxLabelWidth,
                       [this, roomActive](Clay_BoundingBox, bool) {
                           // TLS
                           // 与地址、端口属于同一个端点事务，同样受活动房间锁定。
                           ImGui::BeginDisabled(roomActive);
                           FeedbackCheckbox("##SettingsCollaborationUseTls",
                                            &m_collaborationUseTlsInput);
                           ImGui::EndDisabled();
                       });

        addSettingItem(section,
                       rowIndex,
                       TR("ui.collaboration.directory.status").data(),
                       maxLabelWidth,
                       [room](Clay_BoundingBox, bool) {
                           // 状态直接读取房间的轻量快照；房间缺失显示不可用。
                           ImGui::TextUnformatted(
                               room
                                   ? directoryStateText(room->directoryState())
                                   : TR("ui.collaboration.unavailable").data());
                       });

        // 装饰 section 以内容高度适配，外层 VBox 负责与标题间距。
        m_contentVBox.addLayout("CollaborationServerSettingsSection",
                                section,
                                Sizing::Grow(),
                                Sizing::Fit());
    }

    // 渲染 Clay 描述树并把 ImGui 游标推进到其实际底部。
    const ImVec2 startPosition = ImGui::GetCursorScreenPos();
    const ImVec2 contentSize   = m_contentVBox.renderInCurrent(
        startPosition, { ImGui::GetContentRegionAvail().x, 0.0F });
    ImGui::SetCursorScreenPos(
        { startPosition.x, startPosition.y + contentSize.y + 4.0F });

    if ( sectionOpen ) {
        // 应用按钮位于 Clay 树外，便于根据整页状态统一禁用和展示反馈。
        const float actionInset = 8.0F;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + actionInset);
        // 无房间对象无法应用；活动房间必须先离开后才能变更目录服务。
        ImGui::BeginDisabled(!room || roomActive);
        if ( FeedbackButton(TR("ui.collaboration.apply_server").data(),
                            ImVec2(-actionInset, 0.0F)) ) {
            // 应用时序：
            // - 冻结 UI 三项输入到 endpoint；
            // - 保存当前 AppConfig 服务端点副本；
            // - 请求 CollaborationRoom 应用新端点；
            // - 成功后同步内存配置并尝试写盘；
            // - 写盘失败时恢复内存和房间端点；
            // - 最后更新单一成功或失败反馈状态。
            Network::Collaboration::CollaborationServerEndpoint endpoint;
            // UI 草稿在点击时复制进值类型端点，随后不再依赖输入控件缓冲。
            endpoint.address = m_collaborationServerAddressInputBuffer.data();
            endpoint.signalingPort =
                static_cast<std::uint16_t>(m_collaborationSignalingPortInput);
            endpoint.useTls = m_collaborationUseTlsInput;

            // 保存旧配置以支持房间应用或磁盘持久化任一步失败后的完整回滚。
            auto&      settings         = appConfig.getEditorSettings();
            const auto previousSettings = settings.collaborationServer;
            const bool endpointApplied  = room->setServerEndpoint(endpoint);
            if ( endpointApplied ) {
                // 房间先接受端点后才更新内存配置，避免保存不可用目标。
                settings.collaborationServer.address = endpoint.address;
                settings.collaborationServer.signalingPort =
                    endpoint.signalingPort;
                settings.collaborationServer.useTls = endpoint.useTls;
            }
            if ( endpointApplied && appConfig.save() ) {
                // 两阶段均成功才向用户报告成功。
                m_collaborationServerApplyState =
                    CollaborationServerApplyState::Succeeded;
            } else {
                // 无论失败发生在哪一步，先恢复 AppConfig 内存副本。
                settings.collaborationServer = previousSettings;
                if ( endpointApplied ) {
                    // 房间已切换而磁盘保存失败时，还需恢复其运行时端点。
                    Network::Collaboration::CollaborationServerEndpoint
                        previous;
                    previous.address       = previousSettings.address;
                    previous.signalingPort = previousSettings.signalingPort;
                    previous.useTls        = previousSettings.useTls;
                    static_cast<void>(
                        room->setServerEndpoint(std::move(previous)));
                }
                // 回滚调用结果不覆盖原失败反馈，下一次应用仍可重新尝试。
                m_collaborationServerApplyState =
                    CollaborationServerApplyState::Failed;
            }
        }
        ImGui::EndDisabled();

        // 反馈优先级为活动房间警告、应用成功、应用失败。
        const char* feedbackText = nullptr;
        ImVec4 feedbackColor = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        if ( roomActive ) {
            // 活动警告解释输入禁用原因，而不是沿用上一次应用结果。
            feedbackText =
                TR("ui.settings.collaboration.active_warning").data();
        } else if ( m_collaborationServerApplyState ==
                    CollaborationServerApplyState::Succeeded ) {
            feedbackText = TR("ui.settings.collaboration.apply_success").data();
            feedbackColor = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
        } else if ( m_collaborationServerApplyState ==
                    CollaborationServerApplyState::Failed ) {
            feedbackText  = TR("ui.settings.collaboration.apply_failed").data();
            feedbackColor = ImVec4(1.0F, 0.45F, 0.35F, 1.0F);
        }

        const std::string* directoryError =
            // 网络层具体错误优先于通用应用反馈，并只保存当前帧观察指针。
            room && !room->directoryError().empty() ? &room->directoryError()
                                                    : nullptr;
        if ( feedbackText || directoryError ) {
            // 反馈卡数据契约：
            // - directoryError 优先展示网络层可操作原因；
            // - 无具体错误时使用当前应用状态或活动警告；
            // - 成功使用 CheckMark 色，失败使用固定危险色；
            // - 普通警告使用 TextDisabled 色；
            // - 背景取文字色的 12% alpha，边框保留原 alpha。
            // 反馈卡宽度填满剩余区域，高度预留两行换行文本。
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + actionInset);
            const ImVec2 cardMin   = ImGui::GetCursorScreenPos();
            const float  cardWidth = ImGui::GetContentRegionAvail().x;
            const char*  displayText =
                directoryError ? directoryError->c_str() : feedbackText;
            const float cardHeight =
                ImGui::GetTextLineHeightWithSpacing() * 2.0F + 16.0F;
            ImVec4 cardColor = feedbackColor;
            // 背景仅使用同色低透明度，边框和文字保留完整语义色。
            cardColor.w = 0.12F;
            ImGui::GetWindowDrawList()->AddRectFilled(
                cardMin,
                { cardMin.x + cardWidth, cardMin.y + cardHeight },
                ImGui::ColorConvertFloat4ToU32(cardColor),
                ImGui::GetStyle().FrameRounding);
            ImGui::GetWindowDrawList()->AddRect(
                cardMin,
                { cardMin.x + cardWidth, cardMin.y + cardHeight },
                ImGui::ColorConvertFloat4ToU32(feedbackColor),
                ImGui::GetStyle().FrameRounding);
            ImGui::SetCursorScreenPos({ cardMin.x + 12.0F, cardMin.y + 8.0F });
            ImGui::PushTextWrapPos(cardMin.x + cardWidth - 12.0F);
            // 文本左右各留十二像素，避免贴近卡片边框。
            ImGui::TextColored(feedbackColor, "%s", displayText);
            ImGui::PopTextWrapPos();
            // 手动推进游标确保后续设置内容从卡片下方继续布局。
            ImGui::SetCursorScreenPos({ cardMin.x, cardMin.y + cardHeight });
        }
    }
}

}  // namespace MMM::UI
