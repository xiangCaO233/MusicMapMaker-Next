#include "ui/layout/box/CLayBox.h"
#include "config/skin/SkinConfig.h"
#include "ui/IUIView.h"

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

ImVec2 CLayBox::renderInCurrent(ImVec2 startPos, ImVec2 avail)
{
    // 当 avail.y > 0 时，使用固定高度（弹簧需要已知的总高度来分配空间）
    float layoutH =
        (avail.y > 0.0f) ? avail.y : ImGui::GetContentRegionAvail().y;
    Clay_SetLayoutDimensions({ avail.x, layoutH });
    ImVec2 mousePos = ImGui::GetMousePos();
    Clay_SetPointerState({ mousePos.x - startPos.x, mousePos.y - startPos.y },
                         ImGui::IsMouseDown(ImGuiMouseButton_Left));

    Clay_BeginLayout();
    Clay_SizingAxis hAxis =
        (avail.y > 0.0f) ? Sizing::Fixed(avail.y).axis : Sizing::Fit().axis;
    this->internalGenerate(
        "CLAY_IN_CURRENT", Sizing::Fixed(avail.x).axis, hAxis);
    Clay_EndLayout(ImGui::GetIO().DeltaTime);

    // 获取布局后的实际总尺寸
    auto data = Clay_GetElementData(Clay_GetElementId(ToCS("CLAY_IN_CURRENT")));
    ImVec2 totalSize = { data.boundingBox.width, data.boundingBox.height };

    // 关键修复：在执行 internalExecute 之前，先提交一个 Dummy 以预留空间
    // 防止 internalExecute 中的 SetCursorScreenPos 超出当前窗口边界导致断言崩溃
    ImGui::SetCursorScreenPos(startPos);
    ImGui::Dummy(totalSize);

    // 空间预留后，安全地执行各元素的绘制回调
    this->internalExecute(startPos);

    return totalSize;
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
                    // 传入屏幕坐标系的 BoundingBox（加上 origin 偏移）
                    Clay_BoundingBox screenBox = data.boundingBox;
                    screenBox.x += origin.x;
                    screenBox.y += origin.y;
                    item.drawCallback(screenBox, hovered);
                } else {
                    // 渲染文字
                    ImDrawList* drawList = ImGui::GetWindowDrawList();

                    // 获取对应字体
                    using namespace MMM::Config;
                    auto&   skinMgr = SkinManager::instance();
                    ImFont* font    = nullptr;
                    switch ( static_cast<FontID>(item.fontId) ) {
                    case FontID::Content:
                        font = skinMgr.getFont("content");
                        break;
                    case FontID::Title: font = skinMgr.getFont("title"); break;
                    case FontID::Menu: font = skinMgr.getFont("menu"); break;
                    case FontID::FileManager:
                        font = skinMgr.getFont("filemanager");
                        break;
                    case FontID::SideBar:
                        font = skinMgr.getFont("side_bar");
                        break;
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
                        { item.textColor.r / 255.0f,
                          item.textColor.g / 255.0f,
                          item.textColor.b / 255.0f,
                          item.textColor.a / 255.0f });

                    // 使用字体实际加载尺寸渲染，与测量函数保持一致
                    drawList->AddText(font,
                                      font->LegacySize * font->Scale,
                                      pos,
                                      col,
                                      item.text.c_str());
                }
            }
        } else if ( item.type == ItemType::NestedLayout && item.nestedLayout ) {
            // 如果子布局启用了装饰，先绘制圆角背景 + 边框
            if ( item.nestedLayout->m_decorated ) {
                Clay_ElementId nestedId = Clay_GetElementId(ToCS(item.id));
                auto           data     = Clay_GetElementData(nestedId);
                if ( data.found ) {
                    auto& style    = ImGui::GetStyle();
                    float rounding = style.FrameRounding;

                    ImVec2 pMin = { origin.x + data.boundingBox.x,
                                    origin.y + data.boundingBox.y };
                    ImVec2 pMax = { pMin.x + data.boundingBox.width,
                                    pMin.y + data.boundingBox.height };

                    ImDrawList* dl = ImGui::GetWindowDrawList();

                    // 淡色背景：取 FrameBg 并降低透明度
                    ImVec4 bgCol = style.Colors[ImGuiCol_FrameBg];
                    bgCol.w *= 0.35f;
                    dl->AddRectFilled(pMin,
                                      pMax,
                                      ImGui::ColorConvertFloat4ToU32(bgCol),
                                      rounding);

                    // 边框：取 Border 色
                    ImVec4 borderCol = style.Colors[ImGuiCol_Border];
                    borderCol.w *= 0.6f;
                    dl->AddRect(pMin,
                                pMax,
                                ImGui::ColorConvertFloat4ToU32(borderCol),
                                rounding,
                                0,
                                style.ChildBorderSize);
                }
            }
            item.nestedLayout->internalExecute(origin);
        }
    }
}
}  // namespace MMM::UI
