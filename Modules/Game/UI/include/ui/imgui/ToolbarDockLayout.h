#pragma once

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace MMM::UI
{
/// @brief 获取 UI 线程持有的待处理停靠请求；None 表示解除停靠。
/// @return 只在用户请求和主宿主布局之间传递的值，不持有节点指针。
/// @warning UI 热路径：常态只检查 optional，不修改树、不访问文件系统。
inline std::optional<ImGuiDir>& pendingToolbarDockRequest()
{
    // 工具栏绘制时不能拆分自己的活跃节点，延迟到下一帧 DockSpace 提交前。
    static std::optional<ImGuiDir> request;
    return request;
}

/// @brief 从持久化的边缘名称获得 ImGui 拆分方向。
/// @param edge 已校验或来自历史配置的边缘名称。
/// @return 未识别的名称回退到右侧竖排。
inline ImGuiDir toolbarDockDirection(std::string_view edge)
{
    if ( edge == "top" ) return ImGuiDir_Up;
    if ( edge == "bottom" ) return ImGuiDir_Down;
    if ( edge == "left" ) return ImGuiDir_Left;
    return ImGuiDir_Right;
}

/// @brief 固定工具栏与中心停靠区域的互不重叠矩形。
/// @details 固定工具栏不属于 DockSpace；四边使用同一套几何分配规则。
struct ToolbarWorkspaceGeometry {
    /// @brief 扣除工具栏占位后的停靠宿主左上角。
    ImVec2 m_dockPos;
    /// @brief 扣除工具栏占位后的停靠宿主尺寸。
    ImVec2 m_dockSize;
    /// @brief 固定工具栏留出的边缘位置。
    ImVec2 m_toolbarPos;
    /// @brief 单行或单列工具栏尺寸，长轴贴满可用工作区。
    ImVec2 m_toolbarSize;
};

/// @brief 在工作区边缘留出固定工具栏空间，不创建任何停靠节点。
/// @param position 已扣除菜单、状态栏和侧栏的工作区位置。
/// @param size 对应工作区尺寸。
/// @param fixed 是否启用外部固定带；关闭时停靠区使用全部工作区。
/// @param edge 顶部、底部、左侧或右侧固定边缘。
/// @param thickness 当前方向的内容厚度，包含内边距但不含装饰。
/// @param gap 固定工具栏与停靠宿主之间的视觉间隙。
/// @warning UI 热路径：只计算常量数量的浮点坐标，不修改树、不分配资源。
inline ToolbarWorkspaceGeometry calculateToolbarWorkspaceGeometry(
    ImVec2 position, ImVec2 size, bool fixed, ImGuiDir edge, float thickness,
    float gap)
{
    ToolbarWorkspaceGeometry result{ position, size, position, ImVec2(0, 0) };
    if ( !fixed ) return result;
    const bool horizontal = edge == ImGuiDir_Up || edge == ImGuiDir_Down;
    const int  axis       = horizontal ? 1 : 0;
    // 极小工作区仍保留正的中心区域，避免后续 DockSpace 收到负尺寸。
    const float available = std::max(1.0f, size[axis]);
    const float content   = std::clamp(thickness, 0.0f, available - 1.0f);
    const float spacing   = std::clamp(gap, 0.0f, available - content - 1.0f);
    const float reserved  = content + spacing;
    result.m_toolbarSize  = size;
    result.m_toolbarSize[axis] = content;
    result.m_dockSize[axis]    = available - reserved;
    // 起始边移动宿主原点，末尾边只缩小宿主，工具栏位置始终相对完整工作区。
    if ( edge == ImGuiDir_Up || edge == ImGuiDir_Left )
        result.m_dockPos[axis] += reserved;
    else
        result.m_toolbarPos[axis] += available - content;
    return result;
}

/// @brief 管理器和画布之间的固定带占位身份；实际工具栏始终是独立窗口。
inline constexpr const char* TOOLBAR_MANAGER_SLOT_NAME =
    "###ToolbarManagerLeftSlot";

/// @brief 查找左侧管理器右边包含中央画布的工作子树。
/// @param rootId 主宿主根身份；不能跨宿主借用浮动管理器的位置。
/// @return 管理器在左边展开并停靠时的右侧工作子树，否则为空。
/// @warning UI 热路径：只沿管理器祖先链检查，不遍历画布或对象集合。
inline ImGuiDockNode* toolbarManagerWorkNode(ImGuiID rootId)
{
    const auto* manager = ImGui::FindWindowByName("SideBarManager");
    auto*       node    = manager ? manager->DockNode : nullptr;
    if ( !node || !node->IsVisible ||
         ImGui::DockNodeGetRootNode(node)->ID != rootId )
        return nullptr;
    // 管理器可以位于嵌套分栏中，只接受左边缘与根一致的展开区。
    // 浮动、右侧停靠或中央标签中的管理器均不改变工具栏的外部固定带。
    auto* root = ImGui::DockBuilderGetNode(rootId);
    if ( !root || node->Pos.x > root->Pos.x + 1.0f ) return nullptr;
    for ( ; node->ParentNode; node = node->ParentNode ) {
        auto* parent = node->ParentNode;
        if ( parent->SplitAxis == ImGuiAxis_X &&
             parent->ChildNodes[0] == node &&
             parent->ChildNodes[1]->HasCentralNodeChild )
            return parent->ChildNodes[1];
    }
    return nullptr;
}

/// @brief 管理固定工具栏在管理器之后的占位，不迁移实际工具栏或重建根树。
/// @param rootId 当前宿主根节点。
/// @param enabled 左侧固定且管理器正在显示。
/// @param thickness 图标列与窗口内边距所需宽度。
/// @return 生效的占位叶 ID；关闭、浮动管理器或不适用布局返回零。
/// @warning UI 热路径：常态只查询稳定窗口和祖先；仅占位开关或归属改变时
/// 拆分/合并局部子树。不能重建管理器、设置页或双画布节点。
inline ImGuiID updateToolbarManagerLeftSlot(ImGuiID rootId, bool enabled,
                                            float thickness)
{
    auto* window = ImGui::FindWindowByName(TOOLBAR_MANAGER_SLOT_NAME);
    auto* slot   = window ? window->DockNode : nullptr;
    auto* work   = enabled ? toolbarManagerWorkNode(rootId) : nullptr;
    // 旧位置被用户改变或项目 ini 替换时，先回收专用占位，再重新查询工作子树。
    // 节点合并会释放指针，不能跨移出操作持有 work 或 slot 的观察指针。
    if ( slot && (!work || slot->ParentNode != work) ) {
        slot->SetLocalFlags(0);
        slot->LocalFlagsInWindows = 0;
        slot->UpdateMergedFlags();
        window->WindowClass.DockNodeFlagsOverrideSet = 0;
        const ImGuiID oldSlot                        = slot->ID;
        ImGui::DockBuilderDockWindow(TOOLBAR_MANAGER_SLOT_NAME, 0);
        ImGui::DockBuilderRemoveNode(oldSlot);
        slot = nullptr;
        work = enabled ? toolbarManagerWorkNode(rootId) : nullptr;
    }
    // 管理器不再占左侧时，不在中心强行创造对应空间。
    // 后续外部几何会自行扣除固定条厚度，不能在此重复扣除。
    if ( !work ) return 0;
    // 两侧分隔条已经分别提供间距，占位只保留工具栏自身厚度。
    // 额外增加 gap 会让画布侧重复留白，破坏两侧对称。
    // 使用单独无装饰叶，工具栏本身仍是 NoDocking 的固定窗口。
    const float width = thickness;
    if ( !slot ) {
        const float available =
            work->Size.x - ImGui::GetStyle().DockingSeparatorSize;
        // 极窄窗口必须保留正尺寸画布，不生成无法容纳内容的专用叶。
        // 尺寸足够后会在下一次正常布局中重新尝试，不能缓存失败归属。
        if ( available <= width + 1.0f ) return 0;
        const ImGuiID slotId = ImGui::DockBuilderSplitNode(
            work->ID,
            ImGuiDir_Left,
            std::clamp(width / available, 0.01f, 0.95f),
            nullptr,
            nullptr);
        ImGui::DockBuilderDockWindow(TOOLBAR_MANAGER_SLOT_NAME, slotId);
        ImGui::DockBuilderFinish(rootId);
        slot = ImGui::DockBuilderGetNode(slotId);
    }
    // 宽度按主题更新，但禁止拖动这一条专用分隔条来改变工具栏厚度。
    // WantLockSizeOnce 将像素宽度交给 ImGui，画布仍使用剩余空间。
    slot->SetLocalFlags(
        ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_NoResizeX |
        ImGuiDockNodeFlags_NoDocking | ImGuiDockNodeFlags_NoUndocking);
    slot->Size.x = slot->SizeRef.x = width;
    slot->WantLockSizeOnce         = true;
    return slot->ID;
}

/// @brief 将固定带画布侧的拖拽映射到管理器宽度，保持工具栏自身厚度不变。
/// @param slotId 工具栏当前占位叶身份；零表示没有管理器侧固定带。
/// @warning UI 热路径：在 DockSpace 前提交窄命中区；仅活动拖拽时修改
/// 相邻管理器分支的 SizeRef，交由本帧 DockSpace 重排，不等待后续帧。
inline void drawToolbarManagerResizeHandle(ImGuiID slotId)
{
    auto* slot  = ImGui::DockBuilderGetNode(slotId);
    auto* work  = slot ? slot->ParentNode : nullptr;
    auto* split = work ? work->ParentNode : nullptr;
    // 专用占位只属于管理器右侧的工作分支；树刚恢复时不能借用无关分隔条。
    // 此处只观察直接祖先，不缓存可能在收起或项目切换时被释放的节点指针。
    if ( !split || split->SplitAxis != ImGuiAxis_X ||
         split->ChildNodes[1] != work )
        return;
    auto*       manager   = split->ChildNodes[0];
    const float separator = ImGui::GetStyle().DockingSeparatorSize;
    // 保存业务窗口下限，透明命中窗口稍后会临时采用一像素最小尺寸。
    // 不能把命中区的样式覆盖误用于管理器，否则它会被拖到不可用宽度。
    const float minimum = std::max(1.0f, ImGui::GetStyle().WindowMinSize.x);
    // 工具栏厚度锁住后，原生分隔条不能再直接改变占位叶的宽度。
    // 在同一条边界提供无外观命中区，将左右移动交给管理器和整个画布分支。
    ImGui::SetNextWindowPos(ImVec2(slot->Pos.x + slot->Size.x, slot->Pos.y));
    ImGui::SetNextWindowSize(ImVec2(std::max(1.0f, separator), slot->Size.y));
    ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
    // 分隔条可能仅两像素宽；默认边框会把按钮的有效裁剪区完全挤掉。
    // 窄命中窗口无需装饰，边框必须与内边距同时清零。
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("###ToolbarManagerResizeHandle",
                 nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav);
    // 透明窄窗口必须位于宿主之上，不能被随后提交的 DockSpace 宿主吃掉命中。
    // 只覆盖现有分隔间距，不遮挡任何工具按钮、管理器内容或画布。
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImGui::InvisibleButton("##Resize",
                           ImVec2(std::max(1.0f, separator), slot->Size.y));
    const bool hovered      = ImGui::IsItemHovered();
    const bool active       = ImGui::IsItemActive();
    ImVec2     highlightMin = ImGui::GetItemRectMin();
    ImVec2     highlightMax = ImGui::GetItemRectMax();
    if ( hovered || active ) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if ( active && ImGui::GetIO().MouseDelta.x != 0.0f ) {
        // 按本次鼠标增量立即调整管理器分支，禁止先等待或累积到鼠标释放。
        // 画布保留最小正宽度及固定带厚度；管理器自身更严格的下限沿用其约束入口。
        const float maximum = std::max(
            minimum, split->Size.x - separator - slot->Size.x - minimum);
        const float width = std::clamp(
            manager->Size.x + ImGui::GetIO().MouseDelta.x, minimum, maximum);
        // 本帧 DockSpace 即将重排，高亮也要跟随实际位移而非未裁限的鼠标增量。
        // 到达最小宽度时仍贴着分隔条，不能绘制到工具按钮或画布内容上。
        const float displacement = width - manager->Size.x;
        highlightMin.x += displacement;
        highlightMax.x += displacement;
        manager->Size.x = manager->SizeRef.x = width;
        manager->WantLockSizeOnce            = true;
    }
    // 透明命中区不会自动绘制分隔条；悬浮和拖动必须沿用原生分隔条主题色。
    // DockSpace 内部临时把 Separator 状态色映射到 ResizeGrip 状态色。
    // 此处在映射生效前绘制，直接读取 ResizeGrip，避免与普通分隔线颜色混淆。
    // 此入口早于 DockSpace，使用同视口前景层避免随后绘制的锁定分隔条覆盖高亮。
    // 鼠标离开且没有活动手势时停止提交，正常颜色继续由 DockSpace 提供。
    if ( hovered || active ) {
        ImGui::GetForegroundDrawList(ImGui::GetWindowViewport())
            ->AddRectFilled(
                highlightMin,
                highlightMax,
                ImGui::GetColorU32(active ? ImGuiCol_ResizeGripActive
                                          : ImGuiCol_ResizeGripHovered),
                ImGui::GetStyle().FrameRounding);
    }
    // 独立命中窗口没有保存状态；收起占位后停止提交即自动消失。
    // 样式覆盖仅属于窄窗口，必须在返回宿主布局之前完整恢复。
    ImGui::End();
    ImGui::PopStyleVar(3);
}

/// @brief 提交透明固定带占位，阻止空叶被 DockSpace 自动折叠。
/// @param slotId 本帧有效占位身份；零表示不提交窗口。
/// @warning UI 热路径：只提交一个无输入、无背景窗口，不分配业务资源。
inline void drawToolbarManagerLeftSlot(ImGuiID slotId)
{
    // 零身份同时涵盖收起、非左侧固定和找不到中央分栏的情况。
    // 不能在这些状态提交一个浮动透明窗口，否则会残留无意义宿主。
    if ( slotId == 0 ) return;
    // 占位不参与项目窗口保存，也不产生标题、菜单、边框或用户输入命中。
    // 实际工具栏覆盖内容区域，独立窗口仍拥有完整主题圆角和鼠标操作。
    ImGuiWindowClass windowClass;
    windowClass.DockNodeFlagsOverrideSet =
        ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_NoResizeX |
        ImGuiDockNodeFlags_NoDocking | ImGuiDockNodeFlags_NoUndocking;
    ImGui::SetNextWindowClass(&windowClass);
    ImGui::SetNextWindowDockID(slotId);
    ImGui::Begin(TOOLBAR_MANAGER_SLOT_NAME,
                 nullptr,
                 ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoScrollbar);
    ImGui::End();
}

/// @brief 工具条长度之外的空白占位窗口，仅用于提供可拖动的长度分隔条。
inline constexpr const char* TOOLBAR_REMAINDER_NAME = "###ToolbarDockRemainder";

/// @brief 获取工具栏贴靠画布的外侧节点，跳过仅用于调节长度的内部拆分。
/// @return 浮动时为空，未增加长度拆分时返回工具叶节点。
/// @warning UI 热路径：只检查工具窗口、占位窗口和相邻两个节点，不遍历树。
inline ImGuiDockNode* toolbarDockSlot()
{
    const auto* window = ImGui::FindWindowByName(" ###Toolbar");
    auto*       node   = window ? window->DockNode : nullptr;
    // 浮动窗口没有节点；首次停靠没有内侧占位，直接采用现存叶节点。
    // 只跳过本功能创建的占位拆分，不能跳过用户自己的其他画布分栏。
    if ( !node || !node->ParentNode ) return node;
    // Builder 可能刚写入窗口设置而尚未 Begin；同时兼容活跃窗口和初建设置。
    const auto* remainder   = ImGui::FindWindowByName(TOOLBAR_REMAINDER_NAME);
    ImGuiID     remainderId = remainder ? remainder->DockId : 0;
    if ( !remainder ) {
        if ( const auto* saved = ImGui::FindWindowSettingsByID(
                 ImHashStr(TOOLBAR_REMAINDER_NAME)) )
            remainderId = saved->DockId;
    }
    // 占位必须与工具叶同父且为兄弟；旧 ini 中残留的窗口 ID 不能独立证明归属。
    // 通过直接相邻关系确认专用拆分，避免误把普通分栏当成可回收节点。
    const auto* parent = node->ParentNode;
    const auto* sibling =
        parent->ChildNodes[parent->ChildNodes[0] == node ? 1 : 0];
    return sibling && sibling->ID == remainderId ? node->ParentNode : node;
}

/// @brief 查询工具栏所处的边缘，支持用户直接拖动停靠后的方向同步。
/// @return 浮动或未分栏时无值，否则返回最近拆分的边缘。
/// @warning 每帧只读取活跃节点及其父节点，禁止缓存跨帧节点指针。
inline std::optional<ImGuiDir> currentToolbarDockDirection()
{
    const auto* node = toolbarDockSlot();
    if ( !node || !node->ParentNode ) return std::nullopt;
    const auto* parent = node->ParentNode;
    // 分栏轴决定排布，子节点顺序决定上/下或左/右；不按窗口宽高猜测。
    const bool first = parent->ChildNodes[0] == node;
    return parent->SplitAxis == ImGuiAxis_Y
               ? (first ? ImGuiDir_Up : ImGuiDir_Down)
               : (first ? ImGuiDir_Left : ImGuiDir_Right);
}

/// @brief 设置工具栏专用节点的缩放轴约束，并在固定时保护停靠归属。
/// @param horizontal 是否横排。
/// @param fixed 是否锁定全部交互几何。
/// @warning UI 热路径：只更新本窗口的轻量 WindowClass，不重建节点树。
inline void prepareToolbarDockClass(bool horizontal, bool fixed)
{
    ImGuiWindowClass windowClass;
    // 浮动窗口尺寸由 SizeConstraints 约束；节点标志负责停靠分隔条。
    windowClass.DockNodeFlagsOverrideSet = horizontal
                                               ? ImGuiDockNodeFlags_NoResizeY
                                               : ImGuiDockNodeFlags_NoResizeX;
    if ( fixed ) {
        windowClass.DockNodeFlagsOverrideSet |=
            ImGuiDockNodeFlags_NoResize | ImGuiDockNodeFlags_NoUndocking;
        const auto* window = ImGui::FindWindowByName(" ###Toolbar");
        // 共享标签节点不能隐藏其他窗口的标签；独占工具节点移除全部装饰。
        if ( !window || !window->DockNode ||
             window->DockNode->Windows.Size <= 1 )
            windowClass.DockNodeFlagsOverrideSet |=
                ImGuiDockNodeFlags_NoTabBar |
                ImGuiDockNodeFlags_NoWindowMenuButton |
                ImGuiDockNodeFlags_NoCloseButton;
    }
    ImGui::SetNextWindowClass(&windowClass);
}

/// @brief 在 DockSpace 提交前锁定工具节点厚度，并清理历史保存的限制标志。
/// @param fixed 是否同时锁定长轴并去除装饰；厚度在两种模式都保持固定。
/// @param verticalWidth 竖排内容列和内边距的总宽。
/// @param horizontalHeight 横排内容行和内边距的总高，不含标题栏。
/// @warning UI 热路径：只检查一个窗口和一对兄弟节点；未变时不写几何，
/// 不重建停靠树，不访问文件系统，也不修改共享工具节点中的其他窗口。
inline void applyToolbarDockConstraints(bool fixed, float verticalWidth,
                                        float horizontalHeight)
{
    auto* window = ImGui::FindWindowByName(" ###Toolbar");
    auto* node   = window ? window->DockNode : nullptr;
    if ( !node || node->Windows.Size != 1 ) return;
    const auto edge = currentToolbarDockDirection();
    const bool horizontal =
        edge && (*edge == ImGuiDir_Up || *edge == ImGuiDir_Down);
    // ini 保存的 LocalFlags 独立于 WindowClass；解除固定不能只更新窗口标志。
    // 在独占节点清理本功能拥有的标志，保留 CentralNode 等布局身份标志。
    constexpr ImGuiDockNodeFlags CONTROLLED_FLAGS =
        static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_NoResize) |
        ImGuiDockNodeFlags_NoResizeX | ImGuiDockNodeFlags_NoResizeY |
        ImGuiDockNodeFlags_NoUndocking | ImGuiDockNodeFlags_NoTabBar |
        ImGuiDockNodeFlags_HiddenTabBar |
        ImGuiDockNodeFlags_NoWindowMenuButton |
        ImGuiDockNodeFlags_NoCloseButton;
    ImGuiDockNodeFlags policy = horizontal ? ImGuiDockNodeFlags_NoResizeY
                                           : ImGuiDockNodeFlags_NoResizeX;
    if ( fixed )
        policy |= static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_NoResize) |
                  ImGuiDockNodeFlags_NoUndocking | ImGuiDockNodeFlags_NoTabBar |
                  ImGuiDockNodeFlags_NoWindowMenuButton |
                  ImGuiDockNodeFlags_NoCloseButton;
    node->SetLocalFlags(node->LocalFlags & ~CONTROLLED_FLAGS);
    node->LocalFlagsInWindows =
        (node->LocalFlagsInWindows & ~CONTROLLED_FLAGS) | policy;
    // 宿主先于工具窗口 Begin，因此上一帧 WindowClass 也要同步到当前策略。
    // 只改 LocalFlags 会在 DockSpace 汇总窗口类时重新引入旧的禁止缩放标志。
    window->WindowClass.DockNodeFlagsOverrideSet = policy;
    node->UpdateMergedFlags();
    if ( !edge ) return;
    // 长度拆分的父节点才代表整条贴边区域，厚度约束不能只作用于内部工具叶。
    auto* slot = toolbarDockSlot();
    if ( !slot ) return;
    // 外侧分隔条检查的是整条贴边节点，而不是内部工具叶；两层都要锁短轴。
    if ( slot != node )
        slot->SetLocalFlags((slot->LocalFlags & ~CONTROLLED_FLAGS) |
                            (horizontal ? ImGuiDockNodeFlags_NoResizeY
                                        : ImGuiDockNodeFlags_NoResizeX));
    auto*           parent = slot->ParentNode;
    const ImGuiAxis axis   = horizontal ? ImGuiAxis_Y : ImGuiAxis_X;
    // 工具叶必须独占一侧，父节点保持原尺寸，释放的空间全部还给兄弟画布子树。
    // 不删除或拆分节点，因此并排画布和设置页的 ID 与层次保持原样。
    if ( !parent || parent->SplitAxis != axis ) return;
    const bool first   = parent->ChildNodes[0] == slot;
    auto*      sibling = parent->ChildNodes[first ? 1 : 0];
    if ( !sibling ) return;
    const float available =
        parent->Size[axis] - ImGui::GetStyle().DockingSeparatorSize;
    const float desired = horizontal ? horizontalHeight : verticalWidth;
    // 极小宿主也要为兄弟保留正尺寸，不能产生负的比例或退化节点。
    if ( available <= 1.0f ) return;
    const float thickness =
        std::clamp(desired, 1.0f, std::max(1.0f, available - 1.0f));
    // 常态准确匹配时不改 SizeRef，也不设置 WantLockSizeOnce，避免干扰长轴拖动。
    // DPI、主题标签或宿主尺寸变化导致的短轴偏差才需要重新分配这一对兄弟。
    if ( slot->Size[axis] == thickness && slot->SizeRef[axis] == thickness )
        return;
    ImVec2 toolSize    = slot->Size;
    ImVec2 siblingSize = sibling->Size;
    toolSize[axis]     = thickness;
    siblingSize[axis]  = available - thickness;
    // 两侧同时更新 SizeRef，避免下一轮布局沿用旧的过宽/过高比例。
    ImGui::DockBuilderSetNodeSize(slot->ID, toolSize);
    ImGui::DockBuilderSetNodeSize(sibling->ID, siblingSize);
    // 本轮布局按工具条绝对厚度分配，不再把过大的历史比例缩放回去。
    // 下一轮由 ImGui 自动清除此一次性标志；同一节点正常长轴缩放不受影响。
    slot->WantLockSizeOnce = true;
}

