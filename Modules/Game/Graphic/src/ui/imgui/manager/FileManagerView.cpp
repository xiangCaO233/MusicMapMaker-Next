#include "ui/imgui/manager/FileManagerView.h"
#include "config/skin/SkinConfig.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "ui/layout/box/CLayBox.h"

namespace MMM::Graphic::UI
{
// 内部绘制逻辑 (Clay/ImGui)
void FileManagerView::onUpdate(LayoutContext& layoutContext)
{
    CLayVBox rootVBox;

    CLayHBox labelHBox;
    auto     fh = ImGui::GetFrameHeight();
    // 获取翻译文本
    auto hintText = TR("ui.file_manager.initial_hint");
    labelHBox.addSpring()
        .addElement(hintText,
                    Sizing::Grow(),
                    Sizing::Fixed(fh),
                    [=](Clay_BoundingBox r, bool isHovered) {
                        // 文本在 30px 高度的坑位里垂直居中
                        float offY = (r.height - ImGui::GetFontSize()) * 0.5f;
                        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offY);

                        // 【技巧】为了真正居中，可以用 ImGui 的居中文字函数
                        // 或者计算偏移：(r.width - CalcTextSize.x) * 0.5f
                        ImVec2 textSize = ImGui::CalcTextSize(hintText);
                        // 移动游标实现垂直居中
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                             (r.width - textSize.x) * 0.5f);

                        ImGui::TextEx(hintText);
                    })
        .addSpring();
    CLayHBox buttonHBox;
    buttonHBox.addSpring()
        .addElement(TR("ui.file_manager.open_directory"),
                    Sizing::Grow(),
                    Sizing::Fixed(fh),
                    [=](Clay_BoundingBox r, bool isHovered) {
                        if ( ImGui::Button(TR("ui.file_manager.open_directory"),
                                           { r.width, r.height }) ) {
                            XINFO("打开文件夹");
                        }
                    })
        .addSpring();

    rootVBox.setPadding(12, 12, 12, 12)
        .setSpacing(12)
        .addLayout("labelHBox", labelHBox, Sizing::Grow(), Sizing::Fixed(40))
        .addLayout("buttonHBox", buttonHBox, Sizing::Grow(), Sizing::Fixed(40))
        .addSpring();
    rootVBox.render(layoutContext);
}

}  // namespace MMM::Graphic::UI
