#define IMGUI_DEFINE_MATH_OPERATORS
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "mmmversion.h"
#include "network/UpdateChecker.h"
#include "ui/UIManager.h"
#include "ui/imgui/markdown/MarkdownImageCache.h"
#include "ui/imgui/markdown/MarkdownRenderer.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuHelpActions.h"
#include "ui/imgui/status/IStatusMessageSink.h"
#include "ui/utils/UIWidgetUtils.h"

#include <algorithm>
#include <cstdio>
#include <imgui.h>
#include <memory>
#include <string>

namespace MMM::UI
{
namespace
{
/// @brief 检查更新动作。
/// @details 管理启动静默检查、手动检查、下载、重启安装及三个独立弹窗状态。
/// @warning 网络和下载工作必须留在 UpdateChecker 异步任务中，UI 只轮询快照。
class CheckUpdateAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 构造检查更新动作并创建更新检查器。
    /// @note 检查器独占于动作实例，避免多个菜单入口竞争同一异步状态。
    CheckUpdateAction()
        : m_updateChecker(std::make_unique<MMM::Network::UpdateChecker>())
    {
    }

    /// @brief 启动时自动检查和静默检查状态轮询。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧只轮询更新检查器状态，不执行网络阻塞操作。
    /// @note 启动检查只安排一次；静默状态结束后不再自动重试。
    void update(MainMenuContext& context) override
    {
        if ( !m_hasCheckedOnStartup ) {
            // 先置位保证后续帧不会因任何结果分支重复安排启动检查。
            m_hasCheckedOnStartup = true;

            if ( MMM::Network::UpdateChecker::checkStartupUpdateMarker() ) {
                // 更新器留下成功标记时直接展示完成提示，不再联网查询。
                m_showUpdateSuccessPopup = true;
            } else {
                // 无成功标记时启动后台静默检查，UI 帧不等待网络结果。
                m_isSilentCheck = true;
                m_updateChecker->checkAsync();
            }
        }

        // 手动检查由专用弹窗处理，update 只负责静默启动检查。
        if ( !m_isSilentCheck ) return;

        // UpdateInfo 是线程安全状态快照，按值读取后在本帧判断。
        auto info = m_updateChecker->getInfo();
        if ( info.status == MMM::Network::UpdateStatus::kUpdateFound ) {
            // 静默发现新版本时转为可见下载确认弹窗。
            m_showUpdatePopup = true;
            m_isSilentCheck   = false;
        } else if ( info.status == MMM::Network::UpdateStatus::kUpToDate ) {
            // 已是最新版本使用非模态状态消息，避免启动时打断用户。
            context.statusMessageSink.showStatusMessage(
                TR("ui.help.up_to_date").data(), 5.0f);
            m_isSilentCheck = false;
        } else if ( info.status == MMM::Network::UpdateStatus::kError ) {
            // 启动静默检查失败不弹错误窗口，只结束轮询状态。
            m_isSilentCheck = false;
        }
    }

    /// @brief 启动一次非静默更新检查。
    /// @param context 单帧主菜单上下文，本动作无需读取。
    /// @param activation 激活来源，不改变检查流程。
    /// @warning 只启动异步请求，不得在此等待网络或文件系统结果。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        // 手动检查接管可见弹窗，因此关闭启动静默轮询分支。
        m_isSilentCheck = false;
        // 打开请求延迟到 renderDeferred 转换为 ImGui 状态。
        m_showCheckingPopup = true;
        // 新一轮检查允许发现更新后重新打开确认弹窗。
        m_updatePopupCanceled = false;
        // 清除上一轮启动更新器失败文本，避免污染新任务。
        m_updateRestartError.clear();
        // UpdateChecker 在后台执行请求，当前调用立即返回。
        m_updateChecker->checkAsync();
    }

