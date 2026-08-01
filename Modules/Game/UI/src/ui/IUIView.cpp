#include "ui/IUIView.h"

#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"

#include <cmath>

namespace MMM::UI
{

IUIView::IUIView(const std::string& name)
    : m_name(name)
    , m_layoutCtx(CLayWrapperCore::instance().createWindowContext())
{
}

IUIView::~IUIView()
{
    CLayWrapperCore::instance().destroyWindowContext(m_layoutCtx);
}

LayoutContext::LayoutContext(CLayWrapperCore::WindowContext& layoutContext,
                             const std::string&              windowName,
                             bool                            customWindowFlags,
                             ImGuiWindowFlags windowFlags, bool* open,
                             ImGuiID dockId, ImGuiCond dockCond)
{
    CLayWrapperCore::instance().makeCurrent(layoutContext.context);

    auto&   skinManager = Config::SkinManager::instance();
    ImFont* titleFont   = skinManager.getFont("title");
    if ( titleFont ) ImGui::PushFont(titleFont, titleFont->LegacySize);

    auto& editorSettings = Config::AppConfig::instance().getEditorSettings();
    const float dpiScale =
        Config::AppConfig::instance().getWindowContentScale();
    const float windowRound =
        std::floor(editorSettings.aesthetics.windowRounding * dpiScale);
    const float frameRound =
        std::floor(editorSettings.aesthetics.frameRounding * dpiScale);
    const ImVec2 itemSpacing = {
        std::floor(editorSettings.aesthetics.itemSpacing * dpiScale),
        std::floor(editorSettings.aesthetics.itemSpacing * dpiScale)
    };

    m_dpiScale = dpiScale;
    ImGui::PushStyleVar(
        ImGuiStyleVar_WindowPadding,
        ImVec2(std::floor(editorSettings.aesthetics.windowPadding * dpiScale),
               std::floor(editorSettings.aesthetics.windowPadding * dpiScale)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, windowRound);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, windowRound);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, frameRound);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, frameRound);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, itemSpacing);

    if ( dockId != 0 ) {
        ImGui::SetNextWindowDockID(dockId, dockCond);
    }

    const bool wasOpenBeforeBegin = open != nullptr && *open;
    if ( customWindowFlags ) {
        ImGui::Begin(windowName.c_str(), open, windowFlags);
    } else {
        ImGui::Begin(windowName.c_str(), open);
    }
    FeedbackCurrentWindowCloseButton(wasOpenBeforeBegin, open);

    if ( titleFont ) ImGui::PopFont();

    m_startPos    = ImGui::GetCursorScreenPos();
    m_avail       = ImGui::GetContentRegionAvail();
    m_mousePos    = ImGui::GetMousePos();
    m_isMouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);

    Clay_SetPointerState(
        { m_mousePos.x - m_startPos.x, m_mousePos.y - m_startPos.y },
        m_isMouseDown);
}

LayoutContext::~LayoutContext()
{
    ImGui::SetCursorScreenPos(m_startPos);
    ImGui::Dummy(m_avail);
    ImGui::End();
    ImGui::PopStyleVar(6);
}

}  // namespace MMM::UI
