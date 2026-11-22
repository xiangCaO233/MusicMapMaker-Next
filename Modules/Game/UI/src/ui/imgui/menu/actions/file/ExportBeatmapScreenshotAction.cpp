/// @file ExportBeatmapScreenshotAction.cpp
/// @brief 桌面菜单和触控抽屉共用的谱面长截图动作。
/// @details 捕获活动谱面后分阶段显示参数与输出选择。
/// 更改参数后生成同一画布的缩略图，后台任务不会操作活动会话。
/// 编码与预览占有独立几何批次，共享只读皮肤像素。
/// 选择时显示的尺寸采用成品坐标，纹理只是允许适配面板尺寸的缩略图。
/// 修改时先撤销旧结果，再以最新输入启动一次后台预览。
#include "ui/imgui/menu/actions/MainMenuFileActions.h"

#include "common/platform/DocumentStorage.h"
#include "common/render/NoteRenderData.h"
#include "config/AppConfig.h"
#include "config/EditorConfig.h"
#include "config/EditorSettings.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "graphic/imguivk/VKTexture.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "ui/ITextureLoader.h"
#include "ui/UIManager.h"
#include "ui/export/BeatmapLongScreenshot.h"
#include "ui/image/ImageViewer.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/package/BmsExportTouch.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include "ui/imgui/status/IStatusMessageSink.h"
#include "ui/utils/NativeFileDialog.h"
#include "ui/utils/UIWidgetUtils.h"
#include "ui/workspace/IWorkspacePresentation.h"
#include "ui/workspace/MobileInputEditor.h"

#include <ImGuiFileDialog.h>
#include <imgui.h>
#include <nfd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
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
/// @brief 由 UIManager 管理的预览纹理视图，桌面与触控共用。
constexpr char PREVIEW_VIEW_ID[] = "BeatmapScreenshotPreview";

/// @brief UIManager 持有预览纹理，在独立的纹理资源阶段置换。
/// @details 只保留一张缩略图；参数过时立刻隐藏旧图，低频上传时再安全替换。
/// 正在渲染的命令可能仍引用上一张纹理，因此不能在选项变动时直接销毁描述符。
/// 下次完成同队列的上传后再释放旧纹理；关闭后保留的资源始终只有一张。
class ScreenshotPreviewTexture final : public ITextureLoader
{
public:
    /// @brief 注册无独立窗口的纹理加载视图。
    ScreenshotPreviewTexture()
        : IUIView(PREVIEW_VIEW_ID), ITextureLoader(PREVIEW_VIEW_ID)
    {
    }

    /// @brief 纹理由导出模态使用，无单独的 ImGui 内容。
    /// @note 注册此视图仅为接入资源阶段和渲染器的销毁顺序。
    /// @warning 每帧回调，无工作。
    void update(UIManager*) override {}

    /// @brief 只在收到最新预览像素后进入资源阶段。
    /// @return UI 侧已经接受后台生成的一张图像。
    /// @warning 每帧检查已发布的状态，不分配资源或等待 GPU。
    bool needReload() override { return m_pending.has_value(); }

    /// @brief 在绘制前上传合格缩略图；成功上传会完成同队列的既有引用。
    /// @param physical 用于查找纹理内存类型的物理设备。
    /// @param device 创建描述符和图像资源的逻辑设备。
    /// @param pool 由现有资源阶段提供的上传命令池。
    /// @param queue 负责上传且已与普通 UI 绘制串行的图形队列。
    /// @warning 仅收到新像素时执行，VKTexture 静态上传会等待队列完成。
    void reloadTextures(vk::PhysicalDevice& physical, vk::Device& device,
                        vk::CommandPool& pool, vk::Queue& queue) override
    {
        if ( !m_pending ) return;
        // 上传失败也消费请求，避免资源异常时在每一帧重新分配。
        auto preview = std::move(*m_pending);
        m_pending.reset();
        auto texture =
            std::make_unique<Graphic::VKTexture>(preview.m_pixels.data(),
                                                 preview.m_width,
                                                 preview.m_height,
                                                 physical,
                                                 device,
                                                 pool,
                                                 queue);
        // 仅当上传完成且描述符可用时才替换旧资源。失败时它仍由视图安全持有。
        if ( !texture->isValid() ) return;
        const auto id = texture->getImTextureID();
        if ( !id ) return;
        // 只有描述符和像素都齐备，绘制才能读到这次生成的宽高与身份。
        m_texture = std::move(texture);
        m_id      = id;
        m_visible = true;
    }

