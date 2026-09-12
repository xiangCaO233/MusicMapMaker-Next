#include "ui/imgui/FloatingManagerUI.h"
#include "config/AppConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/ui/UISubViewToggleEvent.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "ui/ICanvasView.h"
#include "ui/ISubView.h"
#include "ui/UIManager.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <utility>

namespace MMM::UI
{
namespace
{
/// @brief 描述鼠标越过侧栏最小尺寸边界的结果。
///
/// 该值只在当前 ImGui 帧内使用，不持有 DockNode，也不跨帧缓存。
struct DockResizeOverrun {
    /// @brief 当前拖拽目标和轴向是否可用于边界计算。
    bool valid{ false };

    /// @brief 被拖拽分隔线的水平或垂直轴。
    ImGuiAxis axis{ ImGuiAxis_None };

    /// @brief 侧栏达到最小尺寸时分隔线所在的屏幕坐标。
    float boundary{ 0.0f };

    /// @brief 鼠标越过最小尺寸边界的非负距离。
    float overrun{ 0.0f };
};

/// @brief 浮动管理器隐藏阈值，低于该值时停止绘制关闭动画。
constexpr float FLOATING_MANAGER_HIDDEN_EPSILON = 0.01f;

/// @brief 计算项目切换占位内容的最小尺寸。
/// @param dpiScale 当前窗口内容缩放。
/// @return 可完整显示切换提示的内容尺寸。
///
/// 过渡占位不依赖具体子视图，使用翻译后文字尺寸加两侧固定逻辑留白，保证项目
/// 关闭旧会话并创建新会话期间停靠布局不会突然缩成零尺寸。
/// @warning UI 热路径：项目切换期间每帧只执行一次文本测量。
ImVec2 getProjectTransitionMinContentSize(float dpiScale)
{
    // DPI 至少按 1 倍处理，避免异常缩放值反向压缩提示留白。
    const float padding = 16.0f * std::max(1.0f, dpiScale);
    // 翻译文本可能改变宽度，因此不能缓存固定像素值。
    const ImVec2 textSize =
        ImGui::CalcTextSize(TR("ui.project.opening").data());
    // ceil 使半像素测量不会导致最后一个像素被裁切。
    return { std::ceil(textSize.x + padding * 2.0f),
             std::ceil(textSize.y + padding * 2.0f) };
}

/// @brief 一次 dock resize 手势实际命中的 split 节点和当前子树。
///
/// parent 拥有分割轴与两个子节点，node 是当前浮动管理器所在的一侧；二者只在
/// 当前 ImGui DockBuilder 树稳定期间有效，禁止在 DockSpace 重建后继续使用。
struct DockResizeTarget {
    /// @brief 是否成功解析到 split 和子节点。
    bool valid{ false };

    /// @brief 当前浮动窗口所在的 split 子树节点。
    ImGuiDockNode* node{ nullptr };

    /// @brief 被拖拽的父 split 节点。
    ImGuiDockNode* parent{ nullptr };

    /// @brief 被拖拽 split 的轴向。
    ImGuiAxis axis{ ImGuiAxis_None };