    /// @brief 渲染更新相关弹窗。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧执行；只绘制已经打开的更新弹窗。
    /// @note Markdown 图片缓存注册一次，并由 UIManager 管理纹理生命周期。
    void renderDeferred(MainMenuContext& context) override
    {
        // 只有有效 UIManager 才能查找或注册跨帧图片缓存视图。
        if ( context.sourceManager && !m_images ) {
            // 稳定注册名允许多个菜单帧复用同一个缓存实例。
            m_images = context.sourceManager->getView<MarkdownImageCache>(
                "UpdateMarkdownImages");
            if ( !m_images ) {
                // 首次需要时才创建缓存，避免未使用更新窗口占用图形资源。
                auto images = std::make_unique<MarkdownImageCache>();
                // 暂存观察指针后将独占所有权转移给 UIManager。
                m_images = images.get();
                context.sourceManager->registerView("UpdateMarkdownImages",
                                                    std::move(images));
            }
        }
        // 三个弹窗各自根据一次性请求和 UpdateChecker 状态决定是否绘制。
        renderUpdateCheckingPopup();
        renderUpdatePopup();
        renderUpdateSuccessPopup();
    }

private:
    /// @brief UIManager 持有图片纹理，菜单动作只保存稳定观察指针。
    /// @warning 指针仅在 UIManager 生命周期内有效，不得释放或跨管理器复用。
    MarkdownImageCache* m_images{ nullptr };
    /// @brief 已扫描的更新版本，避免逐帧重新安排图片任务。
    /// @note 版本变化时重新解析对应更新日志中的远程图片。
    std::string m_imageVersion;
    /// @brief 渲染更新检查中的状态弹窗。
    /// @warning UI 热路径：每帧执行；只轮询更新检查器状态。
    /// @note 检查中、已最新、发现更新和错误四种状态互斥展示。
    void renderUpdateCheckingPopup()
    {
        if ( m_showCheckingPopup ) {
            // 将一次性打开请求交给统一反馈入口后立即清除。
            ::MMM::UI::FeedbackOpenPopup(TR("ui.help.check_update").data());
            m_showCheckingPopup = false;
        }

        // 每帧读取当前 DPI，使弹窗和进度控件跟随窗口缩放。
        float dpiScale = Config::AppConfig::instance().getWindowContentScale();
        // 居中样式作用域在所有分支离开时恢复临时窗口设置。
        Utils::CenteredModalPopupScope modalScope(dpiScale);
        if ( modalScope.begin(TR("ui.help.check_update").data()) ) {
            // 异步状态可能在弹窗打开期间变化，因此每帧获取新快照。
            auto info = m_updateChecker->getInfo();

            if ( info.status == MMM::Network::UpdateStatus::kChecking ) {
                // 检查中文本按活动字体测宽后居中。
                float textWidth =
                    ImGui::CalcTextSize(TR("ui.help.checking").data()).x;
                ImGui::SetCursorPosX((ImGui::GetWindowWidth() - textWidth) *
                                     0.5f);
                ImGui::TextUnformatted(TR("ui.help.checking").data());

                // 不确定进度条使用固定逻辑宽度并随 DPI 缩放。
                float barWidth = 240.0f * dpiScale;
                ImGui::SetCursorPosX((ImGui::GetWindowWidth() - barWidth) *
                                     0.5f);
                float fraction = -1.0f * (float)ImGui::GetTime();
                // 负时间值触发 ImGui 不确定进度动画，不伪造网络百分比。
                ImGui::ProgressBar(
                    fraction, ImVec2(barWidth, 12.0f * dpiScale), "");
            } else if ( info.status == MMM::Network::UpdateStatus::kUpToDate ) {
                // 手动检查完成时在模态窗口中明确展示最新状态。
                float textWidth =
                    ImGui::CalcTextSize(TR("ui.help.up_to_date").data()).x;
                ImGui::SetCursorPosX((ImGui::GetWindowWidth() - textWidth) *
                                     0.5f);
                ImGui::TextUnformatted(TR("ui.help.up_to_date").data());

                // 确认按钮居中并使用统一声音、悬停反馈。
                float btnWidth = 120.0f * dpiScale;
                ImGui::SetCursorPosX((ImGui::GetWindowWidth() - btnWidth) *
                                     0.5f);
                if ( ::MMM::UI::FeedbackButton(
                         TR("ui.help.ok").data(),
                         ImVec2(btnWidth, 32.0f * dpiScale)) ) {
                    // 用户确认后关闭当前检查结果弹窗。
                    ImGui::CloseCurrentPopup();
                }
            } else if ( info.status ==
                        MMM::Network::UpdateStatus::kUpdateFound ) {
                // 先关闭检查弹窗，再登记下载确认弹窗，避免模态嵌套。
                ImGui::CloseCurrentPopup();
                m_showUpdatePopup = true;
            } else if ( info.status == MMM::Network::UpdateStatus::kError ) {
                // 手动检查错误保留在当前弹窗内，由公共错误正文呈现。
                renderErrorBody(info.errorMessage, dpiScale);
            }

            // 与成功 modalScope.begin 路径严格配对。
            ImGui::EndPopup();
        }

        if ( m_showUpdatePopup ) {
            // 已准备切换到更新确认窗口时清除检查弹窗的遗留打开请求。
            m_showCheckingPopup = false;
        }
    }