/// @brief 在工具条长轴增加可拖动分隔条，固定时收回该局部拆分。
/// @details 外侧父节点始终代表贴靠画布的同一边，内侧两个叶仅分配工具条长度。
/// 窗口级尺寸约束不创建停靠分隔条，所以必须让内侧占位节点保持活跃。
/// 固定后占位不再提交，工具条恢复整条边的长度而不保留空白标签窗口。
/// @param fixed 固定后工具条填满当前贴边节点，不保留长度占位。
/// @warning UI 热路径：常态只查两个窗口；仅模式变化或首次停靠时修改局部树。
/// 不能重建根节点，不能改变画布、设置页或用户创建的其他分栏。
inline void updateToolbarLengthDock(bool fixed)
{
    auto* window = ImGui::FindWindowByName(" ###Toolbar");
    auto* node   = window ? window->DockNode : nullptr;
    if ( !node || node->Windows.Size != 1 ) return;
    auto* slot = toolbarDockSlot();
    // 不缓存内部叶节点指针到后续帧：合并会释放子节点并更新工具窗口 DockId。
    // 固定时只移出本功能的占位，其他用户窗口不参与这个回收流程。
    if ( fixed ) {
        if ( slot != node ) {
            // 只移出占位窗口，ImGui 将工具叶合并回原贴边节点并保留外侧身份。
            ImGui::DockBuilderDockWindow(TOOLBAR_REMAINDER_NAME, 0);
        }
        return;
    }
    // 已有长度分隔条时保留用户比例；浮动或共享节点不强行生成额外分栏。
    if ( slot != node || !node->ParentNode ) return;
    const bool horizontal = node->ParentNode->SplitAxis == ImGuiAxis_Y;
    // 先取得数值身份再拆分；Builder 修改树之后不能使用原节点的布局关系。
    // 根 ID 只用于 Finish 提交，不意味着需要重建整个工作区。
    const ImGuiID rootId = ImGui::DockNodeGetRootNode(node)->ID;
    const ImGuiID slotId = node->ID;
    ImGuiID       toolId = 0;
    // 沿长轴切出末端空白，从而提供真正可拖动的长轴分隔条；短轴仍锁定。
    // 占位窗口必须持续提交，否则空叶会被 ImGui 隐藏，分隔条就再次失效。
    const ImGuiID remainderId =
        ImGui::DockBuilderSplitNode(slotId,
                                    horizontal ? ImGuiDir_Right : ImGuiDir_Down,
                                    0.20f,
                                    nullptr,
                                    &toolId);
    // 工具叶放在长轴起点，末端占位为长度分隔条提供稳定的另一侧。
    // 两个窗口都使用固定 ID，工作区 ini 可以保留非固定状态的长度比例。
    ImGui::DockBuilderDockWindow(" ###Toolbar", toolId);
    ImGui::DockBuilderDockWindow(TOOLBAR_REMAINDER_NAME, remainderId);
    // 父节点不再直接拥有窗口，不能指望叶窗口类自动把锁短轴标志向上传播。
    if ( auto* createdSlot = ImGui::DockBuilderGetNode(slotId) )
        createdSlot->SetLocalFlags(createdSlot->LocalFlags |
                                   (horizontal ? ImGuiDockNodeFlags_NoResizeY
                                               : ImGuiDockNodeFlags_NoResizeX));
    ImGui::DockBuilderFinish(rootId);
}