    /// @brief 丢弃过时结果；实际 GPU 资源继续由资源阶段持有。
    /// @note 两次调整间可能仍未进入上传阶段；撤销时同时清除未发布的像素。
    void invalidate()
    {
        m_pending.reset();
        m_visible = false;
    }

    /// @brief 接管后台完成的最新像素，交给下一资源阶段处理。
    /// @param preview 背景任务生成且仍与当前草稿对应的完整图像。
    /// @pre 从 UI 调用，后台任务禁止访问此视图。
    void queueUpload(BeatmapScreenshotPreview preview)
    {
        m_pending = std::move(preview);
    }

    /// @brief 在预留的区域中等比居中显示成品缩略图。
    /// @param room 宿主留给图片的正值宽高，已经扣除标题与尺寸文字。
    /// @pre 调用方已检查 visible()，不会绘制图形上传失败的纹理。
    /// @warning 每帧只读取缓存纹理的尺寸与描述符。
    bool draw(ImVec2 room) const
    {
        const float ratio =
            std::min(room.x / m_texture->width(), room.y / m_texture->height());
        const ImVec2 size{ m_texture->width() * ratio,
                           m_texture->height() * ratio };
        // 使用子区局部光标；滚动时图片随容器移动。
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (room.x - size.x) * 0.5f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (room.y - size.y) * 0.5f);
        ImGui::Image(m_id, size);
        // 触屏用独立按钮打开，滑动预览正文时不能在按下瞬间误开大图。
        return workspaceAllowsHoverFeedback() && ImGui::IsItemClicked();
    }

    /// @brief 借用父窗的概览纹理；子模态使用期间界面参数不会变化。
    /// @warning 每帧最多读取常量描述符，不接触 Vulkan。
    ImTextureID id() const { return m_id; }

    /// @brief 返回纹理的采样密度所需的实际像素尺寸。
    /// @pre 仅在可见且已上传时查询。
    ImVec2 size() const
    {
        return { static_cast<float>(m_texture->width()),
                 static_cast<float>(m_texture->height()) };
    }

    /// @brief 当前参数的图像是否可展示。
    /// @return 纹理完成上传且仍与当前设置一致时为 true。
    /// @warning 每帧只读可见性，无 GPU 调用。
    bool visible() const { return m_visible; }

