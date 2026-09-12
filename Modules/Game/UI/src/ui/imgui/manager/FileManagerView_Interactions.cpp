#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/ui/UISubViewToggleEvent.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "ui/UIManager.h"
#include "ui/imgui/SideBarUI.h"
#include "ui/imgui/manager/FileManagerView.h"
#include "ui/layout/box/CLayBox.h"
#include "ui/utils/NativeFileDialog.h"
#include "ui/utils/UIWidgetUtils.h"
#include <ImGuiFileDialog.h>
#include <nfd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fmt/format.h>
#include <system_error>

/// @file FileManagerView_Interactions.cpp
/// @brief 文件管理器的拖放项目路由、无项目欢迎页和目录选择器入口实现。
/// @details GLFW 拖放在 UI 帧内做窗口命中与文件分类；无项目页只读取最近项目
/// 缓存并通过 Clay 布局，原生与内置目录选择器共享 OpenProjectEvent 出口。

namespace MMM::UI
{
namespace
{
/// @brief 将 ASCII 扩展名转换为小写。
/// @param value 输入扩展名。
/// @return 小写后的扩展名。
/// @note 非 ASCII UTF-8 字节不参与扩展名白名单匹配。
std::string toLowerAscii(std::string value)
{
    // 扩展名按值传入，允许在原字符串缓冲上原地转换。
    std::transform(
        value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            // unsigned char 满足 std::tolower 对输入范围的要求。
            return static_cast<char>(std::tolower(ch));
        });
    return value;
}

/// @brief 判断拖拽文件是否为 zip 兼容谱面包。
/// @param path 拖拽文件路径。
/// @return 支持按临时项目打开时返回 true。
/// @note 只按末级扩展名分类，不读取或验证归档内容。
bool isTemporaryPackagePath(const std::filesystem::path& path)
{
    // 路径扩展名先转换为 UTF-8，再做与平台无关的 ASCII 折叠。
    const auto extension = toLowerAscii(Config::pathToUtf8(path.extension()));
    // 列表与主窗口临时项目包路由支持的容器类型保持一致。
    return extension == ".zip" || extension == ".7z" || extension == ".mcz" ||
           extension == ".osz" || extension == ".mpk";
}
}  // namespace

