#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/EditorSettings.h"
#include "config/Utf8Path.h"
#include "config/skin/translation/Translation.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "logic/BeatmapSettingsSnapshot.h"
#include "ui/Icons.h"
#include "ui/imgui/manager/SettingsView.h"
#include "ui/utils/TimeFormatUtils.h"
#include "ui/utils/UIThemeUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <future>
#include <limits>
#include <memory>
#include <string_view>

#include <utility>
#include <vector>

namespace MMM::UI
{
namespace
{

/// @brief 绘制带减号、当前值和加号的紧凑轨道数步进器。
/// @param bounds 设置项为控件预留的 Clay 边界。
/// @param trackCount 当前持久化轨道数量。
/// @param minimumTrackCount 允许减少到的最小数量。
/// @param id 当前设置项的稳定 ImGui ID。
/// @param removeTooltip 减号按钮提示。
/// @param addTooltip 加号按钮提示。
/// @param updateCount 合法点击后接收相邻目标数量的回调。
///
/// 控件采用“减号—当前值—加号”顺序，不允许直接文本输入。这样每次交互只产生
/// 相邻数量，命令层可以逐次维护新增或删除轨道所需的不变量。
///
/// `minimumTrackCount` 由调用场景决定：草稿轨至少保留一轨，BGM 轨允许为零。
/// 最大值受 `int32_t` 上限保护，避免加一发生有符号溢出。
///
/// 按钮即使禁用也保留 Tooltip 悬浮命中，让用户能够理解当前边界。回调只在有效
/// 点击后执行，不会因为禁用状态或普通悬浮产生命令。
/// @warning 设置窗口渲染期间每帧调用；回调只允许入队，不得同步修改逻辑状态。
template<typename UpdateCount>
void drawTrackCountStepper(Clay_BoundingBox bounds, std::int32_t trackCount,
                           std::int32_t minimumTrackCount, const char* id,
                           const char* removeTooltip, const char* addTooltip,
                           UpdateCount&& updateCount)
{
    // 以当前 frame 高度构造方形按钮，使控件随主题和 DPI 一同缩放。
    const float buttonSize = ImGui::GetFrameHeight();
    // 主题样式只用于计算安全内边距，不在此 helper 中永久修改。
    const auto& style = ImGui::GetStyle();
    // 同时测量加减号，确保二者使用一致且能够完整显示的横向空间。
    const float buttonGlyphWidth =
        std::max(ImGui::CalcTextSize("-").x, ImGui::CalcTextSize("+").x);
    // 预留一像素安全量，并防止极小按钮推导出负内边距。
    const float maxHorizontalPadding =
        std::max(0.0F, (buttonSize - buttonGlyphWidth) * 0.5F - 1.0F);
    // 纵向内边距沿用主题，横向值只在主题过宽时收窄。
    const ImVec2 compactButtonPadding{
        std::min(style.FramePadding.x, maxHorizontalPadding),
        style.FramePadding.y,
    };
    /// 绘制一个采用紧凑内边距的方形反馈按钮。
    /// @param label 包含可见符号和唯一隐藏 ID 的 ImGui 标签。
    /// @return 用户在当前帧有效点击时返回 true。
    ///
    /// 方形按钮只收窄横向内边距，避免大 FramePadding 主题裁掉加减号。
    /// 两项样式在绘制后立即出栈，不影响中间数值文本或页面其他按钮。
    const auto drawButton = [&](const char* label) {
        // 同时压入 padding 和文字居中规则，保证符号位于方形中央。
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, compactButtonPadding);
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5F, 0.5F));
        const bool clicked =
            ::MMM::UI::FeedbackButton(label, ImVec2(buttonSize, buttonSize));
        // 两项样式严格配对恢复，点击结果以值返回。
        ImGui::PopStyleVar(2);
        return clicked;
    };

    // Clay 提供绝对屏幕矩形，ImGui 游标从该行的值列起点开始。
    ImGui::SetCursorScreenPos({ bounds.x, bounds.y });
    // 外层稳定 ID 隔离草稿轨与 BGM 轨内部复用的按钮标签。
    ImGui::PushID(id);
    // 达到场景最小值时只禁用减号，数值与加号仍保持可用。
    ImGui::BeginDisabled(trackCount <= minimumTrackCount);
    if ( drawButton("-##RemovePersistentTrack") ) {
        // 目标值严格为当前值减一，由命令回调异步应用。
        updateCount(trackCount - 1);
    }
    ImGui::EndDisabled();
    if ( ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) ) {
        // AllowWhenDisabled 保证边界原因仍可通过提示传达。
        ImGui::SetTooltip("%s", removeTooltip);
    }

    // 当前数量与两个按钮保持同一行，并按 frame 内边距垂直对齐。
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%d", trackCount);
    ImGui::SameLine();

    // 仅在加一仍可由 int32_t 表示时启用加号。
    const bool canAdd = trackCount < std::numeric_limits<std::int32_t>::max();
    ImGui::BeginDisabled(!canAdd);
    if ( drawButton("+##AddPersistentTrack") ) {
        // 目标值严格为当前值加一，不在 UI 内直接重排轨道内容。
        updateCount(trackCount + 1);
    }
    ImGui::EndDisabled();
    if ( ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) ) {
        // 加号同样在上限禁用时继续显示解释性提示。
        ImGui::SetTooltip("%s", addTooltip);
    }
    // 恢复外层 ID 栈，避免影响后续设置项。
    ImGui::PopID();
}

}  // namespace