/// @brief 提交透明的长度占位节点；其唯一交互是与工具条之间的分隔条。
/// @warning UI 热路径：仅绘制一个无内容窗口，不创建资源、访问文件或等待。
inline void drawToolbarDockRemainder()
{
    auto*       slot    = toolbarDockSlot();
    const auto* toolbar = ImGui::FindWindowByName(" ###Toolbar");
    // 固定或浮动状态没有专用占位，不能把遗留窗口作为普通浮窗绘制出来。
    // 每次都核对当前节点关系；换边或项目布局恢复后旧 DockId 可能已经无效。
    if ( !slot || !toolbar || slot == toolbar->DockNode ) return;
    const auto edge = currentToolbarDockDirection();
    const bool horizontal =
        edge && (*edge == ImGuiDir_Up || *edge == ImGuiDir_Down);
    ImGuiWindowClass windowClass;
    windowClass.DockNodeFlagsOverrideSet =
        ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_NoWindowMenuButton |
        ImGuiDockNodeFlags_NoCloseButton;
    windowClass.DockNodeFlagsOverrideSet |= horizontal
                                                ? ImGuiDockNodeFlags_NoResizeY
                                                : ImGuiDockNodeFlags_NoResizeX;
    // 占位不能被拖出成为独立窗口，但不能禁止它与工具叶之间的长轴分隔条。
    // 它与工具叶使用相同的短轴限制，保证外侧分隔条始终禁止调整厚度。
    windowClass.DockNodeFlagsOverrideSet |= ImGuiDockNodeFlags_NoUndocking;
    ImGui::SetNextWindowClass(&windowClass);
    // 空白区域不是用户视图：不显示标题、窗口菜单、关闭按钮或内容背景。
    // 不使用 NoInputs，否则宿主的分隔条也无法接收该区域附近的鼠标输入。
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    // 空白没有可点击按钮，只提供宿主节点活跃性；其背景由主工作区统一绘制。
    // 即使 Begin 返回不可见，也必须 End，不能破坏主宿主窗口栈。
    ImGui::Begin(TOOLBAR_REMAINDER_NAME,
                 nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoBackground);
    ImGui::End();
    ImGui::PopStyleVar(2);
}