    /// @brief 渲染发现更新后的下载确认与下载进度弹窗。
    /// @warning UI 热路径：每帧执行；下载操作由更新检查器异步执行。
    /// @note 同一弹窗按 UpdateStatus 切换发现、下载、完成与错误正文。
    void renderUpdatePopup()
    {
        // 初始快照用于决定弹窗打开和布局大小。
        auto info = m_updateChecker->getInfo();

        if ( m_showUpdatePopup ) {
            // 显式请求优先打开弹窗，并立即消费一次性标志。
            ::MMM::UI::FeedbackOpenPopup(TR("ui.help.update_found").data());
            m_showUpdatePopup = false;
        } else if ( info.status == MMM::Network::UpdateStatus::kUpdateFound &&
                    !m_updatePopupCanceled ) {
            // 未取消时维持发现更新弹窗，覆盖外部意外关闭后的状态恢复。
            if ( !ImGui::IsPopupOpen(TR("ui.help.update_found").data()) )
                ::MMM::UI::FeedbackOpenPopup(TR("ui.help.update_found").data());
        }

        // 布局使用当前内容缩放，不依赖可能过期的菜单上下文。
        float dpiScale = Config::AppConfig::instance().getWindowContentScale();
        // 居中作用域统一管理弹窗位置和样式恢复。
        Utils::CenteredModalPopupScope modalScope(dpiScale);
        // 更新日志包含图片时需要独立阅读空间，尺寸随主视口工作区收缩。
        const bool showChangelog =
            info.status == MMM::Network::UpdateStatus::kUpdateFound &&
            !info.changelog.empty();
        // 工作区尺寸排除平台保留区域，避免阅读窗口超出可用屏幕。
        const ImVec2 workSize = ImGui::GetMainViewport()->WorkSize;
        // 有日志时提供大窗口，无日志时保持内容自适应尺寸。
        const ImVec2 desiredSize =
            showChangelog
                ? ImVec2(std::min(960.0f * dpiScale, workSize.x * 0.9f),
                         std::min(900.0f * dpiScale, workSize.y * 0.9f))
                : ImVec2(0.0f, 0.0f);
        if ( modalScope.begin(TR("ui.help.update_found").data(),
                              nullptr,
                              ImGuiWindowFlags_None,
                              desiredSize,
                              !showChangelog) ) {
            // Begin 后重新读取快照，减少状态在布局判断与正文之间变化的窗口。
            info = m_updateChecker->getInfo();

            if ( info.status == MMM::Network::UpdateStatus::kUpdateFound ) {
                // 检查完成且尚未下载时展示版本、日志和操作按钮。
                renderUpdateFoundBody(info, dpiScale);
            } else if ( info.status ==
                        MMM::Network::UpdateStatus::kDownloading ) {
                // 下载阶段只展示线程安全进度快照。
                renderDownloadingBody(info, dpiScale);
            } else if ( info.status ==
                        MMM::Network::UpdateStatus::kDownloaded ) {
                // 文件准备完成后提供显式重启安装入口。
                renderDownloadedBody(info, dpiScale);
            } else if ( info.status == MMM::Network::UpdateStatus::kError ) {
                // 检查、下载或准备阶段错误共用错误正文。
                renderErrorBody(info.errorMessage, dpiScale);
            }

            // 与成功 BeginPopup 路径配对，保持弹窗栈平衡。
            ImGui::EndPopup();
        }
    }

