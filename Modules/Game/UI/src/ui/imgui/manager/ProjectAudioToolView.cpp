#include "ui/imgui/manager/ProjectAudioToolView.h"

#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/translation/Translation.h"
#include "event/project/ProjectEvents.h"
#include "logic/EditorEngine.h"
#include "ui/UIManager.h"
#include "ui/imgui/audio/ProjectAudioPreviewControls.h"
#include "ui/imgui/manager/ProjectAudioToolSearch.h"
#include "ui/utils/UIWidgetUtils.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace MMM::UI
{
namespace
{

/// @brief 使用翻译文本和稳定后缀构建 ImGui 显示标签并复用已有容量。
/// @param destination 接收完整标签的缓存字符串。
/// @param text 当前语言的显示文本。
/// @param stableId 以 ### 开头的稳定 ImGui ID。
///
/// 显示部分可以随语言改变，### 后缀维持窗口和弹窗的内部身份不变。
/// 目标字符串属于翻译缓存，复用容量可减少语言未变化时的短期分配。
void assignImGuiLabel(std::string& destination, Translation::TRResult text,
                      const std::string_view stableId)
{
    // 清空内容但保留已有容量，随后按最终长度一次预留。
    destination.clear();
    destination.reserve(text.size() + stableId.size());
    // 先写可见翻译文本，再追加只供 ImGui 解析的稳定后缀。
    destination.append(text.view());
    destination.append(stableId);
}

/// @brief Effect 方块的默认逻辑边长。
///
/// 逻辑尺寸不包含 DPI 和画布缩放，绘制阶段统一转换为屏幕像素。
constexpr float DEFAULT_EFFECT_SIZE = 92.0F;

/// @brief Main 方块的默认和最小逻辑宽度。
///
/// 主轨使用更宽的初始形态，便于与方形 Effect 资源快速区分。
constexpr float DEFAULT_MAIN_WIDTH = 202.0F;

/// @brief Main 方块的默认逻辑高度。
///
/// 高度仍会被试听控件所需的主题最小高度抬升。
constexpr float DEFAULT_MAIN_HEIGHT = 92.0F;

/// @brief 方块边缘缩放热区的逻辑厚度。
///
/// 命中测试在逻辑坐标执行，因此热区不会随画布缩放改变业务尺寸。
constexpr float RESIZE_HIT_THICKNESS = 8.0F;

/// @brief 选中方块缩放控制点的逻辑边长。
///
/// 绘制时乘以 DPI，不乘画布逻辑尺寸以外的额外常量。
constexpr float RESIZE_HANDLE_SIZE = 7.0F;

/// @brief 默认方块布局间距。
///
/// 自动排布只在资源没有持久化位置时使用该间距。
constexpr float ITEM_GAP = 18.0F;

/// @brief 默认方块布局画布留白。
///
/// 新资源从该留白位置开始排列，避免紧贴滚动画布边缘。
constexpr float CANVAS_PADDING = 24.0F;

/// @brief 任一下层方块必须保留的最小可见比例。
///
/// 值由纯布局模块统一定义，拖动与缩放约束必须使用同一比例。
constexpr float MINIMUM_VISIBLE_RATIO =
    ProjectAudioToolLayout::STACK_MINIMUM_VISIBLE_RATIO;

/// @brief 方块开始吸附的逻辑像素距离。
///
/// 阈值用于首次锁定目标线，与释放阈值组成滞回区间。
constexpr float SNAP_THRESHOLD = 8.0F;

/// @brief 已吸附方块脱离目标所需的逻辑像素距离。
///
/// 较大的释放阈值避免指针轻微抖动导致参考线频繁闪烁。
constexpr float SNAP_RELEASE_THRESHOLD = 16.0F;

/// @brief 最远方块之后保留的可滚动画布空间。
///
/// 额外空间允许用户继续向右或向下拖动，而不会贴住滚动极限。
constexpr float CONTENT_END_PADDING = 80.0F;

/// @brief 鼠标在方块外触发试听控件显示的最大逻辑距离。
///
/// 实际近邻搜索还会按画布缩放修正距离，保持屏幕体验稳定。
constexpr float AUDIO_CONTROLS_PROXIMITY = 14.0F;

/// @brief 获取当前主题下固定试听控件行所需的逻辑宽度。
/// @param dpiScale 当前窗口内容缩放。
/// @return 四个按钮与间距、内边距共同要求的逻辑宽度。
///
/// ImGui 提供屏幕像素尺寸，返回前除以安全缩放转换回画布逻辑单位。
/// @warning UI 热路径：只读取当前样式并进行常数次算术运算。
float controlMinimumWidth(float dpiScale)
{
    // 样式引用只在本次计算期间有效，不存入视图状态。
    const auto& style = ImGui::GetStyle();
    // 防御未初始化缩放，避免除零和无限尺寸污染持久化布局。
    const float safeScale = std::max(0.01F, dpiScale);
    // 按钮间距设定上限，使宽松主题不会令方块最小宽度异常膨胀。
    const float spacing =
        std::max(1.0F, std::min(style.ItemInnerSpacing.x, 4.0F));
    // 向上取整确保换算后仍能完整容纳所有固定控件。
    return std::ceil(
        ProjectAudioToolLayout::calculateControlMinimumWidth(
            ImGui::GetFrameHeight(), spacing, style.FramePadding.x, 4U) /
        safeScale);
}

/// @brief 获取指定音频类型允许的最小逻辑宽度。
/// @param type 音频资源类型。
/// @param dpiScale 当前窗口内容缩放。
/// @return 类型形态与试听控件约束中的较大值。
///
/// 自定义宽度和交互缩放都必须经过该下限，防止按钮互相覆盖。
float minimumItemWidth(AudioTrackType type, float dpiScale)
{
    // Main 保持横向卡片形态，Effect 使用默认方形边长。
    const float typeMinimum =
        type == AudioTrackType::Main ? DEFAULT_MAIN_WIDTH : DEFAULT_EFFECT_SIZE;
    // 主题控件可能比默认方块更宽，必须优先保证可操作性。
    return std::max(typeMinimum, controlMinimumWidth(dpiScale));
}

/// @brief 获取类型、文件名、进度条和固定按钮行所需的最小逻辑高度。
/// @param dpiScale 当前窗口内容缩放。
/// @return 可完整容纳方块内部内容的逻辑高度。
///
/// 高度由公共试听控件布局公式计算，保证测量与实际控件组成一致。
/// @warning UI 热路径：只读取 ImGui 样式和字体指标，不分配资源。
float minimumItemHeight(float dpiScale)
{
    // 缩放下限防止初始化阶段返回不可用的无限逻辑高度。
    const auto& style     = ImGui::GetStyle();
    const float safeScale = std::max(0.01F, dpiScale);
    // 进度条随字体适度缩放，但限制在清晰且不过度占高的范围内。
    const float progressHeight =
        std::clamp(ImGui::GetFontSize() * 0.18F, 3.0F, 7.0F);
    // 纵向间距同样限制上限，避免主题间距放大卡片最低高度。
    const float progressSpacing =
        std::max(1.0F, std::min(style.ItemInnerSpacing.y, 3.0F));
    // 纯布局 helper 返回屏幕尺寸，除以 DPI 后向上取整为逻辑尺寸。
    return std::ceil(ProjectAudioToolLayout::calculateControlMinimumHeight(
                         ImGui::GetTextLineHeight(),
                         progressHeight,
                         progressSpacing,
                         ImGui::GetFrameHeight(),
                         style.FramePadding.y,
                         style.ItemSpacing.y) /
                     safeScale);
}

/// @brief 按当前字体测量结果计算可完整显示文件名的默认逻辑宽度。
/// @param label 文件名或资源 ID 显示文本。
/// @param type 音频资源类型。
/// @param dpiScale 当前窗口内容缩放。
/// @return 兼顾文本、内边距和控件下限的默认逻辑宽度。
///
/// 该宽度只用于没有保存自定义宽度的新方块，用户缩放结果不会被覆盖。
float defaultItemWidth(std::string_view label, AudioTrackType type,
                       float dpiScale)
{
    // 文本端指针显式传入，允许 string_view 不以空字符结束。
    const float safeScale = std::max(0.01F, dpiScale);
    const float textWidth =
        ImGui::CalcTextSize(label.data(), label.data() + label.size()).x /
        safeScale;
    // 两侧框内边距由布局 helper 按统一规则计入默认宽度。
    const float horizontalPadding =
        ImGui::GetStyle().FramePadding.x / safeScale;
    // 最终结果不会小于该音频类型对应的可操作下限。
    return ProjectAudioToolLayout::calculateDefaultWidth(
        textWidth, horizontalPadding, minimumItemWidth(type, dpiScale));
}

/// @brief 将项目音频资源路径转换为方块显示标签。
/// @param resource 项目音频资源。
/// @return UTF-8 文件名；路径没有文件名时回退资源 ID。
///
/// 显示标签不参与资源身份判断，重命名和选择始终使用稳定 ID。
std::string audioResourceLabel(const AudioResource& resource)
{
    // 先恢复平台路径对象以正确识别路径分隔符，再提取末级文件名。
    const auto filename = Config::utf8ToPath(resource.m_path).filename();
    // UI 始终使用 UTF-8，空文件名不能生成不可见方块标题。
    const std::string label = Config::pathToUtf8(filename);
    return label.empty() ? resource.m_id : label;
}

/// @brief 判断逻辑矩形是否与当前可见区域相交。
/// @param rect 待测试方块矩形。
/// @param visible 当前可见画布矩形。
/// @return 存在非空交集时返回 true。
///
/// 统一调用布局模块的交集语义，避免绘制剔除与约束算法边界不一致。
bool isVisible(const ProjectAudioToolLayout::Rect& rect,
               const ProjectAudioToolLayout::Rect& visible)
{
    return ProjectAudioToolLayout::intersection(rect, visible).has_value();
}

/// @brief 转换逻辑画布矩形为屏幕像素矩形。
/// @param rect 逻辑画布矩形。
/// @param origin 当前画布逻辑原点对应的屏幕坐标。
/// @param canvasScale DPI 与画布缩放的乘积。
/// @return 可直接用于 ImDrawList 的屏幕矩形。
///
/// 滚动偏移已经包含在 origin 中，调用方不得再次扣除滚动量。
ProjectAudioToolLayout::Rect toScreenRect(
    const ProjectAudioToolLayout::Rect& rect, ImVec2 origin, float canvasScale)
{
    // 位置先按画布倍率缩放再平移，宽高只需缩放。
    return {
        origin.x + rect.x * canvasScale,
        origin.y + rect.y * canvasScale,
        rect.width * canvasScale,
        rect.height * canvasScale,
    };
}

/// @brief 单个音频方块内按需试听按钮的屏幕布局。
struct ItemAudioControlLayout {
    /// @brief 可直接传给通用试听控件的屏幕布局。
    ProjectAudioPreviewControlsLayout controls;

    /// @brief 文件名文本可使用区域的屏幕底边。
    float labelBottom{ 0.0F };
};

/// @brief 计算方块内固定尺寸的播放、暂停、停止和音量按钮布局。
/// @param itemRect 方块完整屏幕区域。
/// @return 由主题控件尺寸决定的统一按钮布局。
/// @warning 每个可见音频方块每帧调用，不得引入分配或阻塞操作。
///
/// 试听控件锚定在方块底部并水平居中，文件名区域只能使用其上方空间。
/// 所有结果均为屏幕像素，直接交给 ImGui 控件和命中测试使用。
/// 方块最小尺寸已保证控件总宽高可容纳，正常输入下 top 不会越过方块顶边。
/// 控件宽度不随方块继续变宽，额外空间只用于居中。
ItemAudioControlLayout calculateItemAudioControlLayout(
    const ProjectAudioToolLayout::Rect& itemRect)
{
    // 内边距设置至少一个像素，避免紧凑主题让内容贴住边框。
    const auto& style             = ImGui::GetStyle();
    const float horizontalPadding = std::max(1.0F, style.FramePadding.x);
    const float verticalPadding   = std::max(1.0F, style.FramePadding.y);
    // 横向按钮间距限制上限，保持固定四按钮行在窄卡片中可用。
    const float spacing =
        std::max(1.0F, std::min(style.ItemInnerSpacing.x, 4.0F));
    // 进度条高度与最小高度计算使用相同约束。
    const float progressHeight =
        std::clamp(ImGui::GetFontSize() * 0.18F, 3.0F, 7.0F);
    const float progressSpacing =
        std::max(1.0F, std::min(style.ItemInnerSpacing.y, 3.0F));
    // 四个按钮均为当前 ImGui 框高的正方形。
    const float buttonSize = ImGui::GetFrameHeight();
    // 三个间隔位于相邻四按钮之间，不额外计算两侧留白。
    const float totalWidth = buttonSize * 4.0F + spacing * 3.0F;
    // 进度条、纵向间距和按钮行共同组成完整控件高度。
    const float totalHeight = progressHeight + progressSpacing + buttonSize;
    // 控件整体贴近方块底部，并保留主题纵向内边距。
    const float top = itemRect.bottom() - verticalPadding - totalHeight;
    return {
        .controls =
            ProjectAudioPreviewControlsLayout{
                .topLeft =
                    {
                        // 居中偏移不能小于横向内边距。
                        itemRect.x +
                            std::max(horizontalPadding,
                                     (itemRect.width - totalWidth) * 0.5F),
                        top,
                    },
                .width           = totalWidth,
                .buttonSize      = buttonSize,
                .buttonSpacing   = spacing,
                .progressHeight  = progressHeight,
                .progressSpacing = progressSpacing,
            },
        // 标签下边界位于控件上方，且不能越过方块自身顶边。
        .labelBottom =
            std::max(itemRect.y,
                     top - std::max(1.0F, style.ItemSpacing.y)),
    };
}

/// @brief 判断屏幕坐标是否位于试听进度条或按钮行组成的完整控件区。
/// @param layout 试听控件的绝对屏幕布局。
/// @param point 待检查的屏幕坐标。
/// @return 坐标位于控件区时返回 true。
/// @warning 每个可见音频方块每帧调用，不得引入分配或阻塞操作。
///
/// 命中区域覆盖进度条和完整按钮行，用于阻止底层拖动手势抢占控件输入。
bool containsItemAudioControls(const ProjectAudioPreviewControlsLayout& layout,
                               ImVec2                                   point)
{
    // 控件高度必须与 calculateItemAudioControlLayout 的组成保持一致。
    const float controlsHeight =
        layout.progressHeight + layout.progressSpacing + layout.buttonSize;
    // 边界采用闭区间，使落在绘制边缘上的指针仍归属于控件。
    return point.x >= layout.topLeft.x &&
           point.x <= layout.topLeft.x + layout.width &&
           point.y >= layout.topLeft.y &&
           point.y <= layout.topLeft.y + controlsHeight;
}

/// @brief 判断逻辑点是否位于矩形内。
/// @param rect 逻辑矩形。
/// @param point 逻辑坐标点。
/// @return 点位于矩形闭区间内时返回 true。
///
/// 该命中规则用于按 Z 序反向查找最上层方块。
bool contains(const ProjectAudioToolLayout::Rect& rect, ImVec2 point)
{
    return point.x >= rect.x && point.x <= rect.right() && point.y >= rect.y &&
           point.y <= rect.bottom();
}

/// @brief 计算逻辑点到矩形的平方距离；矩形内部距离为零。
/// @param rect 逻辑矩形。
/// @param point 逻辑坐标点。
/// @return 点到最近矩形边或角的平方距离。
/// @warning UI 热路径：每帧仅对可见方块执行常数次浮点运算。
float squaredDistanceToRect(const ProjectAudioToolLayout::Rect& rect,
                            ImVec2                              point)
{
    // 点落在水平投影范围内时 X 距离为零，否则取到最近竖边的距离。
    const float deltaX = point.x < rect.x         ? rect.x - point.x
                         : point.x > rect.right() ? point.x - rect.right()
                                                  : 0.0F;
    // Y 轴使用相同规则，两个轴共同区分边缘和角点距离。
    const float deltaY = point.y < rect.y          ? rect.y - point.y
                         : point.y > rect.bottom() ? point.y - rect.bottom()
                                                   : 0.0F;
    // 比较平方距离可避免每个候选方块执行平方根。
    return deltaX * deltaX + deltaY * deltaY;
}

/// @brief 由两个逻辑画布点构造方向无关的矩形。
/// @param first 第一个角点。
/// @param second 第二个角点。
/// @return 左上角与非负宽高组成的标准化矩形。
///
/// 框选允许向任意方向拖动，因此不能假定 first 位于左上方。
ProjectAudioToolLayout::Rect rectFromPoints(ImVec2 first, ImVec2 second)
{
    // 分别取轴向极值，统一反向拖动与正向拖动的矩形表示。
    const float left   = std::min(first.x, second.x);
    const float top    = std::min(first.y, second.y);
    const float right  = std::max(first.x, second.x);
    const float bottom = std::max(first.y, second.y);
    return { left, top, right - left, bottom - top };
}

/// @brief 判断外层矩形是否完整包含内层矩形。
/// @param outer 框选矩形。
/// @param inner 资源方块矩形。
/// @return inner 四条边均位于 outer 容差范围内时返回 true。
///
/// 严格框选模式使用该判断；微小容差吸收缩放和坐标换算误差。
bool containsRect(const ProjectAudioToolLayout::Rect& outer,
                  const ProjectAudioToolLayout::Rect& inner)
{
    // 半逻辑像素容差不应扩大成明显的部分相交选择。
    constexpr float EPSILON = 0.5F;
    return inner.x >= outer.x - EPSILON &&
           inner.right() <= outer.right() + EPSILON &&
           inner.y >= outer.y - EPSILON &&
           inner.bottom() <= outer.bottom() + EPSILON;
}

/// @brief 判断矩形指定轴上的边缘或中心是否与目标参考线重合。
/// @param rect 当前拖动或缩放矩形。
/// @param targetLine 已锁定的逻辑参考线坐标。
/// @param horizontal true 检查 X 轴，false 检查 Y 轴。
/// @return 任一边缘或中心线在容差内重合时返回 true。
///
/// 仅在仍然真正对齐时绘制吸附线，约束修正后的偏移不会产生虚假指示。
bool alignsWithTargetLine(const ProjectAudioToolLayout::Rect& rect,
                          float targetLine, bool horizontal)
{
    // 容差小于一个逻辑像素，只用于处理浮点计算误差。
    constexpr float EPSILON = 0.25F;
    if ( horizontal ) {
        // X 轴候选为左边、水平中心和右边。
        return std::abs(rect.x - targetLine) <= EPSILON ||
               std::abs(rect.x + rect.width * 0.5F - targetLine) <= EPSILON ||
               std::abs(rect.right() - targetLine) <= EPSILON;
    }
    // Y 轴候选为上边、垂直中心和下边。
    return std::abs(rect.y - targetLine) <= EPSILON ||
           std::abs(rect.y + rect.height * 0.5F - targetLine) <= EPSILON ||
           std::abs(rect.bottom() - targetLine) <= EPSILON;
}

/// @brief 绘制一条与主画布布局调整一致的半透明虚线吸附参考线。
/// @param drawList 目标 ImGui 绘制列表。
/// @param start 参考线屏幕起点。
/// @param end 参考线屏幕终点。
/// @param color 线段颜色。
/// @param thickness 线段粗细。
/// @param dashLength 每段实线长度。
/// @param gapLength 相邻实线之间的间隔。
/// @warning UI 拖动热路径：仅吸附生效时调用，按可见画布单轴生成短线段。
///
/// 线段裁剪范围由调用方限制到可见画布，不会延伸到状态栏或搜索区域。
void drawSnapGuide(ImDrawList& drawList, const ImVec2& start, const ImVec2& end,
                   ImU32 color, float thickness, float dashLength,
                   float gapLength)
{
    // 先计算方向和总长度，零长度输入不产生绘制命令。
    const float deltaX = end.x - start.x;
    const float deltaY = end.y - start.y;
    const float length = std::sqrt(deltaX * deltaX + deltaY * deltaY);
    if ( length <= 0.0F ) return;

    // 实线与间隔均保证至少一个像素，避免循环步长退化为零。
    dashLength = std::max(1.0F, dashLength);
    gapLength  = std::max(1.0F, gapLength);
    // 单位方向向量用于把一维距离映射回屏幕坐标。
    const float dx   = deltaX / length;
    const float dy   = deltaY / length;
    const float step = dashLength + gapLength;
    // 最后一段截断到终点，防止绘制越过可见区域边界。
    for ( float distance = 0.0F; distance < length; distance += step ) {
        const float segmentEnd = std::min(distance + dashLength, length);
        drawList.AddLine(
            { start.x + dx * distance, start.y + dy * distance },
            { start.x + dx * segmentEnd, start.y + dy * segmentEnd },
            color,
            thickness);
    }
}

}  // namespace

/// @brief 创建项目音频工具并订阅会改变资源布局的项目事件。
/// @param name 视图名称。
///
/// 保存完成和音频资源变更成功都会使方块缓存失效；事件回调只发布原子标记。
/// 多个事件可合并为下一帧一次 rebuildItems，不需要在回调中访问项目模型。
/// @warning 事件回调可能跨线程执行，不得读取 ImGui 状态或直接修改方块容器。
///
/// m_itemsDirty 使用 release 写入，并由 update 以 acquire 交换消费。
/// 订阅回调不捕获项目指针，因此项目切换不会留下悬空模型引用。
ProjectAudioToolView::ProjectAudioToolView(const std::string& name)
    : IUIView(name)
{
    // 两项订阅共享视图生命周期，ID 分别保存以便精确解除。
    auto& eventBus      = Event::EventBus::instance();
    m_projectSavedSubId = eventBus.subscribe<Event::ProjectSavedEvent>(
        [this](const Event::ProjectSavedEvent&) {
            // 项目保存可能来自外部更新，下一帧重新读取工作区布局。
            m_itemsDirty.store(true, std::memory_order_release);
        });
    m_audioMutationSubId =
        eventBus.subscribe<Event::AudioResourceMutationResultEvent>(
            [this](const Event::AudioResourceMutationResultEvent& event) {
                // 失败事件没有改变资源集合，因此无需丢弃现有方块。
                if ( event.m_success ) {
                    m_itemsDirty.store(true, std::memory_order_release);
                }
            });
}

/// @brief 解除项目音频工具注册的事件订阅。
///
/// 零值表示订阅未建立，析构时只解除实际有效的回调。
/// 解除后事件总线不再持有捕获 this 的闭包。
/// 析构不主动保存工作区，所有合法交互结束路径已经负责持久化。
/// 预览音频实例由公共预览池管理，不属于本视图析构职责。
ProjectAudioToolView::~ProjectAudioToolView()
{
    auto& eventBus = Event::EventBus::instance();
    if ( m_projectSavedSubId != 0 ) {
        // 保存事件与音频变更事件使用不同类型，分别调用对应模板入口。
        eventBus.unsubscribe<Event::ProjectSavedEvent>(m_projectSavedSubId);
    }
    if ( m_audioMutationSubId != 0 ) {
        eventBus.unsubscribe<Event::AudioResourceMutationResultEvent>(
            m_audioMutationSubId);
    }
}

/// @brief 按翻译器版本刷新窗口、弹窗和状态文本缓存。
///
/// 稳定 ImGui ID 只附加到窗口与弹窗标题；普通按钮和提示保存纯显示文本。
/// 先记录读取版本，填充结束后再次比较版本，避免热重载中途生成混合语言缓存。
/// @warning UI 热路径：版本未变化时立即返回，不得每帧重新分配全部字符串。
///
/// 缓存有效位只有在刷新开始和结束版本一致时成立。
/// 若翻译器在复制期间变化，下一帧会重新生成全部相关字段。
void ProjectAudioToolView::refreshTranslationCache()
{
    // 活动翻译器由皮肤系统管理，本函数只在调用期间借用引用。
    auto&          translator = Translation::getActiveTranslator();
    const uint32_t version    = translator.getVersion();
    if ( m_translationCache.valid && m_translationCache.version == version ) {
        // 有效缓存命中版本后无需执行任何哈希查找和字符串复制。
        return;
    }

    // 集中封装键哈希与回退文本参数，保持后续字段赋值简洁一致。
    const auto translate = [&translator](const char* key) {
        return translator.translate(Hash::hashString(key), key);
    };
    // 窗口可见标题随语言变化，### 后的身份在停靠布局中保持稳定。
    assignImGuiLabel(m_translationCache.windowTitle,
                     translate("title.project_audio_tool"),
                     "###ProjectAudioTool");
    // 重命名弹窗同样使用稳定 ID，语言切换不会创建新的弹窗实例。
    assignImGuiLabel(m_translationCache.renamePopupTitle,
                     translate("ui.file_manager.rename_title"),
                     "###ProjectAudioToolRenamePopup");
    // 下列字段均为当前帧直接显示的纯文本，不需要额外隐藏 ID。
    m_translationCache.renameLabel =
        translate("ui.file_manager.rename_label").data();
    m_translationCache.renameAction =
        translate("ui.file_manager.context.rename").data();
    m_translationCache.cancelAction = translate("ui.common.cancel").data();
    m_translationCache.noProject =
        translate("ui.project_audio_tool.no_project").data();
    m_translationCache.hint = translate("ui.project_audio_tool.hint").data();
    m_translationCache.previewEffectOnSelection =
        translate("ui.project_audio_tool.preview_effect_on_selection").data();
    m_translationCache.searchHint =
        translate("ui.project_audio_tool.search_hint").data();
    m_translationCache.noSearchResults =
        translate("ui.search.no_results").data();
    m_translationCache.searchResults =
        translate("ui.project_audio_tool.search_results").data();
    m_translationCache.statusNone =
        translate("ui.project_audio_tool.status_none").data();
    m_translationCache.statusSelected =
        translate("ui.project_audio_tool.status_selected").data();
    m_translationCache.statusBatchSelected =
        translate("ui.project_audio_tool.status_batch_selected").data();
    // 版本最后写入，并仅在翻译期间没有再次变更时标记有效。
    m_translationCache.version = version;
    m_translationCache.valid   = translator.getVersion() == version;
}

/// @brief 请求项目音频工具窗口在下一帧获得焦点。
///
/// 使用边沿标志避免在调用方尚未开始窗口时直接调用 ImGui 焦点 API。
void ProjectAudioToolView::requestFocus()
{
    m_requestFocus = true;
}

/// @brief 从当前项目资源和工作区设置重建音频方块缓存。
/// @param visibleWidth 未应用画布缩放的可见逻辑宽度。
/// @param dpiScale 当前窗口内容缩放。
///
/// 已保存方块按资源 ID 恢复位置、尺寸与层级；新资源使用类型相关默认尺寸排布。
/// 同一项目内重建会保留批量选择，项目切换则清空搜索和跨项目交互状态。
/// 无项目时完整释放所有与旧项目关联的 ID、路径、拖动约束和弹窗状态。
/// 重建结束后按保存层级稳定排序并压实 zOrder，使数组顺序等同于绘制层级。
/// @warning 低频缓存路径：会遍历项目资源并分配容器，只能由脏标记触发。
///
/// visibleWidth 只影响尚无保存位置的 Effect 自动换行。
/// 已有 placement 的位置不会因窗口变窄而自动改写。
/// 非有限保存值视为损坏输入并回退安全默认值。
void ProjectAudioToolView::rebuildItems(float visibleWidth, float dpiScale)
{
    // 项目对象只在当前 UI 线程调用期间借用，不保存裸指针到成员。
    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project ) {
        // 所有方块及派生可见区都与旧项目资源绑定，必须整体清空。
        m_items.clear();
        m_interactionBaseLabelRects.clear();
        m_batchDragEntries.clear();
        m_batchDragUnionCells.clear();
        m_marqueeBaseSelection.clear();
        // 搜索、重命名和选择 ID 不得跨越项目关闭边界。
        m_searchBuffer.fill('\0');
        m_searchResults.clear();
        m_searchFocusRequestId.clear();
        m_renameAudioResourceId.clear();
        m_renameBuffer.fill('\0');
        m_selectedAudioResourceId.clear();
        m_selectedAudioLabel.clear();
        m_openVolumeEditorResourceId.clear();
        // 清除缓存键可保证下一次打开项目一定执行完整初始化。
        m_cachedProjectRoot.clear();
        m_cachedDpiScale = 0.0F;
        // 视角状态恢复默认，避免新项目继承旧项目滚动与缩放。
        m_canvasZoom = 1.0F;
        m_pendingCanvasScroll.reset();
        // 指针交互可能因项目关闭中断，所有进行中状态都直接取消。
        m_batchDragging      = false;
        m_marqueeSelecting   = false;
        m_searchResultsDirty = true;
        return;
    }

    // 规范化后的 UTF-8 根路径作为项目身份缓存键。
    const std::string projectRoot = Config::pathToUtf8(project->m_projectRoot);
    const bool        projectChanged = projectRoot != m_cachedProjectRoot;
    // 同一项目的资源刷新保留仍存在的批量选择集合。
    std::unordered_set<std::string> batchSelectedResourceIds;
    if ( !projectChanged ) {
        for ( const auto& item : m_items ) {
            if ( item.batchSelected ) {
                // 使用稳定资源 ID 而非旧数组索引，允许资源增删和重排。
                batchSelectedResourceIds.insert(item.audioResourceId);
            }
        }
    } else {
        // 搜索焦点请求不能跨项目解析到同名但语义不同的资源。
        m_searchBuffer.fill('\0');
        m_searchFocusRequestId.clear();
    }

    // 重建会使索引变化，所有保存索引的即时交互状态必须作废。
    m_draggingItem.reset();
    m_resizingItem.reset();
    m_batchDragging    = false;
    m_marqueeSelecting = false;
    // 批量拖动快照和可见性约束均基于旧矩形，不可继续复用。
    m_batchDragEntries.clear();
    m_batchDragUnionCells.clear();
    m_marqueeBaseSelection.clear();
    // 音量弹窗锚定旧方块屏幕位置，重建后让用户重新打开。
    m_openVolumeEditorResourceId.clear();
    m_resizeHandle = ResizeHandle::None;
    m_snapLocks    = {};

    // 更新缓存身份与 DPI，后续帧仅在它们变化时再次重建。
    m_cachedProjectRoot = projectRoot;
    m_cachedDpiScale    = dpiScale;
    // 工作区持久化选择、画笔音量和方块布局，是本视图的项目级状态源。
    auto& workspace           = project->m_settings.m_workspace;
    m_selectedAudioResourceId = workspace.m_projectAudioToolSelectedResourceId;
    // 非有限持久化值按默认音量恢复，有限负值夹到允许下限。
    m_brushAudioVolume =
        std::isfinite(workspace.m_projectAudioToolBrushVolume)
            ? std::max(0.0F, workspace.m_projectAudioToolBrushVolume)
            : 1.0F;

    // 将线性保存列表索引为 ID 映射，避免每个资源重复遍历 placements。
    std::unordered_map<std::string, ProjectAudioToolItemPlacement>
        savedPlacements;
    savedPlacements.reserve(workspace.m_projectAudioToolPlacements.size());
    for ( const auto& placement : workspace.m_projectAudioToolPlacements ) {
        if ( !placement.m_audioResourceId.empty() ) {
            // 重复 ID 以后出现的记录覆盖旧值，保持恢复结果确定。
            savedPlacements.insert_or_assign(placement.m_audioResourceId,
                                             placement);
        }
    }

    // 资源集合将按项目当前顺序重新生成，派生标签可见区稍后统一计算。
    m_items.clear();
    m_interactionBaseLabelRects.clear();
    m_items.reserve(project->m_audioResources.size());
    // 默认布局使用简单行流；Main 资源独占一行，Effect 资源可横向换行。
    float defaultCursorX = CANVAS_PADDING;
    float defaultCursorY = CANVAS_PADDING;
    float defaultRowHeight{ 0.0F };
    // 未保存资源从当前最大层级顺序继续编号。
    std::int32_t nextZOrder = 0;
    for ( const auto& resource : project->m_audioResources ) {
        // 每个方块只保存 UI 所需的资源 ID、标签、类型和预览池键。
        Item item;
        item.audioResourceId = resource.m_id;
        // 预览键加入工具前缀，避免与其他窗口的同资源预览实例冲突。
        item.previewPoolKey =
            makeProjectAudioPreviewPoolKey("tool/" + resource.m_id);
        item.label = audioResourceLabel(resource);
        item.type  = resource.m_type;
        // 仅同项目重建时，仍存在的 ID 才能恢复批量选择。
        item.batchSelected = batchSelectedResourceIds.contains(resource.m_id);
        item.rect.width    = defaultItemWidth(item.label, item.type, dpiScale);
        // 默认高度同时满足类型形态和当前主题试听控件的最低需求。
        const float defaultHeight = resource.m_type == AudioTrackType::Main
                                        ? DEFAULT_MAIN_HEIGHT
                                        : DEFAULT_EFFECT_SIZE;
        item.rect.height = std::max(defaultHeight, minimumItemHeight(dpiScale));

        // 保存记录存在时先恢复合法自定义尺寸，再处理位置与层级。
        const auto saved = savedPlacements.find(resource.m_id);
        if ( saved != savedPlacements.end() ) {
            // 零值表示使用自动尺寸；非有限或负值也按未自定义处理。
            const bool hasSavedWidth  = std::isfinite(saved->second.m_width) &&
                                        saved->second.m_width > 0.0F;
            const bool hasSavedHeight = std::isfinite(saved->second.m_height) &&
                                        saved->second.m_height > 0.0F;
            // 自定义标志决定后续保存时是否写回真实尺寸或零占位。
            item.widthCustomized  = hasSavedWidth;
            item.heightCustomized = hasSavedHeight;
            if ( hasSavedWidth ) {
                // 主题或 DPI 改变后，保存宽度仍不得低于新的控件下限。
                item.rect.width =
                    std::max(minimumItemWidth(item.type, dpiScale),
                             saved->second.m_width);
            }
            if ( hasSavedHeight ) {
                // 高度同样对当前主题重新施加最低约束。
                item.rect.height = std::max(minimumItemHeight(dpiScale),
                                            saved->second.m_height);
            }
        }
        if ( saved != savedPlacements.end() &&
             std::isfinite(saved->second.m_x) &&
             std::isfinite(saved->second.m_y) ) {
            // 位置两轴都有效才整体恢复，避免产生半恢复方块。
            item.rect.x = std::max(0.0F, saved->second.m_x);
            item.rect.y = std::max(0.0F, saved->second.m_y);
            item.zOrder = saved->second.m_zOrder;
        } else if ( resource.m_type == AudioTrackType::Main ) {
            // Main 开始前结束未完成的 Effect 行，并从左边留白重新起行。
            if ( defaultCursorX > CANVAS_PADDING ) {
                defaultCursorY += defaultRowHeight + ITEM_GAP;
                defaultCursorX   = CANVAS_PADDING;
                defaultRowHeight = 0.0F;
            }
            item.rect.x = defaultCursorX;
            item.rect.y = defaultCursorY;
            item.zOrder = nextZOrder;
            // Main 独占整行，下一项直接进入其下方新行。
            defaultCursorY += item.rect.height + ITEM_GAP;
        } else {
            // Effect 按可见逻辑宽度横向排列，空间不足时换行。
            const float defaultRight =
                std::max(CANVAS_PADDING + item.rect.width,
                         visibleWidth - CANVAS_PADDING);
            if ( defaultCursorX > CANVAS_PADDING &&
                 defaultCursorX + item.rect.width > defaultRight ) {
                // 当前行已有内容且新方块越界时，按本行最高方块推进 Y。
                defaultCursorY += defaultRowHeight + ITEM_GAP;
                defaultCursorX   = CANVAS_PADDING;
                defaultRowHeight = 0.0F;
            }
            item.rect.x = defaultCursorX;
            item.rect.y = defaultCursorY;
            item.zOrder = nextZOrder;
            // 更新下一项 X 起点和本行最大高度。
            defaultCursorX += item.rect.width + ITEM_GAP;
            defaultRowHeight = std::max(defaultRowHeight, item.rect.height);
        }
        // 保存层级可能稀疏，下一默认层级始终高于已见最大值。
        nextZOrder = std::max(nextZOrder, item.zOrder + 1);
        m_items.push_back(std::move(item));
    }

    // 数组由底到顶排序；相同保存层级使用资源 ID 提供确定性顺序。
    std::ranges::sort(m_items, [](const Item& lhs, const Item& rhs) {
        if ( lhs.zOrder != rhs.zOrder ) return lhs.zOrder < rhs.zOrder;
        return lhs.audioResourceId < rhs.audioResourceId;
    });
    // 压实层级后，数组索引可直接作为当前 Z 序使用。
    for ( std::size_t index = 0; index < m_items.size(); ++index ) {
        m_items[index].zOrder = static_cast<std::int32_t>(index);
    }

    // 资源删除后不能保留指向不存在 ID 的活动画笔状态。
    const bool selectedStillExists =
        std::ranges::any_of(m_items, [this](const Item& item) {
            return item.audioResourceId == m_selectedAudioResourceId;
        });
    if ( !selectedStillExists ) {
        // 同步清除视图显示和项目工作区中的选择。
        m_selectedAudioResourceId.clear();
        m_selectedAudioLabel.clear();
        workspace.m_projectAudioToolSelectedResourceId.clear();
    } else {
        // 仍存在的选择更新标签和类型，以反映重命名或类型变更。
        const auto selected =
            std::ranges::find_if(m_items, [this](const Item& item) {
                return item.audioResourceId == m_selectedAudioResourceId;
            });
        if ( selected != m_items.end() ) {
            m_selectedAudioLabel     = selected->label;
            m_selectedAudioTrackType = selected->type;
        }
    }
    // 所有矩形和层级确定后再计算被上层遮挡的最大标签区域。
    rebuildLabelRects();
    // 标签或资源 ID 可能变化，搜索结果必须在需要时重新评分。
    m_searchResultsDirty = true;
}