/// @brief 渲染谱面设置页。
///
/// 本页编辑当前活动谱面的元数据，包括标题信息、封面类型、推荐 BPM、轨道数量和
/// 工程内资源引用。普通字段先写入 `m_editingMeta` 草稿，本帧发生变化时再通过
/// `CmdUpdateBeatmapMetadata` 提交给逻辑线程。
///
/// 草稿轨和 BGM 轨数量使用各自的专用命令，不写入元数据草稿。这些命令可能需要
/// 调整持久化轨道结构，因此由 EditorEngine 队列串行处理。
///
/// 活动会话可能先于谱面出现，此时没有可供搜索导航定位的设置行。
/// 独立谱面也能打开表单，因此空状态以活动谱面为判断条件，工程可缺省。
///
/// 页面借用文件线程已发布的不可变快照，不读取会话或工程可变对象。
/// 选择、重命名和导入保留点击时的工程及谱面身份，逻辑消费时再复核。
///
/// `.imd` 格式只允许编辑项目能够安全表达的字段。受格式约束的标题、封面类型、
/// BPM 和资源控件会禁用，但版本、轨道信息和只读时长仍按现有能力展示。
///
/// 工程资源路径统一优先保存为工程根目录相对路径。兼容旧数据时，可识别误带工程
/// 根目录名称的相对路径；绝对路径只用于无法相对化或没有 Project 上下文的显示。
///
/// 资源下拉框只枚举工程内允许扩展名的普通文件。文件不存在时以警告色显示当前
/// 引用，但不会在无用户选择时清空数据，便于工程迁移后恢复资源。
///
/// 页面不直接保存 Project 或 BeatMap 文件，也不加载音频、图片与视频内容。
/// 资源扫描只生成候选相对路径，实际消费由音频和渲染模块负责。
///
/// 元数据草稿以载图实例变化作为切换信号。同一实例中，外部工具可能同步修改轨道
/// 数、封面和背景字段，因此这些结构字段随已完成的快照刷新；正在输入的标题文本
/// 则保留在草稿中，避免其他 UI 刷新导致编辑内容丢失。
///
/// 路径兼容 helper 不使用异常接口。权限错误、文件被并发删除和跨卷相对化失败都
/// 通过 `std::error_code` 转为候选缺失或回退显示，不能让设置页中断渲染。
///
/// 文件扩展名比较统一转换为小写。此处的白名单只控制下拉候选可见性，不承担
/// 媒体内容验证；解码器仍需独立检查实际文件格式和内容。
///
/// 资源列表只在弹层需要时请求，文件线程合并封面和视频候选为一次扫描。
/// 在此页没有有效目标时，不排周期性文件请求，因此首页、加载中的谱面和
/// 已关闭的设置窗口均不会有此页读任务排队。关闭时至多有一个任务自然结束。
/// 同时限制刷新任务数量以及候选扫描时机，避免 NFS 等慢盘把普通文件操作排到
/// 多个同目标的设置请求之后。窗口切换和导航不等待后台任务返回。
/// @warning UI 热路径：只读取快照、轮询 future 与绘制；禁止等待会话锁或文件
/// IO。
void SettingsView::drawBeatmapSettings()
{
    using namespace std::chrono_literals;
    // 单独读取发布的活动代次。即使文件线程还没交付新数据，
    // 标签切换也可以立刻撤下原来的表单，并禁止派发旧谱面编辑。
    const auto active        = Logic::activeBeatmapSettingsState();
    const auto now           = std::chrono::steady_clock::now();
    bool       refreshed     = false;
    bool       changedTarget = false;
    // 只消费完成任务，关闭窗口不会等待慢盘；任务不捕获 SettingsView。
    if ( m_beatmapSettingsJob.valid() &&
         m_beatmapSettingsJob.wait_for(0s) == std::future_status::ready ) {
        auto next = m_beatmapSettingsJob.get();
        if ( next && active && next->generation == active->generation ) {
            // 代次只标识活动标签。比较控制块是为了识别同一标签中的替换，
            // 不能用文件名或相同字符串路径判断谱面实例是否仍是原来的对象。
            changedTarget =
                !m_beatmapSettings ||
                next->generation != m_beatmapSettings->generation ||
                next->beatmap.owner_before(m_beatmapSettings->beatmap) ||
                m_beatmapSettings->beatmap.owner_before(next->beatmap);
            if ( changedTarget ||
                 (m_beatmapSettingsMedia &&
                  next->projectRoot != m_beatmapSettingsMedia->projectRoot) ) {
                m_beatmapSettingsMedia.reset();
            }
            // 普通刷新不含候选，不覆盖原来的目录结果。扫描返回空列表时
            // 仍需要替换旧缓存，防止已删除的文件继续显示。
            if ( next->scanned ) m_beatmapSettingsMedia = next;
            m_beatmapSettings = std::move(next);
            refreshed         = true;
        } else if ( !next ) {
            // 任务运行时可能已关掉会话。清掉旧表单和工程候选，
            // 后续新谱面按自己的身份请求数据。
            m_beatmapSettings.reset();
            m_beatmapSettingsMedia.reset();
        }
    }
    // 一个快照同时保有谱面身份和资源内容。扫描请求在普通刷新运行时保留，
    // 直到能够发出下一次任务，避免下拉框恰好打开时丢掉唯一的刷新边沿。
    if ( !m_beatmapSettingsJob.valid() && active &&
         (m_beatmapSettingsScanRequested ||
          now >= m_nextBeatmapSettingsRead) ) {
        const bool scan = std::exchange(m_beatmapSettingsScanRequested, false);
        // 只有普通刷新受读周期限制。打开资源下拉后优先在下一次可用时提交。
        // 无需为了合并请求等待固定时长，字段编辑仍为当帧入队。
        m_beatmapSettingsJob      = Logic::requestBeatmapSettings(scan);
        m_nextBeatmapSettingsRead = now + 150ms;
    }

    if ( !m_beatmapSettings || !active ||
         m_beatmapSettings->generation != active->generation ||
         m_beatmapSettings->beatmap.expired() ) {
        // 没有活动谱面时只显示明确提示，不登记任何捕获空上下文的回调。
        // 空状态跳过表单布局，单独补齐表单的左侧和顶部留白。
        // 内容子窗口紧邻分类分隔线，基准坐标取自当前光标。
        // 局部坐标会随子窗口滚动与停靠变化，不依赖屏幕位置。
        // 偏移使用窗口像素，与下方 Clay 根容器的内边距单位一致。
        const ImVec2 start = ImGui::GetCursorPos();
        ImGui::SetCursorPos({ start.x + 8.0f, start.y + 8.0f });
        // 提示仍作为 ImGui 文本项登记，让滚动区计入文字实际占用的高度。
        ImVec4 dangerCol = Utils::UIThemeUtils::getDangerColor();
        ImGui::TextColored(
            dangerCol, "%s", TR("ui.settings.beatmap.no_beatmap").data());
        return;
    }

    // 整帧借用同一个已校验的结果。后续控件只修改草稿，
    // 提交时还能用快照里的身份阻止操作越过异步载图边界。
    const auto& snapshot    = *m_beatmapSettings;
    const auto& currentMeta = snapshot.metadata;
    const bool  project     = snapshot.hasProject;
    if ( changedTarget ) {
        // 身份包含载入实例，同一路径重载不会继承上一个实例的草稿。
        m_editingMeta = currentMeta;
    } else if ( refreshed ) {
        // 右侧工具栏和原始元数据编辑器可直接修改这些字段，设置页需要同步外部变更。
        // 这里只同步会由其他 UI
        // 即时修改的结构字段，文本输入草稿继续由本页保留。
        m_editingMeta.track_count = currentMeta.track_count;
        // 音频管理器可在设置页外重命名资源，路径字段不属于需保留的文本输入。
        m_editingMeta.song_file_hint = currentMeta.song_file_hint;
        m_editingMeta.song_file_hint_cleared =
            currentMeta.song_file_hint_cleared;
        m_editingMeta.main_audio_path = currentMeta.main_audio_path;
        m_editingMeta.main_cover_path = currentMeta.main_cover_path;
        m_editingMeta.cover_path      = currentMeta.cover_path;
        m_editingMeta.cover_type      = currentMeta.cover_type;
        m_editingMeta.video_starttime = currentMeta.video_starttime;
        m_editingMeta.bgxoffset       = currentMeta.bgxoffset;
        m_editingMeta.bgyoffset       = currentMeta.bgyoffset;
    }
    // 文本草稿与外部结构字段同步完成后，所有控件读取同一份 m_editingMeta。
    // 后续控件统一编辑草稿引用，changed 决定是否提交完整元数据命令。
    // changed 初始为 false，纯展示、折叠和资源悬浮不会触发命令。
    // 文件任务完成后只替换后台结果，控件仍在同一帧读写稳定草稿。
    // 缓存恢复前继续显示最近一次已完成候选，避免编辑时下拉内容闪空。
    auto& meta    = m_editingMeta;
    bool  changed = false;

    // 文件描述随快照发布；本帧刚改过的引用先显示原文，稍后的结果再补足状态。
    const auto displayPath = [](const Logic::BeatmapSettingsPath& known,
                                const std::filesystem::path&      source) {
        return known.source == source ? known.display
                                      : Config::pathToUtf8(source);
    };
    const auto pathExists = [](const Logic::BeatmapSettingsPath& known,
                               const std::filesystem::path&      source) {
        return known.source == source && known.exists;
    };
    // 资源列表可能比当前设置早发布。同一根目录才可以继续用已有结果，
    // 切换项目或同一标签重载时上方已经撤回旧候选。
    const bool mediaReady =
        m_beatmapSettingsMedia &&
        m_beatmapSettingsMedia->projectRoot == snapshot.projectRoot;
    // 草稿初值与后续修改只在本帧内比较，作为差量命令基线。
    // 这样本次编辑不会把所有其它字段强行写回到异步捕获时的值。
    // 文件状态展示仍引用已发布快照，最近一次改名可先显示草稿中的路径。
    auto baseline = meta;

    // IMD 检测只依据当前谱面文件扩展名，用于控制格式不支持的编辑项。
    // 这些限制只依赖源文件格式，不以工程资源是否扫描成功为前提。
    bool isImd = false;
    if ( !currentMeta.map_path.empty() ) {
        // 扩展名转换为 UTF-8 小写形式，兼容大小写不同的文件名。
        auto ext = Config::pathToUtf8(currentMeta.map_path.extension());
        // 转换扩展名时先提升到 unsigned char，避免多字节文件名触发非法参数。
        std::ranges::transform(ext, ext.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        if ( ext == ".imd" ) {
            // 禁用状态仅阻止 UI 写入，不改变已经加载的元数据。
            isImd = true;
        }
    }

    // 根 VBox 每帧重建布局关系，设置草稿和折叠状态独立保留。
    m_contentVBox.clear();
    // 统一间距与内边距使谱面页和其他设置页保持一致。
    m_contentVBox.setSpacing(6).setPadding(8, 8, 8, 8);
    // 行索引覆盖标题与设置项，section 索引只覆盖展开分组。
    size_t rowIndex     = 0;
    size_t sectionIndex = 0;

    // 使用布局缓存中的统一标签列宽，避免设置页每帧重复测量全部标签。
    // 当前内容缩放参与缓存选择，DPI 改变后标签和值列仍保持对齐。
    const float maxLabelW = getCurrentTabLabelWidth(
        Config::AppConfig::instance().getWindowContentScale());

    /// 创建可折叠的谱面设置分组标题。
    /// @param label 本地化标题，也参与当前页面的稳定 ID 构造。
    /// @param defaultOpen StateStorage 无记录时使用的首次展开状态。
    /// @return 展开时返回当前帧内容 section，折叠时返回空指针。
    ///
    /// 折叠状态属于 ImGui 会话，不进入谱面元数据。返回指针由 SettingsView 缓存
    /// 所有，只能在当前布局构建与渲染期间使用。
    auto addHeader = [&](const char* label, bool defaultOpen) -> CLayVBox* {
        // 页面前缀、行、节和标题共同隔离各分组的控件状态。
        std::string baseIdStr = "MAP_S" + std::to_string(sectionIndex) + "_R" +
                                std::to_string(rowIndex) + "_H_" + label;
        // 折叠状态不包含可变行号；搜索展开前面的组时不能重置后面的标题。
        ImGuiID id = ImGui::GetID((std::string("MAP_Header_") + label).c_str());

        // 提前读取展开状态，决定本帧是否登记该组内部控件。
        bool isOpen =
            ImGui::GetStateStorage()->GetInt(id, defaultOpen ? 1 : 0) != 0;
        // 搜索只展开对应组，不能触发配置修改或保存。
        const bool forceOpen = revealSettingsSearchSection(label, id);
        if ( forceOpen ) isOpen = true;

        // 标题独占一行并采用当前 ImGui frame 高度以适应 DPI。
        auto& row = getRow(rowIndex++);
        row.setPadding(0, 0, 0, 0).setSpacing(0);
        float h = ImGui::GetFrameHeight();

        row.addElement(
            (baseIdStr + "_el").c_str(),
            Sizing::Grow(),
            Sizing::Fixed(h),
            [this, label, id, defaultOpen, forceOpen](Clay_BoundingBox r,
                                                      bool) {
                // Clay 分配绝对矩形，ImGui 游标必须移动到标题起点。
                // 记录真实标题边界，条件隐藏的设置可以定位到所属组。
                reportSettingsSearchHeader(label, r);
                ImGui::SetCursorScreenPos({ r.x, r.y });
                // 悬浮与按下色从当前主题 Header 色轻微增亮。
                ImVec4 bgCol = ImGui::GetStyle().Colors[ImGuiCol_Header];
                // 三项颜色压栈与回调末尾 PopStyleColor(3) 配对。
                ImGui::PushStyleColor(ImGuiCol_Header, bgCol);
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                                      { bgCol.x + 0.05f,
                                        bgCol.y + 0.05f,
                                        bgCol.z + 0.05f,
                                        bgCol.w + 0.1f });
                ImGui::PushStyleColor(ImGuiCol_HeaderActive,
                                      { bgCol.x + 0.1f,
                                        bgCol.y + 0.1f,
                                        bgCol.z + 0.1f,
                                        bgCol.w + 0.15f });

                // 临时收窄 WorkRect，避免标题超出 Clay 分配的值域宽度。
                ImGuiWindow* win         = ImGui::GetCurrentWindow();
                float        savedWRMaxX = win->WorkRect.Max.x;
                win->WorkRect.Max.x      = r.x + r.width;
                // 标题取消窗口内边距以完整覆盖这一行。
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                    { 0.0f, 0.0f });

                // 数值 ID 借用指针重载传入，不代表可解引用地址。
                // CollapsingHeader 无嵌套内容，不需要配对 TreePop。
                // Clay 和实际标题必须保持相同的展开状态。
                if ( forceOpen ) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                bool nowOpen = ImGui::TreeNodeEx(
                    (void*)(intptr_t)id,
                    ImGuiTreeNodeFlags_CollapsingHeader |
                        (defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0),
                    "%s",
                    label);

                // 回调结束前恢复样式和 WorkRect，避免污染后续控件。
                ImGui::PopStyleVar();
                win->WorkRect.Max.x = savedWRMaxX;

                // 点击结果写回窗口状态，下一帧据此重建内容 section。
                ImGui::GetStateStorage()->SetInt(id, nowOpen ? 1 : 0);
                ImGui::PopStyleColor(3);
            });

        // 标题布局始终存在，内容布局只在展开分支加入根 VBox。
        m_contentVBox.addLayout((baseIdStr + "_layout").c_str(),
                                row,
                                Sizing::Grow(),
                                Sizing::Fixed(h));

        if ( isOpen ) {
            // 装饰 section 统一提供背景、间距和内边距。
            auto& sec = getSection(sectionIndex++);
            sec.setDecorated(true).setSpacing(4).setPadding(8, 8, 8, 8);
            m_contentVBox.addLayout((baseIdStr + "_sec").c_str(),
                                    sec,
                                    Sizing::Grow(),
                                    Sizing::Fit());
            // Fit 高度紧贴实际设置行，根 VBox 负责分组间距。
            return &sec;
        }
        // 折叠分组返回空指针，不创建隐藏控件或占用高度。
        return nullptr;
    };

    if ( auto* sec =
             addHeader(TR_CACHE("ui.settings.beatmap.info").data(), true) ) {
        // 基本信息组编辑标题、艺术家、谱师、难度版本，并展示只读文件路径。
        // 各字符串保持原始用户文本，不在 UI 层进行裁剪、转义或大小写归一化。
        // 采用统一标签宽度，使所有文本输入和值展示共享对齐基线。

        /// 登记一个编辑元数据字符串的单行输入控件。
        /// @param labelPtr 设置行可见标签，同时作为当前控件 ID 作用域。
        /// @param valRef 直接连接 `m_editingMeta` 中的目标字符串。
        /// @param enabled false 时保留显示但禁止用户编辑。
        ///
        /// ImGui 使用固定 256 字节临时缓冲，输入发生变化后再回写 std::string。
        /// 禁用状态用于 IMD 不支持的字段，不会清空已有文本。
        auto DrawInput = [&](const char*  labelPtr,
                             std::string& valRef,
                             bool         enabled = true) {
            addSettingItem(
                *sec,
                rowIndex,
                labelPtr,
                maxLabelW,
                [labelPtr, &valRef = valRef, &changed, enabled](
                    Clay_BoundingBox r, bool) {
                    // 可见标签建立独立 ID 作用域，内部统一复用 `##Input`。
                    ImGui::PushID(labelPtr);
                    if ( !enabled ) {
                        // 禁用只包围当前输入框，后续设置行恢复正常状态。
                        ImGui::BeginDisabled();
                    }
                    // 每帧从草稿复制到有界缓冲，并显式保证末尾 NUL。
                    char buf[256];
                    strncpy(buf, valRef.c_str(), sizeof(buf));
                    buf[sizeof(buf) - 1] = '\0';
                    // 输入框占满 Clay 分配的值列宽度。
                    ImGui::SetNextItemWidth(r.width);
                    if ( ImGui::InputText("##Input", buf, sizeof(buf)) ) {
                        valRef = buf;
                        // 文本实际改变时才触发元数据命令。
                        changed = true;
                    }
                    if ( !enabled ) {
                        // 与当前控件的 BeginDisabled 严格配对。
                        ImGui::EndDisabled();
                    }
                    // 恢复标签 ID 作用域，避免影响下一设置项。
                    ImGui::PopID();
                });
        };

        // IMD 仅保留版本可编辑；其他文本字段按格式能力禁用。
        DrawInput(
            TR_CACHE("ui.settings.beatmap.name").data(), meta.name, !isImd);
        DrawInput(
            TR_CACHE("ui.settings.beatmap.title").data(), meta.title, !isImd);
        DrawInput(TR_CACHE("ui.settings.beatmap.title_unicode").data(),
                  meta.title_unicode,
                  !isImd);
        DrawInput(
            TR_CACHE("ui.settings.beatmap.artist").data(), meta.artist, !isImd);
        DrawInput(TR_CACHE("ui.settings.beatmap.artist_unicode").data(),
                  meta.artist_unicode,
                  !isImd);
        // 原生谱面保存专辑；不支持该字段的 IMD 保留展示而禁止编辑。
        DrawInput(
            TR_CACHE("ui.settings.beatmap.album").data(), meta.album, !isImd);
        DrawInput(
            TR_CACHE("ui.settings.beatmap.mapper").data(), meta.author, !isImd);
        DrawInput(
            TR_CACHE("ui.settings.beatmap.version").data(), meta.version, true);
        // 难度版本在 IMD 中仍允许编辑，因此显式传入 true。

        // 实际路径及显示文本均由文件线程计算，此处只借用快照。
        // 绝对路径提示在后台已完成编码转换，界面悬浮时不查文件系统。
        const auto& mapPath = snapshot.paths[0];

        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.beatmap.path").data(),
            maxLabelW,
            [&mapPath](Clay_BoundingBox r, bool) {
                // 布局回调在本帧结束前运行，管理器在下一帧才可能替换快照。
                // 直接借用后台转换的字符串，避免每帧重复分配窗口路径和提示。
                ImVec2 cursorPos = ImGui::GetCursorScreenPos();
                ImVec2 textSize  = ImGui::CalcTextSize(mapPath.display.c_str());

                // 超宽相对路径才计算滚动位移，完整可见时保持静止。
                float offset       = 0.0f;
                float visibleWidth = r.width;

                if ( textSize.x > visibleWidth ) {
                    // 额外留出尾部间隔，使往复运动的终点容易辨认。
                    float scrollRange = textSize.x - visibleWidth + 40.0f;
                    // ImGui 单调时间只驱动视觉动画，不修改元数据。
                    float time = (float)ImGui::GetTime();
                    // 正弦曲线产生平滑往复滚动，再钳制形成两端停顿。
                    float t = sinf(time * 0.5f - 1.57f) * 0.5f + 0.5f;
                    t       = std::clamp((t - 0.1f) / 0.8f, 0.0f, 1.0f);
                    offset  = t * scrollRange;
                }

                // 依据 frame 高度计算文本垂直居中位置。
                float textH   = ImGui::GetFontSize();
                float widgetH = ImGui::GetFrameHeight();
                float offsetY = (widgetH - textH) * 0.5f;

                // 剪切到 Clay 值列，路径不会覆盖左侧标签或相邻行。
                ImGui::PushClipRect(
                    cursorPos,
                    ImVec2(cursorPos.x + r.width, cursorPos.y + widgetH),
                    true);

                // 透明 Dummy 提供完整值列的 hover 区域，用于显示绝对路径提示。
                ImGui::SetCursorScreenPos(cursorPos);
                ImGui::Dummy(ImVec2(r.width, widgetH));
                if ( ImGui::IsItemHovered() ) {
                    // Tooltip 保留完整实际路径，滚动文本只负责紧凑展示。
                    ImGui::SetTooltip("%s", mapPath.absoluteDisplay.c_str());
                }

                // 直接提交到窗口 DrawList，使用当前主题文字颜色。
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(cursorPos.x - offset, cursorPos.y + offsetY),
                    ImGui::GetColorU32(ImGuiCol_Text),
                    mapPath.display.c_str());

                // 恢复父窗口裁剪范围，避免截断下一行。
                ImGui::PopClipRect();
            });
    }

    if ( auto* sec = addHeader(
             TR_CACHE("ui.settings.beatmap.cover_type").data(), true) ) {
        // 封面类型组编辑背景媒介、视频起始时间以及二维背景偏移。
        // 切换类型只改变解释方式，不自动清除另一类型下已有资源路径。
        // 采用统一标签宽度，使动态出现的视频起始行保持对齐。

        if ( isImd ) {
            // IMD 格式不支持这些字段的安全回写，整组保持只读展示。
            ImGui::BeginDisabled();
        }

        // cover_type 的整数值与图片、视频两个稳定枚举值对应。
        addRadioSetting(
            *sec,
            rowIndex,
            sectionIndex,
            TR_CACHE("ui.settings.beatmap.cover_type").data(),
            maxLabelW,
            { { TR_CACHE("ui.settings.beatmap.cover_type.image").data(), 0 },
              { TR_CACHE("ui.settings.beatmap.cover_type.video").data(), 1 } },
            (int&)meta.cover_type,
            changed);
        // 单选 helper 只改草稿枚举与 changed，不立即刷新背景资源。

        if ( meta.cover_type == MMM::CoverType::VIDEO ) {
            // 视频起始时间只在视频背景语义下显示，切回图片时保留原值。
            addSettingItem(*sec,
                           rowIndex,
                           TR_CACHE("ui.settings.beatmap.video_start").data(),
                           maxLabelW,
                           [&](Clay_BoundingBox r, bool) {
                               // InputInt
                               // 直接编辑元数据整数单位，消费者负责时间解释。
                               ImGui::SetNextItemWidth(r.width);
                               if ( ImGui::InputInt("##VideoStart",
                                                    &meta.video_starttime) ) {
                                   // 实际输入变化才提交完整元数据命令。
                                   changed = true;
                               }
                           });
        }

        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.beatmap.bg_offset").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // ImGui 双整数控件使用局部连续数组，再分别回写 X/Y 字段。
                int offsets[2] = { meta.bgxoffset, meta.bgyoffset };
                ImGui::SetNextItemWidth(r.width);
                if ( ::MMM::UI::FeedbackDragInt2("##BgOffset", offsets) ) {
                    // 两个方向作为一次交互共同提交，避免中间不一致。
                    meta.bgxoffset = offsets[0];
                    meta.bgyoffset = offsets[1];
                    changed        = true;
                }
            });

        if ( isImd ) {
            // 与组首 BeginDisabled 配对，恢复后续偏好设置的交互状态。
            ImGui::EndDisabled();
        }
    }

    if ( auto* sec = addHeader(
             TR_CACHE("ui.settings.beatmap.preference").data(), true) ) {
        // 谱面偏好组包含推荐 BPM、主轨数、持久化草稿/BGM 轨数和只读时长。
        // 时长来自当前元数据，仅用于展示，不接受在设置页直接覆盖。
        // 主轨数属于元数据；草稿轨和 BGM 轨属于 SessionContext 持久化结构。

        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.beatmap.bpm").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                if ( isImd ) {
                    // IMD 推荐 BPM 在此页不可安全写回，仅保留可见值。
                    ImGui::BeginDisabled();
                }
                // 使用 float 适配 ImGui API，提交时恢复元数据 double 类型。
                float bpm = (float)meta.preference_bpm;
                ImGui::SetNextItemWidth(r.width);
                if ( ::MMM::UI::FeedbackDragFloat(
                         "##BPM", &bpm, 0.1f, -1.0f, 1000.0f, "%.2f") ) {
                    meta.preference_bpm = (double)bpm;
                    // -1 保留格式约定中的未指定语义，上限限制异常输入。
                    changed = true;
                }
                if ( isImd ) {
                    // 禁用作用域只包围 BPM 控件，不影响可编辑轨道字段。
                    ImGui::EndDisabled();
                }
            });

        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.beatmap.tracks").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                // 主轨数仍是元数据字段，由统一 CmdUpdateBeatmapMetadata 提交。
                ImGui::SetNextItemWidth(r.width);
                if ( ImGui::InputInt("##Tracks", &meta.track_count) ) {
                    // 具体有效性和结构同步由逻辑命令处理，本页只记录输入变化。
                    changed = true;
                }
            });

        // 简化编辑不展示隐藏辅助区的数量编辑；既有轨道与数据继续保留。
        if ( Config::AppConfig::instance()
                 .getEditorSettings()
                 .professionalMode ) {
            addSettingItem(
                *sec,
                rowIndex,
                TR_CACHE("ui.settings.beatmap.draft_tracks").data(),
                maxLabelW,
                [&](Clay_BoundingBox r, bool) {
                    // 草稿轨采用独立槽中的实时值；资源扫描不会延迟连续步进。
                    // 只读与此表单同一代次的数量，仍至少保留一条草稿轨。
                    const auto draftTrackCount =
                        std::max(1, active->draftTracks);
                    drawTrackCountStepper(
                        r,
                        draftTrackCount,
                        1,
                        "DraftTrackCount",
                        TR("ui.settings.beatmap.draft_tracks_remove").data(),
                        TR("ui.settings.beatmap.draft_tracks_add").data(),
                        [&](std::int32_t count) {
                            // 回调只向 Engine 入队，避免持锁 UI
                            // 路径直接重建轨道。
                            Logic::submitBeatmapSettingsCommand(
                                snapshot,
                                Logic::CmdUpdateDraftTrackCount{
                                    count,
                                });
                            // 指令发往该快照绑定的谱面，重载后同一标签不会误接收。
                        });
                });

            addSettingItem(
                *sec,
                rowIndex,
                TR_CACHE("ui.settings.beatmap.bgm_tracks").data(),
                maxLabelW,
                [&](Clay_BoundingBox r, bool) {
                    // BGM 轨允许为零，负值上下文按零显示以保护步进边界。
                    const auto bgmTrackCount = std::max(0, active->bgmTracks);
                    drawTrackCountStepper(
                        r,
                        bgmTrackCount,
                        0,
                        "BgmTrackCount",
                        TR("ui.settings.beatmap.bgm_tracks_remove").data(),
                        TR("ui.settings.beatmap.bgm_tracks_add").data(),
                        [&](std::int32_t count) {
                            // 专用命令维护 BGM 轨资源与 SessionContext
                            // 的一致性。
                            Logic::submitBeatmapSettingsCommand(
                                snapshot,
                                Logic::CmdUpdateBgmTrackCount{
                                    count,
                                });
                            // 不提前修改上下文，避免 UI
                            // 与逻辑线程各自维护一份计数。
                        });
                });
        }

        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.beatmap.length").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           // map_length 以毫秒存储，格式化工具接收秒值。
                           const auto lengthText =
                               MMM::UI::Utils::formatCanvasDuration(
                                   meta.map_length / 1000.0);
                           // 时长为只读派生信息，不设置 changed。
                           ImGui::SetCursorScreenPos({ r.x, r.y });
                           ImGui::TextUnformatted(lengthText.c_str());
                       });
    }

    if ( auto* sec = addHeader(TR_CACHE("ui.settings.beatmap.resource").data(),
                               true) ) {
        // 资源组只保存工程内引用，不读取或预览实际媒体内容。
        // 当前引用即使不在候选列表中也继续显示，以保留可修复信息。
        // 采用统一标签宽度，使音频、封面与背景组合框对齐。
        // 每行右侧导入按钮把外部资源复制到当前工程并绑定此谱面。
        // 「无」允许清除已有引用，独立于当前工程里能否找到有效候选。
        // 背景导入遵循当前图片/视频类型，重命名不改变媒体类型。

        if ( isImd ) {
            // IMD 资源字段在此页不可安全回写，整组保留只读展示。
            // 新增导入按钮同处禁用范围，不能绕过格式写入限制。
            ImGui::BeginDisabled();
        }

        // 导入与重命名采用等宽方形图标，三行值列保持对齐。
        // 这里只记录请求和当时的项目/谱面身份；文件选择器在会话锁外打开。
        // 宽度随当前字体框高变化，SettingsView 的最小宽度测量同步覆盖。
        // 长路径只裁剪组合框预览，不能挤掉固定宽的可点击导入按钮。
        const char* audioImportLabel =
            TR_CACHE("ui.settings.beatmap.import_audio").data();
        const char* imageImportLabel =
            TR_CACHE("ui.settings.beatmap.import_image").data();
        const float importButtonWidth = ImGui::GetFrameHeight();
        // 框高已经包含主题垂直内边距，不叠加额外的固定像素尺寸。
        // 导入沿用加号字形，重命名沿用铅笔字形，避免新增字体依赖。
        // 可见标签只保留字形，完整动作名称由 Tooltip 提供。
        /// @brief 为资源行的下拉框预留右侧两个图标按钮宽度。
        /// @note 极窄窗口仍保留正宽度，避免负值传给 ImGui 布局。
        // Clay 在本分组作用域结束后才执行回调，宽度必须按值进入回调。
        const auto resourceComboWidth =
            [importButtonWidth](Clay_BoundingBox bounds) {
                return std::max(1.0F,
                                bounds.width - importButtonWidth * 2.0F -
                                    ImGui::GetStyle().ItemSpacing.x * 2.0F);
            };
        /// @brief 记录资源导入请求，实际选择与复制交给锁外的低频路径。
        /// @note 目标枚举区分同名的封面与背景“导入图片”按钮。
        // 回调执行在本帧结束前，工程和值身份由同一不可变快照保持。
        const auto drawResourceImportButton =
            [this, project, &snapshot, importButtonWidth, &meta](
                const char*                  label,
                BeatmapResourceTarget        target,
                const std::filesystem::path& resourcePath) {
                ImGui::SameLine();
                ImGui::PushID(static_cast<int>(target));
                // 三行共享相同的按钮几何，资源路径再长也不能改变动作列宽度。
                // 相同的隐藏按钮名在资源位置作用域内唯一，封面和背景不会串触发。
                const bool readOnly = project && snapshot.readOnly;
                ImGui::BeginDisabled(!project || readOnly);
                /// @brief 隔离文字按钮内边距，在固定方形中居中绘制资源图标。
                /// @return 当前按钮有效点击时返回 true。
                /// @warning 每帧绘制路径：仅临时压入样式，不加载字体或资源。
                const auto drawIconButton =
                    [importButtonWidth](const char* icon) {
                        // 主题横向内边距可大于半个按钮，不能用于固定宽度图标按钮。
                        // 绘制后立即恢复样式，提示框和后续组合框不继承局部覆盖。
                        Utils::pushFixedButtonStyleVars();
                        const bool clicked = FeedbackButton(
                            icon, ImVec2(importButtonWidth, importButtonWidth));
                        Utils::popFixedButtonStyleVars();
                        return clicked;
                    };
                // 无工程时资源缺少持久归属，按钮保留占位但不能触发。
                if ( drawIconButton(
                         (std::string(ICON_MMM_PLUS) + "##ImportResource")
                             .c_str()) ) {
                    m_beatmapResourceTarget     = target;
                    m_openBeatmapResourcePicker = true;
                    // 保存点击时的媒体类型，跨帧选择器不能随草稿变动串绑。
                    m_importBackgroundVideo =
                        target == BeatmapResourceTarget::Background &&
                        meta.cover_type == CoverType::VIDEO;
                    m_resourceImportProjectRoot = snapshot.projectRoot;
                    m_resourceImportBeatmapPath = snapshot.metadata.map_path;
                    // 请求仅保存值，不把 project 或 beatmap
                    // 观察指针带到弹窗下一帧。
                    // 对话框可能跨帧返回，结果必须与这两项身份重新比较。
                    m_beatmapResourceImportError.clear();
                }
                // 统一提示入口显式采用美学配置，不继承设置窗口放大的内边距。
                // 禁用状态仍提供动作说明，图标本身不承担全部语义。
                Utils::renderTooltip(label, Utils::TooltipDir::Left, true);
                ImGui::SameLine();
                // 没有绑定资源时只能导入，重命名按钮仍保留对齐占位。
                ImGui::BeginDisabled(resourcePath.empty());
                if ( drawIconButton(
                         (std::string(ICON_MMM_PEN) + "##RenameResource")
                             .c_str()) ) {
                    const auto filename =
                        Config::pathToUtf8(resourcePath.filename());
                    // 只编辑最后一个路径分量，子目录位置保持不变。
                    // 实际源路径单独保存，不能由输入框的显示名称反推文件身份。
                    // 不截断 UTF-8 文件名，避免提交一个用户未确认的残缺名称。
                    if ( filename.size() >= m_resourceRenameBuffer.size() ) {
                        m_beatmapResourceImportError =
                            TR("ui.settings.beatmap.rename_name_too_long")
                                .toString();
                    } else {
                        m_renameResourceTarget = target;
                        m_resourceRenamePath   = resourcePath;
                        // 旧路径只用于确认时的身份复核，不在本帧移动文件。
                        m_resourceRenameProjectRoot = snapshot.projectRoot;
                        m_resourceRenameBeatmapPath =
                            snapshot.metadata.map_path;
                        m_resourceRenameBuffer.fill('\0');
                        // 文件扩展名保留在初始输入内，用户可看见格式不变的约束。
                        // 完整清零后复制原名，短文件名不会带入上一次输入的尾部字节。
                        std::memcpy(m_resourceRenameBuffer.data(),
                                    filename.data(),
                                    filename.size());
                        m_openResourceRenamePopup = true;
                        // 焦点与打开请求分别消费，输入期间不再反复全选。
                        m_focusResourceRenameInput = true;
                        m_beatmapResourceImportError.clear();
                    }
                }
                // 向左展开避免提示框覆盖窗口右侧操作区，禁用按钮仍能说明用途。
                Utils::renderTooltip(
                    TR("ui.settings.beatmap.rename_resource").data(),
                    Utils::TooltipDir::Left,
                    true);
                ImGui::EndDisabled();
                // 空绑定禁用区和项目只读禁用区各自配对，不能提前退出回调。
                ImGui::EndDisabled();
                ImGui::PopID();
            };

        const auto* noResource =
            TR_CACHE("ui.settings.beatmap.no_resource").data();
        /// @brief 在候选顶部显示无绑定选项，文件列表为空时也可清除。
        /// @warning 资源下拉打开时执行，不访问磁盘或项目资源清单。
        const auto drawClearReference = [](bool empty) {
            const bool selected = ::MMM::UI::FeedbackSelectable(
                TR_CACHE("ui.settings.beatmap.clear_reference").data(), empty);
            if ( empty ) ImGui::SetItemDefaultFocus();
            return selected;
        };

        // 音频选择优先使用外部格式提示字段，并限定为 Project 登记的主音频资源。
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.beatmap.audio").data(),
            maxLabelW,
            [&,
             resourceComboWidth,
             drawResourceImportButton,
             audioImportLabel,
             noResource,
             drawClearReference](Clay_BoundingBox r, bool) {
                /// @brief 歌曲文件提示只服务于外部格式，不决定实际播放时间线。
                /// 空提示时回退到主音频路径，兼容本项目原生元数据。
                const auto& audioHint = meta.song_file_hint.empty()
                                            ? meta.main_audio_path
                                            : meta.song_file_hint;
                std::string currentAudioPath =
                    displayPath(snapshot.paths[1], audioHint);
                // 预览与比较都使用规范化后的显示路径。
                const char* audioPreview =
                    audioHint.empty() ? noResource : currentAudioPath.c_str();

                // 当前引用不存在时以警告色提示，但不自动清空草稿字段。
                const bool audioExists =
                    project && pathExists(snapshot.paths[1], audioHint);
                bool audioPushed = false;
                if ( !audioExists && !currentAudioPath.empty() ) {
                    // 仅非空失效引用需要警告，未选择资源保持普通颜色。
                    ImGui::PushStyleColor(
                        ImGuiCol_Text, Utils::UIThemeUtils::getWarningColor());
                    audioPushed = true;
                }

                // 组合框占满值列；打开后才遍历 Project 已登记音频。
                ImGui::SetNextItemWidth(resourceComboWidth(r));
                if ( ::MMM::UI::FeedbackBeginCombo("##AudioCombo",
                                                   audioPreview) ) {
                    if ( audioPushed ) {
                        // 弹窗内容不继承失效预览的警告色，立即恢复文本样式。
                        ImGui::PopStyleColor();
                        audioPushed = false;
                    }
                    if ( drawClearReference(audioHint.empty()) &&
                         (!audioHint.empty() ||
                          !meta.song_file_hint_cleared) ) {
                        meta.song_file_hint.clear();
                        meta.main_audio_path.clear();
                        // 持久化这次显式选择，保存时不从别的 Main BGM
                        // 自动填回。
                        meta.song_file_hint_cleared = true;
                        changed                     = true;
                    }
                    if ( project ) {
                        // 只列出后台复制的 Main 候选，不借用工程资源容器。
                        for ( const auto& res : snapshot.audio ) {
                            bool isSelected = (currentAudioPath == res.path);
                            // 可见资源 ID
                            // 后附路径作为隐藏标识，避免同名资源冲突。
                            if ( ::MMM::UI::FeedbackSelectable(
                                     (res.id + "##" + res.path).c_str(),
                                     isSelected) ) {
                                meta.song_file_hint = res.selection;
                                // 选择提示字段后清空旧主音频路径，消除双重来源歧义。
                                meta.main_audio_path.clear();
                                meta.song_file_hint_cleared = false;
                                changed                     = true;
                            }
                            // 打开弹窗时将键盘焦点定位到当前资源。
                            if ( isSelected ) ImGui::SetItemDefaultFocus();
                        }
                    }
                    ::MMM::UI::FeedbackEndCombo();
                }
                // 未打开组合框时，补回预览阶段压入的警告颜色。
                if ( audioPushed ) ImGui::PopStyleColor();
                drawResourceImportButton(
                    audioImportLabel, BeatmapResourceTarget::Audio, audioHint);
                // 主音频的工程登记和谱面绑定由统一导入结果完成。
            });

        // 封面选择扫描工程图片文件，并把选择保存为工程相对路径。
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.beatmap.cover").data(),
            maxLabelW,
            [&,
             resourceComboWidth,
             drawResourceImportButton,
             imageImportLabel,
             noResource,
             drawClearReference](Clay_BoundingBox r, bool) {
                // 当前显示路径经过旧前缀兼容和工程相对化。
                std::string currentCoverPath =
                    displayPath(snapshot.paths[2], meta.cover_path);
                const char* coverPreview = meta.cover_path.empty()
                                               ? noResource
                                               : currentCoverPath.c_str();

                const bool coverExists =
                    project && pathExists(snapshot.paths[2], meta.cover_path);
                bool coverPushed = false;
                if ( !coverExists && !currentCoverPath.empty() ) {
                    // 非空但无法解析的引用使用主题警告色。
                    ImGui::PushStyleColor(
                        ImGuiCol_Text, Utils::UIThemeUtils::getWarningColor());
                    coverPushed = true;
                }

                // 组合框只提交目录刷新请求；磁盘访问由独立文件线程执行。
                // 后台运行期间仍能清除引用或使用本工程上一次的候选。
                ImGui::SetNextItemWidth(resourceComboWidth(r));
                if ( ::MMM::UI::FeedbackBeginCombo("##CoverCombo",
                                                   coverPreview) ) {
                    if ( coverPushed ) {
                        // 弹窗候选恢复普通文本颜色，不继承预览警告样式。
                        ImGui::PopStyleColor();
                        coverPushed = false;
                    }
                    if ( drawClearReference(meta.cover_path.empty()) &&
                         !meta.cover_path.empty() ) {
                        meta.cover_path.clear();
                        changed = true;
                    }
                    // IsWindowAppearing 只在候选 popup 打开的首帧产生边沿。
                    // 保持下拉打开时借用已扫描结果，无需周期性重扫整个工程。
                    m_beatmapSettingsScanRequested =
                        (project && ImGui::IsWindowAppearing()) ||
                        m_beatmapSettingsScanRequested;
                    // 文件线程还没交付时只显示引用操作，仍可以取消或清除。
                    // 已有候选属于当前工程时继续可选，避免刷新期间强制关闭弹窗。
                    if ( project && mediaReady ) {
                        const auto& images = m_beatmapSettingsMedia->images;
                        for ( const auto& imgPath : images ) {
                            // 相对路径同时作为显示文本和唯一隐藏 ID
                            // 的组成部分。
                            bool isSelected = (currentCoverPath == imgPath);
                            if ( ::MMM::UI::FeedbackSelectable(
                                     (imgPath + "##" + imgPath).c_str(),
                                     isSelected) ) {
                                meta.cover_path = Config::utf8ToPath(imgPath);
                                // 只更新草稿，函数末尾统一提交元数据命令。
                                changed = true;
                            }
                            if ( isSelected ) ImGui::SetItemDefaultFocus();
                        }
                    }
                    ::MMM::UI::FeedbackEndCombo();
                }
                // 保持 PushStyleColor 与所有控制流路径严格配对。
                if ( coverPushed ) ImGui::PopStyleColor();
                drawResourceImportButton(imageImportLabel,
                                         BeatmapResourceTarget::Cover,
                                         meta.cover_path);
                // 封面导入不隐式覆盖背景字段。
            });

        // 背景选择依据 cover_type 在图像与视频扩展名集合之间切换。
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.beatmap.background").data(),
            maxLabelW,
            [&,
             resourceComboWidth,
             drawResourceImportButton,
             imageImportLabel,
             noResource,
             drawClearReference](Clay_BoundingBox r, bool) {
                // main_cover_path 是实际背景引用，独立于缩略封面 cover_path。
                std::string currentBgPath =
                    displayPath(snapshot.paths[3], meta.main_cover_path);
                const char* bgPreview = meta.main_cover_path.empty()
                                            ? noResource
                                            : currentBgPath.c_str();

                const bool bgExists =
                    project &&
                    pathExists(snapshot.paths[3], meta.main_cover_path);
                bool bgPushed = false;
                if ( !bgExists && !currentBgPath.empty() ) {
                    // 空引用是合法未选择状态，不使用警告颜色。
                    ImGui::PushStyleColor(
                        ImGuiCol_Text, Utils::UIThemeUtils::getWarningColor());
                    bgPushed = true;
                }

                // 打开组合框后才扫描匹配当前背景类型的资源。
                ImGui::SetNextItemWidth(resourceComboWidth(r));
                if ( ::MMM::UI::FeedbackBeginCombo("##BgCombo", bgPreview) ) {
                    if ( bgPushed ) {
                        // 候选列表恢复主题正常文本色。
                        ImGui::PopStyleColor();
                        bgPushed = false;
                    }
                    if ( drawClearReference(meta.main_cover_path.empty()) &&
                         !meta.main_cover_path.empty() ) {
                        meta.main_cover_path.clear();
                        changed = true;
                    }
                    // IsWindowAppearing 只在候选 popup 打开的首帧产生边沿。
                    // 保持下拉打开时借用已扫描结果，无需周期性重扫整个工程。
                    m_beatmapSettingsScanRequested =
                        (project && ImGui::IsWindowAppearing()) ||
                        m_beatmapSettingsScanRequested;
                    if ( project && mediaReady ) {
                        const auto& backgroundResources =
                            meta.cover_type == CoverType::VIDEO
                                ? m_beatmapSettingsMedia->videos
                                : m_beatmapSettingsMedia->images;

                        for ( const auto& backgroundPath :
                              backgroundResources ) {
                            // 比较统一使用工程相对 UTF-8 路径。
                            bool isSelected = (currentBgPath == backgroundPath);
                            if ( ::MMM::UI::FeedbackSelectable(
                                     (backgroundPath + "##" + backgroundPath)
                                         .c_str(),
                                     isSelected) ) {
                                auto chosenPath =
                                    Config::utf8ToPath(backgroundPath);
                                // 选择只写元数据草稿，不在 UI 回调中加载背景。
                                meta.main_cover_path = chosenPath;
                                changed              = true;

                                // 如果背景是图片且封面为空，则自动沿用同一张图片。
                                // 已有封面始终保留，避免隐式覆盖用户显式选择。
                                // 两种候选在扫描时已按当前背景类型互斥分组，
                                // 可直接判断选中项是不是图片，无需在 UI
                                // 重查后缀。
                                if ( meta.cover_type != CoverType::VIDEO ) {
                                    if ( meta.cover_path.empty() ) {
                                        // 使用同一工程相对路径维持封面与背景可迁移性。
                                        meta.cover_path = chosenPath;
                                    }
                                }
                            }
                            if ( isSelected ) ImGui::SetItemDefaultFocus();
                        }
                    }
                    ::MMM::UI::FeedbackEndCombo();
                }
                // 未打开弹窗时恢复预览阶段可能压入的警告颜色。
                if ( bgPushed ) ImGui::PopStyleColor();
                drawResourceImportButton(
                    TR(meta.cover_type == CoverType::VIDEO
                           ? "ui.settings.beatmap.import_video"
                           : "ui.settings.beatmap.import_image")
                        .data(),
                    BeatmapResourceTarget::Background,
                    meta.main_cover_path);
                // 同一背景行的导入提示与过滤器共享当前类型，不影响封面行。
                // 背景导入保留用户已有的显式封面选择。
            });

        if ( isImd ) {
            // 与资源组首 BeginDisabled 配对，恢复后续页面状态。
            ImGui::EndDisabled();
        }
    }

    // 完成所有分组登记后统一执行 Clay 布局和 ImGui 回调。
    ImVec2 startPos = ImGui::GetCursorScreenPos();
    // 根布局使用当前剩余宽度，高度由展开的标题和设置行自动计算。
    ImVec2 sz = m_contentVBox.renderInCurrent(
        startPos, { ImGui::GetContentRegionAvail().x, 0 });
    // 推进游标，使父窗口正确计算滚动范围。
    ImGui::SetCursorScreenPos({ startPos.x, startPos.y + sz.y });

    // 失败信息在资源控件下方显示，避免同步文件错误被静默吞掉。
    if ( !m_beatmapResourceImportError.empty() ) {
        ImGui::TextColored(Utils::UIThemeUtils::getWarningColor(),
                           "%s",
                           m_beatmapResourceImportError.c_str());
    }

    if ( changed ) {
        // 一帧内所有元数据字段变化合并成一个逻辑命令，保持原子更新语义。
        // 草稿初值用于提取实际变化字段，后台快照到达前已经发生的外部修改
        // 继续由逻辑线程按消费时的最新值保留。
        // 专用轨道数量命令不设置 changed，避免与完整元数据命令重复处理。
        // 只有编辑发生才为差量基线分配所有权，普通绘制不增加引用计数。
        Logic::submitBeatmapSettingsCommand(
            snapshot,
            Logic::CmdUpdateBeatmapMetadata{
                .baseMeta = meta,
                .settingsBaseline =
                    std::make_shared<const BaseMapMeta>(std::move(baseline)) });
    }
}

}  // namespace MMM::UI