/// @brief 在主画布指定边缘创建工具栏节点，不重建其他画布或设置页。
/// @param rootId 当前主宿主根节点。
/// @param canvasId 主画布中心节点；失效时使用根的中央节点。
/// @param direction 指定边缘，None 表示仅解除停靠。
/// @param thickness 横排的高度或竖排的宽度，单位为像素。
/// @return 新工具节点 ID；解除停靠或目标缺失时返回零。
/// @warning 低频用户布局路径：只允许在本帧 DockSpace 和工具栏 Begin 前调用。
inline ImGuiID dockToolbarAtCanvasEdge(ImGuiID rootId, ImGuiID canvasId,
                                       ImGuiDir direction, float thickness)
{
    // 此入口只迁移一个窗口，不能通过删除整棵节点树实现边缘重排。
    // canvasId 是观察身份，移出产生合并后需在当前树中重新确认其存在。
    // 根和叶只在本次低频请求中遍历，常态每帧不会执行这个 Builder 流程。
    // 先移出工具栏，允许 ImGui 合并其独占空节点；共享节点的其他窗口保留。
    // 先收回长度占位，避免工具条换边后留下空的旧停靠带。
    // 两次移出各自可能引起合并，目标节点只能在全部移出结束后重新查找。
    // 不保存旧工具叶或占位叶指针，防止换边时访问已释放的节点。
    ImGui::DockBuilderDockWindow(TOOLBAR_REMAINDER_NAME, 0);
    ImGui::DockBuilderDockWindow(" ###Toolbar", 0);
    if ( direction == ImGuiDir_None ) return 0;
    // 移出会合并空叶并释放旧节点，root->CentralNode 要到 DockSpace 才刷新。
    // 因此只遍历仍属于根的子树，不能解引用这个延迟更新的中央节点缓存。
    auto* target = ImGui::DockBuilderGetNode(canvasId);
    if ( !target || target->IsSplitNode() ) {
        auto* root = ImGui::DockBuilderGetNode(rootId);
        // 深度优先查找仅使用当前树的链接；递归过程没有节点修改和堆分配。
        // 每次找叶完成后才执行拆分，不能在遍历中缓存待释放的节点指针。
        const auto findLeaf = [](auto&&         self,
                                 ImGuiDockNode* node,
                                 bool           centralOnly) -> ImGuiDockNode* {
            if ( !node ) return nullptr;
            if ( !node->IsSplitNode() )
                return !centralOnly || node->IsCentralNode() ? node : nullptr;
            if ( auto* first = self(self, node->ChildNodes[0], centralOnly) )
                return first;
            return self(self, node->ChildNodes[1], centralOnly);
        };
        // 中央叶优先；没有中央标志的自定义布局使用现存第一叶，保留其他分栏。
        target = findLeaf(findLeaf, root, true);
        if ( !target ) target = findLeaf(findLeaf, root, false);
    }
    if ( !target ) return 0;
    const ImGuiID targetId = target->ID;
    const bool    horizontal =
        direction == ImGuiDir_Up || direction == ImGuiDir_Down;
    const float available = (horizontal ? target->Size.y : target->Size.x) -
                            ImGui::GetStyle().DockingSeparatorSize;
    const float ratio =
        std::clamp(thickness / std::max(available, 1.0f), 0.01f, 0.45f);
    // 原画布子树由 ImGui 继承，工具栏只占边缘新叶节点。
    const ImGuiID toolbarId = ImGui::DockBuilderSplitNode(
        targetId, direction, ratio, nullptr, nullptr);
    ImGui::DockBuilderDockWindow(" ###Toolbar", toolbarId);
    ImGui::DockBuilderFinish(rootId);
    return toolbarId;
}

