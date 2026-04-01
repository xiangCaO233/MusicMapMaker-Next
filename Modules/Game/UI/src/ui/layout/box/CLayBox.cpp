#include "ui/layout/box/CLayBox.h"
#include "log/colorful-log.h"

namespace MMM::UI
{

void CLayBox::render(LayoutContext& lctx)
{
    Clay_SetLayoutDimensions({ lctx.m_avail.x, lctx.m_avail.y });
    Clay_BeginLayout();

    // 1. 根节点直接传入外部给定的固定尺寸
    this->internalGenerate("CLAY_ROOT_CONTAINER",
                           Sizing::Fixed(lctx.m_avail.x).axis,
                           Sizing::Fixed(lctx.m_avail.y).axis);

    Clay_EndLayout();
    this->internalExecute(lctx.m_startPos);
}

void CLayBox::internalGenerate(const char* currentId, Clay_SizingAxis w,
                               Clay_SizingAxis h)
{
    Clay__OpenElementWithId(Clay_GetElementId(ToCS(currentId)));

    Clay_ElementDeclaration decl = {};
    decl.layout                  = { .sizing          = { w, h },  // 应用传入的尺寸
                                     .padding         = m_padding,
                                     .childGap        = m_gap,
                                     .childAlignment  = { m_align.x, m_align.y },
                                     .layoutDirection = m_dir };
    Clay__ConfigureOpenElement(decl);

    for ( auto& item : m_items ) {
        if ( item.type == ItemType::NestedLayout && item.nestedLayout ) {
            // 【修复】递归时传入该子布局在 addLayout 时定义的尺寸
            item.nestedLayout->internalGenerate(
                item.id.c_str(), item.w, item.h);
        } else {
            if ( item.type == ItemType::Element )
                Clay__OpenElementWithId(Clay_GetElementId(ToCS(item.id)));
            else
                Clay__OpenElement();

            Clay_ElementDeclaration itemDecl = {};
            itemDecl.layout.sizing           = { item.w, item.h };
            if ( item.aspectRatio > 0 )
                itemDecl.aspectRatio.aspectRatio = item.aspectRatio;

            Clay__ConfigureOpenElement(itemDecl);
            Clay__CloseElement();
        }
    }
    Clay__CloseElement();
}

void CLayBox::internalExecute(ImVec2 origin)
{
    for ( auto& item : m_items ) {
        // 计算当前项的 ID
        Clay_ElementId itemId = Clay_GetElementId(ToCS(item.id));

        if ( item.type == ItemType::Element && item.drawCallback ) {
            auto data = Clay_GetElementData(itemId);
            if ( data.found ) {
                // --- 【核心修改】在这里调用 Clay_PointerOver ---
                // EndLayout 已经执行完毕，此时获取到的 Hover 状态是绝对准确的
                bool hovered = Clay_PointerOver(itemId);

                ImGui::SetCursorScreenPos({ origin.x + data.boundingBox.x,
                                            origin.y + data.boundingBox.y });

                // 传入最新的 hovered 状态
                item.drawCallback(data.boundingBox, hovered);
            }
        } else if ( item.type == ItemType::NestedLayout && item.nestedLayout ) {
            item.nestedLayout->internalExecute(origin);
        }
    }
}
}  // namespace MMM::UI