    /// @brief 当前子树是否为父 split 的第一个子节点。
    ///
    /// 该方向决定尺寸增大时分隔线朝正轴还是负轴移动。
    bool isFirstChild{ true };
};

/// @brief 将数值限制到 0 到 1。
/// @param value 输入值。
/// @return 限制后的值。
/// @warning UI 热路径：仅执行常量范围夹取，不访问全局状态。
float saturate(float value)
{
    // 动画曲线只接受归一化进度，避免过冲产生负透明度。
    return std::clamp(value, 0.0f, 1.0f);
}

/// @brief 计算 ease-out cubic 缓动值。
/// @param value 线性进度。
/// @return 缓动后的进度。
///
/// 显示动画使用快速启动、末端减速的三次曲线，让侧栏尽快恢复可读状态。
/// @warning UI 热路径：每帧执行固定次数浮点运算。
float easeOutCubic(float value)
{
    // 先归一化，保证 inv 始终位于闭区间内。
    const float t   = saturate(value);
    const float inv = 1.0f - t;
    // 1-(1-t)^3 在端点保持精确的 0 和 1。
    return 1.0f - inv * inv * inv;
}

/// @brief 计算 smoothstep 缓动值。
/// @param value 线性进度。
/// @return 缓动后的进度。
///
/// 隐藏动画使用两端斜率为零的曲线，减少透明度接近零时的突变。
/// @warning UI 热路径：每帧执行固定次数浮点运算。
float smoothStep(float value)
{
    // 输入先钳制，避免配置异常造成曲线反向。
    const float t = saturate(value);
    // 标准三次 Hermite 形式不需要额外状态或分支。
    return t * t * (3.0f - 2.0f * t);
}

/// @brief 读取统一 UI 动画过渡速度。
/// @return 每秒推进的线性动画进度。
///
/// 速度来自当前编辑器美学设置，所有浮动管理器共享同一节奏。
/// @warning UI 热路径：只读取当前内存配置，不执行文件 IO。
float getUiAnimationTransitionSpeed()
{
    // AppConfig 在启动时已加载，此处只访问内存中的值对象。
    return Config::AppConfig::instance()
        .getEditorSettings()
        .aesthetics.animationTransitionSpeed();
}

/// @brief 推进浮动管理器显隐动画进度。
/// @param amount 当前动画进度。
/// @param visible 目标是否可见。
/// @return 更新后的动画进度。
///
/// 进度以秒为单位推进并限制单帧最大步长，帧率波动不会使状态越过目标端点。
/// @warning UI 热路径：每帧只读取 DeltaTime 并做一次线性逼近。
float updateFloatingVisibilityAmount(float amount, bool visible)
{
    // 显示目标为一，隐藏目标为零。
    const float target = visible ? 1.0f : 0.0f;
    // 负 DeltaTime 被视为零，超长帧最多直接推进到终点。
    const float step = std::min(1.0f,
                                std::max(0.0f, ImGui::GetIO().DeltaTime) *
                                    getUiAnimationTransitionSpeed());

    if ( amount < target ) {
        // 显示方向只递增且不超过目标。
        return std::min(target, amount + step);
    }
    // 隐藏方向只递减且不低于目标。
    return std::max(target, amount - step);
}

/// @brief 判断窗口尺寸是否已经被压到当前内容最小尺寸以下。
/// @param size 当前窗口尺寸。
/// @param minWindowSize 加入装饰后的最小窗口尺寸。
/// @return 任一轴明显低于最小值时返回 true。
///
/// 半像素容差吸收 ImGui 布局取整误差，避免边界附近每帧来回切换状态。
/// @warning UI 热路径：每帧仅做尺寸比较。
bool isBelowMinWindowSize(ImVec2 size, ImVec2 minWindowSize)
{
    return size.x < minWindowSize.x - 0.5f || size.y < minWindowSize.y - 0.5f;
}

/// @brief 判断是否已经越过本次低于最小尺寸起点后的收起缓冲距离。
/// @param currentShortage 当前鼠标越界距离。
/// @param startShortage 首次锁定最小尺寸时的越界距离。
/// @param minWindowSize 当前窗口最小尺寸。
/// @param axis 本次 resize 的轴向。
/// @return 从锁定起点继续拖过半个最小尺寸时返回 true。
///
/// 使用相对越界量而非绝对鼠标位置，用户可以从不同位置开始拖拽而获得一致阈值。
/// @warning UI 热路径：每帧仅做阈值计算。
bool shouldCollapseAfterMinDragStart(float currentShortage, float startShortage,
                                     ImVec2 minWindowSize, ImGuiAxis axis)
{
    const float minAxisSize =
        axis == ImGuiAxis_X ? minWindowSize.x : minWindowSize.y;
    // 半个最小尺寸提供足够意图确认距离，避免轻微越界就收起。
    const float collapseDistance = std::floor(minAxisSize * 0.5f);
    // 只有锁定后的新增越界量参与判断。
    return currentShortage - startShortage > collapseDistance;
}

/// @brief 判断原生 dock 分隔线是否已经拉出超过当前内容最小尺寸的一半。
/// @param currentSize 当前从收回边缘拉出的轴向尺寸。
/// @param startSize 手势开始时的轴向尺寸。
/// @param minWindowSize 子视图要求的最小窗口尺寸。
/// @param axis 恢复手势的轴向。
/// @return 拉出距离足以确认恢复意图时返回 true。
///
/// 展开阈值与收起阈值对称，避免同一手势在边缘附近反复切换。
/// @warning UI 热路径：侧边栏收回后拖拽时每帧仅做阈值计算。
bool shouldExpandAfterCollapsedDragStart(float currentSize, float startSize,
                                         ImVec2 minWindowSize, ImGuiAxis axis)
{
    const float minAxisSize =
        axis == ImGuiAxis_X ? minWindowSize.x : minWindowSize.y;
    // floor 与收起路径保持相同像素取整规则。
    const float expandDistance = std::floor(minAxisSize * 0.5f);
    // 手势起点允许未来改为非零占位宽度。
    return currentSize - startSize > expandDistance;
}

/// @brief 读取 ImVec2 在指定轴上的值。
/// @param value 待读取的二维值。
/// @param axis 目标轴；非 X 值按 Y 处理。
/// @return 对应轴分量。
/// @warning UI 热路径：用于统一水平和垂直停靠算法。
float getAxisValue(ImVec2 value, ImGuiAxis axis)
{
    // 调用点已验证轴向，只保留无分支表结构的二选一。
    return axis == ImGuiAxis_X ? value.x : value.y;
}

/// @brief 写入 ImVec2 在指定轴上的值。
/// @param value 待原地修改的二维值。
/// @param axis 目标轴；非 X 值按 Y 处理。
/// @param axisValue 新的轴向分量。
/// @warning UI 热路径：只修改选定分量，另一分量保持不变。
void setAxisValue(ImVec2& value, ImGuiAxis axis, float axisValue)
{
    if ( axis == ImGuiAxis_X ) {
        // 水平 resize 只写入宽度或 X 坐标。
        value.x = axisValue;
    } else {
        // 垂直 resize 只写入高度或 Y 坐标。
        value.y = axisValue;
    }
}

/// @brief 根据 split 父子节点构造 resize 目标。
/// @param node 当前侧栏所在的子树。
/// @param parent 候选父 split。
/// @return 校验成功的非拥有目标，失败时 valid 为 false。
///
/// 只有 parent 直接包含 node 且 SplitAxis 有效时才建立目标，防止用陈旧 DockNode
/// 关系修改无关子树。
/// @warning UI 热路径：只校验 DockNode 关系和 split 轴向。
DockResizeTarget makeDockResizeTarget(ImGuiDockNode* node,
                                      ImGuiDockNode* parent)
{
    DockResizeTarget target;
    if ( !node || !parent ) {
        // 空节点无法形成可操作的父子关系。
        return target;
    }

    // 明确识别 node 在父节点两个槽位中的方向。
    const bool isFirstChild  = parent->ChildNodes[0] == node;
    const bool isSecondChild = parent->ChildNodes[1] == node;
    if ( !isFirstChild && !isSecondChild ) {
        // 非直接子节点必须先沿父链重新解析。
        return target;
    }

    const ImGuiAxis axis = parent->SplitAxis;
    if ( axis != ImGuiAxis_X && axis != ImGuiAxis_Y ) {
        // 未分割的父节点没有可拖拽轴。
        return target;
    }

    // 成功结果只保存观察指针，不延长 ImGui 节点生命周期。
    target.valid        = true;
    target.node         = node;
    target.parent       = parent;
    target.axis         = axis;
    target.isFirstChild = isFirstChild;
    return target;
}

/// @brief 设置指定 dock 节点轴向尺寸，并同步补偿同一父 split 的兄弟节点。
/// @param node 待调整的当前侧栏子树。
/// @param axis 父 split 的轴向。
/// @param targetSize 期望写入的轴向尺寸。
/// @param nodeMinSize 当前节点允许的最小尺寸。
/// @return 经过父节点容量和兄弟最小值夹取后的实际尺寸。
///
/// 算法保持父 split 总尺寸不变：当前节点增加多少，兄弟节点就减少多少，并同步
/// Size、SizeRef 和 Pos。没有匹配父 split 时只能安全更新当前节点缓存。
/// @warning UI 热路径：仅在自动收回拖拽续接期间更新当前 DockNode 几何。
float resizeDockNodeAxis(ImGuiDockNode* node, ImGuiAxis axis, float targetSize,
                         float nodeMinSize)
{
    if ( !node ) {
        // 调用方可继续使用请求值计算后续状态，但不写入任何节点。
        return targetSize;
    }

    ImGuiDockNode* parent = node->ParentNode;
    if ( !parent || parent->SplitAxis != axis ) {
        // 缺少同轴父 split 时只保证当前节点不小于最小尺寸。
        const float clampedSize = std::max(nodeMinSize, targetSize);
        setAxisValue(node->Size, axis, clampedSize);
        setAxisValue(node->SizeRef, axis, clampedSize);
        return clampedSize;
    }

    ImGuiDockNode* sibling = parent->ChildNodes[0] == node
                                 ? parent->ChildNodes[1]
                                 : parent->ChildNodes[0];
    if ( !sibling ) {
        // 不完整的 split 无法安全补偿总尺寸。
        return targetSize;
    }

    // 为兄弟保留至少一个像素，避免生成退化 DockNode。
    const float siblingMin = 1.0f;
    // 父位置和父尺寸是两侧重排的共同基准。
    const float parentPos = getAxisValue(parent->Pos, axis);
    const float parentSize =
        std::max(nodeMinSize + siblingMin, getAxisValue(parent->Size, axis));
    const float clampedSize =
        std::clamp(targetSize, nodeMinSize, parentSize - siblingMin);
    // 两个子树尺寸之和维持父节点轴向尺寸。
    const float siblingSize  = std::max(siblingMin, parentSize - clampedSize);
    const bool  isFirstChild = parent->ChildNodes[0] == node;

    // SizeRef 同步更新，防止下一次 DockBuilder 布局恢复旧比例。
    setAxisValue(node->Size, axis, clampedSize);
    setAxisValue(node->SizeRef, axis, clampedSize);
    setAxisValue(sibling->Size, axis, siblingSize);
    setAxisValue(sibling->SizeRef, axis, siblingSize);

    if ( isFirstChild ) {
        // 第一子树从父起点开始，兄弟紧随其后。
        setAxisValue(node->Pos, axis, parentPos);
        setAxisValue(sibling->Pos, axis, parentPos + clampedSize);
    } else {
        // 第二子树位于末端，兄弟占据父起点一侧。
        setAxisValue(sibling->Pos, axis, parentPos);
        setAxisValue(node->Pos, axis, parentPos + siblingSize);
    }

    if ( node->HostWindow ) {
        // 标记 ini 脏位，让调整后的停靠几何可持久化。
        ImGui::MarkIniSettingsDirty(node->HostWindow);
    }

    return clampedSize;
}

/// @brief 将 resize 手势命中的 split 子树锁定到当前内容最小尺寸。
/// @param target 已校验的 resize 目标。
/// @param minWindowSize 当前窗口的二维最小尺寸。
/// @return 实际锁定后的目标轴尺寸；目标无效时返回零。
///
/// 该封装把二维窗口约束投影到命中 split 的单一轴，并复用兄弟补偿逻辑。
/// @warning UI 热路径：仅在 dock 分割线越过最小尺寸时写回命中的 DockNode
/// 几何，阻止压缩继续向兄弟子树传播。
float lockDockResizeTargetToMinSize(const DockResizeTarget& target,
                                    ImVec2                  minWindowSize)
{
    if ( !target.valid || !target.node ) {
        // 零值供调用方识别没有发生尺寸写入。
        return 0.0f;
    }

    // 只读取本次分割轴对应的最小尺寸分量。
    const float minAxisSize = getAxisValue(minWindowSize, target.axis);
    return resizeDockNodeAxis(
        target.node, target.axis, minAxisSize, minAxisSize);
}

/// @brief 将正在续接的自动收回拖拽距离换算为当前侧边栏宽度。
/// @param mouseAxis 当前鼠标轴向屏幕坐标。
/// @param isFirstChild 收回前是否位于 split 第一侧。
/// @param startMouseAxis 透明热区开始拖拽时的坐标。
/// @return 至少一个像素的临时侧栏轴向尺寸。
///
/// 第一侧沿坐标正方向展开，第二侧沿负方向展开，因此两种放置使用相反差值。
/// @warning UI 热路径：仅在鼠标按下拖拽时做一维距离计算。
float getCollapsedResumeDragSize(float mouseAxis, bool isFirstChild,
                                 float startMouseAxis)
{
    const float dragDistance =
        isFirstChild ? mouseAxis - startMouseAxis : startMouseAxis - mouseAxis;
    // 一像素下限确保重新创建的 DockNode 不退化。
    return std::max(1.0f, dragDistance);
}

/// @brief 获取自动收回透明拖拽热区宽度。
/// @param dpiScale 当前窗口内容缩放。
/// @return 覆盖分隔线且便于命中的像素宽度。
///
/// 热区至少为八个缩放像素，同时不得窄于 ImGui 分隔线和两侧 hover 扩展总宽度。
/// @warning UI 热路径：侧边栏收回后每帧只读取 ImGui 样式和 DPI。
float getCollapsedOverlayHitSize(float dpiScale)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    // ceil 避免高 DPI 下得到亚像素命中边界。
    return std::ceil(std::max(
        8.0f * std::max(1.0f, dpiScale),
        style.DockingSeparatorSize + style.WindowBorderHoverPadding * 2.0f));
}

/// @brief 计算自动收回透明拖拽热区的屏幕矩形。
/// @param hostWindow 主 DockHost 窗口。
/// @param axis 收回侧栏原本所在 split 的轴向。
/// @param isFirstChild 侧栏原本是否位于第一侧。
/// @param hitSize 热区在分隔轴上的厚度。
/// @return 居中覆盖原边界的屏幕矩形；宿主为空时返回空矩形。
///
/// 横向 split 生成贯穿宿主宽度的水平热区，纵向 split
/// 生成贯穿宿主高度的垂直热区。
/// @warning UI 热路径：仅根据 DockHost 窗口几何和收回侧向计算一个矩形。
ImRect makeCollapsedOverlayRect(const ImGuiWindow* hostWindow, ImGuiAxis axis,
                                bool isFirstChild, float hitSize)
{
    if ( !hostWindow ) {
        // 默认 ImRect 由调用方通过宽高检查判为不可用。
        return ImRect();
    }

    if ( axis == ImGuiAxis_Y ) {
        // 第一侧边界在宿主上沿，第二侧边界在宿主下沿。
        const float boundary = isFirstChild
                                   ? hostWindow->Pos.y
                                   : hostWindow->Pos.y + hostWindow->Size.y;
        // 热区以边界为中心，向两侧各扩展一半厚度。
        return ImRect(ImVec2(hostWindow->Pos.x, boundary - hitSize * 0.5f),
                      ImVec2(hostWindow->Pos.x + hostWindow->Size.x,
                             boundary + hitSize * 0.5f));
    }

    // X 轴分割对应宿主左沿或右沿的纵向热区。
    const float boundary = isFirstChild
                               ? hostWindow->Pos.x
                               : hostWindow->Pos.x + hostWindow->Size.x;
    return ImRect(ImVec2(boundary - hitSize * 0.5f, hostWindow->Pos.y),
                  ImVec2(boundary + hitSize * 0.5f,
                         hostWindow->Pos.y + hostWindow->Size.y));
}

/// @brief 计算自动收回分隔线 hover/active 样式的可见矩形。
/// @param hostWindow 主 DockHost 窗口。
/// @param axis 分隔线轴向。
/// @param separatorAxis 当前分隔线屏幕坐标。
/// @param thickness 可见线条厚度。
/// @return 覆盖宿主另一轴全长的矩形。
///
/// 可见反馈与透明命中热区分离，拖拽时线条可随鼠标移动而热区仍锚定原边缘。
/// @warning UI 热路径：仅根据主 DockHost 几何和鼠标拖拽位置计算绘制矩形。
ImRect makeCollapsedSeparatorRect(const ImGuiWindow* hostWindow, ImGuiAxis axis,
                                  float separatorAxis, float thickness)
{
    if ( !hostWindow ) {
        // 宿主缺失时没有可绘制的屏幕范围。
        return ImRect();
    }

    if ( axis == ImGuiAxis_Y ) {
        // 水平分隔线贯穿宿主窗口宽度。
        return ImRect(
            ImVec2(hostWindow->Pos.x, separatorAxis - thickness * 0.5f),
            ImVec2(hostWindow->Pos.x + hostWindow->Size.x,
                   separatorAxis + thickness * 0.5f));
    }

    // 垂直分隔线贯穿宿主窗口高度。
    return ImRect(ImVec2(separatorAxis - thickness * 0.5f, hostWindow->Pos.y),
                  ImVec2(separatorAxis + thickness * 0.5f,
                         hostWindow->Pos.y + hostWindow->Size.y));
}

/// @brief 夹住指定 dock 节点轴向尺寸，并补偿同一父 split 中的兄弟节点。
/// @param node 待保护的侧栏 DockNode。
/// @param axis 需要执行最小值约束的轴。
/// @param minValue 当前轴允许的最小尺寸。
///
/// 与主动 resize helper 不同，本函数仅在节点已经低于下限时修复。它保持父 split
/// 的总范围与子节点顺序，且同步 SizeRef，避免下一轮布局立刻恢复越界值。
/// @warning UI 热路径：仅在 dock 尺寸越界时修改当前 DockNode 及兄弟 SizeRef。
void clampDockNodeAxis(ImGuiDockNode* node, ImGuiAxis axis, float minValue)
{
    if ( !node ) {
        // 空观察指针表示窗口当前没有可保护的停靠节点。
        return;
    }

    // 已达下限时不触碰 DockBuilder 缓存。
    const float currentSize = getAxisValue(node->Size, axis);
    if ( currentSize >= minValue ) {
        return;
    }

    ImGuiDockNode* parent = node->ParentNode;
    if ( !parent || parent->SplitAxis != axis ) {
        // 根节点或不同轴父节点没有可在同级补偿的兄弟范围。
        setAxisValue(node->Size, axis, minValue);
        // SizeRef 只允许抬高，保留原先更大的布局偏好。
        setAxisValue(node->SizeRef,
                     axis,
                     std::max(getAxisValue(node->SizeRef, axis), minValue));
        return;
    }

    // 直接兄弟承担当前节点恢复最小尺寸所需的空间差额。
    ImGuiDockNode* sibling = parent->ChildNodes[0] == node
                                 ? parent->ChildNodes[1]
                                 : parent->ChildNodes[0];
    if ( !sibling ) {
        // 不完整 split 的总量关系未知，因此不做部分写入。
        return;
    }

    // 兄弟至少保留一个像素，维持 ImGui 对有效节点尺寸的假设。
    const float siblingMin = 1.0f;
    // 两个子节点以父节点的位置和总尺寸重新排布。
    const float parentPos  = getAxisValue(parent->Pos, axis);
    const float parentSize = getAxisValue(parent->Size, axis);
    // 当前节点精确恢复到下限，剩余空间全部交给兄弟。
    const float siblingSize  = std::max(siblingMin, parentSize - minValue);
    const bool  isFirstChild = parent->ChildNodes[0] == node;

    // 同步实时尺寸和下一次布局参考尺寸。
    setAxisValue(node->Size, axis, minValue);
    setAxisValue(node->SizeRef,
                 axis,
                 std::max(getAxisValue(node->SizeRef, axis), minValue));
    setAxisValue(sibling->Size, axis, siblingSize);
    setAxisValue(sibling->SizeRef, axis, siblingSize);

    if ( isFirstChild ) {
        // 当前节点位于父起点，兄弟从最小尺寸边界开始。
        setAxisValue(node->Pos, axis, parentPos);
        setAxisValue(sibling->Pos, axis, parentPos + minValue);
    } else {
        // 兄弟位于父起点，当前节点贴住父末端。
        setAxisValue(sibling->Pos, axis, parentPos);
        setAxisValue(node->Pos, axis, parentPos + siblingSize);
    }
}

/// @brief 将子视图内容最小尺寸换算为 ImGui 窗口最小尺寸。
/// @param contentMinSize 子视图报告的纯内容区最小尺寸。
/// @param dpiScale 当前窗口内容缩放。
/// @return 包含双侧 padding 和标题区域的窗口尺寸。
///
/// 子视图无需知道宿主窗口装饰；管理器在此统一加入主题 padding 与一行标题高度。
/// @warning UI 热路径：每帧只读取当前样式和常量配置。
ImVec2 toWindowMinSize(ImVec2 contentMinSize, float dpiScale)
{
    auto& aesthetics =
        Config::AppConfig::instance().getEditorSettings().aesthetics;
    // 主题值按 DPI 放大，但异常小于一的缩放不压缩最低可用空间。
    float windowPadding =
        std::floor(aesthetics.windowPadding * std::max(1.0f, dpiScale));
    // 标题高度采用当前 ImGui 字体和样式的真实帧高度。
    float titleHeight = ImGui::GetFrameHeightWithSpacing();
    // 两个轴都至少保持一个像素，防止约束矩形退化。
    return ImVec2(
        std::max(1.0f, contentMinSize.x + windowPadding * 2.0f),
        std::max(1.0f, contentMinSize.y + windowPadding * 2.0f + titleHeight));
}

/// @brief 将指定停靠节点夹到最小窗口尺寸，阻止 dock 分割线继续压缩。
/// @param window 当前 ImGui 窗口，用于同步其即时 Size。
/// @param currentSize 调用点观测到的窗口或 DockNode 尺寸。
/// @param minWindowSize 加入窗口装饰后的最小尺寸。
/// @return 经过水平和垂直约束后的当前尺寸。
///
/// 只有停靠窗口才能通过调整 split 约束尺寸；浮动窗口交给 ImGui 的
/// SetNextWindowSizeConstraints 处理。本函数在两个轴上独立修复并按需标记 ini。
/// @warning UI 热路径：仅在当前窗口已停靠且尺寸越界时更新 DockNode 尺寸缓存。
ImVec2 clampDockNodeToMinSize(ImGuiWindow* window, ImVec2 currentSize,
                              ImVec2 minWindowSize)
{
    if ( !isBelowMinWindowSize(currentSize, minWindowSize) ) {
        // 常见路径无需访问 ImGui 内部 DockNode。
        return currentSize;
    }

    if ( !window || !window->DockNode ) {
        // 非停靠窗口不允许在这里直接改内部几何。
        return currentSize;
    }

    // 记录是否发生写入，避免无条件污染布局 ini。
    ImGuiDockNode* node        = window->DockNode;
    bool           changedSize = false;
    if ( node->Size.x < minWindowSize.x ) {
        // 水平恢复同时补偿同级兄弟节点。
        clampDockNodeAxis(node, ImGuiAxis_X, minWindowSize.x);
        // ImGuiWindow 即时 Size 与 DockNode 保持一致。
        window->Size.x = std::max(window->Size.x, minWindowSize.x);
        currentSize.x  = minWindowSize.x;
        changedSize    = true;
    }
    if ( node->Size.y < minWindowSize.y ) {
        // 垂直约束独立应用，允许只修复一个轴。
        clampDockNodeAxis(node, ImGuiAxis_Y, minWindowSize.y);
        window->Size.y = std::max(window->Size.y, minWindowSize.y);
        currentSize.y  = minWindowSize.y;
        changedSize    = true;
    }

    if ( changedSize && node->HostWindow ) {
        // 只有实际更改且存在宿主窗口时才请求保存布局。
        ImGui::MarkIniSettingsDirty(node->HostWindow);
    }
    return currentSize;
}

/// @brief 计算鼠标越过当前 dock split 最小尺寸手柄的距离。
/// @param target 当前手势对应的父子 split。
/// @param minWindowSize 当前侧栏最小窗口尺寸。
/// @param mousePos 用于计算的鼠标屏幕坐标。
/// @return 有效轴、最小尺寸边界与非负越界距离。
///
/// 第一子树的最小边界从父起点向正方向量取，第二子树从父末端向负方向量取；
/// 两种情况统一返回越过侧栏一侧的正距离。
/// @warning UI 热路径：每帧只读取 DockNode 几何和鼠标位置。
DockResizeOverrun getDockResizeOverrun(const DockResizeTarget& target,
                                       ImVec2 minWindowSize, ImVec2 mousePos)
{
    DockResizeOverrun result;
    if ( !target.valid || !target.parent || !target.node ) {
        // 默认结果 valid 为 false，调用方不会进入锁定逻辑。
        return result;
    }

    ImGuiDockNode*  parent = target.parent;
    const ImGuiAxis axis   = target.axis;
    if ( axis != ImGuiAxis_X && axis != ImGuiAxis_Y ) {
        // 非分割轴不能投影窗口和鼠标坐标。
        return result;
    }

    // 最小值、父几何和鼠标统一投影到本次 split 的一维坐标。
    const float minValue =
        axis == ImGuiAxis_X ? minWindowSize.x : minWindowSize.y;
    const float parentPos    = getAxisValue(parent->Pos, axis);
    const float parentSize   = getAxisValue(parent->Size, axis);
    const float mouseAxis    = getAxisValue(mousePos, axis);
    const bool  isFirstChild = target.isFirstChild;
    // 第一侧从父起点加最小值，第二侧从父末端减最小值。
    const float boundary =
        isFirstChild ? parentPos + minValue
                     : parentPos + std::max(1.0f, parentSize - minValue);
    // 越界方向随侧向相反，最终统一夹到非负值。
    const float overrun =
        isFirstChild ? boundary - mouseAxis : mouseAxis - boundary;

    // 即使尚未越界也返回有效边界，供锁定解除条件使用。
    result.valid    = true;
    result.axis     = axis;
    result.boundary = boundary;
    result.overrun  = std::max(0.0f, overrun);
    return result;
}

/// @brief 判断鼠标位置是否落在指定 split 的分割条交互范围内。
/// @param parent 候选 split 父节点。
/// @param mousePos 点击时的屏幕坐标。
/// @return 鼠标落在扩展后的分隔条矩形内时返回 true。
///
/// 使用两个子节点的交界构造矩形，并按 ImGui 当前边框 hover padding 扩展命中区，
/// 使识别范围与原生 Docking 分隔条一致。
/// @warning UI 热路径：仅读取 DockNode 几何和 ImGui 样式。
bool isDockSplitResizeHandleHit(ImGuiDockNode* parent, ImVec2 mousePos)
{
    if ( !parent ) {
        // 根节点或失效父指针没有 split 交互区域。
        return false;
    }

    // 分隔矩形需要两个完整子树共同定义交界。
    ImGuiDockNode* child0 = parent->ChildNodes[0];
    ImGuiDockNode* child1 = parent->ChildNodes[1];
    if ( !child0 || !child1 ) {
        // 单子节点不构成可拖拽 split。
        return false;
    }

    const ImGuiAxis axis = parent->SplitAxis;
    if ( axis != ImGuiAxis_X && axis != ImGuiAxis_Y ) {
        // 未分割节点不应参与 resize 手势识别。
        return false;
    }

    // Min 从第一子树末端开始，Max 延伸至第二子树对应边界。
    ImRect splitterRect;
    splitterRect.Min = child0->Pos;
    splitterRect.Max = child1->Pos;
    splitterRect.Min[axis] += child0->Size[axis];
    splitterRect.Max[axis ^ 1] += child1->Size[axis ^ 1];

    // 命中扩展至少覆盖实际分隔线厚度。
    const ImGuiStyle& style = ImGui::GetStyle();
    const float       hoverExtend =
        std::max(style.WindowBorderHoverPadding, style.DockingSeparatorSize);
    // 只沿分隔轴扩展，另一轴继续由子树范围约束。
    splitterRect.Expand(axis == ImGuiAxis_Y ? ImVec2(0.0f, hoverExtend)
                                            : ImVec2(hoverExtend, 0.0f));
    return splitterRect.Contains(mousePos);
}

/// @brief 从当前窗口 DockNode 向上查找鼠标命中的 resize split。
/// @param node 当前侧栏窗口的 DockNode。
/// @param mousePos 鼠标按下时的屏幕坐标。
/// @return 从近到远第一个命中的父 split。
///
/// 侧栏可能位于嵌套 DockNode 中，实际被拖动的分隔条不一定是直接父节点，因此沿
/// 父链查找，同时把每一级中包含侧栏的 child 子树带入目标。
/// @warning UI 热路径：点击帧最多沿当前 DockNode 父链向上扫描。
DockResizeTarget findDockResizeTargetAtPos(ImGuiDockNode* node, ImVec2 mousePos)
{
    // child 始终表示当前父节点下包含侧栏窗口的那一侧。
    ImGuiDockNode* child = node;
    for ( ImGuiDockNode* parent = node ? node->ParentNode : nullptr; parent;
          child = parent, parent = parent->ParentNode ) {
        // 先校验父子关系，再用同一父节点的分隔矩形测试点击。
        DockResizeTarget target = makeDockResizeTarget(child, parent);
        if ( target.valid && isDockSplitResizeHandleHit(parent, mousePos) ) {
            // 最近命中优先，符合用户直接操作可见分隔条的预期。
            return target;
        }
    }
    // 没有命中时返回无效目标，不启动手势追踪。
    return DockResizeTarget{};
}

/// @brief 根据拖拽开始时保存的 ID 重新解析当前 resize split。
/// @param node 当前帧侧栏 DockNode。
/// @param splitId 手势开始时父 split 的稳定 ImGuiID。
/// @param childId 手势开始时包含侧栏子树的 ImGuiID。
/// @return 当前 Dock 树中匹配的父子目标。
///
/// 不跨帧保存原始 DockNode 指针；每帧通过 ID 沿当前父链重新定位，允许 ImGui 在
/// DockSpace 布局过程中重建或移动内部节点。
/// @warning UI 热路径：拖拽期间每帧最多沿当前 DockNode 父链向上扫描。
DockResizeTarget findDockResizeTargetByIds(ImGuiDockNode* node, ImGuiID splitId,
                                           ImGuiID childId)
{
    if ( !node || splitId == 0 || childId == 0 ) {
        // 零 ID 表示当前没有已捕获的 resize 手势。
        return DockResizeTarget{};
    }

    // child 随父链上移，保持与保存时相同的直接父子语义。
    ImGuiDockNode* child = node;
    for ( ImGuiDockNode* parent = node->ParentNode; parent;
          child = parent, parent = parent->ParentNode ) {
        if ( parent->ID == splitId && child->ID == childId ) {
            // 再次校验轴向和父子槽位，拒绝恰好复用的无效 ID 组合。
            return makeDockResizeTarget(child, parent);
        }
    }
    // Dock 树已变化时让上层回退到直接父节点或结束手势。
    return DockResizeTarget{};
}

/// @brief 查找指定 DockNode 所属的最近轴向 split 子树。
/// @param node 需要保护的画布 DockNode。
/// @param axis 侧栏拖拽正在影响的轴向。
/// @return 最近的同轴父 split；找不到时尝试直接父节点。
///
/// 保护对象是包含画布的整棵子树而不只是叶窗口，这样恢复尺寸时不会破坏画布内部
/// 其他方向的停靠布局。
/// @warning UI 热路径：只在需要保护主画布尺寸时沿 DockNode 父链向上扫描。
DockResizeTarget findDockNodeAxisTarget(ImGuiDockNode* node, ImGuiAxis axis)
{
    if ( !node || (axis != ImGuiAxis_X && axis != ImGuiAxis_Y) ) {
        // 无效轴无法选择需要保护的父 split。
        return DockResizeTarget{};
    }

    // 从最近父节点开始寻找与侧栏拖拽同轴的分割层级。
    ImGuiDockNode* child = node;
    for ( ImGuiDockNode* parent = node->ParentNode; parent;
          child = parent, parent = parent->ParentNode ) {
        DockResizeTarget target = makeDockResizeTarget(child, parent);
        if ( target.valid && target.axis == axis ) {
            // 最近同轴子树最准确地代表会被本次 resize 挤压的范围。
            return target;
        }
    }

    // 直接父节点回退仍经过 makeDockResizeTarget 完整校验。
    return makeDockResizeTarget(node, node->ParentNode);
}
}  // namespace