/// @brief 按当前 Z 序重建每个方块可用于绘制标签的最大可见矩形。
///
/// 数组后方方块位于更高层，因此只把当前索引之后且实际相交的矩形作为遮挡物。
/// 结果供静止状态绘制使用；拖动和缩放期间由交互增量路径维护。
/// @warning 低频几何重建路径：包含成对相交检查，不得每帧无条件调用。
///
/// largestVisibleCell 的结果是单一矩形，便于标签裁剪和滚动文本绘制。
/// 完全遮挡时的退化处理由布局模块统一定义。
void ProjectAudioToolView::rebuildLabelRects()
{
    // 每个底层方块独立收集其上方实际相交的遮挡矩形。
    for ( std::size_t index = 0; index < m_items.size(); ++index ) {
        std::vector<ProjectAudioToolLayout::Rect> occluders;
        for ( std::size_t higher = index + 1; higher < m_items.size();
              ++higher ) {
            if ( ProjectAudioToolLayout::intersection(m_items[index].rect,
                                                      m_items[higher].rect) ) {
                // 不相交方块不参与可见区域分割，减少布局 helper 输入规模。
                occluders.push_back(m_items[higher].rect);
            }
        }
        // 从剩余可见单元中选择面积最大者，尽量保留完整文件名空间。
        m_items[index].labelRect = ProjectAudioToolLayout::largestVisibleCell(
            m_items[index].rect, occluders);
    }
}

