/// @file ExportBeatmapScreenshotAction.cpp
/// @brief 桌面菜单和触控抽屉共用的谱面长截图动作。
/// @details 捕获活动谱面后分阶段显示参数与输出选择。
/// 只在用户确认后生成固定批次，导出任务不会操作活动会话。
/// 编码线程占有独立像素数据，输出结果通过状态栏发布。
#include "ui/imgui/menu/actions/MainMenuFileActions.h"

#include "common/platform/DocumentStorage.h"
#include "common/render/NoteRenderData.h"
#include "config/AppConfig.h"
#include "config/EditorConfig.h"
#include "config/EditorSettings.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "ui/export/BeatmapLongScreenshot.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/package/BmsExportTouch.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include "ui/imgui/status/IStatusMessageSink.h"
#include "ui/utils/NativeFileDialog.h"
#include "ui/utils/UIWidgetUtils.h"
#include "ui/workspace/MobileInputEditor.h"

#include <ImGuiFileDialog.h>
#include <imgui.h>
#include <nfd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace MMM::UI
{
namespace
{
/// @brief 固定内置选择器 ID，不与另存为或打包的对话框混用。
constexpr char PICKER_ID[] = "BeatmapScreenshotFilePicker";

/// @brief 从正式编辑区取当前物件与 BPM，包含未保存的修改。
/// @warning 仅由显式的菜单动作调用，使用短期会话锁；文件选择与编码在锁外运行。
/// @param context 由会话锁保护的编辑状态。
/// @param chart 本次新建的导出快照。
/// @details 按实时数据建立整谱边界，音频、物件和事件都可延伸边界。
/// 折线仍保留其根与真实子段，不把交互用子实体画成额外物件。
/// 选择指定时间段时，后续只修改起止边界，不丢失跨区间物件。
void captureChart(const Logic::SessionContext& context,
                  BeatmapScreenshot&           chart)
{
    // 采用当前会话的正式轨道数量，辅助区域不进入长图。
    // 从时间零点向前后扩展，保留实际存在的负偏移物件。
    chart.m_lanes = context.trackCount;
    chart.m_defaultBpm =
        context.currentBeatmap->m_baseMapMetadata.preference_bpm;
    chart.m_begin = 0.0;
    // 音频尾部与实时物件尾部都计入整谱，可覆盖音频结束后仍有物件的工程。
    chart.m_end = std::isfinite(context.audioTimelineTotalTime)
                      ? context.audioTimelineTotalTime
                      : 0.0;
    // 音符时间来自 ECS 秒域，音频总时长也已是秒。
    // 不从保存格式重新解析，避免不同导入格式的单位产生偏差。
    const auto includeBounds = [&](double timestamp, double duration) {
        const double tail = timestamp + std::max(0.0, duration);
        if ( !std::isfinite(timestamp) || !std::isfinite(tail) ) return;
        chart.m_begin = std::min(chart.m_begin, timestamp);
        chart.m_end   = std::max(chart.m_end, tail);
    };
    for ( const auto& [entity, note] :
          context.noteRegistry.view<const Logic::NoteComponent>().each() ) {
        // 无效轨道、草稿物件和派生交互节点都没有独立的正式画布位置。
        // 过滤发生在复制前，防止给导出产生未使用的实体。
        if ( note.m_isDraft || note.m_isSubNote || note.m_trackIndex < 0 ||
             note.m_trackIndex >= context.trackCount )
            continue;
        // 折线以根和完整内部子段交给普通画布，子实体只用于交互拾取。
        chart.m_notes.push_back(note);
        includeBounds(note.m_timestamp, note.m_duration);
        for ( const auto& sub : note.m_subNotes )
            includeBounds(sub.timestamp, sub.duration);
    }
    for ( const auto& [entity, timing] :
          context.timelineRegistry.view<const Logic::TimelineComponent>()
              .each() ) {
        if ( !std::isfinite(timing.m_timestamp) ) continue;
        // Timing 可以落在最后一个物件之外，空谱面也会显示其时间线范围。
        chart.m_begin = std::min(chart.m_begin, timing.m_timestamp);
        chart.m_end   = std::max(chart.m_end, timing.m_timestamp);
        if ( timing.m_effect == TimingEffect::BPM )
            chart.m_tempos.push_back(timing);
    }
    // 没有音频、物件与有效红线的空谱面仍能导出。
    // 一秒初始空轨道可提供有效的时间范围和可读图像尺寸。
    if ( chart.m_end <= chart.m_begin ) chart.m_end = chart.m_begin + 1.0;
}

/// @brief 将后台结果映射到翻译键，界面使用同一状态栏反馈。
/// @param result 编码线程交回的状态。
/// @return 统一状态栏的语言键；用户取消不附加新提示。
const char* resultMessage(ScreenshotResult result)
{
    switch ( result ) {
    case ScreenshotResult::Saved: return "ui.long_screenshot.saved";
    case ScreenshotResult::InvalidRange:
        return "ui.long_screenshot.invalid_range";
    case ScreenshotResult::FontError: return "ui.long_screenshot.font_error";
    case ScreenshotResult::WriteError: return "ui.long_screenshot.write_error";
    case ScreenshotResult::PublishError:
        return "ui.long_screenshot.publish_error";
    case ScreenshotResult::Cancelled: return nullptr;
    }
    return "ui.long_screenshot.write_error";
}

/// @brief 管理导出参数、统一保存选择器及后台编码的生命周期。
/// @details 本轮动作从活跃谱面捕获一次，之后不随标签切换更新来源。
/// 一个在途选择器或编码任务占用本动作，避免并发覆盖状态。
/// 系统选择结果由长期存活的菜单动作接管，禁止借用上一帧 context。
/// 关闭参数窗口或取消选择都可以丢弃这份快照。
/// Android 与桌面使用同一个准备和编码入口。
class ExportBeatmapScreenshotAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 桌面文件菜单与移动抽屉共享的操作身份。
    /// @warning 热路径仅返回静态标识。
    /// @return 与触控抽屉绑定语义一致的稳定标识。
    std::string_view operationId() const override
    {
        return "ExportBeatmapScreenshotAction";
    }

    /// @brief 当前有活动谱面且本轮导出未占用选择器或编码器时启用。
    /// @warning 菜单热路径：不获取会话长锁。
    /// @return 存在可用谱面，且之前的选择和保存已经结束。
    /// @note 离线权限在触发时复核，避免渲染菜单时取得会话长锁。
    bool isEnabled(const MainMenuContext&) const override
    {
        return !m_waiting && !m_worker.joinable() &&
               MenuUtil::hasActiveBeatmap(false);
    }

    /// @brief 固定本次导出来源与可选范围，再打开参数弹窗。
    /// @warning 低频用户操作：导航到当前会话并复制本次需要的几何。
    /// @param context 仅用于同步呈现离线禁用提示。
    /// @details 会话锁只覆盖必要数据复制，打开对话框和字体读取持锁之外执行。
    /// 采集的可见设置提供分拍和物件造型，不借用渲染缓冲或播放状态。
    /// 选项草稿默认使用捕获的整谱边界，用户可按需改成自选时段。
    void execute(MainMenuContext& context,
                 const MainMenuItemActivation&) override
    {
        auto& engine = Logic::EditorEngine::instance();
        // 快照在会话锁外建立容器，后续只复制到局部拥有的数据。
        // 离线权限失败时无需构造任何图集、临时文件或系统文档。
        BeatmapScreenshot chart;
        std::string       author;
        {
            // 鉴权、活跃会话解析与读取模型同在一次锁作用域，
            // 避免项目切换期间拿到不一致的作者和物件内容。
            std::lock_guard lock(engine.getSessionMutex());
            auto            session = engine.getActiveNonLogoSession();
            if ( !session || !session->getContext().currentBeatmap ) return;
            // 沿用现有谱面导出的离线只读边界。
            if ( session->isCollaborationOfflineReadOnly() ) {
                context.statusMessageSink.showStatusMessage(
                    TR("ui.long_screenshot.offline").toString(), 5.0f);
                return;
            }
            const auto& source = session->getContext();
            author = source.currentBeatmap->m_baseMapMetadata.author;
            captureChart(source, chart);
            // 使用该会话正在使用的尺寸、分拍、颜色与对齐方式，
            // 包括用户当前尚未重启应用就已生效的编辑器设置。
            m_config = source.lastConfig;
            m_source = session;
        }
        m_begin         = chart.m_begin;
        m_end           = chart.m_end;
        m_chart         = std::move(chart);
        m_author        = std::move(author);
        m_openRequested = true;
    }

    /// @brief 逐帧绘制参数并消费选择结果与已完成任务。
    /// @warning UI 热路径只读发布状态；选择、启动任务由用户确认触发。
    /// @param context 提供当前帧 DPI 和状态栏接收器。
    /// @details 参数窗和输出选择器先推进，再确认后台是否结束。
    /// 所有 ImGui 操作只在 UI 线程进行，编码线程只发布完成标志。
    /// 任务过快时最终结果可在同一个更新周期覆盖正在保存的提示。
    /// 完成状态回收不执行磁盘操作；尚未结束时直接返回绘制流程。
    void renderDeferred(MainMenuContext& context) override
    {
        renderOptions(context.dpiScale);
        renderFilePicker(context.dpiScale);
        // 仅消费工作线程已公开的阶段，轮询过程中不等待编码或文件 I/O。
        // 在用户已取得目标并且生成快照后才提示写入，
        // 取消文件选择不会显示错误的进度提示。
        if ( m_started ) {
            context.statusMessageSink.showStatusMessage(
                TR("ui.long_screenshot.writing").toString(), 4.0f);
            m_started = false;
        }
        if ( m_worker.joinable() ) {
            // 结果尚未发布就立即结束轮询，不等待编码线程。
            // 后台最后一次共享访问是 release store，回收路径才允许 join。
            const int result = m_completion.load(std::memory_order_acquire);
            if ( result >= 0 ) {
                // 发布结果是线程的最后一步；此处回收句柄时无剩余磁盘工作。
                // 回收只发生在工作线程已经结束所有发布、清理操作之后。
                m_worker.join();
                if ( const auto* message =
                         resultMessage(static_cast<ScreenshotResult>(result)) )
                    m_status = message;
            }
        }
        if ( const auto* message = std::exchange(m_status, nullptr) ) {
            context.statusMessageSink.showStatusMessage(TR(message).toString(),
                                                        7.0f);
        }
    }

private:
    /// @brief 本次参数选择，触屏键盘弹出后仍能滚动正文并确认。
    /// @warning 每个可见帧仅绘制固定数量控件，选择器只在确认后打开。
    /// @param scale 应用于触控操作区和模态的 DPI 比例。
    /// @details 每种水印使用独立布尔变量，切换范围不会改变署名。
    /// 谱面没有作者元数据时禁用作者选择，保留 MMM 开关。
    /// 正文在移动端可滚动，确认区留在固定底部。
    /// 保存按钮只在尺寸验证通过时可用。
    /// 选择路径前先关闭此模态，避免嵌套保存选择器。
    void renderOptions(float scale)
    {
        if ( !m_openRequested && !m_optionsOpen ) return;
        // 可见文本跟随当前语言，固定内部身份保证翻译切换不清空模态。
        // 只有等待打开或已经显示时才需要组装显示标题。
        const std::string title = TR("ui.file.long_screenshot").toString() +
                                  "###BeatmapScreenshotOptions";
        // 先消费一次性请求，后续帧不再重新播放弹窗反馈。
        // 这也避免关闭键盘导致的窗口重绘重复打开参数窗。
        if ( std::exchange(m_openRequested, false) ) {
            FeedbackOpenPopup(title.c_str());
            m_optionsOpen = true;
        }
        ExportTouchLayout              touch(scale);
        Utils::CenteredModalPopupScope popup(scale);
        if ( !popup.begin(
                 title.c_str(),
                 &m_optionsOpen,
                 ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse,
                 ImVec2(480.0f * scale,
                        touch.mobile() ? 400.0f * scale : 0.0f)) ) {
            // Escape 等 ImGui 导航也能关闭模态，避免保留不可见草稿。
            if ( !ImGui::IsPopupOpen(title.c_str()) ) m_optionsOpen = false;
            if ( !m_optionsOpen ) m_chart = {};
            return;
        }
        touch.beginContent();
        std::optional<MobileInputBodyScope> body;
        // 和其他带文本输入的移动导出表单共用安全区。
        // 保持底部确定、取消按钮可触及，不把它们放入可滚动内容。
        if ( touch.mobile() ) body.emplace("##BeatmapScreenshotBody");
        if ( ImGui::RadioButton(TR("ui.long_screenshot.full").data(),
                                !m_custom) )
            m_custom = false;
        if ( ImGui::RadioButton(TR("ui.long_screenshot.range").data(),
                                m_custom) )
            m_custom = true;
        // 只有指定范围需要两个时间输入。
        // 显示四位小数而存储 double，读取原先的浮点精度。
        if ( m_custom ) {
            // 单独排放标签，窄屏仍能完整显示数值和时间单位。
            ImGui::TextUnformatted(TR("ui.long_screenshot.begin").data());
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputDouble("##ScreenshotBegin", &m_begin, 0.0, 0.0, "%.4f");
            keepMobileInputItemVisible();
            ImGui::TextUnformatted(TR("ui.long_screenshot.end").data());
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputDouble("##ScreenshotEnd", &m_end, 0.0, 0.0, "%.4f");
            keepMobileInputItemVisible();
        }
        ImGui::Separator();
        // 分列和网格设置同时用于整谱与指定时段，仅影响导出图。
        ImGui::TextUnformatted(TR("ui.long_screenshot.column_seconds").data());
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputInt("##ScreenshotColumnSeconds", &m_columnSeconds, 0, 0);
        keepMobileInputItemVisible();
        FeedbackCheckbox(TR("ui.long_screenshot.beat_lines").data(),
                         &m_drawBeatLines);
        FeedbackCheckbox(TR("ui.long_screenshot.mmm").data(), &m_showMmm);
        // 没有谱师名的项目关闭该控件，
        // 切到有谱师名的谱面时继续采用用户的开关值。
        ImGui::BeginDisabled(m_author.empty());
        FeedbackCheckbox(TR("ui.long_screenshot.author").data(), &m_showAuthor);
        ImGui::EndDisabled();

        const auto begin = m_custom ? m_begin : m_chart.m_begin;
        const auto end   = m_custom ? m_end : m_chart.m_end;
        // 按当前选择的时段与每列时长重新验证 PNG 尺寸。
        // 两个水印都不会改变当前选择范围的合法性。
        const bool valid =
            validScreenshotRange(m_chart.m_lanes, begin, end, m_columnSeconds);
        if ( !valid ) {
            const auto* message =
                m_columnSeconds < 1
                    ? "ui.long_screenshot.invalid_column_seconds"
                    : "ui.long_screenshot.invalid_range";
            ImGui::TextWrapped("%s", TR(message).data());
        }
        if ( body ) body->end();

        const auto size = touch.actionSize();
        ImGui::BeginDisabled(!valid);
        const bool pick =
            FeedbackButton(TR("ui.long_screenshot.save").data(), size);
        ImGui::EndDisabled();
        ImGui::SameLine();
        const bool cancel = FeedbackButton(TR("ui.common.cancel").data(), size);
        // 先结束本模态再进入路径选择，便于移动端释放输入法焦点。
        // 两个操作共用关闭状态，但只有保存移交数据。
        if ( pick || cancel ) {
            m_optionsOpen = false;
            ImGui::CloseCurrentPopup();
        }
        touch.endContent();
        ImGui::EndPopup();
        if ( pick ) {
            // 只把最终选项写入后台快照；取消不改变谱面或字体设置。
            m_chart.m_begin = begin;
            m_chart.m_end   = end;
            // 将两个导出参数一起固定到快照，保存路径选择期间不读取界面草稿。
            m_chart.m_columnSeconds = m_columnSeconds;
            m_chart.m_drawBeatLines = m_drawBeatLines;
            m_chart.m_showMmm       = m_showMmm;
            // 水印开关只在接受参数时生效。
            // 关闭状态的姓名不写进栅格任务，切换 MMM 不影响谱师名。
            if ( m_showAuthor ) m_chart.m_author = m_author;
            selectPath();
        } else if ( !m_optionsOpen ) {
            // 关闭未确认的草稿不会更新之前已保存的 PNG。
            m_chart = {};
        }
    }

    /// @brief 按用户选择器偏好保存 PNG；Android 使用授权文档的异步入口。
    /// @warning 用户触发目录或系统对话框，不能从逐帧路径无条件进入。
    /// @details 按平台要求和用户偏好进入系统或内置选择。
    /// 路径返回时已经包含用户确认的文档身份。
    /// 系统取消回收快照，不会运行编码器。
    /// 内置分支继续使用固定窗口身份和既有覆盖确认能力。
    void selectPath()
    {
        // 路径默认值遵循普通导出偏好，支持用户自选的已有目录。
        // 选择器启动前先设置在途标志，兼容桌面的立即回调。
        const auto directory = MenuUtil::getSaveAsPickerDefaultPath();
        m_waiting            = true;
        if ( NativeFileDialog::requiresSystemPicker() ||
             Config::AppConfig::instance()
                     .getEditorSettings()
                     .filePickerStyle == Config::FilePickerStyle::Native ) {
            // 输出能力只有 PNG；过滤器和默认扩展名保持一致。
            // 平台以后可以返回授权文档的私有路径，动作不改名或拼接文件树。
            constexpr nfdu8filteritem_t filter{ "PNG", "png" };
            PlayPopupOpenFeedback();
            NativeFileDialog::saveFileAsync(
                &filter,
                1,
                directory.c_str(),
                "beatmap.png",
                [this](nfdresult_t result, const std::string& path) {
                    if ( result == NFD_OKAY ) {
                        startWrite(path);
                    } else {
                        m_waiting = false;
                        m_chart   = {};
                        if ( result == NFD_ERROR )
                            m_status = "ui.long_screenshot.write_error";
                    }
                });
            return;
        }
        IGFD::FileDialogConfig config;
        config.path     = directory;
        config.fileName = "beatmap.png";
        // 内置对话框拥有覆盖确认，不能在回调后追加其它扩展名。
        // 保持文件确认的身份和 writer 真正修改的路径一致。
        config.flags = ImGuiFileDialogFlags_Modal |
                       ImGuiFileDialogFlags_HideColumnType |
                       ImGuiFileDialogFlags_ConfirmOverwrite;
        ImGuiFileDialog::Instance()->OpenDialog(
            PICKER_ID, TR("ui.file.long_screenshot").data(), ".png", config);
        PlayPopupOpenFeedback();
    }

    /// @brief 内置选择器复用导出安全区域及其文件覆盖确认。
    /// @warning 仅当选择器打开时逐帧调用文件对话框呈现。
    /// @param scale 用于复用普通谱面导出对话框的安全区域。
    /// @details 仅在选择器打开时固定居中并绘制。
    /// 从选择器取出文件结果后立即关闭，再移交路径给后台。
    /// 取消只清理草稿和在途标志，不显示写入失败提示。
    void renderFilePicker(float scale)
    {
        auto* dialog = ImGuiFileDialog::Instance();
        if ( !dialog->IsOpened(PICKER_ID) ) return;
        Utils::CenteredModalPopupScope style(scale);
        const auto                     size = exportFileDialogSize(scale);
        Utils::prepareCenteredModalWindow(size);
        if ( !dialog->Display(PICKER_ID,
                              ImGuiWindowFlags_NoCollapse |
                                  ImGuiWindowFlags_NoResize |
                                  ImGuiWindowFlags_NoSavedSettings,
                              size) )
            return;
        const auto path =
            dialog->IsOk() ? dialog->GetFilePathName() : std::string{};
        dialog->Close();
        // 关闭、取消或没有选中有效文件时回到可选状态。
        // 不为未确认的路径分配编码任务或产生零字节图片。
        if ( path.empty() ) {
            m_waiting = false;
            m_chart   = {};
            return;
        }
        startWrite(path);
    }

    /// @brief 移交不可变图像数据并启动后台编码，回调不读取当前活动谱面。
    /// @warning 低频提交入口：选中路径后读取皮肤字体并创建工作线程。
    /// @param filename 用户已经确认的 UTF-8 路径。
    /// @details 用弱引用复核导出来源，活动谱面切换不会改写对象。
    /// 几何和贴图在 UI 线程准备以固定同一代皮肤。
    /// 后台只拥有图集、批次和输出路径，文件操作不拿会话锁。
    /// 完整 PNG 就绪后才由平台发布，系统未写文档在失败时清理。
    /// 编码完以 release 发布结果，UI 以 acquire 读取完成标记。
    void startWrite(const std::string& filename)
    {
        m_waiting = false;
        auto path = Config::utf8ToPath(filename);
        // 原窗口可在选择文件的过程中失效；不要降级到另一个活动谱面。
        // 若会话已经被断线保护限制，仍由后台清理空的系统目标。
        const auto source = m_source.lock();
        const bool blocked =
            !source || source->isCollaborationOfflineReadOnly();
        std::optional<BeatmapScreenshotImage> image;
        if ( !blocked ) {
            // 按当前皮肤与会话显示设置生成同一套二维绘制批次。
            auto& skin          = Config::SkinManager::instance();
            m_chart.m_asciiFont = skin.getFontPath("ascii");
            if ( !m_chart.m_author.empty() )
                m_chart.m_cjkFont = skin.getFontPath("cjk");
            // 固定真实画布的几何和贴图库后再把结果转入后台。
            // 动画、框选和悬浮等临时状态不会被纳入导出快照。
            image = prepareBeatmapScreenshot(m_chart, m_config);
        }
        m_chart = {};
        // 保留原路径及系统授权映射；编码和文档写回只在后台执行。
        m_completion.store(-1, std::memory_order_relaxed);
        // 无效渲染输入也需要清理授权占位，只是不显示正在写图。
        // 清理任务不访问待释放的编辑会话。
        // 准备失败仍安排一次授权文档的占位清理。
        // 只有拿到可编码的几何才显示正在保存。
        m_started = image.has_value();
        m_worker  = std::jthread(
            [this, image = std::move(image), path = std::move(path), blocked](
                std::stop_token stop) {
                const auto result =
                    blocked ? ScreenshotResult::Cancelled
                    : image ? writeBeatmapScreenshot(*image, path, stop)
                            : ScreenshotResult::WriteError;
                if ( result != ScreenshotResult::Saved ) {
                    // Android
                    // 创建目标后的空占位需清除；已写好的副本供失败恢复。
                    std::string error;
                    (void)Common::discardUnwrittenDocument(path, error);
                }
                // 后台完成标志最后发布，之后不再触碰输出、界面或快照。
                // 主线程可在下轮立即报告结果或接受新的导出。
                m_completion.store(static_cast<int>(result),
                                   std::memory_order_release);
            });
        if ( blocked ) m_status = "ui.long_screenshot.offline";
    }

    /// @brief 记录打开选项时的分拍与物件样式。
    Config::EditorConfig m_config;
    /// @brief 选择期间拥有的正式轨道快照。
    BeatmapScreenshot m_chart;
    std::weak_ptr<Logic::BeatmapSession>
        /// @brief 用于复核原会话的导出权限。
        m_source;
    /// @brief 保留姓名给水印选项使用。
    std::string m_author;
    /// @brief 下一帧发布的一次性翻译键。
    const char* m_status{};
    /// @brief 指定时段输入，单位秒。
    double m_begin{}, m_end{};
    /// @brief 每列最多覆盖的整数秒数，初次打开使用统一默认值。
    int m_columnSeconds{ DEFAULT_SCREENSHOT_COLUMN_SECONDS };
    /// @brief 仅控制图像中的横向网格，不更改会话显示配置。
    bool m_drawBeatLines{ true };
    /// @brief 两个独立的水印会话选项。
    bool m_showMmm{ true }, m_showAuthor{ true };
    /// @brief 是否使用指定时段。
    bool m_custom{};
    /// @brief 模态请求与可见性分别保存。
    bool m_openRequested{}, m_optionsOpen{};
    /// @brief 选择器在途和编码已启动通知。
    bool m_waiting{}, m_started{};
    /// @warning 每帧在导出在途时只做 acquire 读取，唯一写入者是编码线程。
    std::atomic<int> m_completion{ -1 };
    /// @warning 应用关闭时销毁线程会先请求停止并等待临时文件清理。
    std::jthread m_worker;
};
}  // namespace

/// @brief 创建可由桌面菜单与触控抽屉共享的长截图动作。
/// @return 主菜单拥有的处理器，跨帧保存选项和文件对话框状态。
std::unique_ptr<IMainMenuItemActionHandler>
createExportBeatmapScreenshotAction()
{
    return std::make_unique<ExportBeatmapScreenshotAction>();
}
}  // namespace MMM::UI