/// @brief 创建一个可承载多个互斥子视图的浮动侧栏管理器。
/// @param name 管理器稳定名称，同时作为事件路由目标和 ImGui ID 后缀。
///
/// 构造时订阅全局子视图切换事件；事件回调只接受目标名称匹配且并非自身发出的
/// 消息，防止隐藏操作发布的同步事件再次回流。
FloatingManagerUI::FloatingManagerUI(const std::string& name)
    : IUIView(name), ITextureLoader(name)
{
    // 保存订阅 ID，析构时必须从 EventBus 对称注销。
    m_subId = MMM::Event::EventBus::instance()
                  .subscribe<MMM::Event::UISubViewToggleEvent>(
                      [this](const MMM::Event::UISubViewToggleEvent& e) {
                          XINFO(
                              "FloatingManagerUI get event, targetName:{}, "
                              "targetSubViewId:{}",
                              e.targetFloatManagerName,
                              e.subViewId);
                          // 只处理发给本管理器且来自外部视图的指令。
                          if ( e.targetFloatManagerName == this->m_name &&
                               e.sourceUiName != this->m_name ) {
                              if ( e.showSubView ) {
                                  // 显示请求同时选择事件携带的子视图。
                                  this->restoreSubViewState(e.subViewId, true);
                              } else {
                                  // 隐藏请求保留 ID，但清理交互状态。
                                  this->restoreSubViewState(e.subViewId, false);
                              }
                          }
                      });
}

