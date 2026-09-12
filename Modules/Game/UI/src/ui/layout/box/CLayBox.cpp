#include "ui/layout/box/CLayBox.h"
#include "config/skin/SkinConfig.h"
#include "ui/IUIView.h"

/// @file CLayBox.cpp
/// @brief CLayBox 的布局树生成、ImGui 空间占位和绘制回调执行实现。
/// @details 处理分为生成和执行两阶段：先由 Clay 计算局部边界，再将结果加上
/// ImGui 窗口屏幕原点执行实际绘制。

namespace MMM::UI
{

/// @brief 在外部 LayoutContext 指定的固定区域执行布局。
/// @param lctx 提供可用尺寸和屏幕起点的当前布局上下文。
/// @pre 调用方已通过 CLayWrapperCore 选择该窗口对应的 Clay 上下文。
/// @details 此入口不向 ImGui 提交占位，适用于外层布局已经预留区域的视图。
/// @warning UI 热路径：每帧生成完整容器树并执行全部绘制回调。
void CLayBox::render(LayoutContext& lctx)
{
    // 根布局尺寸严格使用容器分配的可用区域。
    Clay_SetLayoutDimensions({ lctx.m_avail.x, lctx.m_avail.y });
    // Begin/End 包围本帧全部 Clay 元素声明。
    Clay_BeginLayout();

    // 根节点固定到外部分配尺寸，确保弹簧按确定空间分配。
    this->internalGenerate("CLAY_ROOT_CONTAINER",
                           Sizing::Fixed(lctx.m_avail.x).axis,
                           Sizing::Fixed(lctx.m_avail.y).axis);

    // DeltaTime 供 Clay 内部悬停等帧状态推进。
    Clay_EndLayout(ImGui::GetIO().DeltaTime);
    // Clay 计算完成后把局部边界映射到 ImGui 屏幕坐标。
    this->internalExecute(lctx.m_startPos);
}

/// @brief 在当前 ImGui 窗口的指定屏幕区域执行布局。
/// @param startPos 布局左上角屏幕坐标。
/// @param avail 可用宽高，非正高度表示适应内容。
/// @return Clay 计算出的根元素实际宽高。
/// @pre 调用方已选择正确 Clay 上下文且当前存在 ImGui 窗口。
/// @details 与 render 不同，本入口主动提交等尺寸 Dummy 推进 ImGui 内容边界。
/// @warning UI 热路径：每帧生成布局、提交占位并执行回调。
ImVec2 CLayBox::renderInCurrent(ImVec2 startPos, ImVec2 avail)
{
    // 明确高度支持弹簧分配；否则使用 ImGui 当前剩余高度作为计算边界。
    float layoutH =
        (avail.y > 0.0f) ? avail.y : ImGui::GetContentRegionAvail().y;
    Clay_SetLayoutDimensions({ avail.x, layoutH });
    // 指针坐标转换到 Clay 根元素局部空间。
    ImVec2 mousePos = ImGui::GetMousePos();
    Clay_SetPointerState({ mousePos.x - startPos.x, mousePos.y - startPos.y },
                         ImGui::IsMouseDown(ImGuiMouseButton_Left));

    Clay_BeginLayout();
    // 高度未指定时根元素按内容 Fit，避免强制填满剩余窗口。
    Clay_SizingAxis hAxis =
        (avail.y > 0.0f) ? Sizing::Fixed(avail.y).axis : Sizing::Fit().axis;
    this->internalGenerate(
        "CLAY_IN_CURRENT", Sizing::Fixed(avail.x).axis, hAxis);
    Clay_EndLayout(ImGui::GetIO().DeltaTime);

    // 根元素数据决定 ImGui 需要保留的实际内容尺寸。
    auto data = Clay_GetElementData(Clay_GetElementId(ToCS("CLAY_IN_CURRENT")));
    ImVec2 totalSize = { data.boundingBox.width, data.boundingBox.height };

    // 先提交 Dummy 让 ImGui 知道内容边界，防止后续绝对定位越界触发断言。
    ImGui::SetCursorScreenPos(startPos);
    ImGui::Dummy(totalSize);

    // 空间预留后再执行绝对坐标绘制回调。
    this->internalExecute(startPos);

    return totalSize;
}

/// @brief 递归声明当前容器及其所有子项的 Clay 元素。
/// @param currentId 当前容器在布局树中的稳定 ID。
/// @param w 当前容器横向尺寸策略。
/// @param h 当前容器纵向尺寸策略。
/// @details 本阶段只声明节点与文本测量信息，不调用业务绘制回调；空的嵌套
/// 布局指针按无绘制叶节点处理。
/// @warning 布局热路径：每帧遍历 m_items，并为文本节点生成临时 ID。
void CLayBox::internalGenerate(const char* currentId, Clay_SizingAxis w,
                               Clay_SizingAxis h)
{
    // currentId 可能来自 std::string，按非静态字符视图交给 Clay。
    Clay__OpenElementWithId(Clay_GetElementId(ToCS(currentId)));

    // 根声明统一应用容器尺寸、内边距、间距、对齐和方向。
    Clay_ElementDeclaration decl = {};
    decl.layout                  = { .sizing          = { w, h },
                                     .padding         = m_padding,
                                     .childGap        = m_gap,
                                     .childAlignment  = { m_align.x, m_align.y },
                                     .layoutDirection = m_dir };
    Clay__ConfigureOpenElement(decl);

    // 插入顺序即布局顺序，也用于生成确定的文本节点 ID。
    for ( size_t i = 0; i < m_items.size(); ++i ) {
        auto& item = m_items[i];
        if ( item.type == ItemType::NestedLayout && item.nestedLayout ) {
            // 子布局借用 Item 内稳定字符串 ID 并递归生成自身节点。
            item.nestedLayout->internalGenerate(
                item.id.c_str(), item.w, item.h);
        } else if ( item.type == ItemType::Text ) {
            // 文本外包一层具名普通元素，执行阶段才能按 ID 查询其边界。
            // 索引后缀保证同一容器中重复文本内容仍获得不同 ID。
            std::string tid = std::string(currentId) + "_t" + std::to_string(i);
            // 保存生成 ID，供 internalExecute 查询同一布局结果。
            item.id = tid;

            // 包裹元素继承调用方为文本项指定的宽高策略。
            Clay__OpenElementWithId(Clay_GetElementId(ToCS(tid)));
            Clay__ConfigureOpenElement(
                { .layout = { .sizing = { item.w, item.h } } });

            // Clay 使用颜色和字体元数据执行测量，实际字形由 ImGui 绘制。
            Clay_TextElementConfig cfg = { .textColor = item.textColor,
                                           .fontId    = item.fontId,
                                           .fontSize  = item.fontSize };
            Clay__OpenTextElement(ToCS(item.text), cfg);

            // 关闭文本外层的具名包裹容器。
            Clay__CloseElement();
        } else {
            // 只有可绘制普通元素需要稳定 ID，弹簧和间隔仅参与尺寸分配。
            if ( item.type == ItemType::Element )
                Clay__OpenElementWithId(Clay_GetElementId(ToCS(item.id)));
            else
                Clay__OpenElement();

            // 所有非文本叶节点至少携带横纵尺寸策略。
            Clay_ElementDeclaration itemDecl = {};
            itemDecl.layout.sizing           = { item.w, item.h };
            // 正宽高比才启用 Clay aspectRatio 约束。
            if ( item.aspectRatio > 0 )
                itemDecl.aspectRatio.aspectRatio = item.aspectRatio;

            Clay__ConfigureOpenElement(itemDecl);
            Clay__CloseElement();
        }
    }
    // 关闭当前容器根节点，与函数入口 Open 严格配对。
    Clay__CloseElement();
}

/// @brief 查询 Clay 布局结果并递归执行各节点实际绘制。
/// @param origin Clay 局部坐标系对应的 ImGui 屏幕原点。
/// @details Spring 与 Spacer 只影响布局，不产生绘制；Clay 未返回边界的元素
/// 会被跳过，避免向回调传递未初始化矩形。
/// @warning UI 热路径：每帧遍历全部节点，只读取已生成布局和当前皮肤字体。
void CLayBox::internalExecute(ImVec2 origin)
{
    for ( auto& item : m_items ) {
        // Item 字符串在整个查询期间稳定，可安全借用为 Clay_String。
        Clay_ElementId itemId = Clay_GetElementId(ToCS(item.id));

        // 普通元素必须具有回调；文本则由包装层直接绘制。
        if ( (item.type == ItemType::Element && item.drawCallback) ||
             item.type == ItemType::Text ) {
            auto data = Clay_GetElementData(itemId);
            if ( data.found ) {
                // 只有当前布局阶段实际生成的 ID 才具有可用边界。
                // 悬停由 Clay 的局部指针状态计算。
                bool hovered = Clay_PointerOver(itemId);
                // 布局边界加窗口原点后转换为屏幕坐标。
                ImVec2 pos = { origin.x + data.boundingBox.x,
                               origin.y + data.boundingBox.y };

                if ( item.type == ItemType::Element ) {
                    // 元素回调可继续使用 ImGui 当前窗口，因此先移动窗口游标。
                    ImGui::SetCursorScreenPos(pos);
                    // 回调接收屏幕坐标 BoundingBox，便于绘制和命中测试共用。
                    Clay_BoundingBox screenBox = data.boundingBox;
                    screenBox.x += origin.x;
                    screenBox.y += origin.y;
                    item.drawCallback(screenBox, hovered);
                } else {
                    // 文本绕过 ImGui 布局，直接提交到当前窗口 DrawList。
                    ImDrawList* drawList = ImGui::GetWindowDrawList();

                    // FontID 映射到皮肤字体角色，与 Clay 测量回调保持一致。
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
                    // 缺失皮肤字体时回退 ImGui 默认字体，保证文本仍可见。
                    if ( !font ) font = ImGui::GetFont();

                    // Clay 颜色使用 0 到 255，归一化后交给 ImGui 打包 RGBA。
                    ImU32 col = ImGui::ColorConvertFloat4ToU32(
                        { item.textColor.r / 255.0f,
                          item.textColor.g / 255.0f,
                          item.textColor.b / 255.0f,
                          item.textColor.a / 255.0f });

                    // 使用字体实际加载尺寸，确保绘制边界与测量结果一致。
                    drawList->AddText(font,
                                      font->LegacySize * font->Scale,
                                      pos,
                                      col,
                                      item.text.c_str());
                }
            }
        } else if ( item.type == ItemType::NestedLayout && item.nestedLayout ) {
            // 嵌套布局的装饰必须在子元素之前绘制，作为其背景层。
            if ( item.nestedLayout->m_decorated ) {
                // 子布局根 ID 对应其完整边界。
                Clay_ElementId nestedId = Clay_GetElementId(ToCS(item.id));
                auto           data     = Clay_GetElementData(nestedId);
                if ( data.found ) {
                    // 缺失布局结果时跳过装饰，但仍递归执行子布局查询。
                    // 圆角和边框粗细继承当前 ImGui 主题。
                    auto& style    = ImGui::GetStyle();
                    float rounding = style.FrameRounding;

                    // 将 Clay 局部边界转换为屏幕空间角点。
                    ImVec2 pMin = { origin.x + data.boundingBox.x,
                                    origin.y + data.boundingBox.y };
                    ImVec2 pMax = { pMin.x + data.boundingBox.width,
                                    pMin.y + data.boundingBox.height };

                    ImDrawList* dl = ImGui::GetWindowDrawList();

                    // 背景复用 FrameBg，但降低透明度避免压过子项内容。
                    ImVec4 bgCol = style.Colors[ImGuiCol_FrameBg];
                    bgCol.w *= 0.35f;
                    dl->AddRectFilled(pMin,
                                      pMax,
                                      ImGui::ColorConvertFloat4ToU32(bgCol),
                                      rounding);

                    // 边框复用主题 Border 色并适当降低透明度。
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
            // 子布局与父布局共享同一屏幕原点，其边界已包含父级偏移。
            item.nestedLayout->internalExecute(origin);
        }
    }
}
}  // namespace MMM::UI
