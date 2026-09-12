#pragma once

#include "../CLayDefs.h"
#include "imgui.h"
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

namespace MMM::UI
{

class CLayHBox;
class CLayVBox;
class LayoutContext;

/// @brief 使用链式 API 组装 Clay 横向或纵向布局，并把结果映射回 ImGui 绘制。
/// @details 容器保存元素值、文本及绘制回调；嵌套布局只保存非拥有指针，调用方
/// 必须保证子布局在 render 或 renderInCurrent 完成前持续有效。
class CLayBox
{
public:
    /// @brief 传给通用渲染回调的屏幕空间矩形。
    struct Rect {
        /// @brief 依次表示左上角横纵坐标与矩形宽高。
        float x, y, w, h;
    };
    /// @brief 使用稳定元素 ID 和屏幕矩形执行外部渲染的回调类型。
    using RenderCallback =
        std::function<void(const std::string& id, Rect rect)>;

    /// @brief 通过基类安全析构具体方向的布局容器。
    virtual ~CLayBox() = default;

    /// @brief 创建横向排列容器。
    /// @return 按从左到右方向初始化的值对象。
    static CLayHBox HBox();
    /// @brief 创建纵向排列容器。
    /// @return 按从上到下方向初始化的值对象。
    static CLayVBox VBox();

    /// @brief 设置相邻子项之间的像素间距。
    /// @param gap Clay 使用的无符号间距值。
    /// @return 当前容器以支持链式配置。
    CLayBox& setSpacing(uint16_t gap)
    {
        m_gap = gap;
        return *this;
    }
    /// @brief 设置容器四边内边距。
    /// @param l 左侧像素。
    /// @param r 右侧像素。
    /// @param t 顶部像素。
    /// @param b 底部像素。
    /// @return 当前容器以支持链式配置。
    CLayBox& setPadding(uint16_t l, uint16_t r, uint16_t t, uint16_t b)
    {
        m_padding = { l, r, t, b };
        return *this;
    }
    /// @brief 设置子项的横纵对齐方式。
    /// @param align Alignment 便捷配置。
    /// @return 当前容器以支持链式配置。
    CLayBox& setAlignment(Alignment align)
    {
        m_align = align;
        return *this;
    }

    /// @brief 清除所有子项并重置装饰状态。
    /// @note 间距、内边距、对齐和方向配置保持不变，便于下一帧复用容器。
    void clear()
    {
        m_items.clear();
        m_decorated = false;
    }

    /// @brief 启用或禁用圆角边框与淡色背景装饰。
    /// @param v 是否绘制装饰。
    /// @return 当前容器以支持链式配置。
    /// @note 颜色与圆角在绘制时从 ImGui::GetStyle 动态读取。
    CLayBox& setDecorated(bool v)
    {
        m_decorated = v;
        return *this;
    }

    /// @brief 元素获得 Clay 布局结果后执行的即时绘制回调。
    /// @param rect Clay 计算的屏幕空间边界。
    /// @param isHovered 指针当前是否位于元素内。
    using DrawFunc = std::function<void(Clay_BoundingBox rect, bool isHovered)>;
    /// @brief 添加带即时绘制回调的普通元素。
    /// @param id 当前布局树内唯一的稳定元素 ID。
    /// @param w 横向尺寸策略。
    /// @param h 纵向尺寸策略。
    /// @param func Clay 完成布局后执行的绘制函数。
    /// @return 当前容器以支持链式添加。
    CLayBox& addElement(const std::string& id, Sizing w, Sizing h,
                        DrawFunc func)
    {
        m_items.push_back({ .type         = ItemType::Element,
                            .id           = id,
                            .w            = w.axis,
                            .h            = h.axis,
                            .drawCallback = func });
        return *this;
    }

    /// @brief 添加由 Clay 测量、ImGui DrawList 绘制的文本节点。
    /// @param text UTF-8 文本内容。
    /// @param fontId 皮肤字体角色。
    /// @param fontSize Clay 文本配置使用的字号。
    /// @param color 取值为 0 到 255 的 Clay RGBA 颜色。
    /// @return 当前容器以支持链式添加。
    CLayBox& addText(const std::string& text, FontID fontId, uint16_t fontSize,
                     Clay_Color color)
    {
        m_items.push_back({ .type      = ItemType::Text,
                            .id        = "",
                            .w         = Sizing::Fit().axis,
                            .h         = Sizing::Fit().axis,
                            .text      = text,
                            .fontId    = static_cast<uint16_t>(fontId),
                            .fontSize  = fontSize,
                            .textColor = color });
        return *this;
    }

    /// @brief 添加不转移所有权的嵌套布局。
    /// @param id 当前布局树内唯一的稳定元素 ID。
    /// @param nested 生命周期覆盖渲染阶段的子布局。
    /// @param w 子布局横向尺寸策略。
    /// @param h 子布局纵向尺寸策略。
    /// @return 当前容器以支持链式添加。
    CLayBox& addLayout(const char* id, CLayBox& nested,
                       Sizing w = Sizing::Grow(), Sizing h = Sizing::Grow())
    {
        m_items.push_back(
            { ItemType::NestedLayout, id, w.axis, h.axis, nullptr, &nested });
        return *this;
    }

    /// @brief 添加沿容器方向占据剩余空间的弹簧项。
    /// @return 当前容器以支持链式添加。
    CLayBox& addSpring()
    {
        m_items.push_back(
            { ItemType::Spring, "", Sizing::Grow().axis, Sizing::Grow().axis });
        return *this;
    }