/// @brief 注销子视图切换订阅并结束管理器生命周期。
///
/// EventBus 不拥有管理器，必须在成员析构前移除捕获 this 的回调。
FloatingManagerUI::~FloatingManagerUI()
{
    // 使用构造时保存的精确订阅 ID，避免影响其他管理器实例。
    MMM::Event::EventBus::instance()
        .unsubscribe<MMM::Event::UISubViewToggleEvent>(m_subId);
}

/// @brief 注册或替换一个由管理器独占所有权的子视图。
/// @param subViewId 用于切换、事件路由和窗口标题的稳定标识。
/// @param subView 待接管的子视图。
///
/// 注册不自动改变当前可见项，初始化代码可先完整装配全部子视图再恢复工作区状态。
void FloatingManagerUI::registerSubView(const std::string&        subViewId,
                                        std::unique_ptr<ISubView> subView)
{
    // unique_ptr 移入 map 后，其生命周期与管理器一致。
    m_subViews[subViewId] = std::move(subView);
    // 不在此处自动切换，避免注册顺序覆盖持久化的当前状态。
}

/// @brief 响应侧栏按钮，切换、显示或隐藏指定子视图。
/// @param subViewId 用户选择的子视图标识。
///
/// 再次选择当前可见项会隐藏侧栏；选择其他项或已隐藏项会从零开始显示动画。
/// 两个分支都重置 resize、自动收回和画布保护状态，确保旧手势不能污染新窗口。
void FloatingManagerUI::toggleSubView(const std::string& subViewId)
{
    if ( m_currentSubViewId == subViewId && m_isVisible ) {
        // 已激活项再次点击时执行普通隐藏，不留下透明恢复热区。
        m_isVisible = false;
        // 下次显示必须重新观察有效窗口尺寸。
        m_hasSeenUsableSize    = false;
        m_requestShowSizeReset = false;
        m_wasDocked            = false;
        // 普通隐藏既不是最小尺寸锁定，也不是自动收回。
        m_minResizeLockActive     = false;
        m_isAutoCollapsed         = false;
        m_minResizeLockAxis       = -1;
        m_dockResizeGestureActive = false;
        m_dockResizeGestureAxis   = -1;
        // 清除捕获的 split ID，防止 Dock 树变化后误解析。
        m_dockResizeGestureSplitId = 0;
        m_dockResizeGestureChildId = 0;
        // 结束透明热区续接拖拽及其一次性诊断状态。
        m_collapsedResizeDragActive        = false;
        m_collapsedResizeDragAxis          = -1;
        m_collapsedResizeResumeStateLogged = false;
        m_collapsedResizeResumeApplyLogged = false;
        m_collapsedDockId                  = 0;
        // 不再需要恢复被钳制的 ImGui 鼠标坐标。
        m_restoreMouseAfterDockSpace = false;
        // 释放本手势记录的所有画布尺寸快照。
        clearCanvasDockSizeProtection();
    } else {
        if ( m_currentSubViewId != subViewId || !m_isVisible ) {
            // 新显示周期从完全透明开始。
            m_visibilityAnimAmount = 0.0f;
        }
        // 先选择目标，再标记可见，后续 update 才会访问子视图。
        m_currentSubViewId = subViewId;
        m_isVisible        = true;
        // 首帧通过强制最小尺寸避免沿用上一个子视图的小窗口。
        m_hasSeenUsableSize    = false;
        m_requestShowSizeReset = true;
        // 新子视图不继承上一个子视图的停靠手势和收回状态。
        m_minResizeLockActive     = false;
        m_isAutoCollapsed         = false;
        m_minResizeLockAxis       = -1;
        m_dockResizeGestureActive = false;
        m_dockResizeGestureAxis   = -1;
        // Split ID 只属于开始捕获它的那次鼠标手势。
        m_dockResizeGestureSplitId  = 0;
        m_dockResizeGestureChildId  = 0;
        m_collapsedResizeDragActive = false;
        m_collapsedResizeDragAxis   = -1;
        // 收回恢复日志标志随恢复手势一起重置。
        m_collapsedResizeResumeStateLogged = false;
        m_collapsedResizeResumeApplyLogged = false;
        // 新窗口尚无可恢复的原 DockNode。
        m_collapsedDockId            = 0;
        m_restoreMouseAfterDockSpace = false;
        clearCanvasDockSizeProtection();
    }
}

/// @brief 从持久化工作区或外部事件恢复子视图选择与可见状态。
/// @param subViewId 期望恢复的子视图标识。
/// @param visible 期望的侧栏可见状态。
///
/// 未注册的 ID 不能被显示；真正发生可见性或选中项变化时清空全部瞬时 resize
/// 状态。外部隐藏自动收回占位时也必须清理透明热区，即使 visible 值未变化。
void FloatingManagerUI::restoreSubViewState(const std::string& subViewId,
                                            bool               visible)
{
    // 只有已经注册的子视图才允许进入可见状态。
    const bool willShow =
        visible && m_subViews.find(subViewId) != m_subViews.end();
    // 分别识别可见性变化、内容切换和残留自动收回占位。
    const bool changedVisibility = willShow != m_isVisible;
    const bool changedSubView    = willShow && subViewId != m_currentSubViewId;
    const bool clearCollapsedPlaceholder = !willShow && m_isAutoCollapsed;
    if ( willShow && (changedVisibility || changedSubView) ) {
        // 恢复或切换时重新播放显示动画。
        m_visibilityAnimAmount = 0.0f;
    }
    if ( willShow ) {
        // 隐藏请求保留当前 ID，便于工作区记忆最近选择。
        m_currentSubViewId = subViewId;
    }
    m_isVisible = willShow;
    if ( changedVisibility || changedSubView || clearCollapsedPlaceholder ) {
        // 状态边界统一重置尺寸观测和首帧恢复请求。
        m_hasSeenUsableSize    = false;
        m_requestShowSizeReset = willShow;
        m_wasDocked            = false;
        m_minResizeLockActive  = false;
        m_isAutoCollapsed      = false;
        m_minResizeLockAxis    = -1;
        // 外部恢复不续接旧的停靠分隔线手势。
        m_dockResizeGestureActive  = false;
        m_dockResizeGestureAxis    = -1;
        m_dockResizeGestureSplitId = 0;
        m_dockResizeGestureChildId = 0;
        // 自动收回热区和诊断节流状态不得跨工作区恢复。
        m_collapsedResizeDragActive        = false;
        m_collapsedResizeDragAxis          = -1;
        m_collapsedResizeResumeStateLogged = false;
        m_collapsedResizeResumeApplyLogged = false;
        m_collapsedDockId                  = 0;
        // 当前帧没有被 DockSpace 临时钳制的鼠标输入。
        m_restoreMouseAfterDockSpace = false;
        clearCanvasDockSizeProtection();
    }
}

/// @brief 在鼠标按下边沿捕获当前侧栏实际命中的 Dock resize split。
/// @param dockNode 当前侧栏窗口的 DockNode。
///
/// 捕获时保存父 split 与包含侧栏子树的 ID，后续帧通过 ID 重新解析，不保存可能因
/// DockBuilder 更新而失效的裸指针。鼠标释放或 Dock 关系缺失时清除整次手势状态。
/// @warning UI 热路径：可见侧栏每帧调用；父链扫描只发生在按下边沿。
void FloatingManagerUI::updateDockResizeGesture(ImGuiDockNode* dockNode)
{
    // mouseDown 用于同时处理释放清理和按下边沿捕获。
    const bool mouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if ( !dockNode || !dockNode->ParentNode || !mouseDown ) {
        // 当前不存在可持续的分隔线拖拽目标。
        m_dockResizeGestureActive  = false;
        m_dockResizeGestureAxis    = -1;
        m_dockResizeGestureSplitId = 0;
        m_dockResizeGestureChildId = 0;
        m_minResizeLockActive      = false;
        m_minResizeLockAxis        = -1;
        if ( !mouseDown ) {
            // 真正释放时也结束从透明热区续接的恢复拖拽。
            m_collapsedResizeDragActive        = false;
            m_collapsedResizeDragAxis          = -1;
            m_collapsedResizeResumeStateLogged = false;
            m_collapsedResizeResumeApplyLogged = false;
            // 画布尺寸保护只覆盖一次按住手势。
            clearCanvasDockSizeProtection();
        }
        return;
    }

    if ( !ImGui::IsMouseClicked(ImGuiMouseButton_Left) ) {
        // 按住期间沿用开始帧保存的 ID，不重复命中测试。
        return;
    }

    // MouseClickedPos 保留按下边沿位置，不受本帧后续鼠标钳制影响。
    const ImVec2           clickPos = ImGui::GetIO().MouseClickedPos[0];
    const DockResizeTarget target =
        findDockResizeTargetAtPos(dockNode, clickPos);
    // 有效目标保存轴向和双方 ID，供后续帧重新定位。
    m_dockResizeGestureActive = target.valid;
    m_dockResizeGestureAxis   = target.valid ? target.axis : -1;
    m_dockResizeGestureSplitId =
        target.valid && target.parent ? target.parent->ID : 0;
    m_dockResizeGestureChildId =
        target.valid && target.node ? target.node->ID : 0;
    if ( !m_dockResizeGestureActive ) {
        // 点击未命中 split 时取消之前可能残留的最小尺寸锁。
        m_minResizeLockActive      = false;
        m_minResizeLockAxis        = -1;
        m_dockResizeGestureSplitId = 0;
        m_dockResizeGestureChildId = 0;
        clearCanvasDockSizeProtection();
    }
}