/// @brief 为单个或批量移动交互准备固定方块的基础标签可见区。
///
/// 拖动约束已预计算固定方块在其他固定遮挡物下的可见单元，本函数选取最大单元。
/// 正在移动的方块暂以完整矩形作为标签区，随后由增量刷新处理彼此遮挡。
/// @warning 交互开始路径：允许线性遍历，不得在鼠标移动的每帧重复完整准备。
void ProjectAudioToolView::prepareInteractionLabelRects()
{
    // 缓存长度始终与方块数组一致，索引可在拖动期间直接对应。
    m_interactionBaseLabelRects.resize(m_items.size());
    // 批量移动以选择标志判断，单项交互使用当前活动索引。
    const auto isMovingItem = [this](std::size_t index) {
        if ( m_batchDragging ) return m_items[index].batchSelected;
        const auto activeItem =
            m_draggingItem ? m_draggingItem : m_resizingItem;
        return activeItem && *activeItem == index;
    };

    // 约束数组只包含固定项，因此使用独立索引而非方块索引。
    std::size_t fixedConstraintIndex = 0;
    for ( std::size_t index = 0; index < m_items.size(); ++index ) {
        if ( isMovingItem(index) ) {
            // 移动项位置持续变化，基础区使用自身完整矩形。
            m_interactionBaseLabelRects[index] = m_items[index].rect;
            continue;
        }

        // 固定项从预计算可见单元中选取最大面积矩形。
        ProjectAudioToolLayout::Rect visibleRect{};
        if ( fixedConstraintIndex < m_dragVisibilityConstraints.size() ) {
            const auto& fixedVisibleCells =
                m_dragVisibilityConstraints[fixedConstraintIndex]
                    .fixedVisibleCells;
            for ( const auto& cell : fixedVisibleCells ) {
                // 只需保留当前最大单元，不复制完整单元列表。
                if ( ProjectAudioToolLayout::area(cell) >
                     ProjectAudioToolLayout::area(visibleRect) ) {
                    visibleRect = cell;
                }
            }
        }
        if ( ProjectAudioToolLayout::area(visibleRect) <= 0.0F ) {
            // 没有有效约束结果时回退方块矩形，避免标签区退化为空。
            visibleRect = m_items[index].rect;
        }
        m_interactionBaseLabelRects[index] = visibleRect;
        ++fixedConstraintIndex;
    }
    // 基础区就绪后立即叠加当前移动项造成的动态遮挡。
    refreshInteractionLabelRects();
}

/// @brief 根据当前移动方块位置增量刷新所有标签可见区。
///
/// 固定项从交互开始时的基础区出发，仅扣除位于其上方的活动方块遮挡。
/// 批量拖动按移动项层级依次处理，单项拖动只需应用一个遮挡矩形。
/// @warning 拖动热路径：每次鼠标移动调用，必须复用已准备约束并避免文件访问。
///
/// 基础矩形与活动项当前矩形分离，固定遮挡计算不会随指针移动重做。
/// 输出直接写回 Item::labelRect，不产生额外结果容器。
void ProjectAudioToolView::refreshInteractionLabelRects()
{
    // 尺寸不一致说明交互准备已失效，保留现有标签区等待完整重建。
    if ( m_interactionBaseLabelRects.size() != m_items.size() ) return;

    // 拖动与缩放互斥，共用单个活动索引判断。
    const auto activeItem = m_draggingItem ? m_draggingItem : m_resizingItem;
    for ( std::size_t index = 0; index < m_items.size(); ++index ) {
        // 移动项自身标签使用完整当前矩形，固定项从缓存基础区开始。
        const bool movingItem = m_batchDragging
                                    ? m_items[index].batchSelected
                                    : activeItem && *activeItem == index;
        auto visibleRect      = movingItem ? m_items[index].rect
                                           : m_interactionBaseLabelRects[index];

        if ( m_batchDragging ) {
            // 只有层级高于当前固定项的移动方块才会遮挡其标签。
            for ( const auto& entry : m_batchDragEntries ) {
                if ( entry.itemIndex <= index ||
                     entry.itemIndex >= m_items.size() ) {
                    continue;
                }
                // 每次仅扣除一个新遮挡物，复用增量布局 helper。
                visibleRect =
                    ProjectAudioToolLayout::largestVisibleCellWithOneOccluder(
                        visibleRect, m_items[entry.itemIndex].rect);
            }
        } else if ( activeItem && *activeItem > index &&
                    *activeItem < m_items.size() ) {
            // 单项活动方块处于更高层时才影响当前底层方块。
            visibleRect =
                ProjectAudioToolLayout::largestVisibleCellWithOneOccluder(
                    visibleRect, m_items[*activeItem].rect);
        }
        // 将本帧结果直接用于随后 drawItem 的裁剪区域。
        m_items[index].labelRect = visibleRect;
    }
}

/// @brief 将当前选择、画笔音量和方块布局写回项目工作区并保存。
///
/// 自定义尺寸才保存真实宽高；自动尺寸写零，使下次加载可适配新的字体和主题。
/// 方块数组顺序已经等同 Z 序，仍显式保存 zOrder 以支持稳定项目序列化。
/// @warning 低频提交路径：会触发项目保存，只能在交互结束或明确设置变化后调用。
///
/// 该函数保存的是最终稳定布局，不应在鼠标仍按下时调用。
/// 保存失败反馈由 EditorEngine 统一处理，视图不自行重试。
void ProjectAudioToolView::persistWorkspace()
{
    // 项目可能在交互期间关闭，缺失时安全丢弃本次持久化请求。
    auto& engine  = Logic::EditorEngine::instance();
    auto* project = engine.getCurrentProject();
    if ( !project ) return;

    // 先同步轻量选择与画笔状态，再整体重建方块 placement 列表。
    auto& workspace = project->m_settings.m_workspace;
    workspace.m_projectAudioToolSelectedResourceId = m_selectedAudioResourceId;
    workspace.m_projectAudioToolBrushVolume        = m_brushAudioVolume;
    // placement 与当前资源方块一一对应，旧记录不应残留已删除资源。
    workspace.m_projectAudioToolPlacements.clear();
    workspace.m_projectAudioToolPlacements.reserve(m_items.size());
    for ( const auto& item : m_items ) {
        // 资源 ID 是布局记录与项目资源重新关联的稳定键。
        workspace.m_projectAudioToolPlacements.push_back(
            ProjectAudioToolItemPlacement{
                .m_audioResourceId = item.audioResourceId,
                .m_x               = item.rect.x,
                .m_y               = item.rect.y,
                // 零尺寸标记“使用自动默认值”，不是实际退化矩形。
                .m_width  = item.widthCustomized ? item.rect.width : 0.0F,
                .m_height = item.heightCustomized ? item.rect.height : 0.0F,
                .m_zOrder = item.zOrder,
            });
    }
    // 所有字段更新完成后一次保存，避免产生部分工作区快照。
    engine.saveProject();
}

/// @brief 激活指定方块并把它提升到最上层。
/// @param itemIndex 当前方块数组索引。
/// @return 重排后的活动索引；输入无效或结果为空时返回空。
///
/// 选中项移动到数组末尾，随后压实所有 zOrder，并同步当前画笔音频资源。
/// 返回的新索引必须由调用方使用，原索引在容器 erase 后不再代表同一元素。
///
/// 激活总会重排 Z 序，即使该项已经位于数组末尾也保持结果语义一致。
/// 方块标签和音轨类型同步到状态栏缓存，避免后续重复查找。
std::optional<std::size_t> ProjectAudioToolView::activateItem(
    std::size_t itemIndex)
{
    // 防御由旧命中结果或项目刷新造成的越界索引。
    if ( itemIndex >= m_items.size() ) return std::nullopt;

    // 先移出值再擦除原位置，避免持有会失效的引用。
    Item selected = std::move(m_items[itemIndex]);
    m_items.erase(m_items.begin() + static_cast<std::ptrdiff_t>(itemIndex));
    m_items.push_back(std::move(selected));
    // 数组末尾表示最高层，所有层级重新编号为连续值。
    for ( std::size_t index = 0; index < m_items.size(); ++index ) {
        m_items[index].zOrder = static_cast<std::int32_t>(index);
    }

    // 正常情况下刚追加后不会为空，该检查保留容器状态防御。
    if ( m_items.empty() ) return std::nullopt;
    const std::size_t activeIndex = m_items.size() - 1;
    auto&             item        = m_items[activeIndex];
    // 提升到最高层后活动项暂时不受其他方块遮挡。
    item.labelRect            = item.rect;
    m_selectedAudioResourceId = item.audioResourceId;
    m_selectedAudioLabel      = item.label;
    m_selectedAudioTrackType  = item.type;
    // 通过逻辑命令同步画布放置工具使用的音频 ID、类型和音量。
    Logic::EditorEngine::instance().pushCommand(
        Logic::LogicCommand(Logic::CmdSetBrushAudioResource{
            item.audioResourceId,
            item.type,
            m_brushAudioVolume,
        }));
    return activeIndex;
}

/// @brief 按项目选项试听一次新选中的 Effect 音频资源。
/// @param item 本次完成选择的资源方块。
/// @warning 低频用户操作路径：首次试听可能同步加载音频，只允许在明确的
/// 选择动作完成后调用。
///
/// Main 轨道不在选择时自动试听，避免长音乐被频繁重新触发。
/// 工作区选项按项目保存；关闭时本函数不创建或加载预览实例。
void ProjectAudioToolView::previewEffectSelection(const Item& item) const
{
    // 类型是最便宜的前置判断，Main 资源直接退出。
    if ( item.type != AudioTrackType::Effect ) return;

    // 当前项目与项目级开关必须同时有效。
    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project || !project->m_settings.m_workspace
                          .m_projectAudioToolPreviewEffectOnSelection ) {
        return;
    }

    // 使用方块预先生成的独立池键，并沿用当前画笔音量试听。
    (void)controlProjectAudioPreview(*project,
                                     item.audioResourceId,
                                     item.previewPoolKey,
                                     ProjectAudioPreviewAction::Play,
                                     m_brushAudioVolume);
}

/// @brief 清除当前活动音频方块并同步空画笔资源。
///
/// 批量选择不由此函数修改，调用方可独立管理框选集合。
/// 音量保留不变，使用户下次选择资源时继续使用当前画笔音量。
///
/// 状态栏在同一帧即可显示空选择，逻辑命令随后由引擎队列消费。
void ProjectAudioToolView::clearActiveItem()
{
    // 先清除视图显示状态，再提交逻辑命令。
    m_selectedAudioResourceId.clear();
    m_selectedAudioLabel.clear();
    m_selectedAudioTrackType = AudioTrackType::Effect;
    // 空 ID 明确表示画笔不绑定音频资源，类型恢复 Effect 默认值。
    Logic::EditorEngine::instance().pushCommand(
        Logic::LogicCommand(Logic::CmdSetBrushAudioResource{
            {},
            AudioTrackType::Effect,
            m_brushAudioVolume,
        }));
}

/// @brief 开始单个方块拖动并准备吸附与可见性约束。
/// @param itemIndex 命中的当前数组索引。
/// @param mousePosition 按下时的逻辑画布坐标。
///
/// 方块会先提升到最高层，因此后续状态必须使用 activateItem 返回的新索引。
/// 指针到方块左上角的偏移在整个拖动期间保持不变，避免按下后位置跳变。
///
/// 单击与拖动共用起始状态，只有超过 ImGui 拖动阈值才实际改变矩形。
/// 未移动的松开路径可触发 Effect 选择试听。
void ProjectAudioToolView::beginItemDrag(std::size_t itemIndex,
                                         ImVec2      mousePosition)
{
    // 无效索引不能进入持有活动状态的拖动流程。
    const auto activeIndex = activateItem(itemIndex);
    if ( !activeIndex ) return;

    m_draggingItem = *activeIndex;
    // 起点用于区分单击试听与超过阈值的真实拖动。
    m_itemDragStartMouse = mousePosition;
    m_itemDragMoved      = false;
    const auto& item     = m_items[*activeIndex];
    // 保存指针在方块内部的相对位置，后续用其反推候选左上角。
    m_dragOffset = {
        mousePosition.x - item.rect.x,
        mousePosition.y - item.rect.y,
    };
    // 新交互不能继承上一次拖动锁定的参考线。
    m_snapLocks = {};
    // 完整约束只在交互开始构建，移动帧复用缓存。
    rebuildInteractionConstraints();
    prepareInteractionLabelRects();
}