/// @brief 将固定工具栏移出停靠树，仅回收自身的空占位节点。
/// @details 设置页或画布即使和工具栏共用节点，也只移出工具栏这一窗口。
/// @warning UI 热路径：常态只检查两个稳定窗口 ID；仅发现遗留停靠时修改树。
inline void detachFixedToolbar()
{
    auto* toolbar = ImGui::FindWindowByName(" ###Toolbar");
    auto* leaf    = toolbar ? toolbar->DockNode : nullptr;
    // ImGui 合并空叶时会向相邻画布转移 LocalFlags，必须先清掉专用工具限制。
    // 共享标签节点不属于本功能，不能清掉其他窗口主动设置的节点策略。
    if ( leaf && leaf->Windows.Size == 1 ) {
        constexpr ImGuiDockNodeFlags TOOL_FLAGS =
            static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_NoResize) |
            ImGuiDockNodeFlags_NoResizeX | ImGuiDockNodeFlags_NoResizeY |
            ImGuiDockNodeFlags_NoUndocking | ImGuiDockNodeFlags_NoTabBar |
            ImGuiDockNodeFlags_HiddenTabBar |
            ImGuiDockNodeFlags_NoWindowMenuButton |
            ImGuiDockNodeFlags_NoCloseButton;
        auto* slot = toolbarDockSlot();
        // 内层工具叶和外层长度占位父节点各自可能持有 ini 保存的轴限制。
        // 两者均先清理再移出，画布不能继承工具栏专用的隐藏装饰标志。
        // 尚无长度拆分时两者相同，清理操作幂等，不依赖节点身份去重。
        for ( auto* node : { leaf, slot } ) {
            if ( !node ) continue;
            node->SetLocalFlags(node->LocalFlags & ~TOOL_FLAGS);
            node->LocalFlagsInWindows &= ~TOOL_FLAGS;
            node->UpdateMergedFlags();
        }
        toolbar->WindowClass.DockNodeFlagsOverrideSet = 0;
    }
    for ( const char* name : { TOOLBAR_REMAINDER_NAME, " ###Toolbar" } ) {
        const auto* window = ImGui::FindWindowByName(name);
        ImGuiID     dockId = window ? window->DockId : 0;
        // 首帧窗口尚未 Begin 时也要清掉 ini 中的停靠归属，不能留下空工具带。
        if ( !window ) {
            if ( const auto* saved =
                     ImGui::FindWindowSettingsByID(ImHashStr(name)) )
                dockId = saved->DockId;
        }
        // 移出会使空叶自然合并；禁止移除根树或操作其他窗口的 DockId。
        if ( dockId != 0 ) ImGui::DockBuilderDockWindow(name, 0);
    }
}