    /// @brief 渲染更新下载成功后的提示弹窗。
    /// @warning UI 热路径：每帧执行；只绘制完成提示。
    /// @note 成功标记通常来自更新器重启后的启动阶段。
    void renderUpdateSuccessPopup()
    {
        if ( m_showUpdateSuccessPopup ) {
            // 一次性请求转换为 ImGui 弹窗状态后立即清除。
            ::MMM::UI::FeedbackOpenPopup(TR("ui.help.update_success").data());
            m_showUpdateSuccessPopup = false;
        }

        // 成功窗口同样跟随当前 DPI 缩放。
        float dpiScale = Config::AppConfig::instance().getWindowContentScale();
        Utils::CenteredModalPopupScope modalScope(dpiScale);
        // 弹窗未打开时提前退出，不提交后续表格和按钮。
        if ( !modalScope.begin(TR("ui.help.update_success").data()) ) return;

        // 绿色只用于成功结果与当前版本值，保持状态语义一致。
        ImVec4 greenColor(0.3f, 1.0f, 0.3f, 1.0f);
        // 成功消息按活动字体测量并居中。
        float textWidth =
            ImGui::CalcTextSize(TR("ui.help.update_success_msg").data()).x;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - textWidth) * 0.5f);
        ImGui::TextColored(
            greenColor, "%s", TR("ui.help.update_success_msg").data());

        // 版本表禁用持久化，防止静态信息污染用户表格设置。
        if ( ImGui::BeginTable("SuccessInfoTable",
                               2,
                               ImGuiTableFlags_SizingFixedFit |
                                   ImGuiTableFlags_NoSavedSettings) ) {
            // 标签列固定宽度并随 DPI 缩放，值列填充剩余空间。
            ImGui::TableSetupColumn(
                "L", ImGuiTableColumnFlags_WidthFixed, 140.0f * dpiScale);
            ImGui::TableSetupColumn("R", ImGuiTableColumnFlags_WidthStretch);

            // 当前版本来自构建生成头，代表已经启动的新可执行文件。
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(TR("ui.help.current_version").data());
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(greenColor, MMM_VERSION_STRING);

            // 与成功 BeginTable 配对。
            ImGui::EndTable();
        }

        ImGui::Spacing();

        // 确认按钮居中，关闭后不修改更新检查器状态。
        float btnWidth = 120.0f * dpiScale;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - btnWidth) * 0.5f);
        if ( ::MMM::UI::FeedbackButton(TR("ui.help.ok").data(),
                                       ImVec2(btnWidth, 32.0f * dpiScale)) ) {
            ImGui::CloseCurrentPopup();
        }

        // 与成功 modalScope.begin 配对。
        ImGui::EndPopup();
    }

    /// @brief 渲染发现更新时的版本信息和操作按钮。
    /// @param info 更新检查信息。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 绘制路径：点击下载按钮时只启动异步下载。
    /// @note info 是当前帧不可变快照，正文不持有其字符串引用。
    void renderUpdateFoundBody(const MMM::Network::UpdateInfo& info,
                               float                           dpiScale)
    {
        // 版本信息表使用稳定列布局且不保存用户调整。
        if ( ImGui::BeginTable("UpdateInfoTable",
                               2,
                               ImGuiTableFlags_SizingFixedFit |
                                   ImGuiTableFlags_NoSavedSettings) ) {
            // 标签列固定宽度，版本和日期值列占用剩余空间。
            ImGui::TableSetupColumn(
                "L", ImGuiTableColumnFlags_WidthFixed, 140.0f * dpiScale);
            ImGui::TableSetupColumn("R", ImGuiTableColumnFlags_WidthStretch);

            // 局部 helper 统一表格行对齐与可选强调色。
            auto addRow = [&](const char*   label,
                              const char*   value,
                              const ImVec4* color = nullptr) {
                ImGui::TableNextRow();
                // 第一列使用默认文本色展示字段名称。
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(label);
                // 第二列根据是否提供颜色选择强调或次要样式。
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                if ( color )
                    // 最新版本使用强调色突出升级目标。
                    ImGui::TextColored(*color, "%s", value);
                else
                    // 普通构建信息使用禁用色降低视觉权重。
                    ImGui::TextDisabled("%s", value);
            };

            // 当前版本和最新版本始终显示，形成最小可比较信息。
            addRow(TR("ui.help.current_version").data(),
                   info.currentVersion.c_str());
            ImVec4 green(0.3f, 1.0f, 0.3f, 1.0f);
            addRow(TR("ui.help.latest_version").data(),
                   info.latestVersion.c_str(),
                   &green);

            // 服务端未提供发布日期时省略整行，不显示空值。
            if ( !info.releaseDate.empty() )
                addRow(TR("ui.help.release_date").data(),
                       info.releaseDate.c_str());

            if ( info.downloadSize > 0 ) {
                // 固定栈缓冲足以容纳格式化后的字节、KB 或 MB 文本。
                char sizeBuf[64];
                if ( info.downloadSize >= 1024 * 1024 )
                    // 大文件以一位小数 MB 显示，便于估算下载成本。
                    snprintf(sizeBuf,
                             sizeof(sizeBuf),
                             "%.1f MB",
                             info.downloadSize / (1024.0 * 1024.0));
                else if ( info.downloadSize >= 1024 )
                    // 中等文件以整数 KB 显示，避免无意义小数。
                    snprintf(sizeBuf,
                             sizeof(sizeBuf),
                             "%.0f KB",
                             info.downloadSize / 1024.0);
                else
                    // 小文件保留精确字节数。
                    snprintf(sizeBuf,
                             sizeof(sizeBuf),
                             "%lld B",
                             (long long)info.downloadSize);
                // sizeBuf 在当前作用域内有效，addRow 立即提交文本。
                addRow(TR("ui.help.file_size").data(), sizeBuf);
            }
            // 与成功 BeginTable 配对。
            ImGui::EndTable();
        }

        if ( !info.changelog.empty() ) {
            // 仅在服务端提供日志时创建阅读区域和 Markdown 渲染器。
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextUnformatted(TR("ui.help.changelog").data());

            {
                // 滚动条样式通过 RAII 作用域限制在更新日志子窗口。
                Utils::VerticalScrollbarStyleScope scrollbarStyle(dpiScale);
                // 子窗口内容内边距随 DPI 缩放，提升长文可读性。
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                    ImVec2(8.0f * dpiScale, 8.0f * dpiScale));
                // 为底部操作按钮与分隔留出空间，其余高度全部用于阅读。
                const float footerHeight =
                    36.0f * dpiScale + ImGui::GetStyle().ItemSpacing.y * 5.0f +
                    1.0f;
                // 至少保留一个像素高度，避免极小视口传入负尺寸。
                const float contentHeight = std::max(
                    1.0f, ImGui::GetContentRegionAvail().y - footerHeight);
                ImGui::BeginChild("ChangelogScroll",
                                  ImVec2(0.0f, contentHeight),
                                  ImGuiChildFlags_Borders);
                if ( m_images && m_imageVersion != info.latestVersion ) {
                    // 版本变化时扫描一次文档图片，避免每帧重复安排请求。
                    m_images->prepareDocument(info.changelog);
                    m_imageVersion = info.latestVersion;
                }
                // Markdown 渲染器仅接收观察指针，纹理由 UIManager 缓存持有。
                renderMarkdown(info.changelog,
                               MarkdownRenderOptions{ .images = m_images });
                ImGui::EndChild();
                // 与 PushStyleVar 配对，恢复外层弹窗内边距。
                ImGui::PopStyleVar();
            }
        }

        // 操作区与版本、日志内容使用分隔线隔开。
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // 按钮宽度与间距均按当前 DPI 和主题样式计算。
        float buttonWidth = 140.0f * dpiScale;
        // 没有可用下载地址时只显示取消按钮。
        float totalButtons = info.downloadUrl.empty() ? 1.0f : 2.0f;
        float spacing      = ImGui::GetStyle().ItemSpacing.x;
        float totalWidth =
            totalButtons * buttonWidth + (totalButtons - 1) * spacing;
        // 将整个按钮组而非单个按钮在窗口内居中。
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - totalWidth) * 0.5f);

        if ( !info.downloadUrl.empty() ) {
            // 下载入口只在检查结果提供 URL 时出现。
            if ( ::MMM::UI::FeedbackButton(
                     TR("ui.help.download_and_install").data(),
                     ImVec2(buttonWidth, 36.0f * dpiScale)) ) {
                // 新下载开始前清除上一次更新器启动错误。
                m_updateRestartError.clear();
                // 异步下载立即返回，进度由后续帧轮询。
                m_updateChecker->downloadAsync();
            }
            // 取消按钮与下载按钮同行排列。
            ImGui::SameLine();
        }

        if ( ::MMM::UI::FeedbackButton(
                 TR("ui.help.cancel").data(),
                 ImVec2(buttonWidth, 36.0f * dpiScale)) ) {
            // 用户取消仅关闭当前弹窗，不删除已获取的更新信息。
            ImGui::CloseCurrentPopup();
            // 标志阻止 kUpdateFound 状态在下一帧自动重开弹窗。
            m_updatePopupCanceled = true;
        }
    }

    /// @brief 渲染下载中状态。
    /// @param info 更新检查信息。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 绘制路径：只展示更新检查器进度。
    /// @note 有总大小时显示字节进度，否则显示百分比快照。
    void renderDownloadingBody(const MMM::Network::UpdateInfo& info,
                               float                           dpiScale)
    {
        // 下载状态标题基于活动字体测宽后居中。
        float textWidth =
            ImGui::CalcTextSize(TR("ui.help.downloading").data()).x;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - textWidth) * 0.5f);
        ImGui::TextUnformatted(TR("ui.help.downloading").data());

        // 进度条使用比检查动画更宽的阅读尺寸。
        float barWidth = 360.0f * dpiScale;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - barWidth) * 0.5f);
        ImGui::ProgressBar(static_cast<float>(info.downloadProgress),
                           ImVec2(barWidth, 20.0f * dpiScale));

        // 栈缓冲避免逐帧为简短进度文本分配 std::string。
        char progressText[128];
        if ( info.downloadSize > 0 ) {
            // 已知总大小时以整数 MB 展示已下载与总量。
            snprintf(progressText,
                     sizeof(progressText),
                     "%lld / %lld MB",
                     (long long)(info.downloadedBytes / (1024 * 1024)),
                     (long long)(info.downloadSize / (1024 * 1024)));
        } else {
            // 未知总字节数时退化为检查器提供的百分比。
            snprintf(progressText,
                     sizeof(progressText),
                     "%.0f%%",
                     info.downloadProgress * 100.0);
        }
        // 进度说明按当前字体测量并与进度条共同居中。
        float pWidth = ImGui::CalcTextSize(progressText).x;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - pWidth) * 0.5f);
        ImGui::TextDisabled("%s", progressText);
    }

    /// @brief 渲染下载完成状态。
    /// @param info 更新检查信息。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 绘制路径：点击重启按钮时启动更新器进程。
    /// @note 更新器启动失败保留错误文本，允许用户再次尝试。
    void renderDownloadedBody(const MMM::Network::UpdateInfo& info,
                              float                           dpiScale)
    {
        // 完成标题按当前字体宽度居中。
        float textWidth =
            ImGui::CalcTextSize(TR("ui.help.download_complete").data()).x;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - textWidth) * 0.5f);
        ImGui::TextUnformatted(TR("ui.help.download_complete").data());

        // 按钮宽度至少为 200 逻辑像素，并容纳本地化文本。
        float btnWidth = std::max(
            200.0f * dpiScale,
            ImGui::CalcTextSize(TR("ui.help.restart_to_update").data()).x +
                48.0f * dpiScale);
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - btnWidth) * 0.5f);
        if ( ::MMM::UI::FeedbackButton(TR("ui.help.restart_to_update").data(),
                                       ImVec2(btnWidth, 40.0f * dpiScale)) ) {
            // 局部错误字符串接收平台更新器启动失败详情。
            std::string restartError;
            // 每次重试先清除旧错误，成功时保持错误区域为空。
            m_updateRestartError.clear();
            if ( !MMM::Network::UpdateChecker::applyUpdateAndRestart(
                     info.downloadedFilePath,
                     info.updaterFilePath,
                     &restartError) ) {
                // 后端未提供详情时使用稳定英文兜底信息。
                m_updateRestartError = restartError.empty()
                                           ? "Failed to launch updater"
                                           : restartError;
            }
        }

        if ( !m_updateRestartError.empty() ) {
            // 仅在启动更新器失败后显示错误区域。
            ImGui::Spacing();
            // 错误使用固定危险色，与其他更新错误保持视觉一致。
            ImVec4 errColor(1.0f, 0.4f, 0.4f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, errColor);
            ImGui::TextWrapped("%s", m_updateRestartError.c_str());
            // 恢复文本颜色，避免影响后续控件。
            ImGui::PopStyleColor();
        }
    }

    /// @brief 渲染更新错误状态。
    /// @param errorMessage 错误信息。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 绘制路径：只绘制错误文本。
    /// @note 同一正文同时服务检查和下载弹窗的错误状态。
    void renderErrorBody(const std::string& errorMessage, float dpiScale)
    {
        // 标题采用危险色，详细原因使用次要文本色。
        ImVec4 errColor(1.0f, 0.4f, 0.4f, 1.0f);
        float  textWidth =
            ImGui::CalcTextSize(TR("ui.help.update_error").data()).x;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - textWidth) * 0.5f);
        ImGui::TextColored(errColor, "%s", TR("ui.help.update_error").data());

        // 错误详情允许自动换行，避免网络消息撑宽模态窗口。
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", errorMessage.c_str());
        // 恢复外层弹窗文本颜色。
        ImGui::PopStyleColor();

        // 确认按钮居中，关闭当前承载错误正文的弹窗。
        float btnWidth = 120.0f * dpiScale;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - btnWidth) * 0.5f);
        if ( ::MMM::UI::FeedbackButton(TR("ui.help.ok").data(),
                                       ImVec2(btnWidth, 32.0f * dpiScale)) ) {
            ImGui::CloseCurrentPopup();
        }
    }

    /// @brief 是否已完成启动时的自动更新检查。
    /// @note 首次 update 置位，进程生命周期内不再复位。
    bool m_hasCheckedOnStartup = false;

    /// @brief 是否为启动时的静默检查。
    /// @note 只在等待启动检查终态时为 true。
    bool m_isSilentCheck = false;

    /// @brief 是否在下一帧打开更新下载弹窗。
    /// @note 由静默或手动检查发现新版本时置位。
    bool m_showUpdatePopup = false;

    /// @brief 是否在下一帧打开更新检查中弹窗。
    /// @note 由 execute 置位，在渲染阶段消费。
    bool m_showCheckingPopup = false;

    /// @brief 是否在下一帧打开更新成功弹窗。
    /// @note 由启动更新成功标记置位，在渲染阶段消费。
    bool m_showUpdateSuccessPopup = false;

    /// @brief 用户是否取消或关闭了更新弹窗。
    /// @note 新的手动检查开始时清除，避免旧取消状态屏蔽新结果。
    bool m_updatePopupCanceled = false;

    /// @brief 点击重启更新失败时显示的错误信息。
    /// @note 新检查、新下载或新重启尝试开始时清除。
    std::string m_updateRestartError;

    /// @brief 更新检查器实例。
    /// @note 独占对象封装后台任务与线程安全 UpdateInfo 快照。
    std::unique_ptr<MMM::Network::UpdateChecker> m_updateChecker;
};
}  // namespace

/// @brief 创建检查更新动作处理器。
/// @return 独占所有权的更新状态机处理器。
/// @warning 处理器必须在 UI 线程驱动，后台工作由 UpdateChecker 管理。
std::unique_ptr<IMainMenuItemActionHandler> createCheckUpdateAction()
{
    return std::make_unique<CheckUpdateAction>();
}

}  // namespace MMM::UI
