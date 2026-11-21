/// @file CheckUpdateAction.cpp
/// @brief 更新动作共用异步状态机，桌面与触摸正文分别组合展示。
/// @details 本文件只决定弹窗与用户操作，不承担网络下载或安装器执行。
/// 状态快照来自 UpdateChecker，图像生命周期由独立缓存视图管理。
/// UI 层不得通过等待下载完成来推进检查、下载或重启阶段。
#define IMGUI_DEFINE_MATH_OPERATORS
#include "DeferredUpdatePopup.h"
#include "config/AppConfig.h"
#include "config/EditorSettings.h"
#include "config/skin/translation/Translation.h"
#include "mmmversion.h"
#include "network/UpdateChecker.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/markdown/MarkdownImageCache.h"
#include "ui/imgui/markdown/MarkdownRenderer.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuHelpActions.h"
#include "ui/imgui/status/IStatusMessageSink.h"
#include "ui/utils/DownloadRouteSelector.h"
#include "ui/utils/UIWidgetUtils.h"
#include "ui/workspace/IWorkspacePresentation.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <imgui.h>
#include <memory>
#include <string>
#include <string_view>

namespace MMM::UI
{
namespace
{
/// @brief 使用完整发布版本标识，区分同一正式版本下的不同热修复。
/// @param version 更新清单或当前编译版本的正式版本字符串。
/// @param revision 发布基线下的修订号，零对应没有热修复的正式发布。
/// @return 可用于界面展示和持久化比较的版本标识。
/// @note 两种用法共用同一格式，避免跳过记录与界面显示的版本不一致。
/// @warning 只在读取更新结果或展示版本信息时构造字符串。
std::string releaseVersion(const std::string& version, std::int64_t revision)
{
    return revision > 0 ? version + "-hotfix." + std::to_string(revision)
                        : version;
}

/// @brief 检查更新动作。
/// @details 管理启动静默检查、手动检查、下载、重启安装及三个独立弹窗状态。
/// @warning 网络和下载工作必须留在 UpdateChecker 异步任务中，UI 只轮询快照。
class CheckUpdateAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 跨菜单和操作方案复用的稳定动作身份。
    /// @warning UI 热路径只返回静态身份，不构造字符串或执行配置查询。
    std::string_view operationId() const override
    {
        return "CheckUpdateAction";
    }

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
                // 这是上一次安装的结果，独立于自动检查的偏好。
                m_showUpdateSuccessPopup = true;
            } else if ( Config::AppConfig::instance()
                            .getEditorSettings()
                            .m_showUpdateOnStartup ) {
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
            // 跳过只匹配完整发布标识，更高的热修复修订号也能重新提醒。
            // 手动结果不经过此路径，即使用户跳过了同一发布也能查到。
            // 检查期间用户可能更改了设置，以结果到达时的最新偏好为准。
            const auto& settings =
                Config::AppConfig::instance().getEditorSettings();
            m_showUpdatePopup = settings.m_showUpdateOnStartup &&
                                settings.m_skippedUpdateVersion !=
                                    releaseVersion(info.latestVersion,
                                                   info.latestHotfixRevision);
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
        if ( m_updatePreferenceSaveFailed ) {
            // 弹窗已关闭，通过状态栏提示用户本次选择未能持久化。
            context.statusMessageSink.showStatusMessage(
                TR("ui.help.update_preference_save_failed").data(), 5.0f);
            // 保留内存中的选择，以便后续常规保存有机会写入配置。
            m_updatePreferenceSaveFailed = false;
        }
    }

private:
    /// @brief UIManager 持有图片纹理，菜单动作只保存稳定观察指针。
    /// @warning 指针仅在 UIManager 生命周期内有效，不得释放或跨管理器复用。
    MarkdownImageCache* m_images{ nullptr };
    /// @brief 已扫描的更新版本，避免逐帧重新安排图片任务。
    /// @note 版本变化时重新解析对应更新日志中的远程图片。
    std::string m_imageVersion;
    /// @brief 图片预览保存目标而非纹理句柄，保证动画帧与缓存生命周期正确。
    /// @note 双击回调复制目标，不能借用 Markdown 解析中的临时视图。
    std::string m_imagePreviewDestination;
    /// @brief Markdown 双击只登记请求，待退出日志子窗口后再打开预览弹窗。
    bool m_openImagePreview{ false };
    /// @brief 预览窗口的标题栏关闭状态。
    bool m_imagePreviewOpen{ false };
    /// @brief 渲染更新检查中的状态弹窗。
    /// @warning UI 热路径：每帧执行；只轮询更新检查器状态。
    /// @note 检查中、已最新、发现更新和错误四种状态互斥展示。
    void renderUpdateCheckingPopup()
    {
        // PGO 同意框等其它弹层结束后才打开检查弹窗，避免共享宿主内互相抢占。
        Detail::openDeferredUpdatePopup(TR("ui.help.check_update").data(),
                                        m_showCheckingPopup);

        // 每帧读取当前 DPI，使弹窗和进度控件跟随窗口缩放。
        float dpiScale = Config::AppConfig::instance().getWindowContentScale();
        // 居中样式作用域在所有分支离开时恢复临时窗口设置。
        Utils::CenteredModalPopupScope modalScope(dpiScale);
        if ( modalScope.begin(TR("ui.help.check_update").data()) ) {
            // 异步状态可能在弹窗打开期间变化，因此每帧获取新快照。
            auto info = m_updateChecker->getInfo();
            // 已找到更新时直接交接到确认弹窗，由确认弹窗负责此次反馈。
            if ( info.status != MMM::Network::UpdateStatus::kUpdateFound )
                Detail::confirmDeferredUpdatePopup(m_showCheckingPopup);

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

        // 发现更新只登记一次请求；首帧被欢迎页抢焦点时仍可重新展示。
        Detail::openDeferredUpdatePopup(TR("ui.help.update_found").data(),
                                        m_showUpdatePopup);

        // 布局使用当前内容缩放，不依赖可能过期的菜单上下文。
        float dpiScale = Config::AppConfig::instance().getWindowContentScale();
        // 居中作用域统一管理弹窗位置和样式恢复。
        Utils::CenteredModalPopupScope modalScope(dpiScale);
        // 更新日志包含图片时需要独立阅读空间，尺寸随主视口工作区收缩。
        const bool found =
            info.status == MMM::Network::UpdateStatus::kUpdateFound;
        // 触摸版即使没有日志也需要给正文滚动区和底部操作留出空间。
        const bool largePopup = found && (!info.changelog.empty() ||
                                          !workspaceAllowsHoverFeedback());
        // 工作区尺寸排除平台保留区域，避免阅读窗口超出可用屏幕。
        const ImVec2 workSize = ImGui::GetMainViewport()->WorkSize;
        // 有日志时提供大窗口，无日志时保持内容自适应尺寸。
        const ImVec2 desiredSize =
            largePopup
                ? ImVec2(std::min(960.0f * dpiScale, workSize.x * 0.9f),
                         std::min(900.0f * dpiScale, workSize.y * 0.9f))
                : ImVec2(found ? std::min(480.0f * dpiScale, workSize.x * 0.9f)
                               : 0.0f,
                         0.0f);
        if ( modalScope.begin(TR("ui.help.update_found").data(),
                              nullptr,
                              ImGuiWindowFlags_None,
                              desiredSize,
                              !largePopup) ) {
            Detail::confirmDeferredUpdatePopup(m_showUpdatePopup);
            // Begin 后重新读取快照，减少状态在布局判断与正文之间变化的窗口。
            info = m_updateChecker->getInfo();

            // 入口位于正文滚动区外，桌面和触摸界面都能在下载中直接换路。
            // 只发布会话选项，文件重下由后台处理，不能在 UI 等待旧请求退出。
            if ( info.status == MMM::Network::UpdateStatus::kUpdateFound ||
                 info.status == MMM::Network::UpdateStatus::kDownloading ||
                 (info.status == MMM::Network::UpdateStatus::kError &&
                  !info.downloadUrl.empty()) )
                drawDownloadRouteSelector(
                    info.status == MMM::Network::UpdateStatus::kDownloading,
                    dpiScale);

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

            // 图片弹窗属于更新弹窗，必须在退出日志子窗口后提交。
            renderImagePreview(dpiScale);
            // 与成功 BeginPopup 路径配对，保持弹窗栈平衡。
            ImGui::EndPopup();
        } else if ( m_imagePreviewOpen ) {
            // 父窗口关闭时子弹窗也失效，及时归还独立高清图集。
            if ( m_images ) m_images->releasePreviewImage();
            m_imagePreviewOpen = false;
            m_imagePreviewDestination.clear();
        }
    }

    /// @brief 本帧可见操作的网格尺寸，供日志滚动区预留空间。
    /// @details 布局与绘制共用测量值，避免切换语言或窗口尺寸后底部被截断。
    /// 结构只借用当帧的本地化标签，不跟随窗口跨帧缓存。
    /// 下载缺失时仍可关闭提醒，因此操作区至少包含三个有效项。
    /// 每行等宽用于保证后一个按钮不会因前一个本地化文本较长而偏离预期范围。
    struct UpdateFoundActionLayout {
        /// @brief 操作顺序同时决定文本、图标及点击行为。
        enum Action : std::size_t { Install, Later, SkipVersion, HideReminder };

        /// @brief 依次为安装、稍后提醒、跳过发布和关闭启动提醒。
        std::array<const char*, 4> m_labels;
        /// @brief 下载资产缺失时从第二个操作开始绘制。
        std::size_t m_first;
        /// @brief 本帧一行可容纳的等宽操作数。
        std::size_t m_columns;
        /// @brief 受内容宽度限制的单个操作尺寸。
        ImVec2 m_buttonSize;

        /// @brief 按与实际绘制一致的行数和项间距预留底部空间。
        /// @return 完整按钮区高度，分隔线与正文间距由调用方另行预留。
        /// @warning UI 热路径：只计算尺寸，不写入 ImGui 状态。
        float height() const
        {
            // 向上取整使未填满的最后一行也计入高度；列数至少为一。
            const auto rows =
                (m_labels.size() - m_first + m_columns - 1) / m_columns;
            return rows * m_buttonSize.y +
                   (rows - 1) * ImGui::GetStyle().ItemSpacing.y;
        }
    };

    /// @brief 测量当前语言下的更新操作，窄窗口逐步换行以保留按钮文字。
    /// @param info 用于判断当前平台是否有可操作的下载地址。
    /// @param scale 决定按钮基础尺寸。
    /// @param touch 为触摸操作预留最小高度和图标空间。
    /// @return 供正文和按钮绘制共同使用的行列数、尺寸和标签。
    /// @warning UI 热路径：每帧最多测量四个短文本，不分配绘制缓存。
    UpdateFoundActionLayout measureUpdateFoundActions(
        const MMM::Network::UpdateInfo& info, float scale, bool touch) const
    {
        UpdateFoundActionLayout layout{
            .m_labels     = { TR("ui.help.download_and_install").data(),
                              TR("ui.help.cancel").data(),
                              TR("ui.help.skip_update_version").data(),
                              TR("ui.help.hide_startup_update").data() },
            .m_first      = info.downloadUrl.empty()
                                ? UpdateFoundActionLayout::Later
                                : UpdateFoundActionLayout::Install,
            .m_columns    = 1,
            .m_buttonSize = { 0.0f,
                              std::max((touch ? 48.0f : 36.0f) * scale,
                                       ImGui::GetFrameHeight()) }
        };
        const auto& style     = ImGui::GetStyle();
        const float available = ImGui::GetContentRegionAvail().x;
        float       width     = (touch ? 100.0f : 140.0f) * scale;
        for ( auto i = layout.m_first; i < layout.m_labels.size(); ++i ) {
            // 触摸策略在文字外还绘制图标，测量需留出图标和两侧内边距。
            width = std::max(
                width,
                ImGui::CalcTextSize(layout.m_labels[i]).x +
                    (touch ? 56.0f * scale : 2.0f * style.FramePadding.x));
        }
        const auto count = layout.m_labels.size() - layout.m_first;
        // 宽桌面允许整排展示；触摸版最多两列，避免触点密集而误触。
        // 列数变化同时影响正文的剩余高度，本帧即可重新分配。
        if ( !touch &&
             count * width + (count - 1) * style.ItemSpacing.x <= available )
            layout.m_columns = count;
        else if ( 2.0f * width + style.ItemSpacing.x <= available )
            layout.m_columns = 2;
        // 单列时撑满可用宽度；多列时限制按钮宽度，避免文字被相邻项覆盖。
        layout.m_buttonSize.x =
            layout.m_columns == 1 ? std::max(1.0f, available) : width;
        return layout;
    }

    /// @brief 在各平台共享更新操作及持久化语义。
    /// @param info 用户此刻看到的发布快照，跳过必须绑定这次的实际版本。
    /// @param layout 当前内容区域内绘制的按钮尺寸和排列。
    /// @details 稍后再说只处理本次通知。另两种关闭方式写入客户端全局设置，
    /// 并保留检查器已取得的结果供本次会话内的手动查询使用。
    /// @warning UI 热路径：只绘制有限数量按钮，明确点击偏好项时才保存配置。
    void renderUpdateFoundActions(const MMM::Network::UpdateInfo& info,
                                  const UpdateFoundActionLayout&  layout)
    {
        constexpr std::array icons{
            ICON_MMM_DOWNLOAD, ICON_MMM_ARROW_LEFT, ICON_MMM_CLOSE, ICON_MMM_EYE
        };
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        for ( auto i = layout.m_first; i < layout.m_labels.size(); ++i ) {
            const auto column = (i - layout.m_first) % layout.m_columns;
            if ( column == 0 ) {
                // 最后一行不足整列时单独居中，避免只有三个操作时留下空位。
                const auto count =
                    std::min(layout.m_columns, layout.m_labels.size() - i);
                const float width =
                    count * layout.m_buttonSize.x + (count - 1) * spacing;
                ImGui::SetCursorPosX(
                    ImGui::GetCursorPosX() +
                    std::max(
                        0.0f,
                        (ImGui::GetContentRegionAvail().x - width) * 0.5f));
            } else {
                // 只有同一行的后续按钮继承前一项横向位置，换行由默认游标推进。
                ImGui::SameLine();
            }
            if ( !workspaceActionButton(
                     layout.m_labels[i], icons[i], layout.m_buttonSize) )
                continue;

            if ( i == UpdateFoundActionLayout::Install ) {
                // 下载采用既有异步路径，操作区下帧进入进度展示。
                m_updateRestartError.clear();
                m_updateChecker->downloadAsync();
                break;
            }
            if ( i != UpdateFoundActionLayout::Later ) {
                // 跳过单个发布仍会在启动时检查，关闭自动检查也保留此前跳过的版本。
                auto& config   = Config::AppConfig::instance();
                auto& settings = config.getEditorSettings();
                if ( i == UpdateFoundActionLayout::SkipVersion )
                    settings.m_skippedUpdateVersion = releaseVersion(
                        info.latestVersion, info.latestHotfixRevision);
                else
                    settings.m_showUpdateOnStartup = false;
                // 同步保存仅发生在用户点击时，立即退出也能保留选择。
                m_updatePreferenceSaveFailed = !config.save();
            }
            // 用户主动关闭即消费请求，首次可见帧点击也不会再次打开。
            m_showUpdatePopup = false;
            ImGui::CloseCurrentPopup();
            break;
        }
    }

    /// @brief 在更新弹窗上层显示可移动、可调整大小的完整图片预览。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning 每帧重新查询缓存帧，不能持有可能因更新版本而失效的纹理。
    /// @details 预览是更新弹窗的子模态窗口，关闭父窗口时不再接受输入。
    /// 初始位置和大小只设一次，避免拖动或调整大小后被逐帧重置。
    /// 图片绘制使用原始比例和当前图集 UV，避免放大 GIF 时停在首帧。
    void renderImagePreview(float dpiScale)
    {
        // 固定内部 ID，翻译语言变化不应改变弹窗身份。
        const std::string title =
            std::string(TR("ui.help.image_preview").data()) +
            "###UpdateImagePreview";
        if ( m_openImagePreview ) {
            // 双击发生于子窗口，弹窗请求须在父窗口上下文中消费。
            ::MMM::UI::FeedbackOpenPopup(title.c_str());
            // 放大图使用独立高清帧；加载期间先显示原有缩略图。
            if ( m_images )
                m_images->preparePreviewImage(m_imagePreviewDestination);
            m_openImagePreview = false;
            m_imagePreviewOpen = true;
        }

        // 首次显示时放到主视口中央，后续允许用户拖动和缩放。
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const ImVec2         previewSize{
            std::min(1100.0f * dpiScale, viewport->WorkSize.x * 0.9f),
            std::min(850.0f * dpiScale, viewport->WorkSize.y * 0.9f)
        };
        // 工作区比例限制初始大小，留出标题栏和窗口边缘的操作空间。
        // 与更新弹窗绑定同一主视口，避免多视口环境中跳到其他屏幕。
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::SetNextWindowPos(
            viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(previewSize, ImGuiCond_Appearing);
        if ( ImGui::BeginPopupModal(title.c_str(),
                                    &m_imagePreviewOpen,
                                    ImGuiWindowFlags_NoSavedSettings) ) {
            // 只保存 Markdown 目标；动态图片的 UV 必须每帧从缓存查询。
            // 若更新日志换版，旧目标查询失败时显示状态文本而非旧纹理。
            const MarkdownImage image =
                m_images ? m_images->findPreviewImage(m_imagePreviewDestination)
                         : MarkdownImage{};
            // 缓存尚未给出有效帧尺寸时避免比例计算中的除零。
            if ( image.texture && image.size.x > 0.0f && image.size.y > 0.0f ) {
                const ImVec2 available = ImGui::GetContentRegionAvail();
                // 按窗口内容区域等比放大，也允许小视口下等比缩小。
                // 极小窗口仍保留正比例，避免向 ImGui 提交无效图片尺寸。
                // 以缓存报告的帧尺寸计算比例，不使用图集纹理的总尺寸。
                const float scale =
                    std::max(0.01f,
                             std::min(available.x / image.size.x,
                                      available.y / image.size.y));
                const ImVec2 displaySize{ image.size.x * scale,
                                          image.size.y * scale };
                ImGui::SetCursorPosX(
                    ImGui::GetCursorPosX() +
                    std::max(0.0f, (available.x - displaySize.x) * 0.5f));
                // 空白部分单独居中，动画帧的图集 UV 保持不变。
                ImGui::SetCursorPosY(
                    ImGui::GetCursorPosY() +
                    std::max(0.0f, (available.y - displaySize.y) * 0.5f));
                ImGui::Image(image.texture, displaySize, image.uv0, image.uv1);
            } else {
                // 加载失败或资源暂不可用时给出明确状态，避免空白窗口。
                ImGui::TextUnformatted(TR("ui.help.image_unavailable").data());
            }
            ImGui::EndPopup();
        }
        // 标题栏关闭后清除目标，避免下一次预览误用旧图。
        if ( !m_imagePreviewOpen && !m_imagePreviewDestination.empty() ) {
            // 释放请求只更新缓存状态，真正等待在途 GPU 命令由资源准备阶段处理。
            // 清空目标后，后续弹窗帧不能再查询刚关闭的高清描述符。
            if ( m_images ) m_images->releasePreviewImage();
            m_imagePreviewDestination.clear();
        }
    }

    /// @brief 渲染更新下载成功后的提示弹窗。
    /// @warning UI 热路径：每帧执行；只绘制完成提示。
    /// @note 成功标记通常来自更新器重启后的启动阶段。
    void renderUpdateSuccessPopup()
    {
        // 更新完成通知同样等待其它模态结束，不覆盖当前用户确认流程。
        Detail::openDeferredUpdatePopup(TR("ui.help.update_success").data(),
                                        m_showUpdateSuccessPopup);

        // 成功窗口同样跟随当前 DPI 缩放。
        float dpiScale = Config::AppConfig::instance().getWindowContentScale();
        Utils::CenteredModalPopupScope modalScope(dpiScale);
        // 弹窗未打开时提前退出，不提交后续表格和按钮。
        if ( !modalScope.begin(TR("ui.help.update_success").data()) ) return;
        Detail::confirmDeferredUpdatePopup(m_showUpdateSuccessPopup);

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
        const bool touch = !workspaceAllowsHoverFeedback();
        // 两个平台的正文都使用实测操作区高度，窄窗口时会自动预留更多空间。
        const auto actions = measureUpdateFoundActions(info, dpiScale, touch);
        // 触摸版的元数据也在滚动区，操作区保持和桌面一致的行为。
        if ( touch ) {
            renderTouchUpdateFoundBody(info, actions.height());
            ImGui::Separator();
            renderUpdateFoundActions(info, actions);
            return;
        }
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
            // 热修复保留正式版本号，显示独立修订避免同版本更新看起来毫无变化。
            // 格式化只用于展示，网络比较仍使用正式版本和整数修订两个字段。
            // 普通发布不附加零修订，保持原显示格式和翻译表契约。
            // 字符串在整个表格绘制期间存活，不传递临时 c_str。
            // 重启仍由既有按钮接管，展示修订不改变安装时机。
            const auto currentDisplay =
                releaseVersion(info.currentVersion, info.currentHotfixRevision);
            const auto latestDisplay =
                releaseVersion(info.latestVersion, info.latestHotfixRevision);
            addRow(TR("ui.help.current_version").data(),
                   currentDisplay.c_str());
            ImVec4 green(0.3f, 1.0f, 0.3f, 1.0f);
            addRow(TR("ui.help.latest_version").data(),
                   latestDisplay.c_str(),
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
            ImGui::TextDisabled("%s", TR("ui.help.image_preview_hint").data());

            {
                // 滚动条样式通过 RAII 作用域限制在更新日志子窗口。
                Utils::VerticalScrollbarStyleScope scrollbarStyle(dpiScale);
                // 子窗口内容内边距随 DPI 缩放，提升长文可读性。
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                    ImVec2(8.0f * dpiScale, 8.0f * dpiScale));
                // 为底部操作按钮与分隔留出空间，其余高度全部用于阅读。
                // 额外间距包含正文之后的分隔线及上下的布局留白。
                const float footerHeight =
                    actions.height() + ImGui::GetStyle().ItemSpacing.y * 5.0f +
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
                renderMarkdown(
                    info.changelog,
                    MarkdownRenderOptions{
                        .images = m_images,
                        .onImageDoubleClick =
                            [this](std::string_view destination) {
                                // 回调可能处于子窗口中，延迟到父弹窗创建预览。
                                m_imagePreviewDestination.assign(destination);
                                m_openImagePreview = true;
                            } });
                ImGui::EndChild();
                // 与 PushStyleVar 配对，恢复外层弹窗内边距。
                ImGui::PopStyleVar();
            }
        }

        // 操作区与版本、日志内容使用分隔线隔开。
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        renderUpdateFoundActions(info, actions);
    }

    /// @brief 在触摸布局的独立子窗口内滚动版本信息与更新日志。
    /// @param info 当前帧更新结果。
    /// @details 触摸版将版本信息和长日志放在同一滚动区域。
    /// 底部操作由父窗口分别提交，正文滚动或旋转设备时仍可直接点击。
    /// @param actionsHeight 已测量的操作区高度，包含按钮间距。
    /// @warning 每帧只绘制现有快照和缓存图片，不同步等待网络。
    void renderTouchUpdateFoundBody(const MMM::Network::UpdateInfo& info,
                                    float actionsHeight)
    {
        // 从实际余高扣除操作区与分隔，正文滚动时始终保留底部操作。
        const float footer =
            actionsHeight + ImGui::GetStyle().ItemSpacing.y * 2.0f;
        const float body =
            std::max(1.0f, ImGui::GetContentRegionAvail().y - footer);
        // 子窗口只占剩余空间，服务端未提供日志时也保留固定操作区。
        ImGui::BeginChild("TouchUpdateDetails", { 0, body });
        ImGui::PushTextWrapPos(0);
        // 版本标签和值放在同一文本流内，长版本号和翻译均可按宽度换行。
        // 元数据不再对齐到输入框内边距，避免三行说明占掉大半屏幕。
        ImGui::Text(
            "%s: %s",
            TR("ui.help.current_version").data(),
            releaseVersion(info.currentVersion, info.currentHotfixRevision)
                .c_str());
        ImGui::TextColored(
            { .3f, 1, .3f, 1 },
            "%s: %s",
            TR("ui.help.latest_version").data(),
            releaseVersion(info.latestVersion, info.latestHotfixRevision)
                .c_str());
        if ( !info.releaseDate.empty() )
            // 服务端未给日期时省略该行，不为缺失字段保留固定空白高度。
            ImGui::Text("%s: %s",
                        TR("ui.help.release_date").data(),
                        info.releaseDate.c_str());
        if ( info.downloadSize > 0 )
            // 字节数沿用快照，只转换展示单位，不启动额外大小探测请求。
            ImGui::Text("%s: %.1f MB",
                        TR("ui.help.file_size").data(),
                        info.downloadSize / (1024.0 * 1024.0));
        ImGui::PopTextWrapPos();
        // 文本换行作用域在 Markdown 前结束，正文渲染器自行管理段落布局。
        // 单滚动区允许用户从空白处拖动阅读，不要求精确命中细滚动条。
        if ( !info.changelog.empty() ) {
            ImGui::Separator();
            ImGui::TextUnformatted(TR("ui.help.changelog").data());
            // 图片只在日志版本变化时准备；双击映射为移动端连续两次点按。
            if ( m_images && m_imageVersion != info.latestVersion ) {
                // 缓存对象由 UIManager 持有，当前动作只借用观察指针。
                m_images->prepareDocument(info.changelog);
                m_imageVersion = info.latestVersion;
            }
            renderMarkdown(
                info.changelog,
                MarkdownRenderOptions{
                    .images             = m_images,
                    .onImageDoubleClick = [this](std::string_view destination) {
                        // 子区只记录预览意图；父弹窗稍后处理，保持弹窗栈层级。
                        m_imagePreviewDestination.assign(destination);
                        m_openImagePreview = true;
                    } });
        }
        ImGui::EndChild();
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

    /// @brief 是否还有未确认展示的新版本通知。
    /// @note 静默或手动检查发现新版本时置位，持续可见后清除。
    bool m_showUpdatePopup = false;

    /// @brief 是否还有未确认展示的手动检查通知。
    /// @note 由 execute 置位，持续可见或交接到更新确认弹窗时清除。
    bool m_showCheckingPopup = false;

    /// @brief 是否还有未确认展示的更新成功通知。
    /// @note 由启动更新成功标记置位，持续可见后清除。
    bool m_showUpdateSuccessPopup = false;

    /// @brief 更新提醒偏好写盘失败时转交主菜单状态栏一次。
    bool m_updatePreferenceSaveFailed = false;

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