/// @brief 开始从指定控制柄缩放单个方块。
/// @param itemIndex 命中的当前数组索引。
/// @param handle 本次拖动控制的边或角。
/// @param mousePosition 按下时的逻辑画布坐标。
///
/// 保存初始矩形和指针到活动边的偏移，缩放帧始终相对初始状态计算。
/// 方块先提升到最高层，避免缩放控制柄被其他方块遮挡。
///
/// 角控制柄分别保存两个轴向偏移，边控制柄只影响对应单轴。
/// 自定义宽高标志直到尺寸实际变化后才置位。
void ProjectAudioToolView::beginItemResize(std::size_t  itemIndex,
                                           ResizeHandle handle,
                                           ImVec2       mousePosition)
{
    // None 控制柄不代表缩放意图，即使索引有效也直接退出。
    const auto activeIndex = activateItem(itemIndex);
    if ( !activeIndex || handle == ResizeHandle::None ) return;

    m_resizingItem = *activeIndex;
    m_resizeHandle = handle;
    // 初始矩形是固定锚点，避免连续帧增量计算积累误差。
    m_resizeStartRect     = m_items[*activeIndex].rect;
    m_resizePointerOffset = {};
    // 水平活动边保存鼠标相对该边的按下偏移。
    switch ( handle ) {
    case ResizeHandle::Left:
    case ResizeHandle::TopLeft:
    case ResizeHandle::BottomLeft:
        m_resizePointerOffset.x = mousePosition.x - m_resizeStartRect.x;
        break;
    case ResizeHandle::Right:
    case ResizeHandle::TopRight:
    case ResizeHandle::BottomRight:
        m_resizePointerOffset.x = mousePosition.x - m_resizeStartRect.right();
        break;
    default: break;
    }
    // 垂直活动边独立保存偏移，角控制柄会同时命中两个 switch。
    switch ( handle ) {
    case ResizeHandle::Top:
    case ResizeHandle::TopLeft:
    case ResizeHandle::TopRight:
        m_resizePointerOffset.y = mousePosition.y - m_resizeStartRect.y;
        break;
    case ResizeHandle::Bottom:
    case ResizeHandle::BottomLeft:
    case ResizeHandle::BottomRight:
        m_resizePointerOffset.y = mousePosition.y - m_resizeStartRect.bottom();
        break;
    default: break;
    }
    // 缩放吸附从无锁状态开始，并复用单项交互约束结构。
    m_snapLocks = {};
    rebuildInteractionConstraints();
    prepareInteractionLabelRects();
}

/// @brief 清除所有方块的批量选择标记。
///
/// 不修改当前活动资源，也不持久化；调用方决定清除发生在哪个手势阶段。
/// @warning UI 交互路径：线性遍历方块，仅在明确选择状态切换时调用。
void ProjectAudioToolView::clearBatchSelection()
{
    // 方块值对象直接持有选择位，无需维护独立 ID 集合。
    for ( auto& item : m_items ) {
        item.batchSelected = false;
    }
}

/// @brief 统计当前批量选择的方块数量。
/// @return batchSelected 为 true 的方块数。
///
/// 结果用于状态栏和批量拖动预留，不承担选择身份存储。
std::size_t ProjectAudioToolView::batchSelectionCount() const
{
    return static_cast<std::size_t>(std::ranges::count_if(
        m_items, [](const Item& item) { return item.batchSelected; }));
}

/// @brief 根据搜索输入重建按匹配质量排序的资源结果。
///
/// 查询先去除 ASCII 首尾空白，再分别匹配显示标签和稳定资源 ID，取较高分数。
/// 主轨与音效使用不同显示前缀，但前缀不参与匹配评分。
/// 相同分数按显示文本和资源 ID 提供稳定顺序，重建后高亮回到首项。
/// @warning 输入变化路径：会遍历全部方块并排序，不得在输入未改变时每帧调用。
///
/// 搜索算法由 ProjectAudioToolSearch 提供，本视图只组合多个候选字段。
/// 结果不保存方块索引，聚焦时按资源 ID 重新定位以容忍 Z 序变化。
void ProjectAudioToolView::rebuildSearchResults()
{
    // 进入重建即消费脏标记，空查询也形成有效空结果缓存。
    m_searchResultsDirty = false;
    m_searchResults.clear();
    // trim 返回视图，不复制固定搜索缓冲区内容。
    const std::string_view query =
        ProjectAudioToolSearch::trimAsciiWhitespace(m_searchBuffer.data());
    if ( query.empty() ) {
        // 空查询隐藏结果面板，并将键盘高亮恢复到零。
        m_searchHighlightedIndex = 0;
        return;
    }

    // 最坏情况下每个方块都匹配，按此上限一次预留。
    m_searchResults.reserve(m_items.size());
    for ( const auto& item : m_items ) {
        // 用户既可按文件名搜索，也可按内部资源 ID 定位。
        const auto labelScore =
            ProjectAudioToolSearch::scoreCandidate(item.label, query);
        const auto resourceIdScore =
            ProjectAudioToolSearch::scoreCandidate(item.audioResourceId, query);
        // 两个字段都不匹配的方块不进入结果列表。
        if ( !labelScore && !resourceIdScore ) continue;

        // 结果只保存聚焦所需 ID、展示文本和排序分数。
        SearchResult result;
        result.audioResourceId = item.audioResourceId;
        // 类型前缀帮助同名文件在搜索面板中快速辨认。
        result.displayLabel =
            item.type == AudioTrackType::Main ? "[MAIN] " : "[FX] ";
        result.displayLabel.append(item.label);
        // 任一字段高质量匹配即可提升结果，不把两个分数简单累加。
        result.score =
            std::max(labelScore.value_or(std::numeric_limits<int>::min()),
                     resourceIdScore.value_or(std::numeric_limits<int>::min()));
        m_searchResults.push_back(std::move(result));
    }
    // 高分优先；平分时使用稳定文本全序避免输入期间列表抖动。
    std::ranges::sort(m_searchResults,
                      [](const SearchResult& lhs, const SearchResult& rhs) {
                          if ( lhs.score != rhs.score )
                              return lhs.score > rhs.score;
                          if ( lhs.displayLabel != rhs.displayLabel ) {
                              return lhs.displayLabel < rhs.displayLabel;
                          }
                          return lhs.audioResourceId < rhs.audioResourceId;
                      });
    // 新结果集合默认选中最高评分项。
    m_searchHighlightedIndex = 0;
}

/// @brief 排队在画布中激活并居中指定搜索结果。
/// @param audioResourceId 目标稳定资源 ID。
///
/// 实际激活延迟到画布滚动范围计算完成后执行。
void ProjectAudioToolView::requestSearchResultFocus(
    const std::string& audioResourceId)
{
    m_searchFocusRequestId = audioResourceId;
}

/// @brief 为指定方块准备重命名弹窗输入状态。
/// @param itemIndex 当前方块数组索引。
///
/// 固定字符缓冲区会截断过长显示标签并保证末尾空字符，资源身份单独保存为 ID。
/// 弹窗打开和输入聚焦使用独立边沿标记，在下一次 renderRenamePopup 中消费。
void ProjectAudioToolView::requestItemRename(std::size_t itemIndex)
{
    // 旧命中索引可能因资源刷新失效，越界时忽略请求。
    if ( itemIndex >= m_items.size() ) return;
    const auto& item         = m_items[itemIndex];
    m_renameAudioResourceId  = item.audioResourceId;
    m_shouldOpenRenamePopup  = true;
    m_shouldFocusRenameInput = true;
    // 为终止空字符预留一个字节，避免 InputText 读取越界。
    const std::size_t copySize =
        std::min(item.label.size(), m_renameBuffer.size() - 1U);
    // 先清零整个缓冲，短标签覆盖后剩余部分仍保持确定状态。
    m_renameBuffer.fill('\0');
    std::memcpy(m_renameBuffer.data(), item.label.data(), copySize);
    m_renameBuffer[copySize] = '\0';
}

