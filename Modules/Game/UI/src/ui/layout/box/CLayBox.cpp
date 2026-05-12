#include "config/skin/SkinConfig.h"
#include "ui/layout/box/CLayBox.h"

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

    Clay_EndLayout(ImGui::GetIO().DeltaTime);
    this->internalExecute(lctx.m_startPos);
}

void CLayBox::internalGenerate(const char* currentId, Clay_SizingAxis w,
                                Clay_SizingAxis h)
{
    // 默认使用非静态分配，除非确定是字面量。这里先用 ToCS 确保安全。
    Clay__OpenElementWithId(Clay_GetElementId(ToCS(currentId)));

    Clay_ElementDeclaration decl = {};
    decl.layout                  = { .sizing          = { w, h },  // 应用传入的尺寸
                                     .padding         = m_padding,
                                     .childGap        = m_gap,
                                     .childAlignment  = { m_align.x, m_align.y },
                                     .layoutDirection = m_dir };
    Clay__ConfigureOpenElement(decl);

    for ( size_t i = 0; i < m_items.size(); ++i ) {
        auto& item = m_items[i];
        if ( item.type == ItemType::NestedLayout && item.nestedLayout ) {
            // 递归子布局。注意：item.id 存储在 std::string 中，其 c_str()
            // 此时是稳定的。
            item.nestedLayout->internalGenerate(
                item.id.c_str(), item.w, item.h);
        } else if ( item.type == ItemType::Text ) {
            // 文字在 Clay 中需要特殊的生成方式。
            // 为了能通过 ID 找回其位置，我们将其包裹在一个普通的 Element 中。
            std::string tid = std::string(currentId) + "_t" + std::to_string(i);
            item.id         = tid;  // 暂存生成的 ID 供 execute 阶段查询

            Clay__OpenElementWithId(Clay_GetElementId(ToCS(tid)));
            Clay__ConfigureOpenElement(
                { .layout = { .sizing = { item.w, item.h } } });

            Clay_TextElementConfig cfg = { .textColor = item.textColor,
                                           .fontId    = item.fontId,
                                           .fontSize  = item.fontSize };
            Clay__OpenTextElement(ToCS(item.text), cfg);

            Clay__CloseElement();  // 关闭包裹容器
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
        // 计算当前项的 ID。ToCS(std::string) 是安全的。
        Clay_ElementId itemId = Clay_GetElementId(ToCS(item.id));

        if ( (item.type == ItemType::Element && item.drawCallback) ||
             item.type == ItemType::Text ) {
            auto data = Clay_GetElementData(itemId);
            if ( data.found ) {
                bool   hovered = Clay_PointerOver(itemId);
                ImVec2 pos     = { origin.x + data.boundingBox.x,
                               origin.y + data.boundingBox.y };

                if ( item.type == ItemType::Element ) {
                    ImGui::SetCursorScreenPos(pos);
                    item.drawCallback(data.boundingBox, hovered);
                } else {
                    // 渲染文字
                    ImDrawList* drawList = ImGui::GetWindowDrawList();

                    // 获取对应字体
                    using namespace MMM::Config;
                    auto&   skinMgr = SkinManager::instance();
                    ImFont* font    = nullptr;
                    switch ( static_cast<FontID>(item.fontId) ) {
                    case FontID::Content: font = skinMgr.getFont("content"); break;
                    case FontID::Title: font = skinMgr.getFont("title"); break;
                    case FontID::Menu: font = skinMgr.getFont("menu"); break;
                    case FontID::FileManager:
                        font = skinMgr.getFont("filemanager");
                        break;
                    case FontID::SideBar: font = skinMgr.getFont("side_bar"); break;
                    case FontID::SettingInternal:
                        font = skinMgr.getFont("setting_internal");
                        break;
                    case FontID::PureIcons:
                        font = skinMgr.getFont("pure_icons");
                        break;
                    default: font = ImGui::GetFont(); break;
                    }
                    if ( !font ) font = ImGui::GetFont();

                    // 转换颜色 (Clay 颜色字段通常是 0-255)
                    ImU32 col = ImGui::ColorConvertFloat4ToU32(
                        { item.textColor.r / 255.0f, item.textColor.g / 255.0f,
                          item.textColor.b / 255.0f, item.textColor.a / 255.0f });

                    drawList->AddText(font, (float)item.fontSize * font->Scale,
                                      pos, col, item.text.c_str());
                }
            }
        } else if ( item.type == ItemType::NestedLayout && item.nestedLayout ) {
            item.nestedLayout->internalExecute(origin);
        }
    }
}
}  // namespace MMM::UI
