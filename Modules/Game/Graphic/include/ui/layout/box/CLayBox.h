#pragma once

#include "../CLayDefs.h"
#include "imgui.h"
#include "ui/IUIView.h"
#include <cstdint>
#include <functional>
#include <vector>

namespace MMM::Graphic::UI
{

class CLayBox
{
public:
    struct Rect {
        float x, y, w, h;
    };
    using RenderCallback =
        std::function<void(const std::string& id, Rect rect)>;

    virtual ~CLayBox() = default;

    // --- 容器属性 ---
    CLayBox& setSpacing(uint16_t gap)
    {
        m_gap = gap;
        return *this;
    }
    CLayBox& setPadding(uint16_t l, uint16_t r, uint16_t t, uint16_t b)
    {
        m_padding = { l, r, t, b };
        return *this;
    }
    CLayBox& setAlignment(Alignment align)
    {
        m_align = align;
        return *this;
    }

    // --- 添加子项 ---

    // --- 核心优化：带回调的 addElement ---
    // 定义绘图函数：参数是 Clay 算出的 Rect
    using DrawFunc = std::function<void(Clay_BoundingBox rect, bool isHovered)>;
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

    // 添加子布局 (Zero Allocation: 传入引用) ---
    CLayBox& addLayout(const char* id, CLayBox& nested,
                       Sizing w = Sizing::Grow(), Sizing h = Sizing::Grow())
    {
        m_items.push_back(
            { ItemType::NestedLayout, id, w.axis, h.axis, nullptr, &nested });
        return *this;
    }


    // 添加弹簧 (Spring)
    CLayBox& addSpring()
    {
        m_items.push_back(
            { ItemType::Spring, "", Sizing::Grow().axis, Sizing::Grow().axis });
        return *this;
    }

    // 添加固定间隔 (Spacer)
    CLayBox& addSpacing(float px)
    {
        Clay_SizingAxis sx = (m_dir == CLAY_LEFT_TO_RIGHT)
                                 ? Sizing::Fixed(px).axis
                                 : Sizing::Grow().axis;
        Clay_SizingAxis sy = (m_dir == CLAY_TOP_TO_BOTTOM)
                                 ? Sizing::Fixed(px).axis
                                 : Sizing::Grow().axis;
        m_items.push_back({ ItemType::Spacer, "", sx, sy });
        return *this;
    }

    // 设置长宽比 (1.0 = 正方形)
    CLayBox& setAspectRatio(const std::string& id, float ratio)
    {
        for ( auto& i : m_items )
            if ( i.id == id ) i.aspectRatio = ratio;
        return *this;
    }

    // --- 执行布局与映射 ---
    void render(LayoutContext& lctx);

protected:
    CLayBox(Clay_LayoutDirection dir) : m_dir(dir) {}

    enum class ItemType { Element, Spring, Spacer, NestedLayout };
    struct Item {
        ItemType        type;
        std::string     id;
        Clay_SizingAxis w, h;
        DrawFunc        drawCallback;             // 存储绘图逻辑
        CLayBox*        nestedLayout{ nullptr };  // 原始指针，不负责生命周期
        float           aspectRatio{ 0.0f };
        bool            isHovered{ false };  // 用于暂存 Hover 状态
    };

    // 内部递归函数
    void internalGenerate(const char* currentId, Clay_SizingAxis w,
                          Clay_SizingAxis h);
    // 内部绘制执行函数
    void internalExecute(ImVec2 origin);

    Clay_LayoutDirection m_dir;
    uint16_t             m_gap     = 0;
    Clay_Padding         m_padding = { 0 };
    Alignment            m_align   = Alignment::Start();
    std::vector<Item>    m_items;
};

class CLayHBox : public CLayBox
{
public:
    CLayHBox() : CLayBox(CLAY_LEFT_TO_RIGHT) {}
};
class CLayVBox : public CLayBox
{
public:
    CLayVBox() : CLayBox(CLAY_TOP_TO_BOTTOM) {}
};

}  // namespace MMM::Graphic::UI
