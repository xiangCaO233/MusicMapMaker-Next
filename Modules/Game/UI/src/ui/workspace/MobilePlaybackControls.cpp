/// @file MobilePlaybackControls.cpp
/// @brief 触屏播放参数控件，界面意图与编辑器权威状态分开维护。
/// @details 五个入口不持有谱面对象，也不执行桌面工具栏的完整更新流程。
/// 播放开关经应用服务读取，实际动作统一进入已有命令通道。
/// 分拍属于软件编辑配置，未打开谱面时也允许预先选择。
/// 键数和速度绑定真实谱面，欢迎占位会话不能接受这些操作。
/// 键数修改复用可撤销轨道迁移，不覆盖其他元数据字段。
/// 倍速复用当前个人音频预设，不改变原始资源文件的采样速度。
///
/// 会话观察约束：
/// 与逻辑更新共用的会话锁只能尝试获取，不能等待完整播放更新。
/// 观察指针只在成功持锁期间解引用，离开作用域仅留下整数轨数。
/// 不缓存 Session、BeatMap 或字体的共享所有权，避免每帧引用计数复制。
/// 标签索引变化先清除显示缓存，弹层另存打开时的会话身份。
/// 拖动草稿只覆盖手指仍在操作的数值，空闲后恢复权威状态。
/// 输入与布局约束：
/// 普通弹层由 ImGui 处理外部点击和返回，关闭释放由本组件继续拦截。
/// 标题和关闭入口固定，正文变矮时由 Child 滚动提供全部选项。
/// 图标、按钮反馈、文字与圆角沿用移动外壳的统一资源和主题。
/// 弹层不保存到桌面 ini，安全区变化后每帧重新限制可用尺寸。
#include "ui/workspace/MobilePlaybackControls.h"

#include "audio/AudioManager.h"
#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/AudioPresets.h"
#include "config/EditorConfig.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/session/context/SessionContext.h"
#include "ui/IEditorApplicationService.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include "ui/utils/UIWidgetUtils.h"
#include "ui/workspace/MobileButton.h"

#include <algorithm>
#include <cmath>
#include <fmt/format.h>
#include <mutex>
#include <utility>