/// @brief 捕获当前 resize 轴上所有画布 Dock 子树的原始尺寸。
/// @param sourceManager 用于按会话 cameraId 查找画布视图。
/// @param axis 侧栏正在压缩的 ImGuiAxis。
///
/// 侧栏被锁在最小尺寸后继续拖动时，ImGui 可能把越界量传播给其他画布子树。
/// 本函数在手势首次需要保护时记录主画布和时间线所在同轴子树，后续帧复用快照。
/// @warning UI 热路径：一次拖拽至多捕获一次；会按当前会话数线性枚举画布。
void FloatingManagerUI::captureCanvasDockSizeProtection(
    UIManager* sourceManager, int axis)
{
    // 空快照才允许捕获，保证原始尺寸不被后续受挤压值覆盖。
    if ( !sourceManager || !m_canvasDockSizeProtection.empty() ||
         (axis != ImGuiAxis_X && axis != ImGuiAxis_Y) ) {
        return;
    }

    // int 接口来自成员状态，此处规范化为有效 ImGuiAxis。
    const ImGuiAxis dockAxis = axis == ImGuiAxis_Y ? ImGuiAxis_Y : ImGuiAxis_X;
    // 局部 helper 将叶 Dock ID 解析为本次轴向上应保护的整棵子树。
    auto captureDockNode = [&](ImGuiID dockId) {
        if ( dockId == 0 ) {
            // 未停靠或尚未创建的画布没有可记录节点。
            return;
        }

        ImGuiDockNode* dockNode = ImGui::DockBuilderGetNode(dockId);
        if ( !dockNode ) {
            // DockBuilder 当前树中不存在该持久化 ID。
            return;
        }

        // 优先保护最近同轴父子树，找不到时退回叶节点本身。
        DockResizeTarget target = findDockNodeAxisTarget(dockNode, dockAxis);
        ImGuiDockNode*   node   = target.valid ? target.node : dockNode;
        if ( !node ) {
            return;
        }

        // 多个画布标签可能共享同一 DockNode，只记录一次。
        const ImGuiID protectedDockId = node->ID;
        const bool    alreadyCaptured = std::any_of(
            m_canvasDockSizeProtection.begin(),
            m_canvasDockSizeProtection.end(),
            [protectedDockId](const CanvasDockSizeSnapshot& snapshot) {
                return snapshot.dockId == protectedDockId;
            });
        if ( alreadyCaptured ) {
            // 去重避免恢复时重复调整同一子树和兄弟节点。
            return;
        }

        // 退化尺寸无法作为可靠恢复基线。
        const float axisSize = getAxisValue(node->Size, dockAxis);
        if ( axisSize <= 1.0f ) {
            return;
        }

        // 快照只持久化稳定 ID 和标量尺寸，不保存 DockNode 指针。
        m_canvasDockSizeProtection.push_back(
            CanvasDockSizeSnapshot{ protectedDockId, axisSize });
    };

    // 为所有会话画布和一个时间线预留容量，避免循环中反复分配。
    auto&         engine = Logic::EditorEngine::instance();
    const int32_t size   = engine.getSessionCount();
    m_canvasDockSizeProtection.reserve(static_cast<size_t>(size) + 1U);

    for ( int32_t i = 0; i < size; ++i ) {
        // SessionEntry 提供画布注册所使用的稳定 cameraId。
        const Logic::SessionEntry* entry = engine.getSessionEntry(i);
        if ( !entry || entry->cameraId.empty() ) {
            continue;
        }

        auto* canvas = sourceManager->getCanvasView(entry->cameraId);
        if ( !canvas ) {
            // 会话创建或关闭过渡中可能暂时没有对应 UI 画布。
            continue;
        }

        // 叶 Dock ID 由画布接口提供，不依赖具体 Canvas 类型。
        captureDockNode(canvas->getDockId());
    }

    // 时间线不属于普通谱面会话，使用稳定视图名单独捕获。
    auto* timeline = sourceManager->getCanvasView("TimelineWindow");
    if ( timeline ) {
        captureDockNode(timeline->getDockId());
    }
}

/// @brief 把一次侧栏越界拖拽挤压的画布子树恢复到捕获尺寸。
/// @param axis 捕获与恢复共同使用的 resize 轴向。
///
/// 每次通过稳定 Dock ID 获取当前节点，再复用兄弟补偿 helper 写回尺寸；已消失的
/// 节点安全跳过，快照保留到手势释放后统一清空。
/// @warning UI 热路径：仅在侧栏越过最小边界时遍历本手势的少量画布快照。
void FloatingManagerUI::restoreCanvasDockSizeProtection(int axis)
{
    if ( m_canvasDockSizeProtection.empty() ||
         (axis != ImGuiAxis_X && axis != ImGuiAxis_Y) ) {
        // 无快照或轴向无效时不访问 DockBuilder。
        return;
    }

    // 与捕获路径使用相同轴向规范化规则。
    const ImGuiAxis dockAxis = axis == ImGuiAxis_Y ? ImGuiAxis_Y : ImGuiAxis_X;
    for ( const CanvasDockSizeSnapshot& snapshot :
          m_canvasDockSizeProtection ) {
        if ( snapshot.dockId == 0 || snapshot.axisSize <= 1.0f ) {
            // 防御无效快照，避免将节点恢复为退化尺寸。
            continue;
        }

        ImGuiDockNode* dockNode = ImGui::DockBuilderGetNode(snapshot.dockId);
        if ( !dockNode ) {
            // DockSpace 重建后旧 ID 可能已不存在。
            continue;
        }

        // 一像素是恢复 helper 为当前节点保留的绝对下限。
        (void)resizeDockNodeAxis(dockNode, dockAxis, snapshot.axisSize, 1.0f);
    }
}

/// @brief 结束画布尺寸保护周期并释放快照容量中的元素。
///
/// vector 容量保留供下次手势复用，避免频繁堆分配。
void FloatingManagerUI::clearCanvasDockSizeProtection()
{
    // 只清除元素，不 shrink_to_fit。
    m_canvasDockSizeProtection.clear();
}

/// @brief 记录自动收回前所在 DockNode 的分割轴和侧向。
/// @param dockNode 收回时当前侧栏窗口的 DockNode。
///
/// 透明恢复热区需要知道原 split 轴、侧栏位于哪一侧以及应重新停靠的节点 ID。
/// 优先使用手势开始时捕获的父子 ID；解析失败时退回当前直接父节点。
void FloatingManagerUI::rememberCollapsedDockPlacement(ImGuiDockNode* dockNode)
{
    if ( !dockNode || !dockNode->ParentNode ) {
        // 缺少父 split 时采用左侧 X 轴默认值，并尽量保留叶 ID。
        m_collapsedDockAxis         = ImGuiAxis_X;
        m_collapsedDockId           = dockNode ? dockNode->ID : 0;
        m_collapsedDockIsFirstChild = true;
        return;
    }

    // 当前直接父轴用于手势目标失效时回退。
    const ImGuiAxis  axis   = dockNode->ParentNode->SplitAxis;
    DockResizeTarget target = findDockResizeTargetByIds(
        dockNode, m_dockResizeGestureSplitId, m_dockResizeGestureChildId);
    if ( !target.valid ) {
        // Dock 树若已变化，至少尝试当前直接父子关系。
        target = makeDockResizeTarget(dockNode, dockNode->ParentNode);
    }

    // 非 Y 轴统一归一化为 X，透明热区只处理两个有效方向。
    const ImGuiAxis rememberedAxis = target.valid ? target.axis : axis;
    m_collapsedDockAxis =
        rememberedAxis == ImGuiAxis_Y ? ImGuiAxis_Y : ImGuiAxis_X;
    // 保存叶节点 ID，恢复显示时交给 LayoutContext 请求重新停靠。
    m_collapsedDockId = dockNode->ID;
    // 侧向决定透明热区锚点和展开拖拽方向。
    m_collapsedDockIsFirstChild =
        target.valid ? target.isFirstChild
                     : dockNode->ParentNode->ChildNodes[0] == dockNode;
}

/// @brief 查询管理器的目标可见状态。
/// @return 侧栏应显示时返回 true；隐藏动画期间仍可能绘制残余帧。
bool FloatingManagerUI::isVisible() const
{
    return m_isVisible;
}

/// @brief 获取当前选择或最近隐藏的子视图标识。
/// @return 对成员字符串的只读引用，生命周期不超过管理器。
const std::string& FloatingManagerUI::getCurrentSubViewId() const
{
    return m_currentSubViewId;
}

/// @brief 判断当前可见子视图是否需要并行准备。
/// @param snapshot 当前帧 UI 快照。
/// @return 当前子视图需要准备时返回 true。
///
/// 管理器只转发当前可见子视图的能力；隐藏项不会消耗准备时间，也不会访问其缓存。
/// @warning UI 热路径：每帧准备枚举时调用，只做 map 查找和能力查询。
bool FloatingManagerUI::needsParallelUiPrepare(
    const UiFrameSnapshot& snapshot) const
{
    if ( !m_isVisible ) {
        // 隐藏动画不更新业务数据，绘制仍使用最后一帧结果。
        return false;
    }

    auto it = m_subViews.find(m_currentSubViewId);
    if ( it == m_subViews.end() ) {
        // 工作区中的陈旧 ID 不具备可准备对象。
        return false;
    }

    // 子视图自行比较快照版本与内部缓存状态。
    IParallelUiPreparable* preparable = it->second->asParallelUiPreparable();
    return preparable && preparable->needsParallelUiPrepare(snapshot);
}

/// @brief 查询当前可见子视图是否必须在 UI 主线程准备。
/// @return 子视图准备逻辑会访问 ImGui 时返回 true。
///
/// 不存在或未实现能力接口时保守返回 true，防止未知实现被调度到工作线程。
/// @warning UI 热路径：只查询当前子视图能力，不复制所有权。
bool FloatingManagerUI::requiresMainThreadUiPrepare() const
{
    auto it = m_subViews.find(m_currentSubViewId);
    if ( it == m_subViews.end() ) {
        // 无有效子视图时采用最安全的线程约束。
        return true;
    }

    // 能力缺失同样视为只能由传统 UI 主线程路径处理。
    IParallelUiPreparable* preparable = it->second->asParallelUiPreparable();
    return !preparable || preparable->requiresMainThreadUiPrepare();
}