/// @brief 消费落在文件管理器上的待处理 GLFW 文件拖放事件。
/// @param sourceManager 当前 UI 管理器，用于把目标管理器页签随项目一起打开。
/// @details 谱面包留给主窗口级 ProjectDropRouter；普通文件以父目录打开项目，
/// 再按扩展名切换到谱面、音频或默认文件浏览器。
/// @warning UI 热路径：无待处理事件时立即返回；有事件时执行低频文件类型查询。
void FileManagerView::handleDragDrop(UIManager* sourceManager)
{
    // 绝大多数帧没有拖放，避免额外窗口和文件系统处理。
    if ( m_pendingDrops.empty() ) return;

    // 根窗口与子窗口都视为文件管理器区域，活动控件不阻止拖放命中。
    bool isHovered =
        ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows |
                               ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    // 按 GLFW 投递顺序处理本帧积累的拖放批次。
    for ( const auto& drop : m_pendingDrops ) {
        // 批次原始坐标不用于命中，当前窗口 Hover 状态是权威依据。
        if ( drop.paths.empty() ) {
            // 防御平台回调给出空路径数组。
            continue;
        }

        // 当前交互只以批次首个路径决定打开项目和目标页签。
        std::filesystem::path p = Config::utf8ToPath(drop.paths[0]);
        if ( !isHovered ) {
            // 不在文件管理器区域的批次由其他全局拖放路由处理。
            continue;
        }

        if ( isTemporaryPackagePath(p) ) {
            // 谱包由主窗口级路由统一解包并打开，避免重复项目切换。
            continue;
        }

        // error_code 重载避免拖放无效路径通过异常离开 UI 回调。
        std::error_code filesystemError;
        filesystemError.clear();
        const bool isDirectory =
            std::filesystem::is_directory(p, filesystemError) &&
            !filesystemError;
        // 目录拖放由主窗口级路由处理，文件管理器只处理普通文件父目录。
        if ( isDirectory ) continue;
        // 保留条件表达式以明确目录语义；当前目录分支已在上方交给全局路由。
        std::filesystem::path projectPath = isDirectory ? p : p.parent_path();

        // 日志同时记录原文件与推导出的项目目录，便于定位错误路由。
        XINFO("File dropped on FileManager: {}, opening project: {}",
              Config::pathToUtf8(p),
              Config::pathToUtf8(projectPath));

        // 项目打开统一交给控制器事件，不从 UI 线程直接加载目录。
        Event::OpenProjectEvent ev;
        ev.m_projectPath = projectPath;
        Event::EventBus::instance().publish(ev);
        // 项目事件先于页签事件发布，使视图切换时根目录已经进入加载流程。

        // 文件扩展名决定打开项目后应展示的内容管理器。
        auto ext = Config::pathToUtf8(p.extension());
        // 未识别类型默认保留文件浏览器，便于查看原文件位置。
        SideBarTab targetTab = SideBarTab::FileExplorer;
        if ( ext == ".osu" || ext == ".imd" || ext == ".mc" ) {
            // 已知谱面格式直接切换谱面浏览器。
            targetTab = SideBarTab::BeatMapExplorer;
        } else if ( ext == ".mp3" || ext == ".ogg" || ext == ".wav" ||
                    ext == ".flac" || ext == ".opus" || ext == ".aac" ||
                    ext == ".m4a" ) {
            // 已知音频格式切换音频浏览器。
            targetTab = SideBarTab::AudioExplorer;
        }

        // 发布显示事件，确保 SideBarManager 展开并选择推导出的子视图。
        Event::UISubViewToggleEvent evt;
        // 来源名称用于侧栏避免反向重复持久化。
        evt.sourceUiName           = m_subViewName;
        evt.uiManager              = sourceManager;
        evt.targetFloatManagerName = "SideBarManager";
        // 管理器内部仍使用本地化子视图 ID。
        evt.subViewId = TabToSubViewId(targetTab);
        // 拖放始终要求显示目标，而不是切换当前可见状态。
        evt.showSubView = true;
        Event::EventBus::instance().publish(evt);
    }
    // 所有批次无论是否命中都在本帧消费，避免后续帧重复打开项目。
    m_pendingDrops.clear();
}