namespace MMM::UI
{
namespace
{
/// @brief 按最长标签限制预设列数，窄屏换行而不扩大按钮越界。
/// @param widestLabel 同组中最长的短标签，使用当前字体测量。
/// @param preferredColumns 宽屏首选列数，不是强制列数。
/// @param scale 当前内容缩放，不覆盖主题留白和圆角。
/// @return 实际列数和等宽按钮宽度。
/// @warning 弹层热路径：只测量短文本，不分配容器或加载字体。
std::pair<int, float> presetGrid(const char* widestLabel, int preferredColumns,
                                 float scale)
{
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    // 宽度来自当前正文 Child，滚动条已从可用区扣除。
    // 使用外层宽度会让最右预设伸进滚动条，故不能复用弹窗尺寸。
    const float available = ImGui::GetContentRegionAvail().x;
    // 与 MobileButton 的文字边距相同，最长标签也能完整显示。
    const float minimum = std::max(
        64.0f * scale, ImGui::CalcTextSize(widestLabel).x + 20.0f * scale);
    // 首选列数只是上限；至少保留一列，避免极窄窗口出现除零。
    // 各按钮共享余量，不因某个数字较短而改变下一项起点。
    const int columns =
        std::clamp(static_cast<int>((available + gap) / (minimum + gap)),
                   1,
                   preferredColumns);
    // 间距只存在于相邻列之间，首尾不再重复加入外边距。
    // 负余量防御性钳为一像素，最终绘制仍受父窗口裁剪。
    return { columns,
             std::max(1.0f, (available - gap * (columns - 1)) / columns) };
}
}  // namespace
/// @brief 仅保留尚未完整释放的关闭手势，不为反馈建立定时窗口。
/// @warning 每帧只读取输入标量，不等待触点释放。
/// @details 在画布输入之前推进关闭保护，函数不会消费或重发编辑命令。
void MobilePlaybackControls::beginFrame()
{
    // 在画布采样输入之前执行，释放帧也必须保持拦截。
    // 触点与释放事件均消失以后才清除保护，不依赖固定延时。
    m_releaseGuard =
        m_releaseGuard && (ImGui::IsMouseDown(0) || ImGui::IsMouseReleased(0));
}

/// @brief 以实际文字宽度保证英文暂停标签和最小倍速不会撑出工具栏。
/// @warning 每帧只测量固定数量短标签，不执行资源重载。
/// @param scale 与移动导航按钮一致的窗口内容缩放。
/// @return 足以显示所有标签和边距的像素宽度，向上取整。
float MobilePlaybackControls::buttonWidth(float scale)
{
    // 取整个工具栏的最大宽度，不只测量当前播放状态。
    // 播放变暂停或 1x 变 0.25x 时，主画布不会随之左右跳动。
    float width = 64.0f * scale;
    // 数值取合法范围中较长的表示；工具文字与实际按钮共用当前字体。
    for ( const char* label :
          { "0.25x",
            "32K",
            "64",
            TR_CACHE("ui.toolbar.short.move").data(),
            TR_CACHE("ui.toolbar.short.marquee").data(),
            TR_CACHE("ui.toolbar.short.draw").data(),
            TR_CACHE("ui.toolbar.short.play_pause").data(),
            TR_CACHE("ui.tools.bpm_measure.pause").data() } )
        width = std::max(width, ImGui::CalcTextSize(label).x + 16.0f * scale);
    // 字体可能返回分数像素，向下舍入会裁掉最后一列字形。
    return std::ceil(width);
}

/// @brief 只改变弹层意图，实际 OpenPopup 始终使用同一父窗口 ID。
/// @param popup 参数类型，不保存打开前的业务值。
/// @details 只登记请求，真正 OpenPopup 在固定父窗口中执行。
void MobilePlaybackControls::open(Popup popup)
{
    // 只登记意图，避免在按钮窗口的 ID 栈中打开另一父窗口的弹层。
    // 此刻固定会话身份；之后切换标签只能关闭弹层，不能静默改绑。
    m_popup             = popup;
    m_popupSessionIndex = m_sessionIndex;
    // 重新打开不恢复上次拖动草稿，滑块首先读取本次权威值。
    m_editingTracks = m_editingSpeed = false;
    // 首次按住滑块但数值未改变时也需要有效草稿，不能沿用默认的 1x。
    // 音频运行时是初始来源，后续拖动只更新本次手势的草稿。
    if ( popup == Popup::Speed )
        m_speedDraft = static_cast<float>(
            Audio::AudioManager::instance().getPlaybackSpeed());
    m_openRequested = true;
}

/// @brief 读取已发布播放状态及非阻塞轨数缓存，绘制五个直接操作入口。
/// @warning 每帧只尝试一次会话锁，忙时沿用标量，不复制会话共享所有权。
/// @param manager 提供寿命覆盖当前帧的应用服务观察指针。
/// @param width 已扣除主题留白与滚动条的实际内容宽度。
/// @param scale 仅缩放交互尺寸，不改变参数的合法范围。
void MobilePlaybackControls::drawButtons(UIManager& manager, float width,
                                         float scale)
{
    auto&     engine = Logic::EditorEngine::instance();
    const int index  = engine.getActiveSessionIndex();
    if ( index != m_sessionIndex ) {
        // 标签切换后宁可暂时显示占位，也不复用上一谱面的键数。
        m_sessionIndex = index;
        m_trackCount   = 0;
    }
    // 加载完成状态由引擎发布，不靠每帧遍历会话列表推断。
    // 它只决定入口可用性，命令执行时仍由会话自身验证对象。
    const bool hasBeatmap = engine.hasActiveBeatmap();
    if ( hasBeatmap ) {
        // 同一把锁也用于整轮逻辑更新，因此这里只尝试一次，绝不阻塞 UI。
        // 成功后借用 SessionEntry，只复制一个整数，不延长会话的共享所有权。
        std::unique_lock<std::recursive_mutex> lock(engine.getSessionMutex(),
                                                    std::try_to_lock);
        // try_lock 前标签可能刚被切换，持锁后再次核对索引。
        // 锁内 getter 的递归加锁立即成功，不引入第二次跨线程等待。
        if ( lock.owns_lock() && index == engine.getActiveSessionIndex() ) {
            // 注册表条目与上下文只在当前锁作用域内有效。
            // 实际复制仅有轨数，音符、采样和媒体元数据不参与 UI 查询。
            const auto* entry = engine.getSessionEntry(index);
            if ( entry && entry->session &&
                 entry->session->getContext().currentBeatmap )
                if ( !m_editingTracks )
                    m_trackCount = entry->session->getContext().trackCount;
        }
    } else {
        m_trackCount = 0;
    }
    // 初始化时没有虚构的播放状态，服务观察指针不跨帧持有。
    // 播放显示与菜单共用权威来源，按钮不自行维护播放计时器。
    const auto*  service = manager.getEditorApplicationService();
    const bool   playing = service && service->isPlaybackPlaying();
    const ImVec2 size{ width, 72.0f * scale };
    // 空白工作区不能通过占位入口启动音频，但保留固定按钮位置。
    // 图标及文字变化不改变内部 ID，也不重置反馈动画。
    ImGui::BeginDisabled(!hasBeatmap);
    if ( MobileNavigationButton(
             "playback",
             playing ? ICON_MMM_PAUSE : ICON_MMM_PLAY,
             playing ? TR_CACHE("ui.tools.bpm_measure.pause").data()
                     : TR_CACHE("ui.toolbar.short.play_pause").data(),
             size,
             playing) )
        MenuUtil::dispatchCommand(Logic::CmdSetPlayState{ !playing });
    ImGui::EndDisabled();

    ImGui::BeginDisabled(!hasBeatmap);
    // 速度读取音频运行时，未载入谱面时显示占位。
    // 沿用桌面的两位有效数字展示，不隐式量化实际播放速度。
    const auto speedText =
        hasBeatmap
            ? fmt::format("{:.2g}x",
                          Audio::AudioManager::instance().getPlaybackSpeed())
            : "--";
    if ( MobileNavigationButton("playback-speed",
                                ICON_MMM_SPEEDOMETER,
                                speedText.c_str(),
                                size,
                                m_popup == Popup::Speed) )
        open(Popup::Speed);
    ImGui::EndDisabled();
    // 尚未读到新会话轨数时保持禁用，不能把默认 4K 冒充元数据。
    // 已有缓存但逻辑繁忙时仍能操作，命令只携带目标轨数。
    ImGui::BeginDisabled(!hasBeatmap || m_trackCount <= 0);
    const auto keyText =
        m_trackCount > 0 ? fmt::format("{}K", m_trackCount) : "--";
    if ( MobileNavigationButton("key-count",
                                ICON_MMM_KEYBOARD,
                                keyText.c_str(),
                                size,
                                m_popup == Popup::Keys) )
        open(Popup::Keys);
    ImGui::EndDisabled();
    // 分拍与桌面一样属于编辑配置，无活动谱面时仍可预先调整。
    const int divisor =
        Config::AppConfig::instance().getEditorConfig().settings.beatDivisor;
    // 短标签只显示当前分拍数，完整含义由点击后的常驻标题说明。
    // 不在组件内保存第二份分拍值，其他配置入口的修改可直接回显。
    const auto divisorText = fmt::format("{}", divisor);
    if ( MobileNavigationButton("beat-divisor",
                                ICON_MMM_BARS,
                                divisorText.c_str(),
                                size,
                                m_popup == Popup::Divisor) )
        open(Popup::Divisor);
    // 与桌面缩放入口相同，只显示放大镜图标，不挤入当前百分比。
    // 缩放属于全局视觉配置，无谱面时同样允许预先调整。
    if ( MobileNavigationButton("timeline-zoom",
                                ICON_MMM_SEARCH,
                                "",
                                size,
                                m_popup == Popup::Zoom) )
        open(Popup::Zoom);
}

/// @brief 在屏幕内居中组合标题、可滚动正文与常驻关闭入口。
/// @details 分拍沿用皮肤预设，键数与倍速使用桌面相同范围及预设。
/// @warning 每帧只绘制当前参数；文件持久化仅在明确点击或拖动结束时执行。
/// @param manager 与按钮共用的应用服务，不创建新的配置所有者。
/// @param scale 当前缩放，视口上限在本次绘制重新计算。
/// @param enabled 工具栏隐藏时也推进所属弹层的关闭流程。
void MobilePlaybackControls::drawPopup(UIManager& manager, float scale,
                                       bool enabled)
{
    // 所有参数共用一个 Popup ID，同时最多存在一个可编辑弹层。
    // 在外壳面板结束后提交，不能继承工具栏窄小的裁剪区。
    ImGui::PushID("MobilePlaybackControls");
    constexpr const char* id = "##ParameterPopup";
    if ( m_openRequested && enabled ) ImGui::OpenPopup(id);
    // 打开请求只消费一次，外部点击关闭后不能在下一帧自动重开。
    m_openRequested = false;
    if ( !ImGui::IsPopupOpen(id) ) {
        // 包括点击外部和系统返回；关闭手势的释放不能在后方创建物件。
        if ( m_popup != Popup::None ) m_releaseGuard = true;
        // 包括系统返回关闭；背景画布在当前释放序列结束前仍不可绘制。
        m_popup = Popup::None;
        // 返回键可能中断尚未松手的滑条，关闭后必须解除草稿读取保护。
        m_editingTracks = m_editingSpeed = false;
        ImGui::PopID();
        return;
    }
    const auto& style    = ImGui::GetStyle();
    const auto* viewport = ImGui::GetMainViewport();
    const float margin   = 12.0f * scale;
    // 横竖屏先限制到安全工作区，再使用舒适的首选阅读尺寸。
    // 不累积上次的裁剪尺寸，旋转以后能恢复正常宽度。
    const ImVec2 popupSize{
        std::max(1.0f,
                 std::min(420.0f * scale, viewport->WorkSize.x - margin * 2)),
        std::max(1.0f,
                 std::min(350.0f * scale, viewport->WorkSize.y - margin * 2))
    };
    ImGui::SetNextWindowPos(
        viewport->GetCenter(), ImGuiCond_Always, { .5f, .5f });
    ImGui::SetNextWindowSize(popupSize);
    // 普通弹层允许点击外部或返回收起；自身输入仍优先于画布和抽屉。
    if ( ImGui::BeginPopup(id,
                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                               ImGuiWindowFlags_NoSavedSettings) ) {
        auto& engine = Logic::EditorEngine::instance();
        // 隐藏工具栏或失去原会话时，只关闭组件自己的弹层。
        // 分拍和缩放属于全局配置，不因活动谱面变化取消选择。
        if ( !enabled ||
             (m_popup != Popup::Divisor && m_popup != Popup::Zoom &&
              (!engine.hasActiveBeatmap() ||
               engine.getActiveSessionIndex() != m_popupSessionIndex)) ) {
            // 打开期间切换或关闭谱面时，不能把旧弹层中的参数应用到另一张谱面。
            ImGui::CloseCurrentPopup();
        } else {
            const char* title =
                m_popup == Popup::Divisor
                    ? TR_CACHE("ui.toolbar.beat_divisor").data()
                : m_popup == Popup::Keys
                    ? TR_CACHE("ui.settings.beatmap.tracks").data()
                : m_popup == Popup::Zoom
                    ? TR_CACHE("ui.settings.visual.timeline_zoom").data()
                    : TR_CACHE("ui.toolbar.playback_speed").data();
            // 标题使用业务翻译，长英文换行而不挤压关闭入口。
            ImGui::TextWrapped("%s", title);
            ImGui::Separator();
            const float buttonHeight =
                std::max(48.0f * scale, ImGui::GetFrameHeight());
            // 页脚位于正文 Child 之外；矮屏和大字号只缩小正文，不裁掉关闭按钮。
            const float bodyHeight =
                std::max(1.0f,
                         ImGui::GetContentRegionAvail().y - buttonHeight -
                             style.ItemSpacing.y);
            if ( ImGui::BeginChild("ParameterBody", { 0, bodyHeight }) ) {
                // 触屏滑块只接收拖动，禁用 Ctrl+点击进入文本输入的桌面分支。
                // 整数滑块自身完成取整，不需要固定延时或定时吸附。
                const auto flags =
                    ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput;
                // 把滑条命中高度提高到手指尺寸，横向 padding 和圆角仍沿用主题。
                // 样式只覆盖本次滑条，预设与关闭入口不继承这段高度补偿。
                ImGui::PushStyleVar(
                    ImGuiStyleVar_FramePadding,
                    { style.FramePadding.x,
                      std::max(style.FramePadding.y,
                               (48.0f * scale - ImGui::GetFontSize()) * .5f) });
                ImGui::SetNextItemWidth(-1);
                auto* service = manager.getEditorApplicationService();
                // 每帧读取配置副本，只在本次真正操作以后修改目标字段。
                // 不使用打开时的旧副本覆盖其他入口刚修改的设置。
                if ( m_popup == Popup::Zoom ) {
                    // 配置副本只覆盖本次操作；其余编辑与美化字段必须保留。
                    // 应用服务不可用时仍可展示当前配置，但不会绕过服务提交。
                    auto config =
                        service
                            ? service->editorConfig()
                            : Config::AppConfig::instance().getEditorConfig();
                    // 与桌面保持同一正数范围，对数映射保留低倍率的触摸精度。
                    float percent = std::clamp(
                        config.visual.timelineZoom * 100.0f, 10.0f, 1000.0f);
                    // 下界不能取零，否则对数滑条映射失去定义。
                    // 上界与画布滚轮共用，不能额外开放桌面不支持的倍率。
                    bool changed = FeedbackSliderFloat(
                        "##TimelineZoom",
                        &percent,
                        10.0f,
                        1000.0f,
                        "",
                        flags | ImGuiSliderFlags_Logarithmic);
                    // 拖动和本帧显示同时吸附到十个百分点，避免先闪过原始小数。
                    // 预设 25% 或加减得到的 105% 在未拖动时仍保留原精度。
                    if ( changed || ImGui::IsItemActive() ) {
                        const float snapped =
                            std::round(percent / 10.0f) * 10.0f;
                        // 触点按下但尚未位移时也对齐，显示与提交从首帧起一致。
                        // 仅吸附后的值变化才发布，防止空闲弹层重复更新配置。
                        changed |= snapped != percent;
                        percent = snapped;
                    }
                    // 隐藏原生滑条文字后覆盖吸附值；不创建新命中区或抢走触点。
                    const auto text     = fmt::format("{:.0f}%", percent);
                    const auto textSize = ImGui::CalcTextSize(text.c_str());
                    // 使用实际滑条矩形，不依赖字体基线或窗口估算宽度。
                    // DrawList 继承正文 Child 裁剪，不会覆盖固定关闭区。
                    const auto minimum = ImGui::GetItemRectMin();
                    const auto maximum = ImGui::GetItemRectMax();
                    ImGui::GetWindowDrawList()->AddText(
                        { (minimum.x + maximum.x - textSize.x) * .5f,
                          (minimum.y + maximum.y - textSize.y) * .5f },
                        ImGui::GetColorU32(ImGuiCol_Text),
                        text.c_str());
                    // 大命中高度只作用于滑条，后续按钮继续使用统一移动样式。
                    ImGui::PopStyleVar();
                    // 三个动作与桌面一致，按实际文字测量宽度并允许窄屏换行。
                    // 微小容差仅抵消倍率转百分比的浮点误差，不改变步长。
                    const auto [columns, width] = presetGrid("100%", 3, scale);
                    // 边界只禁用方向按钮，保留其占位以避免预设排列跳动。
                    ImGui::BeginDisabled(percent <= 10.0f);
                    if ( MobileButton("-###ZoomOut",
                                      { width, buttonHeight }) ) {
                        // 向下选下一个五的倍数，兼容旧配置中的非整点数值。
                        // 先对齐再减一档，不能让 100% 因浮点误差跳到 90%。
                        percent = (std::ceil(percent / 5.0f - .0001f) - 1) * 5;
                        changed = true;
                    }
                    ImGui::EndDisabled();
                    if ( columns > 1 ) ImGui::SameLine();
                    if ( MobileButton("100%###ZoomReset",
                                      { width, buttonHeight }) ) {
                        // 重置不依赖双击和键盘输入，单次触摸即可恢复默认。
                        percent = 100.0f;
                        changed = true;
                    }
                    // 第二个动作可能已经换行；第三个按实际列数决定续行。
                    // 极窄屏可退为单列，仍保留三个独立的触摸目标。
                    if ( 2 % columns != 0 ) ImGui::SameLine();
                    ImGui::BeginDisabled(percent >= 1000.0f);
                    if ( MobileButton("+###ZoomIn", { width, buttonHeight }) ) {
                        // 与减号对称向上选择整档，连续点击保持固定步长。
                        percent = (std::floor(percent / 5.0f + .0001f) + 1) * 5;
                        changed = true;
                    }
                    ImGui::EndDisabled();
                    // 常用倍率与桌面一致，完整范围仍由滑条与加减入口提供。
                    const auto [presetColumns, presetWidth] =
                        presetGrid("400%", 4, scale);
                    // 预设另起一组计算列数，不继承上方三动作的换行状态。
                    // 最长文本决定最小宽度，英文数字和百分号不会被截断。
                    int column = 0;
                    for ( const int preset : { 25, 50, 200, 400 } ) {
                        if ( column++ % presetColumns != 0 ) ImGui::SameLine();
                        if ( MobileButton(fmt::format("{}%", preset).c_str(),
                                          { presetWidth, buttonHeight }) ) {
                            percent = static_cast<float>(preset);
                            changed = true;
                        }
                    }
                    // 每次只回写最新配置的缩放字段，沿用桌面发布入口。
                    // 缩放只影响时间尺度，不提交谱面编辑或音频速度命令。
                    if ( changed && service ) {
                        // 统一出口再次钳位，覆盖按钮舍入后的浮点边界误差。
                        // 连续拖动立即发布，不等待抬手或固定毫秒窗口同步画布。
                        config.visual.timelineZoom =
                            std::clamp(percent / 100.0f, .1f, 10.0f);
                        service->updateEditorConfig(config);
                        // 与桌面包装入口一致，用户布局的持久化走后台单槽队列。
                        Config::AppConfig::instance().saveLayoutChanges();
                    }
                } else if ( m_popup == Popup::Divisor ) {
                    auto config =
                        service
                            ? service->editorConfig()
                            : Config::AppConfig::instance().getEditorConfig();
                    int  value   = config.settings.beatDivisor;
                    bool changed = FeedbackSliderInt(
                        "##Divisor", &value, 1, 64, "%d", flags);
                    ImGui::PopStyleVar();
                    // 预设排列根据可用宽度换行，不把桌面四列硬挤进窄屏。
                    // 预设借用当前皮肤缓存，不复制向量或查询文件系统。
                    // 跳过非法皮肤值，让预设与滑条保持相同合法范围。
                    const auto& presets =
                        Config::SkinManager::instance().getCommonDivisors();
                    int column                  = 0;
                    const auto [columns, width] = presetGrid("1/64", 4, scale);
                    for ( const int preset : presets ) {
                        if ( preset < 1 || preset > 64 ) continue;
                        if ( column++ % columns != 0 ) ImGui::SameLine();
                        // 皮肤可能重复提供相同分拍，用索引作用域避免重复控件
                        // ID。
                        ImGui::PushID(column);
                        if ( MobileButton(fmt::format("1/{}", preset).c_str(),
                                          { width, buttonHeight }) ) {
                            value   = preset;
                            changed = true;
                        }
                        ImGui::PopID();
                    }
                    // 应用服务统一处理通知和保存，不直接改逻辑上下文。
                    // 布局保存入口与桌面一致，不等待后台保存任务完成。
                    if ( changed && service ) {
                        config.settings.beatDivisor = value;
                        service->updateEditorConfig(config);
                        Config::AppConfig::instance().saveLayoutChanges();
                    }
                } else if ( m_popup == Popup::Keys ) {
                    int  value   = m_trackCount;
                    bool changed = FeedbackSliderInt(
                        "##Keys", &value, 1, 32, "%dK", flags);
                    // 手指按住期间沿用本地整数，旧快照不会把滑块拉回。
                    // 释放后恢复权威读取，撤销或其他入口的修改仍能回显。
                    // 这里只记录 UI
                    // 捕获状态，不暂停播放或等待逻辑消费上一条命令。
                    m_editingTracks = ImGui::IsItemActive();
                    ImGui::PopStyleVar();
                    // 常用 4K 到 8K 与桌面一致，滑条仍能到达完整的 1 到 32。
                    // 列数受内容宽度约束，字体放大也不会强制挤成三列。
                    const auto [columns, width] = presetGrid("8K", 3, scale);
                    int column                  = 0;
                    for ( const int preset : { 4, 5, 6, 7, 8 } ) {
                        if ( column++ % columns != 0 ) ImGui::SameLine();
                        if ( MobileButton(fmt::format("{}K", preset).c_str(),
                                          { width, buttonHeight }) ) {
                            value   = preset;
                            changed = true;
                        }
                    }
                    if ( changed ) {
                        // 同一轨道迁移动作同时回写元数据并保留自动采样相对轨道，支持撤销。
                        // 只发送目标轨数，逻辑线程处理最新谱面，不复制或覆盖其他元数据字段。
                        m_trackCount = value;
                        MenuUtil::dispatchCommand(
                            Logic::CmdUpdateTrackCount{ value });
                    }
                } else {
                    float value =
                        m_editingSpeed
                            ? m_speedDraft
                            : static_cast<float>(Audio::AudioManager::instance()
                                                     .getPlaybackSpeed());
                    bool changed = FeedbackSliderFloat(
                        "##Speed", &value, .25f, 2.0f, "%.4fx", flags);
                    // 释放帧即使数值未变，也必须保存最后一次已提交的速度。
                    // 交互状态在后续按钮提交前读取，避免 LastItem 被覆盖。
                    const bool commit = ImGui::IsItemDeactivatedAfterEdit();
                    m_editingSpeed    = ImGui::IsItemActive();
                    ImGui::PopStyleVar();
                    // 预设点击立即持久化；滑条只在释放时写盘，连续调整仍即时入队。
                    // 两条路径提交同一速度字段，不修改原始音频资源。
                    bool presetClicked          = false;
                    const auto [columns, width] = presetGrid("0.75x", 2, scale);
                    int column                  = 0;
                    for ( const float preset : { .25f, .5f, .75f, 1.0f } ) {
                        if ( column++ % columns != 0 ) ImGui::SameLine();
                        if ( MobileButton(
                                 fmt::format("{:.2g}x", preset).c_str(),
                                 { width, buttonHeight }) ) {
                            value   = preset;
                            changed = presetClicked = true;
                        }
                    }
                    if ( changed ) {
                        // 个人音频预设与桌面共用；连续调整立即提交播放命令，释放时才写盘。
                        // 草稿仅服务本次拖动，不作为后台音频速度的第二个权威来源。
                        m_speedDraft = value;
                        auto& app    = Config::AppConfig::instance();
                        app.getAudioPresets().current().m_speed =
                            std::clamp<double>(value, .25, 2.0);
                        MenuUtil::dispatchCommand(Logic::CmdSetPlaybackSpeed{
                            std::clamp<double>(value, .25, 2.0) });
                    }
                    if ( commit || presetClicked )
                        Config::AppConfig::instance().save();
                }
            }
            // Child 即使被裁剪也必须 End，不能把窗口栈留给后续组件。
            ImGui::EndChild();
            // 关闭入口独立于正文滚动，窄屏滚到末尾也始终可用。
            // 统一移动按钮继续负责图标、音效和短促亮度反馈。
            if ( MobileActionButton("close",
                                    ICON_MMM_CLOSE,
                                    TR_CACHE("ui.window.close").data(),
                                    { -1, buttonHeight }) )
                ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
}
}  // namespace MMM::UI