/// @brief 按当前子视图的线程约束准备数据。
/// @param snapshot 当前帧 UI 快照。
///
/// UIManager 已根据 requiresMainThreadUiPrepare
/// 完成分组，本函数只转发不可变快照。
/// @warning 可能在工作线程调用；不得在管理器层访问 ImGui 或改变子视图注册表。
void FloatingManagerUI::prepareUiFrameData(const UiFrameSnapshot& snapshot)
{
    auto it = m_subViews.find(m_currentSubViewId);
    if ( it == m_subViews.end() ) {
        // 子视图可能在准备任务调度前被工作区切换移除。
        return;
    }

    if ( IParallelUiPreparable* preparable =
             it->second->asParallelUiPreparable() ) {
        // 能力对象由子视图自身提供，生命周期受 m_subViews 稳定所有权保护。
        preparable->prepareUiFrameData(snapshot);
    }
}

/// @brief 将当前子视图准备结果切换到主线程可读状态。
///
/// 该步骤在所有准备任务汇合后执行，子视图可用交换缓冲避免绘制读取半成品。
/// @warning UI 热路径：UI 主线程每帧至多调用一次，不执行阻塞等待。
void FloatingManagerUI::swapPreparedUiFrameData()
{
    auto it = m_subViews.find(m_currentSubViewId);
    if ( it == m_subViews.end() ) {
        // 当前项已失效时没有可发布的准备结果。
        return;
    }

    if ( IParallelUiPreparable* preparable =
             it->second->asParallelUiPreparable() ) {
        // 仅能力子视图拥有双缓冲发布阶段。
        preparable->swapPreparedUiFrameData();
    }
}

/// @brief 在主 DockSpace 渲染前应用停靠最小尺寸约束。
/// @param sourceManager 用于同步子视图和定位需要保护的画布。
///
/// 该前置阶段必须早于 ImGui DockSpace 布局：它解析上一帧窗口与 DockNode，捕获
/// 新 resize 手势，锁定侧栏最小尺寸，并临时钳住鼠标坐标，避免 ImGui 把继续拖动
/// 传播给兄弟画布。拖过确认阈值后转入自动收回状态。
/// @warning UI 热路径：可见侧栏每帧执行；仅在越界拖拽时写 DockNode 几何。
void FloatingManagerUI::applyDockResizeConstraintsBeforeDockSpace(
    UIManager* sourceManager)
{
    // 隐藏侧栏没有现存窗口可在 DockSpace 前约束。
    if ( !m_isVisible ) return;
    if ( m_collapsedResizeDragActive &&
         ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
        // 透明热区恢复手势由 update 内创建窗口后接管。
        return;
    }

    // 最小内容尺寸和窗口装饰都使用当前 DPI。
    const float dpiScale =
        MMM::Config::AppConfig::instance().getWindowContentScale();
    auto it = m_subViews.find(m_currentSubViewId);
    if ( it == m_subViews.end() ) {
        // 当前 ID 已不再注册时同步隐藏并清理侧栏状态。
        hideCurrentSubView(sourceManager);
        return;
    }

    // 子视图先同步项目生命周期，确保其最小尺寸匹配当前内容模式。
    it->second->syncProjectUiState(sourceManager);

    // 项目过渡期间使用稳定占位尺寸，正常时采用子视图契约。
    const bool projectTransition =
        sourceManager && sourceManager->isProjectTransitionInProgress();
    const ImVec2 minWindowSize = toWindowMinSize(
        projectTransition ? getProjectTransitionMinContentSize(dpiScale)
                          : it->second->getMinContentSize(dpiScale),
        dpiScale);
    // ### 后缀与 update 中一致，标题变化不改变内部窗口 ID。
    const std::string windowName = m_currentSubViewId + "###" + m_name;
    ImGuiWindow*      window     = ImGui::FindWindowByName(windowName.c_str());
    if ( !window || !window->DockNode ) {
        // 首帧或浮动窗口由常规 ImGui 尺寸约束处理。
        return;
    }

    // 捕获新手势后立即修复已低于最小值的节点尺寸。
    ImGuiDockNode* dockNode = window->DockNode;
    updateDockResizeGesture(dockNode);
    ImVec2 currentWindowSize =
        clampDockNodeToMinSize(window, dockNode->Size, minWindowSize);
    // 首选按手势保存 ID 解析实际被拖动的嵌套 split。
    DockResizeTarget resizeTarget = findDockResizeTargetByIds(
        dockNode, m_dockResizeGestureSplitId, m_dockResizeGestureChildId);
    if ( !resizeTarget.valid && m_dockResizeGestureActive ) {
        // 简单布局可直接使用当前节点和直接父 split 回退。
        DockResizeTarget directTarget =
            makeDockResizeTarget(dockNode, dockNode->ParentNode);
        if ( directTarget.valid &&
             m_dockResizeGestureAxis == static_cast<int>(directTarget.axis) ) {
            resizeTarget = directTarget;
        }
    }
    // 前一轮已钳制时使用保存的真实鼠标位置继续计算越界。
    const ImVec2            dockResizeMousePos = m_restoreMouseAfterDockSpace
                                                     ? m_mousePosBeforeDockClamp
                                                     : ImGui::GetMousePos();
    const DockResizeOverrun dockResizeOverrun =
        getDockResizeOverrun(resizeTarget, minWindowSize, dockResizeMousePos);

    if ( !dockResizeOverrun.valid || !m_dockResizeGestureActive ||
         m_dockResizeGestureAxis != static_cast<int>(dockResizeOverrun.axis) ||
         !ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
        // 非有效按住手势不保持最小边界锁。
        m_minResizeLockActive = false;
        m_minResizeLockAxis   = -1;
        if ( !ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
            // 鼠标释放结束本轮画布尺寸保护周期。
            clearCanvasDockSizeProtection();
        }
        return;
    }

    // 首次有效越界前记录同轴画布尺寸，防止兄弟子树被挤压。
    captureCanvasDockSizeProtection(sourceManager, dockResizeOverrun.axis);

    const float axisSize =
        getAxisValue(currentWindowSize, dockResizeOverrun.axis);
    const float axisMinSize =
        getAxisValue(minWindowSize, dockResizeOverrun.axis);
    // 半像素容差与窗口最小尺寸判定保持一致。
    const bool atMinBoundary = axisSize <= axisMinSize + 0.5f;

    if ( atMinBoundary && dockResizeOverrun.overrun > 0.0f ) {
        // 把命中的侧栏子树精确锁回最小尺寸。
        const float lockedAxisSize =
            lockDockResizeTargetToMinSize(resizeTarget, minWindowSize);
        if ( resizeTarget.node == dockNode && lockedAxisSize > 0.0f ) {
            // 直接命中当前节点时同步调用点的局部尺寸快照。
            setAxisValue(
                currentWindowSize, dockResizeOverrun.axis, lockedAxisSize);
        }
        // 修复 ImGui 对其他画布子树产生的同轴挤压。
        restoreCanvasDockSizeProtection(dockResizeOverrun.axis);

        if ( !m_minResizeLockActive ||
             m_minResizeLockAxis != static_cast<int>(dockResizeOverrun.axis) ) {
            // 记录首次锁定时越界量，之后阈值按相对拖动计算。
            m_minResizeLockActive       = true;
            m_minResizeLockAxis         = dockResizeOverrun.axis;
            m_minResizeLockStartOverrun = dockResizeOverrun.overrun;
        }

        if ( shouldCollapseAfterMinDragStart(dockResizeOverrun.overrun,
                                             m_minResizeLockStartOverrun,
                                             minWindowSize,
                                             dockResizeOverrun.axis) ) {
            // 保存原停靠方向后隐藏，并保留透明恢复占位。
            rememberCollapsedDockPlacement(dockNode);
            hideCurrentSubView(sourceManager, true);
            restoreDockResizeMouseAfterDockSpace();
            return;
        }

        // ImGui DockSpace 仍会读取 IO；临时钳到边界阻止它继续压缩兄弟。
        ImGuiIO& io = ImGui::GetIO();
        if ( !m_restoreMouseAfterDockSpace ) {
            // 真实位置和增量只在首次钳制时保存，帧末完整恢复。
            m_mousePosBeforeDockClamp    = io.MousePos;
            m_mouseDeltaBeforeDockClamp  = io.MouseDelta;
            m_restoreMouseAfterDockSpace = true;
        }
        // 只钳制 resize 轴，另一轴保持正常鼠标交互。
        setAxisValue(
            io.MousePos, dockResizeOverrun.axis, dockResizeOverrun.boundary);
        setAxisValue(io.MouseDelta, dockResizeOverrun.axis, 0.0f);
    } else if ( m_minResizeLockActive && dockResizeOverrun.overrun <= 0.0f ) {
        // 用户拖回有效范围时立即解除边界锁。
        m_minResizeLockActive = false;
        m_minResizeLockAxis   = -1;
    }
}

/// @brief 恢复被临时钳住的鼠标位置。
///
/// DockSpace 已消费被限制到最小边界的坐标后，必须把真实位置和增量写回 ImGuiIO，
/// 让同一帧后续控件与下一帧手势继续看到物理鼠标轨迹。
/// @warning UI 热路径：帧末调用；仅在前置约束实际钳制过输入时写入 ImGuiIO。
void FloatingManagerUI::restoreDockResizeMouseAfterDockSpace()
{
    if ( !m_restoreMouseAfterDockSpace ) {
        // 常见路径没有保存值，无需触碰全局输入状态。
        return;
    }

    // 位置与增量必须成对恢复，避免后续控件看到不一致速度。
    ImGui::GetIO().MousePos      = m_mousePosBeforeDockClamp;
    ImGui::GetIO().MouseDelta    = m_mouseDeltaBeforeDockClamp;
    m_restoreMouseAfterDockSpace = false;
}

/// @brief 隐藏当前子视图并同步取消侧边栏选中状态。
/// @param sourceManager 事件中携带的 UI 管理器来源。
/// @param keepCollapsedPlaceholder 是否保留可拖出的透明边缘热区。
///
/// 正常按钮隐藏不保留占位；越过最小尺寸触发的自动收回保留当前 ID、原 Dock
/// 放置和透明热区。函数向侧栏按钮发布关闭状态，并终止所有进行中的 resize 状态。
void FloatingManagerUI::hideCurrentSubView(UIManager* sourceManager,
                                           bool       keepCollapsedPlaceholder)
{
    if ( !m_isVisible || m_currentSubViewId.empty() ) {
        // 已隐藏路径仍根据有效当前项决定是否保留自动收回占位。
        m_isVisible = false;
        m_isAutoCollapsed =
            keepCollapsedPlaceholder && !m_currentSubViewId.empty() &&
            m_subViews.find(m_currentSubViewId) != m_subViews.end();
        // 无需重复发布关闭事件，避免订阅者收到相同状态。
        return;
    }

    // 事件以管理器自身为来源，构造函数回调会排除回流。
    MMM::Event::UISubViewToggleEvent evt;
    evt.sourceUiName           = m_name;
    evt.uiManager              = sourceManager;
    evt.targetFloatManagerName = m_name;
    evt.subViewId              = m_currentSubViewId;
    evt.showSubView            = false;
    // 侧栏按钮订阅此事件后取消对应选中态。
    MMM::Event::EventBus::instance().publish(evt);
    // 隐藏目标立即生效，透明度可继续按现有动画量递减。
    m_isVisible            = false;
    m_hasSeenUsableSize    = false;
    m_requestShowSizeReset = false;
    m_wasDocked            = false;
    m_minResizeLockActive  = false;
    // 只有自动收回路径保留边缘恢复入口。
    m_isAutoCollapsed          = keepCollapsedPlaceholder;
    m_minResizeLockAxis        = -1;
    m_dockResizeGestureActive  = false;
    m_dockResizeGestureAxis    = -1;
    m_dockResizeGestureSplitId = 0;
    m_dockResizeGestureChildId = 0;
    // 原停靠轴、侧向和 Dock ID 不在这里清除，供透明热区定位使用。
    m_collapsedResizeDragActive        = false;
    m_collapsedResizeDragAxis          = -1;
    m_collapsedResizeResumeStateLogged = false;
    m_collapsedResizeResumeApplyLogged = false;
    m_restoreMouseAfterDockSpace       = false;
    // 结束保护周期，但保留 vector 容量供下一次手势复用。
    clearCanvasDockSizeProtection();
}