private:
    std::optional<BeatmapScreenshotPreview> m_pending;  ///< 最新待上传的像素。
    std::unique_ptr<Graphic::VKTexture>     m_texture;  ///< 已上传的唯一纹理。
    ImTextureID m_id{};       ///< 缓存描述符，绘制时不重新查询。
    bool        m_visible{};  ///< 参数调整后隐藏已过时的纹理。
};

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
/// 预览任务和保存任务使用独立的取消状态，关闭选项只会撤销预览。
/// 磁盘选择期间保留当前图集和参数，系统回调不读取新的活跃谱面。
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
        return !m_optionsOpen && !m_openRequested && !m_waiting &&
               !m_worker.joinable() && MenuUtil::hasActiveBeatmap(false);
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
        // 固定本轮整谱范围及皮肤输入。编辑参数时可以复用图集，不再读图。
        m_begin = m_fullBegin = chart.m_begin;
        m_end = m_fullEnd = chart.m_end;
        auto& skin        = Config::SkinManager::instance();
        chart.m_asciiFont = skin.getFontPath("ascii");
        if ( !author.empty() ) chart.m_cjkFont = skin.getFontPath("cjk");
        m_atlas = loadBeatmapScreenshotAtlas();
        if ( context.sourceManager && !m_previewTexture ) {
            // 只在第一次打开时注册；更新图像不影响视图的所有权和绘制顺序。
            auto preview     = std::make_unique<ScreenshotPreviewTexture>();
            m_previewTexture = preview.get();
            context.sourceManager->registerView(PREVIEW_VIEW_ID,
                                                std::move(preview));
        }
        m_previewDirty  = true;
        m_previewAt     = std::chrono::steady_clock::now();
        m_chart         = std::move(chart);
        m_author        = std::move(author);
        m_openRequested = true;
    }

    /// @brief 逐帧绘制参数并消费选择结果与已完成任务。
    /// @warning UI 热路径只读发布状态；选择、启动任务由用户确认触发。
    /// @note 先收取像素再绘制表单；若同一帧编辑参数，表单会撤销尚未上传的结果。
    /// @param context 提供当前帧 DPI 和状态栏接收器。
    /// @details 参数窗和输出选择器先推进，再确认后台是否结束。
    /// 所有 ImGui 操作只在 UI 线程进行，编码线程只发布完成标志。
    /// 任务过快时最终结果可在同一个更新周期覆盖正在保存的提示。
    /// 完成状态回收不执行磁盘操作；尚未结束时直接返回绘制流程。
    void renderDeferred(MainMenuContext& context) override
    {
        collectPreview();
        renderOptions(context);
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
    /// @param context 提供 DPI 和子模态的纹理资源管理器。
    /// @details 每种水印使用独立布尔变量，切换范围不会改变署名。
    /// 谱面没有作者元数据时禁用作者选择，保留 MMM 开关。
    /// 正文在移动端可滚动，确认区留在固定底部。
    /// 桌面宽度充足时预览有独立区域；窄屏将预览放进同一个滚动正文。
    /// 保存按钮只在尺寸验证通过时可用。
    /// 选择路径前先关闭此模态，避免嵌套保存选择器。
    void renderOptions(MainMenuContext& context)
    {
        const float scale = context.dpiScale;
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
                 ImVec2(900.0f * scale, 520.0f * scale),
                 false) ) {
            if ( !ImGui::IsPopupOpen(title.c_str()) ) m_optionsOpen = false;
            // 未开始绘制不一定代表已关闭，只有 popup 真正消失才释放快照。
            if ( !m_optionsOpen ) {
                stopPreview();
                m_chart = {};
                m_atlas.reset();
            }
            return;
        }
        touch.beginContent();
        const auto room       = ImGui::GetContentRegionAvail();
        const bool sideBySide = !touch.mobile() && room.x >= 650.0f * scale;
        // 用最终安全区的宽度选布局，避免只看平台而在窄桌面挤压预览。
        const float bodyHeight =
            std::max(1.0f, room.y - ImGui::GetFrameHeightWithSpacing());
        std::optional<MobileInputBodyScope> body;
        // 移动端还要扣除输入法区域；使用现有安全区，不重复推算键盘高度。
        // 桌面正文预留一行操作空间，滚动选项不会把保存按钮移出视口。
        if ( touch.mobile() ) {
            body.emplace("##BeatmapScreenshotBody");
        } else {
            const float width = sideBySide ? 305.0f * scale : 0.0f;
            ImGui::BeginChild("##ScreenshotOptions",
                              { width, bodyHeight },
                              ImGuiChildFlags_Borders);
        }

        const bool   changed = drawFields();
        const double begin   = m_custom ? m_begin : m_fullBegin;
        const double end     = m_custom ? m_end : m_fullEnd;
        // 预估计算只看已启用且有内容的署名，空作者无需预留一行。
        const bool validZoom =
            std::isfinite(m_zoomPercent) && m_zoomPercent > 0.0;
        const auto estimate = estimateBeatmapScreenshot(
            m_chart.m_lanes,
            begin,
            end,
            m_columnSeconds,
            m_zoomPercent,
            m_showMmm + (m_showAuthor && !m_author.empty()));
        if ( !estimate ) {
            const auto* message =
                !validZoom ? "ui.long_screenshot.invalid_zoom"
                : m_columnSeconds < 1
                    ? "ui.long_screenshot.invalid_column_seconds"
                    : "ui.long_screenshot.invalid_range";
            ImGui::TextWrapped("%s", TR(message).data());
        }

        if ( changed ) {
            // 即使先前的任务已经完成，参数一变也必须立即让旧结果失效。
            stopPreview();
            m_previewDirty = true;
            // 输入数字过程中合并更新；尺寸会立即更新，只有几何和像素稍后重建。
            m_previewAt = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(180);
        }
        if ( estimate ) requestPreview(begin, end);

        if ( sideBySide ) {
            // 左侧表单滚动只影响选项；右侧为成品保留独立的可用宽高。
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("##ScreenshotPreview",
                              { 0.0f, bodyHeight },
                              ImGuiChildFlags_Borders);
        } else {
            ImGui::Separator();
        }
        drawPreview(estimate, sideBySide, scale, context.sourceManager);
        if ( body )
            body->end();
        else
            ImGui::EndChild();

        const auto size = touch.actionSize();
        ImGui::BeginDisabled(!estimate);
        // 缩略图可能还在后台绘制；只要尺寸合法即可确认，保存会单独完成。
        const bool pick =
            FeedbackButton(TR("ui.long_screenshot.save").data(), size);
        ImGui::EndDisabled();
        ImGui::SameLine();
        const bool cancel = FeedbackButton(TR("ui.common.cancel").data(), size);
        // 先结束本模态再进入路径选择，便于移动端释放输入法焦点。
        // 两个操作共用关闭状态，但只有保存移交数据。
        if ( pick || cancel ) {
            stopPreview();
            m_optionsOpen = false;
            ImGui::CloseCurrentPopup();
        }
        touch.endContent();
        // 在父弹窗的 ID 栈中打开共享查看器，关闭大图后回到当前参数。
        if ( m_imageViewer ) m_imageViewer->drawScreenshotPopup(scale);
        ImGui::EndPopup();
        if ( pick ) {
            // 当前草稿可能还在刷新；不使用尚未提交的预览结果来推断参数。
            applyOptions(begin, end);
            selectPath();
        } else if ( !m_optionsOpen ) {
            m_chart = {};
            m_atlas.reset();
        }
    }

    /// @brief 绘制导出草稿，返回是否改变任何会影响成品的参数。
    /// @return 任一当前有效控件修改了草稿时返回 true，合并为一次预览请求。
    /// @warning 热路径只编辑本动作的标量值，不扫描谱面或等待后台。
    bool drawFields()
    {
        bool changed = false;
        if ( FeedbackRadioButton(TR("ui.long_screenshot.full").data(),
                                 !m_custom) &&
             m_custom ) {
            m_custom = false;
            changed  = true;
        }
        if ( FeedbackRadioButton(TR("ui.long_screenshot.range").data(),
                                 m_custom) &&
             !m_custom ) {
            m_custom = true;
            changed  = true;
        }
        if ( m_custom ) {
            // 切换整谱范围不会改写用户已输入的指定区间。
            ImGui::TextUnformatted(TR("ui.long_screenshot.begin").data());
            ImGui::SetNextItemWidth(-1.0f);
            changed |= ImGui::InputDouble(
                "##ScreenshotBegin", &m_begin, 0.0, 0.0, "%.4f");
            keepMobileInputItemVisible();
            ImGui::TextUnformatted(TR("ui.long_screenshot.end").data());
            ImGui::SetNextItemWidth(-1.0f);
            changed |=
                ImGui::InputDouble("##ScreenshotEnd", &m_end, 0.0, 0.0, "%.4f");
            keepMobileInputItemVisible();
        }
        ImGui::Separator();
        ImGui::TextUnformatted(TR("ui.long_screenshot.zoom").data());
        // 输出比例可含小数；预览为适配面板而缩放时不会反向修改这里的数值。
        ImGui::SetNextItemWidth(-1.0f);
        changed |= ImGui::InputDouble(
            "##ScreenshotZoom", &m_zoomPercent, 0.0, 0.0, "%.12g");
        keepMobileInputItemVisible();
        ImGui::TextUnformatted(TR("ui.long_screenshot.column_seconds").data());
        // 每列的时长按秒计算，因此改变视觉倍率不会改写这里的分栏间隔。
        ImGui::SetNextItemWidth(-1.0f);
        changed |= ImGui::InputInt(
            "##ScreenshotColumnSeconds", &m_columnSeconds, 0, 0);
        keepMobileInputItemVisible();
        changed |= FeedbackCheckbox(TR("ui.long_screenshot.beat_lines").data(),
                                    &m_drawBeatLines);
        changed |=
            FeedbackCheckbox(TR("ui.long_screenshot.mmm").data(), &m_showMmm);
        ImGui::BeginDisabled(m_author.empty());
        // 没有谱师名时不生成空白页眉行；只保留可用于以后谱面的开关状态。
        changed |= FeedbackCheckbox(TR("ui.long_screenshot.author").data(),
                                    &m_showAuthor);
        ImGui::EndDisabled();
        return changed;
    }

    /// @brief 显示实际 PNG 尺寸和最新成品缩略图。
    /// @param estimate 直接从当前选项推导的尺寸；无效输入不显示旧预览。
    /// @param sideBySide 独立预览区可直接使用剩余高度，滚动正文需明确图片槽。
    /// @param scale 窄屏预览槽逻辑像素所用的 DPI 倍率。
    /// @param manager 打开查看器时按需注册其资源视图。
    /// @warning 热路径只绘制尺寸、状态与已生成的纹理。
    void drawPreview(const std::optional<BeatmapScreenshotSize>& estimate,
                     bool sideBySide, float scale, UIManager* manager)
    {
        ImGui::TextUnformatted(TR("ui.long_screenshot.preview").data());
        if ( !estimate ) return;
        // 数字与草稿同步，不依赖慢速栅格任务返回的纹理宽高。
        // 翻译只有单位和列数标签，皮肤覆写不会进入格式串解释器。
        ImGui::Text("%d × %d %s · %s %d",
                    estimate->m_width,
                    estimate->m_height,
                    TR("ui.long_screenshot.pixels").data(),
                    TR("ui.long_screenshot.columns").data(),
                    estimate->m_columns);
        if ( m_previewTexture && m_previewTexture->visible() &&
             m_preparedPreview ) {
            bool open =
                FeedbackButton(TR("ui.image_viewer.open_preview").data());
            // 纹理所有权保留在资源视图。这里不更新图形后端或重复上传像素。
            auto area = ImGui::GetContentRegionAvail();
            if ( !sideBySide ) area.y = 260.0f * scale;
            // 键盘可以收缩纵向空间；避免给 ImGui 传递非正尺寸。
            if ( area.x > 0.0f && area.y > 0.0f )
                open |= m_previewTexture->draw(area);
            if ( open && manager ) {
                // 点击时才注册查看器，浏览普通缩略图不占用额外的图形资源。
                m_imageViewer = &ImageViewer::exportPopup(*manager);
                m_imageViewer->openScreenshot(
                    m_preparedPreview,
                    { 0, 0, estimate->m_width, estimate->m_height },
                    m_previewTexture->id(),
                    m_previewTexture->size());
            }
        } else {
            // 有任何阶段仍在处理时显示进度，上传失败则允许继续保存。
            ImGui::TextWrapped(
                "%s",
                TR(m_previewTexture &&
                           (m_previewDirty || m_previewWorker.joinable() ||
                            m_previewTexture->needReload())
                       ? "ui.long_screenshot.preview_loading"
                       : "ui.long_screenshot.preview_unavailable")
                    .data());
        }
    }

    /// @brief 固定当前草稿供预览或保存；捕获的整谱边界独立保留。
    /// @param begin 已解析的起点，单位秒。
    /// @param end 已解析的终点，与起点采用同一个范围模式。
    /// @note 系统文档选择可跨帧返回，因此保存只读取已经固定的这份选项。
    void applyOptions(double begin, double end)
    {
        m_chart.m_begin = begin;
        // 这里处理的是捕获后的副本，选项绝不反向写入当前编辑器会话。
        m_chart.m_end                 = end;
        m_chart.m_columnSeconds       = m_columnSeconds;
        m_chart.m_timelineZoomPercent = m_zoomPercent;
        m_chart.m_drawBeatLines       = m_drawBeatLines;
        m_chart.m_showMmm             = m_showMmm;
        m_chart.m_author              = m_showAuthor ? m_author : std::string{};
    }

    /// @brief 丢弃在途预览，下一轮只提交最新草稿。
    /// @note 已转入文件选择的任务另有生命周期，撤销预览不会清理其快照。
    /// @warning 用户更改参数或关闭时发出非阻塞取消请求，不在此回收线程。
    void stopPreview()
    {
        m_previewDirty = false;
        m_previewWorker.request_stop();
        // 保留线程句柄直到完成标记发布，避免用户输入时等待旧图采样结束。
        if ( m_imageViewer ) m_imageViewer->closeScreenshot();
        // 大图借用的概览标识要先解除，再让父窗撤销原有预览。
        m_preparedPreview.reset();
        if ( m_previewTexture ) m_previewTexture->invalidate();
    }

    /// @brief 只在用户调整后的低频入口生成画布批次，栅格化由后台执行。
    /// @param begin 合法目标区间的开始。
    /// @param end 合法目标区间的结束。
    /// @details 批次生成仍访问当前 UI 线程的皮肤几何规则，图集已经缓存。
    /// 字体与逐行栅格均在后台完成，界面只安排最后一次有效请求。
    /// @warning 每帧仅检查脏标记；有新请求才遍历捕获的谱面，不读取会话。
    /// @warning
    /// 共享图集的所有权在任务启动时复制一次，使关闭模态后后台仍可采样。
    void requestPreview(double begin, double end)
    {
        if ( !m_previewDirty || m_previewWorker.joinable() ||
             std::chrono::steady_clock::now() < m_previewAt )
            return;
        m_previewDirty = false;
        if ( !m_atlas || !m_previewTexture ) return;
        applyOptions(begin, end);
        auto image = prepareBeatmapScreenshot(m_chart, m_config, m_atlas);
        // 交给后台的是只读快照，以后的输入与文件选择都不引用此任务的几何。
        if ( !image ) return;
        // 一份只读批次同时供全图缩略和按需的局部高清取样。
        m_preparedPreview =
            std::make_shared<BeatmapScreenshotImage>(std::move(*image));
        m_previewDone.store(false, std::memory_order_relaxed);
        m_previewWorker = std::jthread(
            [this, image = m_preparedPreview](std::stop_token stop) mutable {
                m_previewPixels = previewBeatmapScreenshot(*image, stop);
                // 参数已失效时任务可能是最后一个所有者，在发布前释放引用。
                image.reset();
                m_previewDone.store(true, std::memory_order_release);
            });
    }

    /// @brief 只接收当前请求的数据，将旧任务结果直接丢弃。
    /// @details 完成与取消可以同时成立，显示以界面最后一次输入为准。
    /// 只有工作完成后才移动像素所有权，后台不需要锁或调用纹理方法。
    /// @warning 每帧 acquire 查询；工作线程已完成栅格化及清理后才回收句柄。
    void collectPreview()
    {
        if ( !m_previewWorker.joinable() ||
             !m_previewDone.load(std::memory_order_acquire) )
            return;
        const bool cancelled =
            m_previewWorker.get_stop_token().stop_requested();
        m_previewWorker.join();
        // join 之后才可移动结果，下一次启动前将完成标记复位。
        if ( !cancelled && m_previewPixels && m_optionsOpen )
            m_previewTexture->queueUpload(std::move(*m_previewPixels));
        m_previewPixels.reset();
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
                        m_atlas.reset();
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
            m_atlas.reset();
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
            // 复用预览采用的图集；绘制选项在打开选择器前已经固定。
            image =
                prepareBeatmapScreenshot(m_chart, m_config, std::move(m_atlas));
        }
        m_chart = {};
        m_atlas.reset();
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
    /// @brief 本轮预览与保存共用的只读皮肤图集。
    /// @note 在选项或选择器结束后释放界面持有的引用，在途任务各自保留所需像素。
    std::shared_ptr<const BeatmapScreenshotAtlas> m_atlas;
    /// @brief UIManager 拥有的资源视图，只在相同管理器的生命周期内访问。
    ScreenshotPreviewTexture* m_previewTexture{};
    /// @brief 与文件树共用的查看器，由 UIManager 保持生命周期。
    ImageViewer* m_imageViewer{};
    /// @brief 最近一次预览的原始绘制批次，供查看器按原图尺寸局部重绘。
    /// @warning 每次刷新仅复制一次共享所有权，后台工作期间保持批次地址稳定。
    std::shared_ptr<const BeatmapScreenshotImage> m_preparedPreview;
    /// @brief 完成后的像素由后台写入，界面取得发布标记后独占消费。
    /// @note 关闭弹窗只请求停止，不会在后台写入期间重置此缓冲。
    std::optional<BeatmapScreenshotPreview> m_previewPixels;
    /// @warning 后台完成后 release 写入，界面有任务时每帧 acquire 查询。
    /// @note 只保证从独占写入到 UI 消费的顺序；失效判定由 UI 管理。
    std::atomic<bool> m_previewDone{};
    /// @brief 草稿改动合并到一次低频刷新，不延迟输入和尺寸显示。
    /// @note 该时刻仅由界面线程读写，不用于后台等待或同步。
    std::chrono::steady_clock::time_point m_previewAt;
    /// @brief 当前选项是否需要一份新预览。
    /// @note 无效参数仍保持此标记，用户改回有效值才进入栅格阶段。
    /// 选项关闭时清理该标记，避免无窗口的后台预览继续分配资源。
    bool m_previewDirty{};
    std::weak_ptr<Logic::BeatmapSession>
        /// @brief 用于复核原会话的导出权限。
        m_source;
    /// @brief 保留姓名给水印选项使用。
    std::string m_author;
    /// @brief 下一帧发布的一次性翻译键。
    const char* m_status{};
    /// @brief 指定时段输入，单位秒。
    double m_begin{}, m_end{};
    /// @brief 最初捕获的整谱范围，不被指定时间段覆盖。
    /// @note 应用草稿修改的是另一份导出区间，切回整谱仍能恢复这对端点。
    double m_fullBegin{}, m_fullEnd{};
    /// @brief 用户可输入任意有效的正百分比，100 保留原时间刻度。
    /// @note 仅保存在导出动作中，连续两次打开模态可沿用同一输入。
    double m_zoomPercent{ 100.0 };
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
    /// @warning 析构先停止并回收预览线程，再释放线程写入的结果。
    /// @note 必须保持在结果缓冲之后声明，确保线程先于缓冲析构。
    std::jthread m_previewWorker;
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