    /// @brief 添加沿主布局方向具有固定尺寸的间隔项。
    /// @param px 间隔像素。
    /// @return 当前容器以支持链式添加。
    CLayBox& addSpacing(float px)
    {
        // 横向容器固定宽度，纵向容器让宽度随父容器增长。
        Clay_SizingAxis sx = (m_dir == CLAY_LEFT_TO_RIGHT)
                                 ? Sizing::Fixed(px).axis
                                 : Sizing::Grow().axis;
        // 纵向容器固定高度，横向容器让高度随父容器增长。
        Clay_SizingAxis sy = (m_dir == CLAY_TOP_TO_BOTTOM)
                                 ? Sizing::Fixed(px).axis
                                 : Sizing::Grow().axis;
        m_items.push_back({ ItemType::Spacer, "", sx, sy });
        return *this;
    }

    /// @brief 为已添加的指定元素设置宽高比。
    /// @param id 待匹配的稳定元素 ID。
    /// @param ratio 宽高比；1 表示正方形，非正值表示禁用约束。
    /// @return 当前容器以支持链式配置。
    CLayBox& setAspectRatio(const std::string& id, float ratio)
    {
        for ( auto& i : m_items )
            if ( i.id == id ) i.aspectRatio = ratio;
        return *this;
    }

    /// @brief 使用 LayoutContext 给定区域生成并执行完整布局。
    /// @param lctx 当前窗口布局上下文。
    /// @warning UI 热路径：每帧重建 Clay 命令并执行元素回调。
    void render(LayoutContext& lctx);

    /// @brief 在当前 ImGui 窗口上下文中执行布局渲染。
    /// @param startPos 起始屏幕绝对坐标。
    /// @param avail 可用空间；非正高度表示按内容适配。
    /// @return 布局生成后的实际尺寸。
    /// @warning UI 热路径：会提交 Dummy 占位并执行全部可见元素回调。
    ImVec2 renderInCurrent(ImVec2 startPos, ImVec2 avail);

protected:
    /// @brief 按指定 Clay 方向初始化空容器。
    /// @param dir 从左到右或从上到下的布局方向。
    CLayBox(Clay_LayoutDirection dir)
        : m_dir(dir)
        , m_gap(0)
        , m_padding({ 0, 0, 0, 0 })
        , m_align(Alignment::Start())
        , m_items({})
    {
    }

    /// @brief 容器内部可生成的节点类别。
    enum class ItemType { Element, Text, Spring, Spacer, NestedLayout };
    /// @brief 单个布局节点的尺寸、内容和绘制状态。
    struct Item {
        /// @brief 决定生成和执行分支的节点类别。
        ItemType type;
        /// @brief Clay 查询布局结果时使用的稳定 ID。
        std::string id;
        /// @brief 横向与纵向尺寸策略。
        Clay_SizingAxis w, h;
        /// @brief 普通元素在布局完成后执行的绘制逻辑。
        DrawFunc drawCallback;
        /// @brief 非拥有嵌套布局指针，由调用方维持生命周期。
        CLayBox* nestedLayout{ nullptr };
        /// @brief 正值启用的宽高比约束。
        float aspectRatio{ 0.0f };
        /// @brief 预留的悬停快照，供后续跨阶段交互扩展。
        bool isHovered{ false };

        /// @brief 文本节点的 UTF-8 内容。
        std::string text;
        /// @brief 文本节点映射到皮肤字体的 FontID 数值。
        uint16_t fontId;
        /// @brief Clay 文本配置字号。
        uint16_t fontSize;
        /// @brief Clay 0 到 255 范围的文本颜色。
        Clay_Color textColor;
    };

    /// @brief 递归生成当前容器及嵌套容器的 Clay 布局节点。
    /// @param currentId 当前容器的稳定 ID。
    /// @param w 当前容器横向尺寸。
    /// @param h 当前容器纵向尺寸。
    void internalGenerate(const char* currentId, Clay_SizingAxis w,
                          Clay_SizingAxis h);
    /// @brief 按布局结果递归执行普通元素、文本和装饰绘制。
    /// @param origin Clay 局部坐标转换到屏幕坐标的原点。
    void internalExecute(ImVec2 origin);

    /// @brief 当前容器主布局方向。
    Clay_LayoutDirection m_dir;
    /// @brief 相邻子项间距。
    uint16_t m_gap;
    /// @brief 容器四边内边距。
    Clay_Padding m_padding;
    /// @brief 子项横纵对齐方式。
    Alignment m_align;
    /// @brief 按插入顺序保存的布局节点。
    std::vector<Item> m_items;

    /// @brief 是否绘制装饰（圆角边框+淡色背景），使用 ImGui 主题色
    bool m_decorated{ false };
};

class CLayHBox : public CLayBox
{
public:
    /// @brief 创建从左到右排列的空容器。
    CLayHBox() : CLayBox(CLAY_LEFT_TO_RIGHT) {}
};
/// @brief 从上到下排列的 CLayBox 具体类型。
class CLayVBox : public CLayBox
{
public:
    /// @brief 创建从上到下排列的空容器。
    CLayVBox() : CLayBox(CLAY_TOP_TO_BOTTOM) {}
};

/// @brief 构造横向布局值对象。
inline CLayHBox CLayBox::HBox()
{
    return CLayHBox();
}
/// @brief 构造纵向布局值对象。
inline CLayVBox CLayBox::VBox()
{
    return CLayVBox();
}

}  // namespace MMM::UI