/// @brief 从自动收回透明拖拽热区恢复当前子视图并同步侧边栏选中状态。
/// @param sourceManager 事件中携带的 UI 管理器来源。
///
/// 恢复发生在鼠标仍按住且已越过展开阈值的同一手势中，因此窗口初始尺寸允许从
/// 一个像素继续增长，并把 resize 活动状态传递给正常窗口 update。
void FloatingManagerUI::showCurrentSubViewFromCollapsedOverlay(
    UIManager* sourceManager)
{
    if ( m_currentSubViewId.empty() ||
         m_subViews.find(m_currentSubViewId) == m_subViews.end() ) {
        // 当前项失效时删除残留占位，避免永久透明热区。
        m_isAutoCollapsed = false;
        return;
    }

    // 通知侧栏按钮恢复对应选中态；自身订阅会忽略该来源。
    MMM::Event::UISubViewToggleEvent evt;
    evt.sourceUiName           = m_name;
    evt.uiManager              = sourceManager;
    evt.targetFloatManagerName = m_name;
    evt.subViewId              = m_currentSubViewId;
    evt.showSubView            = true;
    // 发布后本地状态仍由本函数显式恢复，不依赖异步回调顺序。
    MMM::Event::EventBus::instance().publish(evt);

    // 视图立即可见，但不强制最小尺寸，以便沿用当前拖出距离。
    m_isVisible            = true;
    m_hasSeenUsableSize    = true;
    m_requestShowSizeReset = false;
    // 标记曾经停靠，防止恢复帧被当作首次浮动窗口重置。
    m_wasDocked           = true;
    m_minResizeLockActive = false;
    m_isAutoCollapsed     = false;
    m_minResizeLockAxis   = -1;
    // 把透明热区捕获的轴向传给正常 Dock resize 状态机。
    m_dockResizeGestureActive = true;
    m_dockResizeGestureAxis   = m_collapsedResizeDragAxis;
    // 父子 ID 在重新创建窗口后解析，当前先清零旧树信息。
    m_dockResizeGestureSplitId = 0;
    m_dockResizeGestureChildId = 0;
    // 保持 drag active，让 update 本帧应用 requestedAxisSize。
    m_collapsedResizeDragActive = true;
    // 新恢复周期允许各输出一次状态和应用诊断日志。
    m_collapsedResizeResumeStateLogged = false;
    m_collapsedResizeResumeApplyLogged = false;
    m_restoreMouseAfterDockSpace       = false;
    // 新手势尚未捕获画布保护尺寸。
    clearCanvasDockSizeProtection();
}

/// @brief 绘制自动收回后的透明拖拽热区，并在拖出阈值后恢复子视图。
/// @param sourceManager 用于同步项目状态并随切换事件传递来源。
/// @return 本帧是否已触发子视图恢复。
///
/// 热区覆盖原 DockHost 边缘但不绘制背景；hover 或 active 时只绘制 ImGui
/// Separator 反馈。按住后按原侧向计算正向拖出距离，超过半个最小尺寸才恢复窗口。
/// @warning UI 热路径：自动收回期间每帧调用；只创建一个透明 ImGui 窗口和热区。
bool FloatingManagerUI::renderCollapsedResizeOverlay(UIManager* sourceManager)
{
    if ( !m_isAutoCollapsed || m_currentSubViewId.empty() ) {
        // 没有有效占位状态时不创建额外 ImGui 窗口。
        return false;
    }

    auto it = m_subViews.find(m_currentSubViewId);
    if ( it == m_subViews.end() ) {
        // 注册项已移除时废弃自动收回状态。
        m_isAutoCollapsed = false;
        return false;
    }

    // 项目模式可能改变子视图最小尺寸，绘制前先同步。
    it->second->syncProjectUiState(sourceManager);

    // 展开阈值使用当前内容或项目过渡占位的窗口最小尺寸。
    const float dpiScale =
        MMM::Config::AppConfig::instance().getWindowContentScale();
    const bool projectTransition =
        sourceManager && sourceManager->isProjectTransitionInProgress();
    const ImVec2 minWindowSize = toWindowMinSize(
        projectTransition ? getProjectTransitionMinContentSize(dpiScale)
                          : it->second->getMinContentSize(dpiScale),
        dpiScale);
    ImGuiWindow* hostWindow = ImGui::FindWindowByName("RightDockHost");
    if ( !hostWindow ) {
        // 主 DockHost 尚未创建时结束本轮拖拽但保留占位等待后续帧。
        m_collapsedResizeDragActive        = false;
        m_collapsedResizeDragAxis          = -1;
        m_collapsedResizeResumeStateLogged = false;
        m_collapsedResizeResumeApplyLogged = false;
        return false;
    }

    // 保存值只允许 X/Y，两者之外回退到 X 轴。
    const ImGuiAxis axis =
        m_collapsedDockAxis == ImGuiAxis_Y ? ImGuiAxis_Y : ImGuiAxis_X;
    const float  hitSize = getCollapsedOverlayHitSize(dpiScale);
    const ImRect hitRect = makeCollapsedOverlayRect(
        hostWindow, axis, m_collapsedDockIsFirstChild, hitSize);
    if ( hitRect.GetWidth() <= 0.0f || hitRect.GetHeight() <= 0.0f ) {
        // 无效宿主几何不能建立可点击热区。
        m_collapsedResizeDragActive        = false;
        m_collapsedResizeDragAxis          = -1;
        m_collapsedResizeResumeStateLogged = false;
        m_collapsedResizeResumeApplyLogged = false;
        return false;
    }

    ImGui::SetNextWindowPos(hitRect.Min, ImGuiCond_Always);
    ImGui::SetNextWindowSize(hitRect.GetSize(), ImGuiCond_Always);
    ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);

    const std::string overlayName = "##CollapsedResizeOverlay_" + m_name;
    ImGuiWindowFlags  overlayFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoNavFocus;
    ImGui::Begin(overlayName.c_str(), nullptr, overlayFlags);
    ImGui::SetCursorScreenPos(hitRect.Min);
    ImGui::InvisibleButton("##CollapsedResizeHotZone", hitRect.GetSize());

    const bool overlayActive =
        ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool overlayHovered = ImGui::IsItemHovered();
    if ( overlayHovered || overlayActive ) {
        ImGui::SetMouseCursor(axis == ImGuiAxis_Y ? ImGuiMouseCursor_ResizeNS
                                                  : ImGuiMouseCursor_ResizeEW);
    }

    bool  shouldShowSubView = false;
    float mouseAxis         = 0.0f;
    float dragDistance      = 0.0f;
    float separatorAxis = axis == ImGuiAxis_Y
                              ? (m_collapsedDockIsFirstChild
                                     ? hostWindow->Pos.y
                                     : hostWindow->Pos.y + hostWindow->Size.y)
                              : (m_collapsedDockIsFirstChild
                                     ? hostWindow->Pos.x
                                     : hostWindow->Pos.x + hostWindow->Size.x);

    if ( !overlayActive ) {
        m_collapsedResizeDragActive        = false;
        m_collapsedResizeDragAxis          = -1;
        m_collapsedResizeResumeStateLogged = false;
        m_collapsedResizeResumeApplyLogged = false;
    } else {
        mouseAxis = getAxisValue(ImGui::GetMousePos(), axis);
        if ( !m_collapsedResizeDragActive ||
             m_collapsedResizeDragAxis != static_cast<int>(axis) ) {
            m_collapsedResizeDragActive         = true;
            m_collapsedResizeDragAxis           = axis;
            m_collapsedResizeDragStartMouseAxis = mouseAxis;
        }

        dragDistance = m_collapsedDockIsFirstChild
                           ? mouseAxis - m_collapsedResizeDragStartMouseAxis
                           : m_collapsedResizeDragStartMouseAxis - mouseAxis;
        separatorAxis += (m_collapsedDockIsFirstChild ? 1.0f : -1.0f) *
                         std::max(0.0f, dragDistance);
        if ( shouldExpandAfterCollapsedDragStart(
                 std::max(0.0f, dragDistance), 0.0f, minWindowSize, axis) ) {
            shouldShowSubView = true;
        }
    }

    if ( overlayHovered || overlayActive ) {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float       thickness =
            std::max(1.0f, std::floor(style.DockingSeparatorSize));
        const ImRect separatorRect = makeCollapsedSeparatorRect(
            hostWindow, axis, separatorAxis, thickness);
        const ImU32 separatorColor =
            ImGui::GetColorU32(overlayActive ? ImGuiCol_SeparatorActive
                                             : ImGuiCol_SeparatorHovered);
        ImGui::GetForegroundDrawList()->AddRectFilled(
            separatorRect.Min, separatorRect.Max, separatorColor);
    }

    ImGui::End();
    ImGui::PopStyleVar(3);

    if ( shouldShowSubView ) {
        XINFO(
            "Sidebar collapsed resize threshold: manager={}, subView={}, "
            "axis={}, dockId={}, firstChild={}, dragDistance={}, "
            "minAxisSize={}, startMouseAxis={}, mouseAxis={}",
            m_name,
            m_currentSubViewId,
            static_cast<int>(axis),
            m_collapsedDockId,
            m_collapsedDockIsFirstChild,
            std::max(0.0f, dragDistance),
            getAxisValue(minWindowSize, axis),
            m_collapsedResizeDragStartMouseAxis,
            mouseAxis);
        showCurrentSubViewFromCollapsedOverlay(sourceManager);
        return true;
    }
    return false;
}