/// @brief 渲染未打开项目时的文件浏览器占位内容。
/// @param layoutContext 当前子视图布局区域和 DPI 缩放。
/// @details 页面由提示、打开目录按钮和可选最近项目列表组成；最近项目只使用
/// AppConfig 缓存路径，不在渲染期间验证文件系统存在性。
/// @note Clay 回调只在本次 render 内执行，路径和标签均按值或稳定视图捕获。
/// @warning UI 热路径：未打开项目且子视图可见时每帧执行。
/// 避免文件系统扫描或高开销所有权操作。
void FileManagerView::renderEmptyProjectView(LayoutContext& layoutContext)
{
    // 复用主视图尺寸辅助函数，保证最小尺寸与实际布局参数一致。
    const auto metrics = getEmptyProjectViewMetrics(layoutContext.m_dpiScale);
    // Clay padding 使用 uint16_t，需要先限制负值再向上取整。
    auto toLayoutPixels = [](float value) {
        // 度量值范围很小，非负向上取整后可安全存入 Clay uint16_t 字段。
        return static_cast<uint16_t>(std::ceil(std::max(0.0f, value)));
    };

    // 页面 padding 与行间距转换为 Clay 接受的整数像素。
    const uint16_t layoutPadding = toLayoutPixels(metrics.padding);
    const uint16_t layoutGap     = toLayoutPixels(metrics.gap);

    // 根 VBox 依次容纳提示、按钮和最近项目区域。
    CLayVBox rootVBox;
    // 按钮标签在布局前缓存，保证测量和绘制使用同一翻译文本。
    const char* openDirectoryLabel =
        TR("ui.file_manager.open_directory").data();
    // 按钮固定宽度包含文本、FramePadding 与边框余量。
    const float openButtonWidth =
        std::ceil(ImGui::CalcTextSize(openDirectoryLabel).x +
                  ImGui::GetStyle().FramePadding.x * 2.0f + 2.0f);

    // 提示行两侧弹簧让文本区域保持水平居中。
    CLayHBox labelHBox;
    labelHBox.setAlignment(Alignment::Center());
    labelHBox.addSpring()
        .addElement(
            "InitialHint",
            Sizing::Grow(),
            Sizing::Fixed(metrics.buttonHeight),
            [=](Clay_BoundingBox r, bool isHovered) {
                // 绘制时重新取得翻译视图，语言变化可在下一帧立即反映。
                const char* label = TR("ui.file_manager.initial_hint").data();
                // 文本实际边界用于水平与垂直居中。
                const ImVec2 textSize  = ImGui::CalcTextSize(label);
                const float  textLineH = ImGui::GetTextLineHeight();
                // 窄窗口下偏移不允许为负，文本从区域起点开始裁剪。
                const float offsetX =
                    std::max(0.0f, (r.width - textSize.x) * 0.5f);
                const float offsetY =
                    std::max(0.0f, (r.height - textLineH) * 0.5f);
                // Clay 给出绝对屏幕矩形，直接设置 ImGui 屏幕游标。
                ImGui::SetCursorScreenPos({ r.x + offsetX, r.y + offsetY });
                ImGui::TextUnformatted(label);
            })
        .addSpring();

    // 打开目录按钮同样通过两侧弹簧居中。
    CLayHBox buttonHBox;
    buttonHBox.setAlignment(Alignment::Center());
    buttonHBox.addSpring()
        .addElement(
            "OpenDirButton",
            Sizing::Fixed(openButtonWidth),
            Sizing::Fixed(metrics.buttonHeight),
            [this, openDirectoryLabel](Clay_BoundingBox r, bool isHovered) {
                // 可见按钮使用统一 FeedbackButton 入口处理悬浮和点击音效。
                if ( ::MMM::UI::FeedbackButton(openDirectoryLabel,
                                               { r.width, r.height }) ) {
                    // 用户点击低频路径才打开阻塞式或模态目录选择器。
                    this->openFolderPicker();
                }
            })
        .addSpring();

    // 最近项目容器即使为空也可安全加入根布局并占据剩余空间。
    CLayVBox recentVBox;
    // recentProjects 是软件级缓存，不属于当前项目状态。
    const auto& recent =
        Config::AppConfig::instance().getEditorConfig().recentProjects;

    if ( !recent.empty() ) {
        // 列表存在时才应用顶部 padding 与项目项间距。
        recentVBox
            .setPadding(
                layoutPadding, 0, toLayoutPixels(metrics.recentTopPadding), 0)
            .setSpacing(layoutGap);
        // 列表标题固定一行高度并使用弱化文本颜色。
        recentVBox.addElement(
            "RecentTitle",
            Sizing::Grow(),
            Sizing::Fixed(metrics.recentTitleHeight),
            [](Clay_BoundingBox r, bool isHovered) {
                // 标题按当前字体行高在 Clay 区域内垂直居中。
                const float textLineH = ImGui::GetTextLineHeight();
                const float offsetY =
                    std::max(0.0f, (r.height - textLineH) * 0.5f);
                ImGui::SetCursorScreenPos({ r.x, r.y + offsetY });
                ImGui::TextDisabled("%s", TR("ui.file.open_recent").data());
            });

        // 每个缓存路径生成一个独立可滚动选择项。
        for ( size_t i = 0; i < recent.size(); ++i ) {
            const auto& path = recent[i];
            // 索引构成稳定 Clay ID，即使两个路径文件名相同也不冲突。
            recentVBox.addElement(
                fmt::format("RecentItem_{}", i),
                Sizing::Grow(),
                Sizing::Fixed(metrics.recentItemHeight),
                [path, i](Clay_BoundingBox r, bool isHovered) {
                    // isHovered 不直接使用，选择项通过 ImGui 当前 Item
                    // 状态判断。 路径按值捕获，配置数组变化不会使回调引用失效。
                    std::filesystem::path p = Config::utf8ToPath(path);
                    // 优先显示末级目录名，无法取得时回退完整缓存文本。
                    std::string name = Config::pathToUtf8(p.filename());
                    if ( name.empty() ) name = path;
                    // ImGui ID 使用索引而非可见名称，支持同名项目。
                    const std::string itemId =
                        fmt::format("RecentProject_{}", i);
                    // 滚动选择项在宽度不足时水平滚动，并以完整路径作提示。
                    Utils::renderScrollingSelectable(
                        itemId,
                        name,
                        r.width,
                        r.height,
                        [p]() {
                            // 点击只发布打开事件，实际校验与加载由项目控制器处理。
                            Event::OpenProjectEvent ev;
                            ev.m_projectPath = p;
                            Event::EventBus::instance().publish(ev);
                        },
                        path);
                    if ( ImGui::IsItemHovered() ) {
                        // 手形指针提示整行最近项目可点击。
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    }
                });
        }
    }

    // 根布局应用统一外边距，并为前三个区域指定高度策略。
    rootVBox
        .setPadding(layoutPadding, layoutPadding, layoutPadding, layoutPadding)
        .setSpacing(layoutGap)
        .addLayout("labelHBox",
                   labelHBox,
                   Sizing::Grow(),
                   Sizing::Fixed(metrics.hintRowHeight))
        .addLayout("buttonHBox",
                   buttonHBox,
                   Sizing::Grow(),
                   Sizing::Fixed(metrics.buttonRowHeight))
        .addLayout("recentVBox", recentVBox, Sizing::Grow(), Sizing::Grow());

    // 末尾弹簧吸收最近列表未使用的剩余高度。
    rootVBox.addSpring();
    // LayoutContext 已包含当前子视图绝对起点与可用尺寸。
    rootVBox.render(layoutContext);
}