/// @brief 把用户的边缘停靠意图转换成宿主外部固定带。
/// @param fixed 固定开关；检测到四边停靠时设为 true。
/// @param horizontal 排布方向；顶部和底部为横排。
/// @param savedEdge 持久化的边缘名称，由调用方在变更时保存。
/// @return 软件布局偏好改变时返回 true；常态浮动或固定帧返回 false。
/// @warning UI 热路径：每帧只检查一个请求和工具节点，只有实际停靠或菜单
/// 请求才修改局部树；函数不访问文件系统，不重新构建中心工作区。
inline bool updateToolbarFixedDockIntent(bool& fixed, bool& horizontal,
                                         std::string& savedEdge)
{
    auto edge = currentToolbarDockDirection();
    if ( auto& request = pendingToolbarDockRequest(); request ) {
        // 菜单请求优先于仍在原节点的窗口，不能先把旧方向再次固定。
        edge = *request;
        request.reset();
        if ( *edge == ImGuiDir_None ) {
            // 手动选择横竖排表示继续浮动；移出后不再把旧节点当成停靠意图。
            detachFixedToolbar();
            return false;
        }
    }
    if ( !edge ) return false;
    const bool  row     = *edge == ImGuiDir_Up || *edge == ImGuiDir_Down;
    const char* name    = *edge == ImGuiDir_Up     ? "top"
                          : *edge == ImGuiDir_Down ? "bottom"
                          : *edge == ImGuiDir_Left ? "left"
                                                   : "right";
    const bool  changed = !fixed || horizontal != row || savedEdge != name;
    // 边缘是用户固定位置的明确选择，不再提供停靠后另外勾选固定的中间状态。
    fixed      = true;
    horizontal = row;
    savedEdge  = name;
    // 移出发生在宿主几何计算之前，同一帧即可给中心区域扣除正确的厚度。
    // 只处理工具栏及历史专用占位，其他窗口的归属和相对分栏继续保留。
    detachFixedToolbar();
    return changed;
}