/// @brief 绘制并处理音频资源重命名模态弹窗。
/// @param dpiScale 当前窗口内容缩放。
///
/// Enter 与确认按钮共享提交路径；取消只清除待处理 ID，不产生项目命令。
/// 弹窗标题带稳定 ImGui ID，语言切换不会丢失打开状态。
/// @warning UI 热路径：每帧调用，但仅在弹窗打开时构造输入与按钮。
///
/// 重命名请求提交后由资源变更事件使方块和搜索缓存失效。
/// 输入缓冲大小固定，避免弹窗编辑期间动态扩容。
void ProjectAudioToolView::renderRenamePopup(float dpiScale)
{
    // OpenPopup 只消费一次请求边沿，后续打开状态由 ImGui 维护。
    if ( m_shouldOpenRenamePopup ) {
        FeedbackOpenPopup(m_translationCache.renamePopupTitle.c_str());
        m_shouldOpenRenamePopup = false;
    }

    // 局部 open 允许标题栏关闭按钮结束本帧模态显示。
    bool                           open = true;
    Utils::CenteredModalPopupScope modalScope(dpiScale);
    if ( !modalScope.begin(m_translationCache.renamePopupTitle.c_str(),
                           &open,
                           ImGuiWindowFlags_NoCollapse,
                           { 380.0F * dpiScale, 0.0F }) ) {
        // 弹窗未打开时作用域 helper 已处理必要状态，无需绘制内容。
        return;
    }

    ImGui::TextUnformatted(m_translationCache.renameLabel);
    if ( m_shouldFocusRenameInput ) {
        // 首次出现时把键盘焦点交给输入框并自动选择现有名称。
        ImGui::SetKeyboardFocusHere();
        m_shouldFocusRenameInput = false;
    }
    // InputText 固定写入成员缓冲，EnterReturnsTrue 只报告提交意图。
    const bool enterPressed =
        ImGui::InputText("##ProjectAudioToolRenameInput",
                         m_renameBuffer.data(),
                         m_renameBuffer.size(),
                         ImGuiInputTextFlags_EnterReturnsTrue |
                             ImGuiInputTextFlags_AutoSelectAll);
    // 两个动作使用相同 DPI 后宽度，高度沿用主题默认框高。
    const ImVec2 buttonSize{ 132.0F * dpiScale, 0.0F };
    const bool   confirmClicked =
        FeedbackButton(m_translationCache.renameAction, buttonSize);
    if ( (enterPressed || confirmClicked) &&
         !m_renameAudioResourceId.empty() ) {
        // 命令保存稳定资源 ID 和用户缓冲内容，实际文件操作由逻辑层处理。
        Logic::EditorEngine::instance().pushCommand(
            Logic::LogicCommand(Logic::CmdRenameAudioResource{
                .id          = m_renameAudioResourceId,
                .newFileName = m_renameBuffer.data(),
            }));
        // 提交后清除目标并关闭弹窗，等待资源变更事件触发方块重建。
        m_renameAudioResourceId.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if ( FeedbackButton(m_translationCache.cancelAction, buttonSize) ) {
        // 取消不修改输入缓冲也无妨，下次请求会完整覆盖它。
        m_renameAudioResourceId.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

/// @brief 开始拖动包含命中方块的批量选择集合。
/// @param itemIndex 按下的方块索引。
/// @param mousePosition 按下时的逻辑画布坐标。
///
/// 所有选中方块先稳定移动到数组末尾，使整个批次位于未选中方块之上。
/// 函数保存每个选中方块的初始矩形、总体包围盒和几何并集单元。
/// 拖动帧只平移这些快照并应用预计算的固定方块可见性约束。
/// @warning 交互开始路径：会重排方块并构建几何缓存，不得在拖动帧重复调用。
///
/// 批量选择标志在重排过程中随 Item 值移动，不需额外 ID 映射。
/// 总体包围盒用于吸附，几何并集用于精确可见性保护，两者职责不同。
void ProjectAudioToolView::beginBatchDrag(std::size_t itemIndex,
                                          ImVec2      mousePosition)
{
    // 只有点击已批量选中的有效方块才进入批量拖动。
    if ( itemIndex >= m_items.size() || !m_items[itemIndex].batchSelected ) {
        return;
    }

    // 新容器先收集未选中项，再收集选中项，保持各集合内部原相对顺序。
    std::vector<Item> reorderedItems;
    reorderedItems.reserve(m_items.size());
    for ( auto& item : m_items ) {
        if ( !item.batchSelected ) {
            // 值移动避免复制标签和预览键字符串。
            reorderedItems.push_back(std::move(item));
        }
    }
    for ( auto& item : m_items ) {
        if ( item.batchSelected ) {
            // 选中项追加到末尾后整体获得最高层级。
            reorderedItems.push_back(std::move(item));
        }
    }
    m_items = std::move(reorderedItems);
    // 重排后重新压实 zOrder，使数组顺序继续代表绘制顺序。
    for ( std::size_t index = 0; index < m_items.size(); ++index ) {
        m_items[index].zOrder = static_cast<std::int32_t>(index);
    }

    // 拖动条目保存重排后的索引和固定初始矩形。
    m_batchDragEntries.clear();
    std::vector<ProjectAudioToolLayout::Rect> selectedRects;
    // 并集构建需要所有选择矩形，容量按当前选择数预留。
    selectedRects.reserve(batchSelectionCount());
    bool hasBounds = false;
    for ( std::size_t index = 0; index < m_items.size(); ++index ) {
        auto& item = m_items[index];
        if ( !item.batchSelected ) continue;

        // 初始矩形在拖动期间不变，用统一位移生成每个当前矩形。
        m_batchDragEntries.push_back(BatchDragEntry{
            .itemIndex = index,
            .startRect = item.rect,
        });
        selectedRects.push_back(item.rect);
        // 选中项位于顶层，交互开始时标签可先使用完整方块区域。
        item.labelRect = item.rect;
        if ( !hasBounds ) {
            // 第一个选中项初始化总体包围盒。
            m_batchDragInitialBounds = item.rect;
            hasBounds                = true;
        } else {
            // 后续方块逐轴扩张包围盒，同时保留已有最小坐标。
            const float right =
                std::max(m_batchDragInitialBounds.right(), item.rect.right());
            const float bottom =
                std::max(m_batchDragInitialBounds.bottom(), item.rect.bottom());
            m_batchDragInitialBounds.x =
                std::min(m_batchDragInitialBounds.x, item.rect.x);
            m_batchDragInitialBounds.y =
                std::min(m_batchDragInitialBounds.y, item.rect.y);
            m_batchDragInitialBounds.width = right - m_batchDragInitialBounds.x;
            m_batchDragInitialBounds.height =
                bottom - m_batchDragInitialBounds.y;
        }
    }
    // 防御选择状态在重排期间异常消失，不建立空批次交互。
    if ( !hasBounds ) return;

    // 当前包围盒从初始值开始，后续约束会增量更新它。
    m_batchDragCurrentBounds = m_batchDragInitialBounds;
    // 几何并集用于检查不规则选择整体与固定方块的可见比例。
    m_batchDragUnionCells =
        ProjectAudioToolLayout::buildUnionCells(selectedRects);
    // 保存指针在总体包围盒内的按下偏移，防止拖动开始跳变。
    m_batchDragOffset = {
        mousePosition.x - m_batchDragInitialBounds.x,
        mousePosition.y - m_batchDragInitialBounds.y,
    };
    // 标志置位后再构建仅包含固定项的约束缓存。
    m_batchDragging = true;
    m_snapLocks     = {};
    rebuildBatchDragConstraints();
    prepareInteractionLabelRects();
}

/// @brief 命中测试指针靠近方块的哪条缩放边或哪个角。
/// @param item 待测试方块。
/// @param mousePosition 逻辑画布坐标。
/// @return 角优先于单边的缩放控制柄；未命中返回 None。
///
/// 命中厚度按逻辑单位计算，四角必须优先判断以支持双轴缩放光标。
///
/// 函数只判断到边线的轴向距离，调用方已经保证鼠标命中方块。
ProjectAudioToolView::ResizeHandle ProjectAudioToolView::hitTestResizeHandle(
    const Item& item, ImVec2 mousePosition) const
{
    // 分别计算指针到四条边的轴向距离是否进入热区。
    const bool nearLeft =
        std::abs(mousePosition.x - item.rect.x) <= RESIZE_HIT_THICKNESS;
    const bool nearRight =
        std::abs(mousePosition.x - item.rect.right()) <= RESIZE_HIT_THICKNESS;
    const bool nearTop =
        std::abs(mousePosition.y - item.rect.y) <= RESIZE_HIT_THICKNESS;
    const bool nearBottom =
        std::abs(mousePosition.y - item.rect.bottom()) <= RESIZE_HIT_THICKNESS;
    // 角点组合优先，否则同一位置会被较早的单边判断吞掉。
    if ( nearLeft && nearTop ) return ResizeHandle::TopLeft;
    if ( nearRight && nearTop ) return ResizeHandle::TopRight;
    if ( nearLeft && nearBottom ) return ResizeHandle::BottomLeft;
    if ( nearRight && nearBottom ) return ResizeHandle::BottomRight;
    if ( nearLeft ) return ResizeHandle::Left;
    if ( nearRight ) return ResizeHandle::Right;
    if ( nearTop ) return ResizeHandle::Top;
    if ( nearBottom ) return ResizeHandle::Bottom;
    // 方块内部但不靠近边缘时属于移动而非缩放命中。
    return ResizeHandle::None;
}

/// @brief 为单项拖动或缩放预计算吸附目标与固定方块可见性约束。
///
/// 活动方块已提升到数组末尾，其余方块保持固定并作为吸附目标。
/// 对每个固定方块，只把位于它上方且低于活动项的固定遮挡物纳入基础约束。
/// @warning 交互开始路径：包含嵌套相交检查，鼠标移动期间必须复用结果。
///
/// 吸附目标不包含活动方块自身，避免候选矩形吸附到原位置产生自锁。
/// 约束数组顺序与跳过活动项后的固定方块顺序一致。
/// 活动方块位于最高层，因此不需要考虑它上方的静止遮挡物。
void ProjectAudioToolView::rebuildInteractionConstraints()
{
    // 新交互完整替换旧目标和约束，避免项目或 Z 序变化后混用。
    m_dragSnapTargets.clear();
    m_dragVisibilityConstraints.clear();
    const auto activeItem = m_draggingItem ? m_draggingItem : m_resizingItem;
    // 没有合法活动索引时保持空约束集合。
    if ( !activeItem || *activeItem >= m_items.size() ) return;

    const std::size_t movingIndex = *activeItem;
    // 单项活动时固定项数量最多为总数减一。
    m_dragSnapTargets.reserve(m_items.size() - 1);
    m_dragVisibilityConstraints.reserve(m_items.size() - 1);
    for ( std::size_t baseIndex = 0; baseIndex < m_items.size(); ++baseIndex ) {
        if ( baseIndex == movingIndex ) continue;
        // 每个固定方块矩形均可提供边缘或中心吸附参考线。
        m_dragSnapTargets.push_back(m_items[baseIndex].rect);

        // 固定遮挡只考虑层级高于 base 且低于活动方块的项。
        std::vector<ProjectAudioToolLayout::Rect> fixedOccluders;
        for ( std::size_t higherIndex = baseIndex + 1;
              higherIndex < movingIndex;
              ++higherIndex ) {
            if ( ProjectAudioToolLayout::intersection(
                     m_items[baseIndex].rect, m_items[higherIndex].rect) ) {
                // 不相交项不会分割 base 的可见区域。
                fixedOccluders.push_back(m_items[higherIndex].rect);
            }
        }
        // 预处理结果供后续候选矩形约束直接复用。
        m_dragVisibilityConstraints.push_back(
            ProjectAudioToolLayout::prepareVisibilityConstraint(
                m_items[baseIndex].rect, fixedOccluders));
    }
}

/// @brief 为批量拖动预计算未选中方块的吸附与可见性约束。
///
/// beginBatchDrag 已把选中项移到数组末尾，因此固定项形成从零开始的连续区间。
/// 每个固定项只考虑其他固定项造成的基础遮挡，移动批次在拖动帧动态加入。
/// @warning 交互开始路径：包含嵌套相交检查，不得在每次鼠标移动时重建。
///
/// 固定项数量由总数减去批次条目数得到，依赖 beginBatchDrag 的重排不变量。
/// 选中项彼此相对位置由初始矩形保存，不作为固定可见性约束目标。
/// 约束缓存只在本次批量拖动生命周期内有效。
void ProjectAudioToolView::rebuildBatchDragConstraints()
{
    // 批量集合或状态无效时保留空缓存并直接返回。
    m_dragSnapTargets.clear();
    m_dragVisibilityConstraints.clear();
    if ( !m_batchDragging || m_batchDragEntries.empty() ) return;

    // 重排不变量保证末尾条目数等于批量拖动项数。
    const std::size_t fixedItemCount =
        m_items.size() - m_batchDragEntries.size();
    m_dragSnapTargets.reserve(fixedItemCount);
    m_dragVisibilityConstraints.reserve(fixedItemCount);
    for ( std::size_t baseIndex = 0; baseIndex < fixedItemCount; ++baseIndex ) {
        // 所有固定矩形均可作为整个批次包围盒的吸附目标。
        m_dragSnapTargets.push_back(m_items[baseIndex].rect);

        // 只在固定区间内收集层级更高且相交的遮挡方块。
        std::vector<ProjectAudioToolLayout::Rect> fixedOccluders;
        for ( std::size_t higherIndex = baseIndex + 1;
              higherIndex < fixedItemCount;
              ++higherIndex ) {
            if ( ProjectAudioToolLayout::intersection(
                     m_items[baseIndex].rect, m_items[higherIndex].rect) ) {
                // 相交检查过滤不会影响该固定项标签可见区的方块。
                fixedOccluders.push_back(m_items[higherIndex].rect);
            }
        }
        // 约束顺序与固定项索引一致，拖动算法可直接并行遍历。
        m_dragVisibilityConstraints.push_back(
            ProjectAudioToolLayout::prepareVisibilityConstraint(
                m_items[baseIndex].rect, fixedOccluders));
    }
}

/// @brief 绘制一个可见音频方块的背景、标签、类型和选择装饰。
/// @param item 只读方块状态。
/// @param canvasOrigin 逻辑画布原点对应的屏幕坐标。
/// @param canvasScale DPI 与画布缩放乘积。
/// @param dpiScale 当前窗口内容缩放。
/// @param hovered 指针是否命中该方块。
/// @param pressed 该方块是否处于按下交互状态。
/// @param drawList 目标 ImGui 绘制列表。
///
/// Main 与 Effect 使用固定区分色；悬浮和按下只调整填充亮度，不改变类型语义。
/// 标签严格裁剪到当前最大可见单元，并避开方块底部的试听控件区域。
/// 当前活动项绘制八个缩放柄，批量选择项改用绿色内框避免语义冲突。
/// @warning UI 热路径：每个可见方块每帧调用，只允许提交绘制命令和常数计算。
///
/// 函数不创建 ImGui Item，因此命中和输入必须由 update 单独处理。
/// 试听控件随后覆盖在方块底部，标签区域已提前为其留空。
void ProjectAudioToolView::drawItem(const Item& item, ImVec2 canvasOrigin,
                                    float canvasScale, float dpiScale,
                                    bool hovered, bool pressed,
                                    ImDrawList& drawList) const
{
    // 所有几何先统一转换到屏幕空间，后续 DrawList 不混用逻辑坐标。
    const auto screenRect = toScreenRect(item.rect, canvasOrigin, canvasScale);
    const ImVec2 minimum{ screenRect.x, screenRect.y };
    const ImVec2 maximum{ screenRect.right(), screenRect.bottom() };
    // 活动选择使用稳定资源 ID 判断，不依赖可能变化的数组索引。
    const bool selected = item.audioResourceId == m_selectedAudioResourceId;

    // 类型颜色使用较高不透明度，保证重叠方块仍可辨认边界。
    const auto&  style = ImGui::GetStyle();
    const ImVec4 mainColor{ 0.89F, 0.56F, 0.23F, 0.95F };
    const ImVec4 effectColor{ 0.22F, 0.67F, 0.88F, 0.95F };
    ImVec4 fill = item.type == AudioTrackType::Main ? mainColor : effectColor;
    if ( hovered ) {
        // 悬浮只提升 RGB 并夹到一，保持原 Alpha 不变。
        fill.x = std::min(1.0F, fill.x * 1.13F);
        fill.y = std::min(1.0F, fill.y * 1.13F);
        fill.z = std::min(1.0F, fill.z * 1.13F);
    }
    if ( pressed ) {
        // 按下时整体压暗，提供与拖动或缩放同步的视觉反馈。
        fill.x *= 0.82F;
        fill.y *= 0.82F;
        fill.z *= 0.82F;
    }
    // 填充与外框共享主题圆角，避免边缘出现不一致缝隙。
    const float rounding = style.FrameRounding;
    drawList.AddRectFilled(
        minimum, maximum, ImGui::ColorConvertFloat4ToU32(fill), rounding);
    // 当前活动项使用文本色粗边框，普通项使用主题边框色。
    drawList.AddRect(
        minimum,
        maximum,
        ImGui::GetColorU32(selected ? ImGuiCol_Text : ImGuiCol_Border),
        rounding,
        0,
        selected ? std::max(3.0F * dpiScale, style.FrameBorderSize)
                 : std::max(1.0F, style.FrameBorderSize));
    if ( item.batchSelected ) {
        // 先叠加轻透明填充，使批量选择在不同类型底色上均可见。
        constexpr ImU32 BATCH_SELECTION_FILL = IM_COL32(76, 255, 190, 38);
        drawList.AddRectFilled(
            minimum, maximum, BATCH_SELECTION_FILL, rounding);
    }

    // 标签逻辑矩形已考虑 Z 序遮挡，此处只做屏幕转换。
    const auto labelRect =
        toScreenRect(item.labelRect, canvasOrigin, canvasScale);
    // 固定试听控件占据方块底部，标签区域不得与之重叠。
    const auto audioControlLayout = calculateItemAudioControlLayout(screenRect);
    const float horizontalPadding = std::max(1.0F, style.FramePadding.x);
    const float verticalPadding   = std::max(1.0F, style.FramePadding.y);
    // 只有控件上边位于标签区内部时才缩短标签底边。
    const float labelBottom =
        audioControlLayout.labelBottom > labelRect.y
            ? std::min(labelRect.bottom(), audioControlLayout.labelBottom)
            : labelRect.bottom();
    // 极端遮挡下仍保留最小正高度，避免生成退化裁剪矩形。
    const float labelHeight = std::max(1.0F, labelBottom - labelRect.y);
    const float filenameHeight =
        std::min(ImGui::GetTextLineHeight(), labelHeight);
    // 文件名贴近可用区底部绘制，给顶部类型标识留下空间。
    const ImVec2 labelStart{
        labelRect.x + horizontalPadding,
        std::max(labelRect.y, labelBottom - verticalPadding - filenameHeight),
    };
    // 两侧扣除水平内边距，并夹到至少一个像素。
    const float labelWidth =
        std::max(1.0F, labelRect.width - horizontalPadding * 2.0F);
    // 标签裁剪同时受遮挡可见区和试听控件上边界限制。
    drawList.PushClipRect(
        { labelRect.x, labelRect.y }, { labelRect.right(), labelBottom }, true);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.06F, 0.08F, 0.11F, 1.0F));
    // 超长文件名沿可见宽度滚动，文字颜色固定为深色以匹配亮底。
    Utils::drawScrollingText(
        item.label, labelStart, labelWidth, filenameHeight);
    ImGui::PopStyleColor();
    drawList.PopClipRect();

    // 类型短标签放在左上角，不参与本地化以保持紧凑固定宽度。
    const char* typeLabel = item.type == AudioTrackType::Main ? "MAIN" : "FX";
    drawList.AddText(
        { minimum.x + horizontalPadding, minimum.y + verticalPadding },
        ImGui::GetColorU32(ImVec4(0.06F, 0.08F, 0.11F, 0.78F)),
        typeLabel);

    if ( item.batchSelected ) {
        // 绿色内框与外层活动边框分离，表达多选集合而非主选择。
        constexpr ImU32 BATCH_SELECTION_BORDER = IM_COL32(76, 255, 190, 235);
        // 内缩随 DPI 增长，并避免覆盖方块外框。
        const float inset = std::max(2.0F, 3.0F * dpiScale);
        drawList.AddRect({ minimum.x + inset, minimum.y + inset },
                         { maximum.x - inset, maximum.y - inset },
                         BATCH_SELECTION_BORDER,
                         std::max(0.0F, rounding - inset),
                         0,
                         std::max(2.0F, 2.25F * dpiScale));
    }

    if ( selected && !item.batchSelected ) {
        // 单一活动项才显示缩放柄，批量选择只支持整体平移。
        const float halfHandle = RESIZE_HANDLE_SIZE * dpiScale * 0.5F;
        // 八个中心覆盖四角和四边中点，对应全部 ResizeHandle 枚举。
        const std::array<ImVec2, 8> handleCenters{
            ImVec2{ minimum.x, minimum.y },
            ImVec2{ (minimum.x + maximum.x) * 0.5F, minimum.y },
            ImVec2{ maximum.x, minimum.y },
            ImVec2{ minimum.x, (minimum.y + maximum.y) * 0.5F },
            ImVec2{ maximum.x, (minimum.y + maximum.y) * 0.5F },
            ImVec2{ minimum.x, maximum.y },
            ImVec2{ (minimum.x + maximum.x) * 0.5F, maximum.y },
            ImVec2{ maximum.x, maximum.y },
        };
        for ( const auto center : handleCenters ) {
            // 每个控制点为固定屏幕方块，填充和边框形成高对比轮廓。
            const ImVec2 handleMinimum{ center.x - halfHandle,
                                        center.y - halfHandle };
            const ImVec2 handleMaximum{ center.x + halfHandle,
                                        center.y + halfHandle };
            // 填充沿用文本色，在亮暗主题下均保持可见。
            drawList.AddRectFilled(handleMinimum,
                                   handleMaximum,
                                   ImGui::GetColorU32(ImGuiCol_Text),
                                   std::min(rounding, halfHandle));
            // 窗口背景色细边框将控制点与活动项粗边框分开。
            drawList.AddRect(handleMinimum,
                             handleMaximum,
                             ImGui::GetColorU32(ImGuiCol_WindowBg),
                             std::min(rounding, halfHandle));
        }
    }
}

/// @brief 计算滚动画布覆盖可见区域和全部方块所需的逻辑尺寸。
/// @param visibleWidth 当前可见逻辑宽度。
/// @param visibleHeight 当前可见逻辑高度。
/// @return 不小于可见区域且包含方块末端留白的内容尺寸。
///
/// 左上边界固定为零，只需追踪所有方块右边和下边的最大值。
/// @warning UI 热路径：每帧线性扫描方块，不得加入排序或文件访问。
///
/// 结果不会主动缩小方块位置；滚动夹取由 update 使用返回尺寸完成。
ImVec2 ProjectAudioToolView::calculateContentSize(float visibleWidth,
                                                  float visibleHeight) const
{
    // 空画布至少与视口同大，因此不会出现负滚动范围。
    float right  = visibleWidth;
    float bottom = visibleHeight;
    for ( const auto& item : m_items ) {
        // 每个最远边缘后附加操作留白，允许继续拖动和查看控制柄。
        right  = std::max(right, item.rect.right() + CONTENT_END_PADDING);
        bottom = std::max(bottom, item.rect.bottom() + CONTENT_END_PADDING);
    }
    // 结果仍为逻辑单位，调用方再乘 canvasScale 得到 Dummy 尺寸。
    return { right, bottom };
}

/// @brief 绘制项目音频工具并推进搜索、预览、选择、拖动和缩放交互。
/// @param sourceManager 调用视图的 UI 管理器，用于判断项目切换状态。
///
/// 窗口顶部提供项目选项和可调高度搜索结果，中部子窗口承载可缩放滚动画布，
/// 底部状态栏展示当前资源、批量选择数量和以视口中心为锚点的缩放滑条。
/// 指针坐标统一转换为逻辑画布空间后再执行命中、吸附和可见性约束。
/// 单项拖动、批量拖动、缩放、框选和试听控件互斥，避免同一按键驱动多个状态机。
/// 所有布局写回只发生在交互结束或明确设置变化时，不在连续移动期间保存项目。
/// @warning UI 热路径：每帧执行；不得引入无条件文件访问、全量排序或阻塞等待。
///
/// 搜索结果和方块绘制均使用裁剪或可见性判断限制规模。
/// 事件线程只设置脏标记，所有容器修改集中在本 UI 线程入口。
/// 交互期间的标签可见区使用预计算约束增量更新，松开后才完整重建。
/// 画布内容 Dummy 必须最后提交，以兼容绝对定位控件和 ImGui 滚动边界。
/// 搜索聚焦通过稳定资源 ID 延迟解析，不保存可能因激活而变化的数组索引。
/// 试听音量编辑器同样按资源 ID 跨帧锚定，资源不可见时主动关闭。
/// 鼠标命中按 Z 序从顶向底查找，绘制则按相反顺序从底向顶提交。
/// Ctrl+滚轮以指针为缩放锚点，状态栏滑条以视口中心为锚点。
/// 两种缩放路径最终都夹取滚动范围，防止内容缩小时留下越界空白。
/// 框选支持严格包含和宽松相交两种项目统一选择模式。
/// Ctrl 或 Shift 框选从手势开始快照追加，普通框选先清除旧批量集合。
/// 单项激活与拖动会提升 Z 序，批量拖动则整体提升所有选中方块。
/// 拖动和缩放使用吸附滞回锁，只有仍真实对齐时才绘制参考线。
/// 连续交互只更新内存几何；鼠标松开时统一保存最终工作区状态。
/// 搜索面板高度由用户拖柄状态保存，但始终为画布和状态栏保留最低空间。
/// 画布缩放只改变观察变换，不修改方块持久化的逻辑矩形。
/// DPI 变化会重建自动尺寸下限，但保留合法的自定义尺寸和位置。
/// 资源变更成功后由事件统一标脏，重命名命令不直接修改方块缓存。
/// 右键重命名只在几何手势空闲且指针不位于试听控件时触发。
/// 参考线和框选框均在方块之后绘制，但在流式 Dummy 之前提交。
/// 状态栏文本使用裁剪区保护右侧缩放控件的固定交互面积。
void ProjectAudioToolView::update(UIManager* sourceManager)
{
    // 翻译缓存先于窗口创建刷新，保证标题和弹窗 ID 在本帧一致。
    refreshTranslationCache();
    if ( m_requestFocus ) {
        // 焦点请求只消费一次，避免窗口持续抢占用户正在操作的其他视图。
        ImGui::SetNextWindowFocus();
        m_requestFocus = false;
    }
    // 窗口内容缩放用于屏幕控件尺寸，并与画布缩放共同构成 canvasScale。
    const float dpiScale =
        Config::AppConfig::instance().getWindowContentScale();
    // 默认大小仅在首次使用时应用，后续尊重用户调整和停靠布局。
    ImGui::SetNextWindowSize({ 720.0F * dpiScale, 520.0F * dpiScale },
                             ImGuiCond_FirstUseEver);
    // LayoutContext 管理窗口 Begin/End 生命周期和 m_isOpen 打开状态。
    LayoutContext layoutContext(m_layoutCtx,
                                m_translationCache.windowTitle,
                                true,
                                ImGuiWindowFlags_None,
                                &m_isOpen);

    // 项目切换期间模型指针和资源集合不稳定，只展示统一占位。
    if ( !sourceManager || sourceManager->isProjectTransitionInProgress() ) {
        Utils::renderProjectTransitionPlaceholder();
        return;
    }
    // 当前项目在本帧入口读取一次，后续回调不跨帧保存该指针。
    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project ) {
        // 无项目状态不绘制搜索、画布和项目设置控件。
        ImGui::TextUnformatted(m_translationCache.noProject);
        return;
    }

    // 使用自动换行提示，窄窗口不会横向扩大工具尺寸。
    ImGui::TextWrapped("%s", m_translationCache.hint);

    // 选择 Effect 时是否自动试听属于项目工作区偏好。
    auto& previewEffectOnSelection =
        project->m_settings.m_workspace
            .m_projectAudioToolPreviewEffectOnSelection;
    if ( ::MMM::UI::FeedbackCheckbox(
             m_translationCache.previewEffectOnSelection,
             &previewEffectOnSelection) ) {
        // 设置变化低频发生，可立即保存以保证重新打开项目后恢复。
        Logic::EditorEngine::instance().saveProject();
    }

    // 搜索框占满可用宽度，Enter 提交当前高亮结果。
    ImGui::SetNextItemWidth(-1.0F);
    const bool searchSubmitted =
        ImGui::InputTextWithHint("##ProjectAudioToolSearch",
                                 m_translationCache.searchHint,
                                 m_searchBuffer.data(),
                                 m_searchBuffer.size(),
                                 ImGuiInputTextFlags_EnterReturnsTrue |
                                     ImGuiInputTextFlags_AutoSelectAll);
    // 仅输入框拥有键盘焦点时处理上下方向键，避免抢占画布操作。
    const bool searchInputActive = ImGui::IsItemActive();
    if ( ImGui::IsItemEdited() ) {
        // 文本实际变化才使评分结果失效，获得焦点本身不触发重建。
        m_searchResultsDirty = true;
    }
    if ( m_searchResultsDirty ) {
        // 搜索结果在同一帧输入变化后立即更新，保持键盘反馈连续。
        rebuildSearchResults();
    }

    // 移动标记用于稍后把高亮行滚动到结果面板中央。
    bool searchHighlightMoved = false;
    if ( searchInputActive && !m_searchResults.empty() ) {
        if ( ImGui::IsKeyPressed(ImGuiKey_DownArrow) ) {
            // 下移在末项后循环回首项，结果非空保证取模合法。
            m_searchHighlightedIndex =
                (m_searchHighlightedIndex + 1) % m_searchResults.size();
            searchHighlightMoved = true;
        } else if ( ImGui::IsKeyPressed(ImGuiKey_UpArrow) ) {
            // 加上 size 后再减一，避免无符号索引向下溢出。
            m_searchHighlightedIndex =
                (m_searchHighlightedIndex + m_searchResults.size() - 1) %
                m_searchResults.size();
            searchHighlightMoved = true;
        }
    }
    if ( searchSubmitted && !m_searchResults.empty() ) {
        // Enter 只排队资源 ID，实际画布居中在滚动边界计算后执行。
        requestSearchResultFocus(
            m_searchResults[m_searchHighlightedIndex].audioResourceId);
    }

    // 与评分函数使用相同的去空白查询判断结果面板是否显示。
    const std::string_view searchQuery =
        ProjectAudioToolSearch::trimAsciiWhitespace(m_searchBuffer.data());
    // 预留底部状态栏高度，搜索面板和画布不能占用该区域。
    const float statusHeight =
        ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    if ( !searchQuery.empty() ) {
        // 空查询完全隐藏结果摘要和分隔拖柄，把空间全部交给画布。
        if ( m_searchResults.empty() ) {
            // 无匹配时只显示弱化文本，不创建空滚动子窗口。
            ImGui::TextDisabled("%s", m_translationCache.noSearchResults);
        } else {
            // 先展示结果总数，再根据剩余窗口高度约束结果面板。
            ImGui::TextDisabled("%s: %zu",
                                m_translationCache.searchResults,
                                m_searchResults.size());
            // 单行高度固定，便于列表裁剪器和键盘滚动定位使用同一尺度。
            const ImGuiStyle& style           = ImGui::GetStyle();
            const float       resultRowHeight = ImGui::GetFrameHeight();
            // 分隔拖柄提供比视觉线更高的命中区，适配高 DPI 输入。
            const float splitterHeight =
                std::max(6.0F * dpiScale, style.SeparatorSize * 4.0F);
            // 无论搜索结果多少，至少为画布保留三行控件高度。
            const float minimumCanvasHeight =
                resultRowHeight * 3.0F + style.WindowPadding.y * 2.0F;
            // 保留高度还包括状态栏、拖柄和各区之间的主题间距。
            const float reservedHeight = minimumCanvasHeight + statusHeight +
                                         splitterHeight +
                                         style.ItemSpacing.y * 3.0F;
            const float searchLayoutAvailableHeight =
                ImGui::GetContentRegionAvail().y;
            // 纯搜索布局 helper 将用户高度夹在内容和可用窗口范围内。
            const float resultListHeight =
                ProjectAudioToolSearch::calculateResultPaneHeight(
                    m_searchResultPaneHeight,
                    resultRowHeight,
                    m_searchResults.size(),
                    style.WindowPadding.y,
                    searchLayoutAvailableHeight,
                    reservedHeight);
            // 结果列表拥有独立纵向滚动，不影响主画布滚动位置。
            ImGui::BeginChild("ProjectAudioToolSearchResults",
                              { 0.0F, resultListHeight },
                              true);
            if ( searchHighlightMoved ) {
                // 键盘移动后将目标行中心对齐面板中心，并夹到非负滚动。
                const float targetScroll =
                    (static_cast<float>(m_searchHighlightedIndex) + 0.5F) *
                        resultRowHeight -
                    resultListHeight * 0.5F;
                ImGui::SetScrollY(std::max(0.0F, targetScroll));
            }

            // 长结果集合只提交当前可见行，行高显式传给裁剪器。
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(m_searchResults.size()),
                          resultRowHeight);
            while ( clipper.Step() ) {
                for ( int resultIndex = clipper.DisplayStart;
                      resultIndex < clipper.DisplayEnd;
                      ++resultIndex ) {
                    // 结果索引在本轮重建后稳定，转换为 size_t 访问缓存。
                    auto& result =
                        m_searchResults[static_cast<std::size_t>(resultIndex)];
                    // 稳定资源 ID 区分可能具有相同显示文件名的结果。
                    ImGui::PushID(result.audioResourceId.c_str());
                    const bool clicked = FeedbackSelectable(
                        result.displayLabel.c_str(),
                        static_cast<std::size_t>(resultIndex) ==
                            m_searchHighlightedIndex,
                        ImGuiSelectableFlags_None,
                        { 0.0F, resultRowHeight });
                    if ( ImGui::IsItemHovered() ) {
                        // 鼠标悬浮同步键盘高亮，Enter 将提交该行。
                        m_searchHighlightedIndex =
                            static_cast<std::size_t>(resultIndex);
                    }
                    if ( clicked ) {
                        // 点击既更新高亮也排队画布聚焦，不在列表内直接滚动画布。
                        m_searchHighlightedIndex =
                            static_cast<std::size_t>(resultIndex);
                        requestSearchResultFocus(result.audioResourceId);
                    }
                    ImGui::PopID();
                }
            }
            // 完成结果行后关闭独立子窗口，后续拖柄回到父窗口坐标系。
            ImGui::EndChild();

            // 不可见按钮提供整条分隔拖柄的稳定命中区域。
            const ImVec2 splitterStart = ImGui::GetCursorScreenPos();
            const float  splitterWidth =
                std::max(1.0F, ImGui::GetContentRegionAvail().x);
            ImGui::InvisibleButton("##ProjectAudioToolSearchResultSplitter",
                                   { splitterWidth, splitterHeight });
            const bool splitterActive  = ImGui::IsItemActive();
            const bool splitterHovered = ImGui::IsItemHovered();
            if ( splitterActive ) {
                // 使用本帧 MouseDelta 增量调整，并再次经过相同高度约束。
                m_searchResultPaneHeight =
                    ProjectAudioToolSearch::calculateResultPaneHeight(
                        resultListHeight + ImGui::GetIO().MouseDelta.y,
                        resultRowHeight,
                        m_searchResults.size(),
                        style.WindowPadding.y,
                        searchLayoutAvailableHeight,
                        reservedHeight);
            }
            if ( splitterHovered || splitterActive ) {
                // 命中或拖动时显示纵向调整光标，明确交互方向。
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            }

            // 视觉线按普通、悬浮和活动三态选择主题颜色。
            const ImU32 splitterColor = ImGui::GetColorU32(
                splitterActive ? ImGuiCol_SeparatorActive
                               : (splitterHovered ? ImGuiCol_SeparatorHovered
                                                  : ImGuiCol_Separator));
            // 细线绘制在较高命中区的垂直中心。
            const float splitterLineY = splitterStart.y + splitterHeight * 0.5F;
            ImGui::GetWindowDrawList()->AddLine(
                { splitterStart.x, splitterLineY },
                { splitterStart.x + splitterWidth, splitterLineY },
                splitterColor,
                std::max(1.0F, style.SeparatorSize));
        }
    }
    // 搜索区域结束后用固定分隔线明确主画布起点。
    ImGui::Separator();
    // 状态栏高度从剩余区域扣除，画布子窗口占据其余全部空间。
    ImVec2 childSize = ImGui::GetContentRegionAvail();
    childSize.y      = std::max(1.0F, childSize.y - statusHeight);
    // 画布使用独立双轴滚动，并禁止子窗口因内容拖动改变自身位置。
    ImGui::BeginChild(
        "ProjectAudioToolCanvas",
        childSize,
        true,
        ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoMove);
    // BeginChild 后读取扣除滚动条后的真实可见像素尺寸。
    const ImVec2      visibleSizePixels = ImGui::GetContentRegionAvail();
    const std::string projectRoot = Config::pathToUtf8(project->m_projectRoot);
    if ( projectRoot != m_cachedProjectRoot ) {
        // 项目切换先恢复视角默认值，方块缓存稍后在统一脏判断中重建。
        m_canvasZoom = 1.0F;
        m_pendingCanvasScroll.reset();
    }

    // canvasCursor 是本帧流式内容起点，滚动换算均以它为锚。
    const ImVec2 canvasCursor = ImGui::GetCursorScreenPos();
    ImVec2       scroll{ ImGui::GetScrollX(), ImGui::GetScrollY() };
    // 加回当前滚动量得到不随视口滚动变化的内容空间屏幕原点。
    const ImVec2 unscrolledCanvasOrigin{ canvasCursor.x + scroll.x,
                                         canvasCursor.y + scroll.y };
    if ( m_pendingCanvasScroll ) {
        // 滑条缩放产生的目标滚动在下一帧子窗口创建后应用。
        scroll = *m_pendingCanvasScroll;
        m_pendingCanvasScroll.reset();
        ImGui::SetScrollX(scroll.x);
        ImGui::SetScrollY(scroll.y);
    }
    // 当前逻辑原点屏幕坐标等于未滚动原点减去实际滚动量。
    ImVec2 canvasOrigin{ unscrolledCanvasOrigin.x - scroll.x,
                         unscrolledCanvasOrigin.y - scroll.y };
    // 允许活动项阻挡时仍识别子窗口悬浮，保证连续拖动不丢失画布状态。
    const bool canvasHovered =
        ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows |
                               ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    const auto& io = ImGui::GetIO();
    // Ctrl+滚轮缩放只在没有其他画布手势时生效。
    const bool cameraZoomAllowed = canvasHovered && io.KeyCtrl &&
                                   std::abs(io.MouseWheel) > 0.01F &&
                                   !m_draggingItem && !m_resizingItem &&
                                   !m_batchDragging && !m_marqueeSelecting;
    if ( cameraZoomAllowed ) {
        // 缩放 helper 同时修正滚动，使指针下逻辑位置保持在原屏幕位置。
        const auto zoomResult = ProjectAudioToolLayout::zoomCameraAtPointer(
            m_canvasZoom,
            io.MouseWheel,
            dpiScale,
            scroll.x,
            scroll.y,
            io.MousePos.x - unscrolledCanvasOrigin.x,
            io.MousePos.y - unscrolledCanvasOrigin.y);
        // 立即应用新倍率和滚动，当前帧后续命中使用更新后的变换。
        m_canvasZoom = zoomResult.zoom;
        scroll       = { zoomResult.scrollX, zoomResult.scrollY };
        canvasOrigin = { unscrolledCanvasOrigin.x - scroll.x,
                         unscrolledCanvasOrigin.y - scroll.y };
        ImGui::SetScrollX(scroll.x);
        ImGui::SetScrollY(scroll.y);
    }

    // canvasScale 是逻辑坐标与屏幕像素之间唯一换算倍率。
    const float canvasScale = dpiScale * m_canvasZoom;
    // 可见逻辑尺寸随画布缩放反向变化，并夹到正值。
    const float visibleWidth =
        std::max(1.0F, visibleSizePixels.x / canvasScale);
    const float visibleHeight =
        std::max(1.0F, visibleSizePixels.y / canvasScale);
    // 新方块默认排布只随窗口 DPI，不随用户画布缩放重新换行。
    const float layoutVisibleWidth =
        std::max(1.0F, visibleSizePixels.x / dpiScale);
    // 事件标脏、项目身份或 DPI 改变任一成立时重建方块缓存。
    if ( m_itemsDirty.exchange(false, std::memory_order_acq_rel) ||
         projectRoot != m_cachedProjectRoot ||
         std::abs(dpiScale - m_cachedDpiScale) > 1e-4F ) {
        rebuildItems(layoutVisibleWidth, dpiScale);
    }

    // 内容尺寸覆盖视口和最远方块，作为滚动上限与末尾 Dummy 的共同依据。
    const ImVec2 contentLogical =
        calculateContentSize(visibleWidth, visibleHeight);
    // 屏幕内容尺寸减去视口尺寸得到两个轴向最大滚动量。
    const ImVec2 maximumScroll{
        std::max(0.0F, contentLogical.x * canvasScale - visibleSizePixels.x),
        std::max(0.0F, contentLogical.y * canvasScale - visibleSizePixels.y),
    };
    // 项目、DPI 或缩放变化后，旧滚动位置可能超出新的内容边界。
    const ImVec2 constrainedScroll{
        std::clamp(scroll.x, 0.0F, maximumScroll.x),
        std::clamp(scroll.y, 0.0F, maximumScroll.y),
    };
    if ( std::abs(constrainedScroll.x - scroll.x) > 1e-4F ||
         std::abs(constrainedScroll.y - scroll.y) > 1e-4F ) {
        // 只有实际越界时写回 ImGui，避免无意义地重设滚动目标。
        scroll       = constrainedScroll;
        canvasOrigin = { unscrolledCanvasOrigin.x - scroll.x,
                         unscrolledCanvasOrigin.y - scroll.y };
        ImGui::SetScrollX(scroll.x);
        ImGui::SetScrollY(scroll.y);
    }

    // 搜索结果聚焦在滚动边界明确后处理，可安全将目标夹到内容范围。
    if ( !m_searchFocusRequestId.empty() ) {
        const auto requestedItem =
            std::ranges::find_if(m_items, [this](const Item& item) {
                return item.audioResourceId == m_searchFocusRequestId;
            });
        if ( requestedItem != m_items.end() ) {
            // 激活会把方块提升到最高层，因此继续使用返回的新索引。
            const auto activeIndex = activateItem(static_cast<std::size_t>(
                std::distance(m_items.begin(), requestedItem)));
            if ( activeIndex ) {
                // 搜索选择沿用普通单击的 Effect 自动试听规则。
                previewEffectSelection(m_items[*activeIndex]);
                const auto& activeRect = m_items[*activeIndex].rect;
                // 目标滚动使方块逻辑中心尽可能位于视口中心。
                const float targetScrollX =
                    (activeRect.x + activeRect.width * 0.5F) * canvasScale -
                    visibleSizePixels.x * 0.5F;
                const float targetScrollY =
                    (activeRect.y + activeRect.height * 0.5F) * canvasScale -
                    visibleSizePixels.y * 0.5F;
                // 靠近内容边缘时夹到有效范围，方块可能无法严格居中。
                scroll.x     = std::clamp(targetScrollX, 0.0F, maximumScroll.x);
                scroll.y     = std::clamp(targetScrollY, 0.0F, maximumScroll.y);
                canvasOrigin = { unscrolledCanvasOrigin.x - scroll.x,
                                 unscrolledCanvasOrigin.y - scroll.y };
                ImGui::SetScrollX(scroll.x);
                ImGui::SetScrollY(scroll.y);
                // 激活改变 Z 序和选择，完成后立即持久化工作区。
                persistWorkspace();
            }
        }
        // 无论目标是否仍存在，请求只消费一次，避免每帧重复查找。
        m_searchFocusRequestId.clear();
    }

    // 可见画布矩形和鼠标位置都转换为逻辑单位，供所有几何算法复用。
    const ProjectAudioToolLayout::Rect visibleCanvas{
        scroll.x / canvasScale,
        scroll.y / canvasScale,
        visibleWidth,
        visibleHeight,
    };
    const ImVec2 mouseLogical{
        (io.MousePos.x - canvasOrigin.x) / canvasScale,
        (io.MousePos.y - canvasOrigin.y) / canvasScale,
    };
    // 命中从数组末尾向前搜索，首个命中即为视觉最上层方块。
    std::optional<std::size_t> hoveredItem;
    if ( canvasHovered ) {
        for ( std::size_t reverse = m_items.size(); reverse > 0; --reverse ) {
            if ( contains(m_items[reverse - 1].rect, mouseLogical) ) {
                // 找到最高层命中后无需检查被其覆盖的底层方块。
                hoveredItem = reverse - 1;
                break;
            }
        }
    }

    // 所有绝对定位方块和交互装饰提交到当前画布子窗口绘制列表。
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    // 任一几何手势进行时隐藏试听控件，防止控件与拖动同时响应。
    const bool audioControlsEnabled = !m_draggingItem && !m_resizingItem &&
                                      !m_batchDragging && !m_marqueeSelecting;
    std::optional<std::size_t> audioControlsItem;
    if ( audioControlsEnabled && !m_openVolumeEditorResourceId.empty() ) {
        // 音量编辑器打开时优先把控件锚定到对应资源，即使鼠标已移开。
        const auto openVolumeItem = std::ranges::find(
            m_items, m_openVolumeEditorResourceId, &Item::audioResourceId);
        if ( openVolumeItem != m_items.end() &&
             isVisible(openVolumeItem->rect, visibleCanvas) ) {
            // 只有资源仍存在且可见时才能继续绘制锚定弹窗。
            audioControlsItem = static_cast<std::size_t>(
                std::distance(m_items.begin(), openVolumeItem));
        } else {
            // 资源删除或滚出视口后关闭锚定状态，避免悬空弹窗。
            m_openVolumeEditorResourceId.clear();
        }
    }
    if ( audioControlsEnabled && !audioControlsItem && canvasHovered ) {
        // 没有固定音量编辑器时，首先选择鼠标直接命中的最高层方块。
        audioControlsItem = hoveredItem;
    }
    if ( audioControlsEnabled && !audioControlsItem && canvasHovered ) {
        // 指针略在方块外时查找最近可见方块，使底部控件不易突然消失。
        const float maximumDistanceSquared = AUDIO_CONTROLS_PROXIMITY *
                                             AUDIO_CONTROLS_PROXIMITY /
                                             (m_canvasZoom * m_canvasZoom);
        // 使用平方距离比较，初始上限按画布缩放修正到稳定屏幕距离。
        float nearestDistanceSquared = maximumDistanceSquared;
        for ( std::size_t reverse = m_items.size(); reverse > 0; --reverse ) {
            const std::size_t index = reverse - 1U;
            // 不可见方块不会显示控件，也不参与近邻竞争。
            if ( !isVisible(m_items[index].rect, visibleCanvas) ) continue;
            const float distanceSquared =
                squaredDistanceToRect(m_items[index].rect, mouseLogical);
            if ( distanceSquared < nearestDistanceSquared ) {
                // 反向遍历配合严格小于，在等距时保留更高层方块。
                nearestDistanceSquared = distanceSquared;
                audioControlsItem      = index;
            }
        }
    }

    // 绘制顺序从底到顶，与压实后的数组 Z 序完全一致。
    for ( std::size_t index = 0; index < m_items.size(); ++index ) {
        const auto& item = m_items[index];
        // 视口外方块直接剔除，不提交背景、文字或控制柄命令。
        if ( !isVisible(item.rect, visibleCanvas) ) continue;

        drawItem(item,
                 canvasOrigin,
                 canvasScale,
                 dpiScale,
                 hoveredItem == index,
                 (m_draggingItem == index || m_resizingItem == index ||
                  (m_batchDragging && item.batchSelected)) &&
                     ImGui::IsMouseDown(ImGuiMouseButton_Left),
                 *drawList);
    }

    // 试听控件结果在方块绘制后处理，使按钮覆盖在卡片内容之上。
    bool        audioControlsHovered = false;
    std::string volumeEditorResourceId;
    bool        brushVolumeChanged = false;
    if ( audioControlsItem && *audioControlsItem < m_items.size() ) {
        // 索引检查防御同帧激活重排后潜在的旧候选值。
        auto& item = m_items[*audioControlsItem];
        if ( item.previewPoolKey.empty() ) {
            // 旧工作区数据缺失预览键时按资源 ID 惰性补建。
            item.previewPoolKey =
                makeProjectAudioPreviewPoolKey("tool/" + item.audioResourceId);
        }
        // 试听控件布局完全在屏幕坐标中计算和命中。
        const auto itemScreenRect =
            toScreenRect(item.rect, canvasOrigin, canvasScale);
        const auto controls = calculateItemAudioControlLayout(itemScreenRect);
        // 整体控件区命中用于阻止后续方块拖动状态机接管左键。
        const bool controlsPointerInside = containsItemAudioControls(
            controls.controls, ImGui::GetIO().MousePos);
        // 公共控件同时驱动预览播放和画笔音量编辑，并返回交互摘要。
        const auto result =
            renderProjectAudioPreviewControls(item.audioResourceId.c_str(),
                                              *project,
                                              item.audioResourceId,
                                              item.previewPoolKey,
                                              m_brushAudioVolume,
                                              &m_brushAudioVolume,
                                              controls.controls,
                                              controlsPointerInside);
        // 弹窗打开也视为控件占用输入，避免点击弹窗时触发画布手势。
        audioControlsHovered =
            controlsPointerInside || result.hovered || result.volumeEditorOpen;
        if ( result.volumeEditorOpen ) {
            // 保存稳定 ID 以跨帧继续锚定音量编辑器。
            m_openVolumeEditorResourceId = item.audioResourceId;
            volumeEditorResourceId       = item.audioResourceId;
            // 合并本帧变化，后续统一同步画笔命令和项目工作区。
            brushVolumeChanged = brushVolumeChanged || result.volumeChanged;
        } else if ( m_openVolumeEditorResourceId == item.audioResourceId ) {
            m_openVolumeEditorResourceId.clear();
        }
    }

    // 音量编辑器打开后确保其资源成为当前活动画笔目标。
    if ( !volumeEditorResourceId.empty() ) {
        const auto volumeItem = std::ranges::find(
            m_items, volumeEditorResourceId, &Item::audioResourceId);
        // 资源切换与仅调整当前资源音量使用不同命令路径。
        const bool resourceChanged =
            m_selectedAudioResourceId != volumeEditorResourceId;
        if ( resourceChanged && volumeItem != m_items.end() ) {
            // 激活重排会影响遮挡关系，因此成功后完整重建标签区。
            if ( activateItem(static_cast<std::size_t>(
                     std::distance(m_items.begin(), volumeItem))) ) {
                rebuildLabelRects();
            }
        } else if ( brushVolumeChanged ) {
            // 当前资源不变时只提交带新音量的画笔资源命令。
            Logic::EditorEngine::instance().pushCommand(
                Logic::LogicCommand(Logic::CmdSetBrushAudioResource{
                    m_selectedAudioResourceId,
                    m_selectedAudioTrackType,
                    m_brushAudioVolume,
                }));
        }
        if ( resourceChanged || brushVolumeChanged ) {
            // 仅状态实际变化时保存，悬浮控件不会产生磁盘写入。
            persistWorkspace();
        }
    }

    // 空闲状态下右键最高层方块请求重命名，试听控件区域不响应。
    if ( hoveredItem && !audioControlsHovered && !m_draggingItem &&
         !m_resizingItem && !m_batchDragging && !m_marqueeSelecting &&
         ImGui::IsMouseClicked(ImGuiMouseButton_Right) ) {
        requestItemRename(*hoveredItem);
    }
    // 当前缩放中保持原控制柄；空闲时才重新执行边缘命中。
    ResizeHandle hoveredResizeHandle = ResizeHandle::None;
    if ( m_resizingItem ) {
        hoveredResizeHandle = m_resizeHandle;
    } else if ( hoveredItem && !audioControlsHovered &&
                !m_items[*hoveredItem].batchSelected ) {
        hoveredResizeHandle =
            hitTestResizeHandle(m_items[*hoveredItem], mouseLogical);
    }
    // 控制柄类型映射到 ImGui 标准轴向或对角缩放光标。
    switch ( hoveredResizeHandle ) {
    case ResizeHandle::Left:
    case ResizeHandle::Right:
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        break;
    case ResizeHandle::Top:
    case ResizeHandle::Bottom:
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        break;
    case ResizeHandle::TopLeft:
    case ResizeHandle::BottomRight:
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
        break;
    case ResizeHandle::TopRight:
    case ResizeHandle::BottomLeft:
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW);
        break;
    case ResizeHandle::None: break;
    }

    /// @brief 按当前框选矩形刷新批量选择状态。
    ///
    /// 基础选择快照用于 Ctrl 或 Shift 追加模式，普通模式的快照全部为未选中。
    /// Strict 模式要求完整包含方块，宽松模式只要求存在矩形交集。
    /// @warning 框选热路径：每次鼠标移动线性遍历方块，只执行几何判断。
    const auto refreshMarqueeSelection = [&]() {
        // 快照与方块数量不一致时说明资源集合变化，本次框选不能继续。
        if ( m_marqueeBaseSelection.size() != m_items.size() ) return;
        // 两个任意方向点先标准化为左上角和非负宽高矩形。
        const auto selectionRect = rectFromPoints(m_marqueeStart, m_marqueeEnd);
        // 极小指针抖动不形成有效区域，保留基础选择状态。
        const bool selectionValid =
            selectionRect.width > 0.5F && selectionRect.height > 0.5F;
        // 选择模式从当前编辑器设置读取，与主画布框选语义一致。
        const auto selectionMode =
            Config::AppConfig::instance().getEditorSettings().selectionMode;
        for ( std::size_t index = 0; index < m_items.size(); ++index ) {
            // 每帧从交互开始快照重算，反向拖动不会累积错误选择。
            bool selected = m_marqueeBaseSelection[index] != 0;
            if ( selectionValid ) {
                // 追加模式将基础选择与本帧几何命中做逻辑或。
                selected =
                    selected ||
                    (selectionMode == Config::SelectionMode::Strict
                         ? containsRect(selectionRect, m_items[index].rect)
                         : ProjectAudioToolLayout::intersection(
                               selectionRect, m_items[index].rect)
                               .has_value());
            }
            m_items[index].batchSelected = selected;
        }
    };

    // 空闲画布左键根据命中对象依次进入批量拖动、缩放、单项拖动或框选。
    if ( canvasHovered && !audioControlsHovered && !m_draggingItem &&
         !m_resizingItem && !m_batchDragging && !m_marqueeSelecting &&
         ImGui::IsMouseClicked(ImGuiMouseButton_Left) ) {
        if ( hoveredItem ) {
            // 已批量选择方块优先启动整体拖动，不再显示单项缩放语义。
            if ( m_items[*hoveredItem].batchSelected ) {
                beginBatchDrag(*hoveredItem, mouseLogical);
            } else if ( hoveredResizeHandle != ResizeHandle::None ) {
                // 单项缩放开始前清除旧批量选择，避免两种装饰并存。
                clearBatchSelection();
                beginItemResize(
                    *hoveredItem, hoveredResizeHandle, mouseLogical);
            } else {
                // 方块主体普通按下进入单项拖动，并可在未移动时解释为选择试听。
                clearBatchSelection();
                beginItemDrag(*hoveredItem, mouseLogical);
            }
        } else {
            // 点击空白先清除活动画笔资源，并立即保存选择变化。
            if ( !m_selectedAudioResourceId.empty() ) {
                clearActiveItem();
                persistWorkspace();
            }
            // Ctrl 或 Shift 保留交互开始前的批量选择，其余情况从空集合开始。
            const bool additiveSelection =
                ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift;
            // 框选起止点初始化为同一逻辑坐标，首帧尚未形成有效矩形。
            m_marqueeSelecting = true;
            m_marqueeStart     = mouseLogical;
            m_marqueeEnd       = mouseLogical;
            // 使用字节快照避免 vector<bool> 代理语义，并与方块索引一一对应。
            m_marqueeBaseSelection.assign(m_items.size(), 0);
            if ( additiveSelection ) {
                // 追加模式复制现有选择位，拖动过程中始终以此为基线。
                for ( std::size_t index = 0; index < m_items.size(); ++index ) {
                    m_marqueeBaseSelection[index] =
                        m_items[index].batchSelected ? 1 : 0;
                }
            } else {
                // 普通框选立即清空旧集合，避免零面积起始帧保留选择。
                clearBatchSelection();
            }
        }
    }

    if ( m_marqueeSelecting ) {
        // 按下期间每帧更新终点并以基础快照重新计算结果。
        m_marqueeEnd = mouseLogical;
        refreshMarqueeSelection();
        if ( !ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
            // 松开结束框选并释放仅供该手势使用的基础选择缓存。
            m_marqueeSelecting = false;
            m_marqueeBaseSelection.clear();
        }
    }

    // 单项拖动状态保存最高层方块索引，资源刷新越界时分支不会执行。
    if ( m_draggingItem && *m_draggingItem < m_items.size() ) {
        auto& item = m_items[*m_draggingItem];
        // 总位移相对按下起点计算，用于稳定判断是否超过拖动阈值。
        const float deltaX = mouseLogical.x - m_itemDragStartMouse.x;
        const float deltaY = mouseLogical.y - m_itemDragStartMouse.y;
        // ImGui 屏幕像素阈值除以画布倍率后转换为逻辑距离。
        const float dragThreshold =
            ImGui::GetIO().MouseDragThreshold / canvasScale;
        // 一旦超过阈值，本手势余下阶段始终视为拖动而非单击。
        m_itemDragMoved = m_itemDragMoved || deltaX * deltaX + deltaY * deltaY >
                                                 dragThreshold * dragThreshold;
        if ( ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
            if ( m_itemDragMoved ) {
                // 原始候选位置保持指针在方块内的初始相对偏移。
                ProjectAudioToolLayout::Rect rawRect{
                    mouseLogical.x - m_dragOffset.x,
                    mouseLogical.y - m_dragOffset.y,
                    item.rect.width,
                    item.rect.height,
                };
                // 先应用边缘和中心吸附，再执行底层方块可见性约束。
                rawRect =
                    ProjectAudioToolLayout::snapRect(rawRect,
                                                     visibleCanvas,
                                                     m_dragSnapTargets,
                                                     SNAP_THRESHOLD,
                                                     SNAP_RELEASE_THRESHOLD,
                                                     m_snapLocks);
                // 内容边界以本帧逻辑画布尺寸为限，并保护固定方块最低可见比例。
                item.rect = ProjectAudioToolLayout::constrainVisibility(
                    rawRect,
                    ProjectAudioToolLayout::Rect{
                        0.0F,
                        0.0F,
                        contentLogical.x,
                        contentLogical.y,
                    },
                    m_dragVisibilityConstraints,
                    MINIMUM_VISIBLE_RATIO);
                // 活动项位于最高层，其自身标签可使用完整当前矩形。
                item.labelRect = item.rect;
                refreshInteractionLabelRects();
            }
        } else {
            // 松开后进行一次完整遮挡重建，并释放交互专用缓存。
            rebuildLabelRects();
            m_interactionBaseLabelRects.clear();
            if ( !m_itemDragMoved ) {
                // 未超过拖动阈值的手势解释为选择，可按项目设置试听 Effect。
                previewEffectSelection(item);
            }
            // 选择提升层级或方块位置变化都在手势结束时一次保存。
            persistWorkspace();
            m_draggingItem.reset();
            m_itemDragMoved = false;
            m_dragSnapTargets.clear();
            m_dragVisibilityConstraints.clear();
            // 清除参考线锁，下一次手势重新建立吸附滞回状态。
            m_snapLocks = {};
        }
    }

    // 批量拖动使用总体包围盒求位移，再把同一位移应用到每个初始矩形。
    if ( m_batchDragging && !m_batchDragEntries.empty() ) {
        if ( ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
            // 指针偏移固定到初始总体包围盒，保持批次整体不跳动。
            ProjectAudioToolLayout::Rect rawBounds{
                mouseLogical.x - m_batchDragOffset.x,
                mouseLogical.y - m_batchDragOffset.y,
                m_batchDragInitialBounds.width,
                m_batchDragInitialBounds.height,
            };
            // 吸附针对整体包围盒，参考线与整个选择集合的边缘或中心对齐。
            rawBounds = ProjectAudioToolLayout::snapRect(rawBounds,
                                                         visibleCanvas,
                                                         m_dragSnapTargets,
                                                         SNAP_THRESHOLD,
                                                         SNAP_RELEASE_THRESHOLD,
                                                         m_snapLocks);
            // 结合选择并集单元，约束平移后所有固定方块的最低可见比例。
            m_batchDragCurrentBounds =
                ProjectAudioToolLayout::constrainTranslatedVisibility(
                    m_batchDragCurrentBounds,
                    rawBounds,
                    m_batchDragInitialBounds,
                    m_batchDragUnionCells,
                    ProjectAudioToolLayout::Rect{
                        0.0F,
                        0.0F,
                        contentLogical.x,
                        contentLogical.y,
                    },
                    m_dragVisibilityConstraints,
                    MINIMUM_VISIBLE_RATIO);
            // 约束后的总体包围盒与初始包围盒差值即为最终统一位移。
            const float deltaX =
                m_batchDragCurrentBounds.x - m_batchDragInitialBounds.x;
            const float deltaY =
                m_batchDragCurrentBounds.y - m_batchDragInitialBounds.y;
            for ( const auto& entry : m_batchDragEntries ) {
                // 防御项目刷新后的旧索引，合法条目均从初始矩形而非上帧位置平移。
                if ( entry.itemIndex >= m_items.size() ) continue;
                auto& item = m_items[entry.itemIndex];
                item.rect  = {
                    entry.startRect.x + deltaX,
                    entry.startRect.y + deltaY,
                    entry.startRect.width,
                    entry.startRect.height,
                };
                // 移动批次处于最高层，各自身标签暂用完整方块区域。
                item.labelRect = item.rect;
            }
            refreshInteractionLabelRects();
        } else {
            // 松开后一次性重建静止标签区、保存布局并释放全部批次缓存。
            rebuildLabelRects();
            m_interactionBaseLabelRects.clear();
            persistWorkspace();
            m_batchDragging = false;
            m_batchDragEntries.clear();
            m_batchDragUnionCells.clear();
            m_dragSnapTargets.clear();
            m_dragVisibilityConstraints.clear();
            m_snapLocks = {};
        }
    }

    // 单项缩放根据固定初始矩形和当前指针计算候选矩形。
    if ( m_resizingItem && *m_resizingItem < m_items.size() ) {
        auto& item = m_items[*m_resizingItem];
        if ( ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
            // 控制柄先分解为水平和垂直活动边，角柄会同时设置两轴。
            using ProjectAudioToolLayout::ResizeEdge;
            ResizeEdge horizontalEdge = ResizeEdge::None;
            ResizeEdge verticalEdge   = ResizeEdge::None;
            // 左侧柄固定初始右边，右侧柄固定初始左边。
            switch ( m_resizeHandle ) {
            case ResizeHandle::Left:
            case ResizeHandle::TopLeft:
            case ResizeHandle::BottomLeft:
                horizontalEdge = ResizeEdge::Minimum;
                break;
            case ResizeHandle::Right:
            case ResizeHandle::TopRight:
            case ResizeHandle::BottomRight:
                horizontalEdge = ResizeEdge::Maximum;
                break;
            default: break;
            }
            // 上侧柄固定初始底边，下侧柄固定初始顶边。
            switch ( m_resizeHandle ) {
            case ResizeHandle::Top:
            case ResizeHandle::TopLeft:
            case ResizeHandle::TopRight:
                verticalEdge = ResizeEdge::Minimum;
                break;
            case ResizeHandle::Bottom:
            case ResizeHandle::BottomLeft:
            case ResizeHandle::BottomRight:
                verticalEdge = ResizeEdge::Maximum;
                break;
            default: break;
            }

            // 当前主题和音频类型共同决定本次缩放下限。
            const float minimumWidth  = minimumItemWidth(item.type, dpiScale);
            const float minimumHeight = minimumItemHeight(dpiScale);
            // 每帧从初始矩形重算，避免连续舍入误差累计。
            ProjectAudioToolLayout::Rect rawRect = m_resizeStartRect;
            if ( horizontalEdge == ResizeEdge::Minimum ) {
                // 移动左边时夹到画布零边界和最小宽度允许的最大 X。
                const float right = m_resizeStartRect.right();
                rawRect.x = std::clamp(mouseLogical.x - m_resizePointerOffset.x,
                                       0.0F,
                                       right - minimumWidth);
                rawRect.width = right - rawRect.x;
            } else if ( horizontalEdge == ResizeEdge::Maximum ) {
                // 移动右边时保持左边不变，仅对宽度施加下限。
                rawRect.width =
                    std::max(minimumWidth,
                             mouseLogical.x - m_resizePointerOffset.x -
                                 m_resizeStartRect.x);
            }
            if ( verticalEdge == ResizeEdge::Minimum ) {
                // 移动上边时保持底边固定，并防止越过画布顶部。
                const float bottom = m_resizeStartRect.bottom();
                rawRect.y = std::clamp(mouseLogical.y - m_resizePointerOffset.y,
                                       0.0F,
                                       bottom - minimumHeight);
                rawRect.height = bottom - rawRect.y;
            } else if ( verticalEdge == ResizeEdge::Maximum ) {
                // 移动下边时保持顶边固定，仅对高度施加下限。
                rawRect.height =
                    std::max(minimumHeight,
                             mouseLogical.y - m_resizePointerOffset.y -
                                 m_resizeStartRect.y);
            }
            // 尺寸下限应用后再吸附活动边，避免吸附产生过小矩形。
            rawRect =
                ProjectAudioToolLayout::snapResizeRect(rawRect,
                                                       horizontalEdge,
                                                       verticalEdge,
                                                       visibleCanvas,
                                                       m_dragSnapTargets,
                                                       minimumWidth,
                                                       minimumHeight,
                                                       SNAP_THRESHOLD,
                                                       SNAP_RELEASE_THRESHOLD,
                                                       m_snapLocks);
            // 最终约束保护被活动方块覆盖的固定方块最低可见面积。
            item.rect = ProjectAudioToolLayout::constrainResizeVisibility(
                item.rect,
                rawRect,
                m_dragVisibilityConstraints,
                MINIMUM_VISIBLE_RATIO);
            // 活动缩放项位于最高层，标签随当前矩形即时更新。
            item.labelRect = item.rect;
            refreshInteractionLabelRects();
            if ( horizontalEdge != ResizeEdge::None &&
                 std::abs(item.rect.width - m_resizeStartRect.width) > 1e-4F ) {
                // 只有宽度真实变化才标记自定义，单纯点击控制柄不覆盖自动宽度。
                item.widthCustomized = true;
            }
            if ( verticalEdge != ResizeEdge::None &&
                 std::abs(item.rect.height - m_resizeStartRect.height) >
                     1e-4F ) {
                // 高度自定义标志采用同一实际变化判定。
                item.heightCustomized = true;
            }
        } else {
            // 松开后恢复完整标签遮挡结果并一次保存最终尺寸。
            rebuildLabelRects();
            m_interactionBaseLabelRects.clear();
            persistWorkspace();
            m_resizingItem.reset();
            m_resizeHandle = ResizeHandle::None;
            m_dragSnapTargets.clear();
            m_dragVisibilityConstraints.clear();
            m_snapLocks = {};
        }
    }

    // 参考线只需要当前活动几何的边界；批量拖动使用总体包围盒。
    std::optional<ProjectAudioToolLayout::Rect> snapGuideBounds;
    if ( m_batchDragging ) {
        // 约束后的当前包围盒反映整个选择集合真实对齐位置。
        snapGuideBounds = m_batchDragCurrentBounds;
    } else {
        // 单项拖动与缩放都直接使用活动方块当前矩形。
        const auto activeItem =
            m_draggingItem ? m_draggingItem : m_resizingItem;
        if ( activeItem && *activeItem < m_items.size() ) {
            // 越界活动索引不生成参考线，等待交互状态清理。
            snapGuideBounds = m_items[*activeItem].rect;
        }
    }
    if ( snapGuideBounds ) {
        // 可见逻辑边界转换为屏幕坐标，参考线不会绘制到画布窗口外。
        const ImVec2 visibleMinimum{
            canvasOrigin.x + visibleCanvas.x * canvasScale,
            canvasOrigin.y + visibleCanvas.y * canvasScale,
        };
        const ImVec2 visibleMaximum{
            canvasOrigin.x + visibleCanvas.right() * canvasScale,
            canvasOrigin.y + visibleCanvas.bottom() * canvasScale,
        };
        // 半透明暖色参考线与方块类型色区分，并保持在深浅背景上可见。
        constexpr ImU32 SNAP_GUIDE_COLOR = IM_COL32(255, 218, 96, 150);
        // 粗细、实线长度和间隔均随 DPI 调整并设定屏幕像素下限。
        const float snapGuideThickness = std::max(1.0F, 1.25F * dpiScale);
        const float snapGuideDash      = std::max(4.0F, 6.0F * dpiScale);
        const float snapGuideGap       = std::max(3.0F, 4.0F * dpiScale);
        // X 轴锁存在且活动几何仍对齐时绘制贯穿可见高度的竖线。
        if ( m_snapLocks.x.targetLine &&
             alignsWithTargetLine(
                 *snapGuideBounds, *m_snapLocks.x.targetLine, true) ) {
            // 逻辑参考线坐标经画布倍率和原点转换到屏幕 X。
            const float guideX =
                canvasOrigin.x + *m_snapLocks.x.targetLine * canvasScale;
            drawSnapGuide(*drawList,
                          { guideX, visibleMinimum.y },
                          { guideX, visibleMaximum.y },
                          SNAP_GUIDE_COLOR,
                          snapGuideThickness,
                          snapGuideDash,
                          snapGuideGap);
        }
        // Y 轴采用相同校验，绘制贯穿可见宽度的横线。
        if ( m_snapLocks.y.targetLine &&
             alignsWithTargetLine(
                 *snapGuideBounds, *m_snapLocks.y.targetLine, false) ) {
            // 逻辑参考线坐标经画布倍率和原点转换到屏幕 Y。
            const float guideY =
                canvasOrigin.y + *m_snapLocks.y.targetLine * canvasScale;
            drawSnapGuide(*drawList,
                          { visibleMinimum.x, guideY },
                          { visibleMaximum.x, guideY },
                          SNAP_GUIDE_COLOR,
                          snapGuideThickness,
                          snapGuideDash,
                          snapGuideGap);
        }
    }

    // 框选装饰在所有方块之后绘制，确保选择范围覆盖在内容上层。
    if ( m_marqueeSelecting ) {
        // 起止逻辑点标准化后转换为屏幕矩形。
        const auto selectionRect =
            toScreenRect(rectFromPoints(m_marqueeStart, m_marqueeEnd),
                         canvasOrigin,
                         canvasScale);
        const ImVec2 minimum{ selectionRect.x, selectionRect.y };
        const ImVec2 maximum{ selectionRect.right(), selectionRect.bottom() };
        // 轻透明填充表达范围但不遮蔽方块标签。
        drawList->AddRectFilled(minimum, maximum, IM_COL32(120, 170, 255, 42));
        // 边框使用较高 Alpha 和 DPI 后线宽，明确实时框选边界。
        drawList->AddRect(minimum,
                          maximum,
                          IM_COL32(120, 170, 255, 180),
                          0.0F,
                          0,
                          std::max(1.5F, 1.5F * dpiScale));
    }

    // 绝对定位控件全部提交完成后再声明画布内容边界。提前提交覆盖整张画布的
    // Dummy 会让后续回退到方块坐标的 ImGui Item 落入非流式布局状态，在
    // 新版 ImGui 中可能被裁成残片；末尾 Dummy 同时满足滚动范围和边界检查。
    // 游标恢复到子窗口流式起点，Dummy 只声明尺寸而不偏移绝对绘制坐标。
    ImGui::SetCursorScreenPos(canvasCursor);
    ImGui::Dummy(
        { contentLogical.x * canvasScale, contentLogical.y * canvasScale });
    // 画布内容边界提交后结束滚动子窗口，后续弹窗回到工具窗口层级。
    ImGui::EndChild();
    // 重命名弹窗每帧在窗口上下文中推进，打开请求由其内部消费。
    renderRenamePopup(dpiScale);
    // 状态栏与画布之间使用分隔线，避免缩放控件混入滚动内容。
    ImGui::Separator();

    // 状态行左侧是可裁剪文本，右侧为固定范围缩放滑条。
    const ImVec2 statusRowStart = ImGui::GetCursorScreenPos();
    const float  statusRowWidth = ImGui::GetContentRegionAvail().x;
    // 滑条宽度在 120 到 220 DPI 像素间取值，窄窗口不超过整行宽度。
    const float zoomSliderWidth =
        std::min(statusRowWidth,
                 std::max(120.0F * dpiScale,
                          std::min(220.0F * dpiScale, statusRowWidth * 0.42F)));
    // 文本区扣除滑条和主题间距，极窄时夹到零。
    const float statusTextWidth = std::max(
        0.0F,
        statusRowWidth - zoomSliderWidth - ImGui::GetStyle().ItemSpacing.x);
    // 裁剪左侧状态文本，防止长文件名覆盖右侧缩放滑条。
    ImGui::PushClipRect(statusRowStart,
                        { statusRowStart.x + statusTextWidth,
                          statusRowStart.y + ImGui::GetFrameHeight() },
                        true);
    if ( m_selectedAudioResourceId.empty() ) {
        // 无活动资源时展示明确空状态，不显示残留类型和标签。
        ImGui::TextUnformatted(m_translationCache.statusNone);
    } else {
        // 活动状态展示类型短标签与当前文件名，资源 ID 不占用状态栏。
        ImGui::Text(
            "%s: %s  %s",
            m_translationCache.statusSelected,
            m_selectedAudioTrackType == AudioTrackType::Main ? "MAIN" : "FX",
            m_selectedAudioLabel.c_str());
    }
    // 批量选择数量独立于活动资源，可与单项状态同时显示。
    const std::size_t batchCount = batchSelectionCount();
    if ( batchCount > 0 ) {
        // 使用分隔符弱化显示，不创建额外状态栏行。
        ImGui::SameLine();
        ImGui::TextDisabled(
            "| %s: %zu", m_translationCache.statusBatchSelected, batchCount);
    }
    // 恢复裁剪后再定位滑条，否则滑条也会被左侧文本区裁掉。
    ImGui::PopClipRect();

    // 滑条右对齐到状态行边界，垂直位置与文本共享同一起点。
    ImGui::SetCursorScreenPos(
        { statusRowStart.x + statusRowWidth - zoomSliderWidth,
          statusRowStart.y });
    // 显式宽度保证百分比文本和拖动轨道都落在预留区域内。
    ImGui::SetNextItemWidth(zoomSliderWidth);
    // 内部倍率转换为整数百分比，并限制到 UI 支持的 50% 至 400%。
    int zoomPercentage = std::clamp(
        static_cast<int>(std::lround(m_canvasZoom * 100.0F)), 50, 400);
    if ( ImGui::SliderInt("##ProjectAudioToolCanvasZoom",
                          &zoomPercentage,
                          50,
                          400,
                          "%d%%",
                          ImGuiSliderFlags_AlwaysClamp) ) {
        // 状态栏缩放以视口中心为锚点，区别于 Ctrl+滚轮的指针锚点。
        const auto zoomResult = ProjectAudioToolLayout::zoomCameraToPointer(
            m_canvasZoom,
            static_cast<float>(zoomPercentage) * 0.01F,
            dpiScale,
            scroll.x,
            scroll.y,
            visibleSizePixels.x * 0.5F,
            visibleSizePixels.y * 0.5F);
        // 新倍率立即保存，但滚动目标延迟到下一帧子窗口开始后应用。
        m_canvasZoom = zoomResult.zoom;

        // 使用新倍率重算可见逻辑尺寸和内容滚动边界。
        const float nextCanvasScale = dpiScale * m_canvasZoom;
        const float nextVisibleWidth =
            std::max(1.0F, visibleSizePixels.x / nextCanvasScale);
        const float nextVisibleHeight =
            std::max(1.0F, visibleSizePixels.y / nextCanvasScale);
        // 方块布局不改变，只有视口覆盖范围和末端留白对应像素发生变化。
        const ImVec2 nextContentLogical =
            calculateContentSize(nextVisibleWidth, nextVisibleHeight);
        // 新最大滚动量必须基于新 canvasScale，不能复用滑条变化前的上限。
        const ImVec2 nextMaximumScroll{
            std::max(
                0.0F,
                nextContentLogical.x * nextCanvasScale - visibleSizePixels.x),
            std::max(
                0.0F,
                nextContentLogical.y * nextCanvasScale - visibleSizePixels.y),
        };
        // 将 helper 给出的中心锚定滚动夹到新边界后排队。
        m_pendingCanvasScroll = ImVec2{
            std::clamp(zoomResult.scrollX, 0.0F, nextMaximumScroll.x),
            std::clamp(zoomResult.scrollY, 0.0F, nextMaximumScroll.y),
        };
    }
}

}  // namespace MMM::UI