void FloatingManagerUI::update(UIManager* sourceManager)
{
    if ( m_isAutoCollapsed ) {
        m_visibilityAnimAmount = 0.0f;
    } else {
        m_visibilityAnimAmount =
            updateFloatingVisibilityAmount(m_visibilityAnimAmount, m_isVisible);
    }

    if ( !m_isVisible ) {
        if ( m_visibilityAnimAmount <= FLOATING_MANAGER_HIDDEN_EPSILON ) {
            m_visibilityAnimAmount = 0.0f;
            if ( !renderCollapsedResizeOverlay(sourceManager) ) {
                return;
            }
        } else if ( m_currentSubViewId.empty() ) {
            return;
        }
    }

    const float dpiScale =
        MMM::Config::AppConfig::instance().getWindowContentScale();
    auto it = m_subViews.find(m_currentSubViewId);
    if ( it == m_subViews.end() ) {
        hideCurrentSubView(sourceManager);
        return;
    }

    it->second->syncProjectUiState(sourceManager);

    const bool projectTransition =
        sourceManager && sourceManager->isProjectTransitionInProgress();
    const ImVec2 minWindowSize = toWindowMinSize(
        projectTransition ? getProjectTransitionMinContentSize(dpiScale)
                          : it->second->getMinContentSize(dpiScale),
        dpiScale);
    const bool resumeCollapsedResize =
        m_collapsedResizeDragActive &&
        ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        (m_collapsedResizeDragAxis == ImGuiAxis_X ||
         m_collapsedResizeDragAxis == ImGuiAxis_Y);
    const ImGuiAxis resumeAxis =
        m_collapsedResizeDragAxis == ImGuiAxis_Y ? ImGuiAxis_Y : ImGuiAxis_X;
    const float resumeMouseAxis =
        resumeCollapsedResize ? getAxisValue(ImGui::GetMousePos(), resumeAxis)
                              : 0.0f;
    const float requestedAxisSize =
        resumeCollapsedResize
            ? getCollapsedResumeDragSize(resumeMouseAxis,
                                         m_collapsedDockIsFirstChild,
                                         m_collapsedResizeDragStartMouseAxis)
            : 0.0f;
    const ImVec2 activeMinWindowSize =
        resumeCollapsedResize ? ImVec2(1.0f, 1.0f) : minWindowSize;
    ImGui::SetNextWindowSizeConstraints(activeMinWindowSize,
                                        ImVec2(FLT_MAX, FLT_MAX));
    if ( !resumeCollapsedResize &&
         (m_requestShowSizeReset || !m_hasSeenUsableSize) ) {
        ImGui::SetNextWindowSize(minWindowSize, ImGuiCond_Always);
    }

    // 使用 ### 后缀强制固定 ImGui 内部窗口
    // ID，即使显示的标题变化也不会丢失停靠状态
    std::string   windowName   = m_currentSubViewId + "###" + m_name;
    const ImGuiID resumeDockId = resumeCollapsedResize ? m_collapsedDockId : 0;
    const float   visibilityAlpha = m_isVisible
                                        ? easeOutCubic(m_visibilityAnimAmount)
                                        : smoothStep(m_visibilityAnimAmount);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                        ImGui::GetStyle().Alpha * visibilityAlpha);
    {
        LayoutContext  lctx{ m_layoutCtx,     windowName,
                             false,           ImGuiWindowFlags_NoTitleBar,
                             nullptr,         resumeDockId,
                             ImGuiCond_Always };
        ImVec2         currentWindowSize = ImGui::GetWindowSize();
        const bool     isDocked          = ImGui::IsWindowDocked();
        ImGuiWindow*   currentWindow     = ImGui::GetCurrentWindow();
        ImGuiDockNode* dockNode =
            isDocked && currentWindow ? currentWindow->DockNode : nullptr;
        if ( resumeCollapsedResize ) {
            m_dockResizeGestureActive  = true;
            m_dockResizeGestureAxis    = resumeAxis;
            m_dockResizeGestureSplitId = 0;
            m_dockResizeGestureChildId = 0;
            m_minResizeLockActive      = false;
            m_minResizeLockAxis        = -1;
            if ( !m_collapsedResizeResumeStateLogged ) {
                XINFO(
                    "Sidebar collapsed resize resume begin: manager={}, "
                    "subView={}, axis={}, savedDockId={}, windowDockId={}, "
                    "dockNodeId={}, isDocked={}, hasDockNode={}, hasParent={}, "
                    "firstChild={}, requestedAxisSize={}, startMouseAxis={}, "
                    "mouseAxis={}",
                    m_name,
                    m_currentSubViewId,
                    static_cast<int>(resumeAxis),
                    m_collapsedDockId,
                    currentWindow ? currentWindow->DockId : 0,
                    dockNode ? dockNode->ID : 0,
                    isDocked,
                    dockNode != nullptr,
                    dockNode && dockNode->ParentNode,
                    m_collapsedDockIsFirstChild,
                    requestedAxisSize,
                    m_collapsedResizeDragStartMouseAxis,
                    resumeMouseAxis);
                m_collapsedResizeResumeStateLogged = true;
            }
        } else {
            updateDockResizeGesture(dockNode);
        }
        DockResizeTarget resizeTarget = findDockResizeTargetByIds(
            dockNode, m_dockResizeGestureSplitId, m_dockResizeGestureChildId);
        if ( !resizeTarget.valid && m_dockResizeGestureActive ) {
            DockResizeTarget directTarget =
                makeDockResizeTarget(dockNode, dockNode->ParentNode);
            if ( directTarget.valid &&
                 m_dockResizeGestureAxis ==
                     static_cast<int>(directTarget.axis) ) {
                resizeTarget = directTarget;
            }
        }
        const ImVec2 dockResizeMousePos = m_restoreMouseAfterDockSpace
                                              ? m_mousePosBeforeDockClamp
                                              : ImGui::GetMousePos();
        const DockResizeOverrun dockResizeOverrun = getDockResizeOverrun(
            resizeTarget, minWindowSize, dockResizeMousePos);
        bool collapsedDuringUpdate = false;
        if ( resumeCollapsedResize && isDocked && dockNode ) {
            const float appliedAxisSize = resizeDockNodeAxis(
                dockNode, resumeAxis, requestedAxisSize, 1.0f);
            setAxisValue(currentWindow->Size, resumeAxis, appliedAxisSize);
            setAxisValue(currentWindow->SizeFull, resumeAxis, appliedAxisSize);
            currentWindowSize = dockNode->Size;
            if ( !m_collapsedResizeResumeApplyLogged ) {
                XINFO(
                    "Sidebar collapsed resize resume applied: manager={}, "
                    "subView={}, axis={}, dockNodeId={}, parentAxis={}, "
                    "requestedAxisSize={}, appliedAxisSize={}, nodeAxisSize={}",
                    m_name,
                    m_currentSubViewId,
                    static_cast<int>(resumeAxis),
                    dockNode->ID,
                    dockNode->ParentNode
                        ? static_cast<int>(dockNode->ParentNode->SplitAxis)
                        : -1,
                    requestedAxisSize,
                    appliedAxisSize,
                    getAxisValue(dockNode->Size, resumeAxis));
                m_collapsedResizeResumeApplyLogged = true;
            }
            ImGui::SetMouseCursor(resumeAxis == ImGuiAxis_Y
                                      ? ImGuiMouseCursor_ResizeNS
                                      : ImGuiMouseCursor_ResizeEW);
            if ( ImGuiWindow* hostWindow = dockNode->HostWindow ) {
                const ImGuiStyle& style = ImGui::GetStyle();
                const float       thickness =
                    std::max(1.0f, std::floor(style.DockingSeparatorSize));
                const float separatorAxis =
                    m_collapsedDockIsFirstChild
                        ? getAxisValue(dockNode->Pos, resumeAxis) +
                              appliedAxisSize
                        : getAxisValue(dockNode->Pos, resumeAxis);
                const ImRect separatorRect = makeCollapsedSeparatorRect(
                    hostWindow, resumeAxis, separatorAxis, thickness);
                ImGui::GetForegroundDrawList()->AddRectFilled(
                    separatorRect.Min,
                    separatorRect.Max,
                    ImGui::GetColorU32(ImGuiCol_SeparatorActive));
            }
        }
        if ( !resumeCollapsedResize && m_wasDocked && !isDocked ) {
            currentWindowSize.x =
                std::max(currentWindowSize.x, minWindowSize.x);
            currentWindowSize.y = minWindowSize.y;
            ImGui::SetWindowSize(currentWindowSize, ImGuiCond_Always);
        }
        m_wasDocked =
            resumeCollapsedResize ? (m_wasDocked || isDocked) : isDocked;
        m_requestShowSizeReset = false;
        if ( isDocked && !resumeCollapsedResize ) {
            currentWindowSize = clampDockNodeToMinSize(
                currentWindow, currentWindowSize, minWindowSize);
        }

        const bool belowMin =
            isBelowMinWindowSize(currentWindowSize, minWindowSize);
        if ( !belowMin ) {
            m_hasSeenUsableSize = true;
        }

        if ( !isDocked || !dockResizeOverrun.valid ||
             !m_dockResizeGestureActive ||
             m_dockResizeGestureAxis !=
                 static_cast<int>(dockResizeOverrun.axis) ||
             !ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
            m_minResizeLockActive = false;
            m_minResizeLockAxis   = -1;
            if ( !ImGui::IsMouseDown(ImGuiMouseButton_Left) ) {
                clearCanvasDockSizeProtection();
            }
        } else {
            captureCanvasDockSizeProtection(sourceManager,
                                            dockResizeOverrun.axis);

            const float axisSize =
                getAxisValue(currentWindowSize, dockResizeOverrun.axis);
            const float axisMinSize =
                getAxisValue(minWindowSize, dockResizeOverrun.axis);
            const bool atMinBoundary = axisSize <= axisMinSize + 0.5f;

            if ( atMinBoundary && dockResizeOverrun.overrun > 0.0f ) {
                const float lockedAxisSize =
                    lockDockResizeTargetToMinSize(resizeTarget, minWindowSize);
                if ( resizeTarget.node == dockNode && lockedAxisSize > 0.0f ) {
                    setAxisValue(currentWindowSize,
                                 dockResizeOverrun.axis,
                                 lockedAxisSize);
                }
                restoreCanvasDockSizeProtection(dockResizeOverrun.axis);

                if ( !m_minResizeLockActive ||
                     m_minResizeLockAxis !=
                         static_cast<int>(dockResizeOverrun.axis) ) {
                    m_minResizeLockActive       = true;
                    m_minResizeLockAxis         = dockResizeOverrun.axis;
                    m_minResizeLockStartOverrun = dockResizeOverrun.overrun;
                }

                if ( shouldCollapseAfterMinDragStart(
                         dockResizeOverrun.overrun,
                         m_minResizeLockStartOverrun,
                         minWindowSize,
                         dockResizeOverrun.axis) ) {
                    rememberCollapsedDockPlacement(dockNode);
                    hideCurrentSubView(sourceManager, true);
                    collapsedDuringUpdate = true;
                }
            } else if ( m_minResizeLockActive &&
                        dockResizeOverrun.overrun <= 0.0f ) {
                m_minResizeLockActive = false;
                m_minResizeLockAxis   = -1;
            }
        }

        if ( !collapsedDuringUpdate ) {
            if ( !m_isVisible ) {
                ImGui::BeginDisabled();
            }
            if ( projectTransition ) {
                MMM::UI::Utils::renderProjectTransitionPlaceholder();
            } else {
                it->second->onUpdate(lctx, sourceManager);
            }
            if ( !m_isVisible ) {
                ImGui::EndDisabled();
            }
        }
    }
    ImGui::PopStyleVar();
}

/// @brief 是否需要重载
/// @return 管理器自身或当前子视图存在一次性纹理重载请求时返回 true。
///
/// 管理器脏位通过 exchange 消费，子视图脏位由其实现自行管理；隐藏或不存在的
/// 当前项不会触发无关子视图扫描。
bool FloatingManagerUI::needReload()
{
    // 管理器级请求优先返回，避免同帧重复查询子视图状态。
    if ( std::exchange(m_needReload, false) ) return true;
    // 只转发当前选择项的纹理需求。
    const auto iterator = m_subViews.find(m_currentSubViewId);
    return iterator != m_subViews.end() &&
           iterator->second->needsTextureReload();
}

/// @brief 重载当前子视图请求的纹理。
/// @param physicalDevice Vulkan 物理设备。
/// @param logicalDevice Vulkan 逻辑设备。
/// @param cmdPool 上传命令池。
/// @param queue 上传命令提交队列。
///
/// 资源参数原样转发给当前子视图；管理器不创建或持有具体纹理资源。
void FloatingManagerUI::reloadTextures(vk::PhysicalDevice& physicalDevice,
                                       vk::Device&         logicalDevice,
                                       vk::CommandPool&    cmdPool,
                                       vk::Queue&          queue)
{
    // 工作区切换期间当前 ID 可能暂时没有注册对象。
    const auto iterator = m_subViews.find(m_currentSubViewId);
    if ( iterator != m_subViews.end() &&
         iterator->second->needsTextureReload() ) {
        // 再次检查脏位，避免无请求时执行昂贵上传。
        iterator->second->reloadTextures(
            physicalDevice, logicalDevice, cmdPool, queue);
    }
}

}  // namespace MMM::UI
