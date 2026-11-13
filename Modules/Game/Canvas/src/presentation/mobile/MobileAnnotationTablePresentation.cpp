#include "MobileAuxiliaryWindow.h"
#include "MobileReadPress.h"
#include "canvas/AnnotationTableData.h"
#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/EditorSettings.h"
#include "config/VisualConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/logic/LogicCommandEvent.h"
#include "presentation/AnnotationTablePresentation.h"
#include "ui/imgui/markdown/MarkdownRenderer.h"
#include "ui/interaction/EditorInteraction.h"
#include "ui/utils/TimeFormatUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fmt/format.h>
#include <imgui.h>
#include <string>
#include <string_view>
#include <vector>

namespace MMM::Canvas
{
namespace
{
/// @brief 触屏批注列表将阅读、选择和编辑分开，不把滑动解释成文本修改。
/// @details 窗口自持的版本化数据是唯一读取源，确认时沿用正式批注命令。
class MobileAnnotationTablePresentation final
    : public AnnotationTablePresentation
{
public:
    /// @brief 接鼠标切回原表格；触控方案使用紧凑列表。
    // 能力来自当前操作方案；移动编译目标并不意味着始终使用触控布局。
    // 外接鼠标后仍由同一窗口退回原桌面表格，不维护第二份数据源。
    // 这个查询不持有配置所有权，也不访问输入设备或会话互斥锁。
    /// @warning 每帧只读取当前操作方案能力，不枚举设备或访问会话。
    bool touchLayout() const override
    {
        return UI::Interaction::activeProfile().touchGestures();
    }
    /// @brief 触屏初次打开按可用屏幕取尺寸，避免高 DPI 下只剩一条列表摘要。
    /// @warning 每帧只测量字体和工作区，窗口位置仍由原恢复逻辑处理。
    void prepareWindow() const override
    {
        if ( !touchLayout() ) return;
        prepareMobileAuxiliaryWindow();
    }
    /// @brief 关闭窗口放弃副本，未确认编辑不制造历史。
    // 清理只影响尚未确认的本地副本，不发送删除、撤销或保存命令。
    // 当前原始代际被封锁，关窗后立即重开也不能消费旧手指的抬手。
    // 正文缓冲的旧字节不会被读取；下一次显式编辑总会先清零并复制。
    // 稳定 ID 和实例令牌同时清空，不能把同名批注从别的谱面接续过来。
    void reset() override
    {
        m_id.clear();
        m_original.clear();
        m_instance = 0;
        m_request  = false;
        m_press.cancel();
        m_readId.clear();
        m_labels.clear();
        m_labelRevision = ~std::uint64_t{ 0 };
    }
    /// @brief 同一时间戳多条批注各自保留稳定行与完整 Markdown 阅读位置。
    /// @warning 每帧只遍历可见行，不读会话或磁盘；文本只在编辑起笔复制。
    bool render(const AnnotationTableData&  data,
                std::optional<std::size_t>& selected) override
    {
        if ( !touchLayout() ) {
            // 方案切换时仍结束本窗口的旧模态栈，不能留下不可操作的遮罩。
            reset();
            editor(data);
            return false;
        }
        // rows 由窗口低频刷新并持有，下面所有引用止于本帧绘制。
        // 任何按钮都只排队命令，不会在 clipper 遍历中删除或追加缓存元素。
        // 独立缓存包含视口外批注，不依赖主画布或时间线是否可见。
        // 列表、详情和编辑确认使用同一缓存版本，避免正文与目标分离。
        const auto& rows = data.rows();
        const bool  same = m_instance == data.instanceId();
        // 切谱与同路径重载立即丢弃旧副本，不能让重复 ID 写到另一个谱面。
        if ( !same ) reset();
        m_instance = data.instanceId();
        refreshLabels(data);
        // 列表和阅读区共享剩余高度，操作行保留在两个滚动区之外。
        // 这样长正文不会把编辑、删除与历史按钮推到列表的末尾。
        // 最小列表高度只保证两条摘要可见，不修改应用字号或全局 DPI。
        // 窗口本身仍可滚动，极矮窗口不靠缩小触摸命中区勉强容纳。
        const float available  = ImGui::GetContentRegionAvail().y;
        const float actions    = ImGui::GetFrameHeightWithSpacing() * 2;
        const float listHeight = std::max(ImGui::GetFrameHeight() * 2,
                                          (available - actions) * 0.48f);
        ImGui::TextWrapped("%s", TR("ui.mobile.annotation.list_hint").data());
        if ( ImGui::BeginChild("TouchAnnotationRows",
                               { 0, listHeight },
                               ImGuiChildFlags_Borders) ) {
            // 摘要采用固定两行高度，完整正文仍由下方 Markdown 阅读区负责。
            // clipper 的行距包含主题 ItemSpacing，不能只把文字高度传进去。
            // 可见行的工作量随窗口高度变化，不逐帧遍历或排序全量批注。
            // 滚动后重新计算显示范围，行 ID 仍使用批注 ID 而非屏幕序号。
            const float      rowHeight = ImGui::GetTextLineHeight() * 2 +
                                         ImGui::GetStyle().FramePadding.y * 2;
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(rows.size()),
                          rowHeight + ImGui::GetStyle().ItemSpacing.y);
            while ( clipper.Step() )
                for ( int i = clipper.DisplayStart; i < clipper.DisplayEnd;
                      ++i ) {
                    const auto& row = rows[static_cast<std::size_t>(i)];
                    // 每行只借用脏标记路径生成的摘要，常态帧不格式化或分配字符串。
                    ImGui::PushID(row.id.c_str());
                    const auto& label = m_labels[static_cast<std::size_t>(i)];
                    UI::FeedbackSelectable(
                        label.c_str(),
                        selected == static_cast<std::size_t>(i),
                        ImGuiSelectableFlags_None,
                        { 0, rowHeight });
                    // Selectable
                    // 负责统一外观反馈，选择意图不读取它的合成点击结果。
                    // 页面可能延后鼠标点击；原始轻点只在实际 Up 时交付一次。
                    // 长按从原始起点冻结本行，即使页面稍后重排也不编辑邻近行。
                    // 滑动、多指和系统取消均没有离散结果，不能意外修改阅读选择。
                    const auto result = m_press.update(
                        ImGui::GetItemID(),
                        MobileReadPress::originInside(
                            ImGui::GetItemRectMin(), ImGui::GetItemRectMax()) &&
                            ImGui::IsWindowHovered(
                                ImGuiHoveredFlags_AllowWhenBlockedByActiveItem),
                        true);
                    if ( result != MobileReadPress::Result::None )
                        selected = static_cast<std::size_t>(i);
                    if ( result == MobileReadPress::Result::Hold &&
                         data.writable() )
                        edit(row);
                    ImGui::PopID();
                }
        }
        ImGui::EndChild();
        // 选中索引只用于当前缓存；窗口刷新时会按稳定 ID 恢复它。
        // 删除或远端修改之后仍先检查边界，不能引用已替换容器的旧地址。
        // 跳转是只读定位动作；播放或只读期间仍可以阅读和定位。
        // 编辑、删除与历史按钮才受写入许可控制。
        if ( selected && *selected < rows.size() ) {
            const auto& row = rows[*selected];
            if ( UI::FeedbackButton(TR("ui.annotation.table.jump").data()) ) {
                const float offset = Config::AppConfig::instance()
                                         .getVisualConfig()
                                         .getEffectiveVisualOffset();
                UI::Interaction::dispatchCommand(Event::LogicCommandEvent(
                    Logic::CmdSeek{ row.timestamp - offset }));
            }
            ImGui::SameLine();
            // 许可来自独立缓存的低频发布，不在每个按钮前等待逻辑长锁。
            // 正式会话执行命令时继续复核权限，因此 UI 的显示状态不是写入授权。
            // 删除只传批注 ID，不连带删除关联物件或同时间戳的其他批注。
            // 用户仍可使用一次撤销恢复该条，不在 UI 端伪造缓存删除。
            ImGui::BeginDisabled(!data.writable());
            if ( UI::FeedbackButton(TR("ui.annotation.edit").data()) )
                edit(row);
            ImGui::SameLine();
            if ( UI::FeedbackButton(TR("ui.annotation.delete").data()) ) {
                UI::Interaction::dispatchCommand(Event::LogicCommandEvent(
                    Logic::CmdRemoveBeatmapAnnotation{ row.id }));
            }
            ImGui::EndDisabled();
            // 历史是正式会话的动作栈，不通过 UI 修改列表来冒充撤销成功。
            ImGui::SameLine();
            ImGui::BeginDisabled(!data.writable());
            if ( UI::FeedbackButton(TR("ui.edit.undo").data()) )
                UI::Interaction::dispatchCommand(
                    Event::LogicCommandEvent(Logic::CmdUndo{}));
            ImGui::SameLine();
            if ( UI::FeedbackButton(TR("ui.edit.redo").data()) )
                UI::Interaction::dispatchCommand(
                    Event::LogicCommandEvent(Logic::CmdRedo{}));
            ImGui::EndDisabled();
            if ( ImGui::BeginChild("TouchAnnotationDetail",
                                   { 0, 0 },
                                   ImGuiChildFlags_Borders) ) {
                // 阅读区独立滚动；点选新条目时回到正文顶部，不能继承旧条偏移。
                if ( m_readId != row.id ) {
                    ImGui::SetScrollY(0);
                    m_readId = row.id;
                }
                if ( row.targetMissing )
                    ImGui::TextWrapped(
                        "%s", TR("ui.annotation.target_missing").data());
                // 详情正文沿用原 Markdown 渲染器，链接和多行显示保留。
                // 子窗口裁剪与滚动独立，不把正文滚动传给上方摘要列表。
                // 不同批注共用阅读窗时显式回顶，同一条更新则保留阅读位置。
                // 不将悬浮提示作为阅读正文或发现操作按钮的唯一入口。
                UI::renderMarkdown(row.content);
            }
            ImGui::EndChild();
        } else
            ImGui::TextDisabled("%s", TR("ui.annotation.table.empty").data());
        // 模态提交位于列表和正文子窗之后，二者不会成为模态的意外父窗口。
        // 同一缓存实例在这里再验证稳定 ID 与原正文，旧索引不能直接用于保存。
        // 正常返回表明已完成列表与详情呈现，外层不会重复绘制桌面八列表格。
        // 导出工具仍由外层共用，不在此复制另一份文件选择或编码实现。
        editor(data);
        return true;
    }

private:
    /// @brief 只在数据、语言或时间偏好改变时重建派生摘要。
    /// @details 不改变业务缓存，切换显示方式也不能提交批注修改。
    /// @warning 低频脏标记路径允许分配；常态 UI 帧只比较三个标量后返回。
    void refreshLabels(const AnnotationTableData& data)
    {
        // 实例身份在进入这里前已复核，缓存不能跨谱面复用同号修订。
        // 行数据版本同时覆盖目标时间变化和 BPM 拍位上下文变化。
        // 翻译版本单独参与键，切语言后旧占位作者不会残留。
        // 仅字体或窗口尺寸改变不影响文本，因此不触发整表重建。
        // 这里读取的偏好是轻量枚举，不复制完整编辑设置。
        // 三个键相同的常态帧直接返回，容器容量与字符串地址保持稳定。
        const auto language = Translation::getActiveTranslator().getVersion();
        const auto format   = Config::AppConfig::instance()
                                  .getEditorSettings()
                                  .timeFormatPreference;
        if ( m_labelRevision == data.displayRevision() &&
             m_language == language && m_format == format )
            return;
        const auto& rows = data.rows();
        // 行与拍位上下文属于同一数据版本，不把原有屏幕字符串当成持久数据。
        // 版本变化才全量构造；clipper 绘制始终只读这些稳定字符串。
        // 正文变化、目标时间变化与拍位上下文变化都由显示修订号覆盖。
        // 语言版本独立于数据，未知作者提示不应停留在上一次语言。
        // 时间偏好也是独立键；仅改变显示格式不能制造谱面历史。
        // 首行保留完整 UTF-8 字节，由控件裁剪，不在任意字节处截断文字。
        // 缓存按原行顺序构造，不额外排序或改变同时间戳批注的稳定身份。
        m_labels.resize(rows.size());
        for ( std::size_t i = 0; i < rows.size(); ++i ) {
            const auto&            row = rows[i];
            const std::string_view line(
                row.content.data(),
                std::min(row.content.find('\n'), row.content.size()));
            m_labels[i] = fmt::format(
                "{} · {}\n{}###Row",
                UI::Utils::formatCanvasTime(row.timestamp,
                                            data.timeFormatContext()),
                row.author.empty() ? TR("ui.annotation.unknown_author").view()
                                   : std::string_view(row.author),
                line);
        }
        m_labelRevision = data.displayRevision();
        m_language      = language;
        m_format        = format;
    }
    /// @brief 显式编辑或静止长按才复制正文，普通选择不修改谱面。
    // 这里只为已有 ID 建立副本，不提供隐式新增或批量覆盖路径。
    // 原正文另外保留，供确认时检测协作改写或本地删除后的失效。
    // 固定缓冲区遵循业务正文上限，并为终结符保留一个字节。
    // 无论通过按钮还是长按进入，初始化顺序与保存目标都相同。
    void edit(const AnnotationTableRow& row)
    {
        m_id       = row.id;
        m_original = row.content;
        m_text.fill('\0');
        std::copy_n(row.content.data(),
                    std::min(row.content.size(), m_text.size() - 1),
                    m_text.data());
        m_request = true;
    }
    /// @brief 确认前复核实例、稳定 ID、源正文及许可，协作改写不能被旧副本覆盖。
    /// @warning 每帧只检查已缓存批注；发布命令仅在明确按钮边沿。
    void editor(const AnnotationTableData& data)
    {
        // 模态 ID 固定且属于当前父窗口，切换语言不改变其内部身份。
        // 只在 request 边沿调用 OpenPopup，普通关闭之后不会自动重开。
        // 窗口宽度受实际工作区约束，不沿用桌面的固定 520 像素正文区。
        // 这里的尺寸只影响本次弹窗，不写入用户主题或窗口密度。
        constexpr const char* popup = "###TouchAnnotationEditor";
        if ( m_request ) {
            ImGui::OpenPopup(popup);
            m_request = false;
        }
        const auto viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowSize(
            { std::min(viewport->WorkSize.x * 0.9f, ImGui::GetFontSize() * 32),
              0 });
        if ( ImGui::BeginPopupModal(
                 popup, nullptr, ImGuiWindowFlags_AlwaysAutoResize) ) {
            // 切换操作方案或窗口归属时不猜测新目标，也不借 Undo
            // 撤回用户上一笔。
            const auto& rows = data.rows();
            // 索引只用于当前数据中查找 ID；副本不持有行引用或后台对象。
            // 原批注被删除时关闭旧编辑，不把保存解释为重新创建该条批注。
            // 同路径重新载入也会改变实例令牌，禁止把旧正文写入新谱面。
            // 关闭模态仅放弃草稿，原来的撤销与重做栈保持不变。
            const auto found =
                std::find_if(rows.begin(), rows.end(), [&](const auto& row) {
                    return row.id == m_id;
                });
            const bool valid =
                m_instance == data.instanceId() && found != rows.end();
            if ( !valid )
                ImGui::CloseCurrentPopup();
            else {
                ImGui::TextUnformatted(TR("ui.annotation.editor_title").data());
                // 正文比较用于拒绝可见的协作冲突，不尝试自动合并自由文本。
                // 用户可取消后重新打开取得当前版本，避免旧副本静默覆盖对方修改。
                // 作者、关联目标和时间锚点都不进入更新载荷，沿用业务层保留语义。
                // 许可或冲突改变时立即更新按钮状态，不能只相信打开时的判断。
                const bool conflict = found->content != m_original;
                if ( conflict )
                    ImGui::TextWrapped(
                        "%s", TR("ui.mobile.annotation.changed").data());
                // 不使用 Enter 即保存，移动输入法的换行与确认按钮职责明确。
                ImGui::InputTextMultiline(
                    "##Content",
                    m_text.data(),
                    m_text.size(),
                    { -1,
                      std::min(viewport->WorkSize.y * 0.35f,
                               ImGui::GetFontSize() * 10) });
                ImGui::BeginDisabled(!data.writable() || conflict ||
                                     !m_text.front());
                // 确认只发布一次 Upsert，界面随即关闭，不等待后台执行。
                // 空正文不能进入更新，清除整条批注应使用列表明确的删除动作。
                // 点击取消始终可用，播放或只读不能把用户锁在编辑窗口内。
                // 输入法换行不会隐式提交，避免手机确认键造成意外保存。
                if ( UI::FeedbackButton(TR("ui.annotation.save").data()) ) {
                    UI::Interaction::dispatchCommand(Event::LogicCommandEvent(
                        Logic::CmdUpsertBeatmapAnnotation{
                            .annotationId = m_id, .content = m_text.data() }));
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndDisabled();
                // 取消与保存分属两个按钮，移动输入法的 Enter 只进入正文。
                // 取消不要求写入许可，离线或播放状态发生变化后仍然可以退出。
                // 关闭后 request 已经消费，下一帧不会因为 ID 仍存在而重新弹出。
                // 下一次编辑会重新复制当前正文，不能恢复这份已放弃的副本。
                ImGui::SameLine();
                if ( UI::FeedbackButton(TR("ui.annotation.cancel").data()) )
                    ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
    /// @brief 原始触点门闩，不复用表格滚动的合成鼠标点击。
    // 平台状态全部由窗口独占，生命周期不依赖主画布渲染线程。
    // 这里只保存轻量身份和固定正文副本，不拥有活动会话或共享快照。
    // 所有低频字符串复制都发生于显式编辑或阅读目标改变。
    // 连续帧不重新查询 Registry，不发网络同步，也不等待后台数据。
    /// @brief 仅数据或显示偏好变化时重建，绘制阶段不追加行字符串。
    std::vector<std::string> m_labels;
    /// @brief 平台显示缓存的三个独立失效条件，均由当前 UI 线程更新。
    std::uint64_t                m_labelRevision{ ~std::uint64_t{ 0 } };
    std::uint32_t                m_language{};
    Config::TimeFormatPreference m_format{};
    MobileReadPress              m_press;
    /// @brief 缓存地址只作身份，不解引用或持有谱面。
    std::uintptr_t m_instance{};
    std::string    m_id, m_original,
        /// @brief 仅编辑起笔或阅读目标改变时复制。
        m_readId;
    std::array<char, ::MMM::MAX_BEATMAP_ANNOTATION_CONTENT_BYTES + 1>
        /// @brief 固定上限且始终 NUL 结尾。
        m_text{};
    /// @brief 显式请求消费一次，关闭后不因状态持久而重新打开。
    bool m_request{};
};
}  // namespace
/// @brief 移动平台和 CPU 测试构造同一生产呈现对象。
std::unique_ptr<AnnotationTablePresentation>
createMobileAnnotationTablePresentation()
{
    return std::make_unique<MobileAnnotationTablePresentation>();
}
}  // namespace MMM::Canvas