/// @brief 模式切换时移出固定工具栏，非固定模式保留用户当前停靠或浮动状态。
/// @param rootId 主宿主根节点。
/// @param fixed 是否使用宿主外部固定工具栏。
/// @param toolbarWidth 历史宽度参数；保留签名但不再自动创建非固定工具叶。
/// @param workspaceSize 历史宿主尺寸参数；现有树由宿主窗口管理，不在此重设。
/// @return 固定或浮动时返回零，否则返回当前停靠节点。
/// @warning 低频布局入口：不能逐帧把用户浮动窗口送回旧节点。
inline ImGuiID updateToolbarDockLayout(ImGuiID rootId, bool fixed,
                                       float toolbarWidth, ImVec2 workspaceSize)
{
    // 固定带由宿主几何预留，不再保留工具节点、长度占位或节点装饰。
    if ( fixed ) {
        detachFixedToolbar();
        return 0;
    }
    const auto* toolbar    = ImGui::FindWindowByName(" ###Toolbar");
    ImGuiID     existingId = toolbar ? toolbar->DockId : 0;
    if ( !toolbar ) {
        // 启动读取 ini 时窗口尚未 Begin，仍须尊重保存的叶节点。
        if ( const auto* saved =
                 ImGui::FindWindowSettingsByID(ImHashStr(" ###Toolbar")) )
            existingId = saved->DockId;
    }
    if ( auto* node = ImGui::DockBuilderGetNode(existingId);
         node && !node->IsSplitNode() &&
         ImGui::DockNodeGetRootNode(node)->ID == rootId )
        return existingId;
    // 解除固定后保持外部窗口，允许用户缩放长轴、拖动并重新停靠。
    if ( toolbar ) return 0;
    if ( !ImGui::DockBuilderGetNode(rootId) ) return 0;
    // 初次显示非固定工具栏保持浮动；自动创建停靠叶会被误判为用户固定意图。
    // 已保存的停靠归属仍由上方分支继承，下一帧再转换为对应固定带。
    (void)toolbarWidth;
    (void)workspaceSize;
    return 0;
}
}  // namespace MMM::UI