/// @brief 按编辑器文件选择器偏好打开项目目录选择流程。
/// @details 原生模式阻塞等待 NFD 结果并立即发布项目事件；内置模式只配置并
/// 打开 ImGuiFileDialog，结果由后续文件操作弹窗渲染流程消费。
/// @note 两种模式都只发布 OpenProjectEvent，不直接调用 ProjectController。
/// @warning 用户点击低频路径：原生模式会阻塞 UI 线程直到关闭系统对话框。
void FileManagerView::openFolderPicker()
{
    // 文件选择器样式和历史目录来自软件级编辑器设置。
    auto& config = Config::AppConfig::instance().getEditorSettings();
    if ( config.filePickerStyle == Config::FilePickerStyle::Native ) {
        // 系统对话框不经过 ImGui 弹窗入口，先显式播放打开反馈。
        ::MMM::UI::PlayPopupOpenFeedback();
        // NFD 在成功时分配 UTF-8 路径，由本函数负责释放。
        nfdu8char_t* outPath = nullptr;
        nfdresult_t  result  = NativeFileDialog::pickFolder(&outPath, nullptr);
        // 只有 NFD_OKAY 保证 outPath 有效，取消不会产生项目打开事件。
        if ( result == NFD_OKAY ) {
            // 成功目录转换为平台路径并通过统一事件打开项目。
            Event::OpenProjectEvent ev;
            ev.m_projectPath = Config::utf8ToPath(outPath);
            Event::EventBus::instance().publish(ev);
            // 仅成功结果具有需要释放的路径缓冲。
            NFD_FreePathU8(outPath);
        }
        // 取消与错误不改变项目；NFD 错误日志由上层统一处理。
    } else {
        // 内置选择器使用上次路径、单选和模态配置。
        IGFD::FileDialogConfig fdConfig;
        fdConfig.path = config.lastFilePickerPath;
        // 历史路径只作为初始位置，不在打开弹窗前执行同步文件系统校验。
        fdConfig.countSelectionMax = 1;
        fdConfig.flags             = ImGuiFileDialogFlags_Modal;
        // 记录调用前状态，避免每帧重复请求时反复播放打开音效。
        const bool wasOpen =
            ImGuiFileDialog::Instance()->IsOpened("ProjectFolderPicker");
        // nullptr 过滤器表示目录选择模式。
        ImGuiFileDialog::Instance()->OpenDialog(
            "ProjectFolderPicker",
            TR("ui.file_manager.open_directory").data(),
            nullptr,
            fdConfig);
        if ( !wasOpen &&
             ImGuiFileDialog::Instance()->IsOpened("ProjectFolderPicker") ) {
            // 只有从关闭变为打开的边沿播放一次弹窗反馈。
            ::MMM::UI::PlayPopupOpenFeedback();
        }
    }
}

}  // namespace MMM::UI
