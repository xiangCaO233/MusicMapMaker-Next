#include "config/skin/SkinConfig.h"
#include "graphic/imguivk/VKContext.h"
#include "imgui_impl_glfw.h"
#include "log/colorful-log.h"

namespace MMM::Graphic
{

static void check_vk_result(VkResult err)
{
    if ( err == VK_SUCCESS ) return;
    XERROR("[vulkan] Error: VkResult = {}", static_cast<uint32_t>(err));
}

/**
 * @brief 初始化 imgui Vulkan
 */
void VKContext::imguiVulkanInit(GLFWwindow* window_handle)
{
    float main_scale = ImGui_ImplGlfw_GetContentScaleForMonitor(
        glfwGetPrimaryMonitor());  // Valid on GLFW 3.3+ only
    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    (void)io;
    // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Enable Gamepad Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    // Enable Docking
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // Enable Multi-Viewport / Platform
    // Windows
    // io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    // io.ConfigViewportsNoAutoMerge = true;
    // io.ConfigViewportsNoTaskBarIcon = true;

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    // ImGui::StyleColorsLight();

    // Setup scaling
    ImGuiStyle& style = ImGui::GetStyle();
    // Bake a fixed style scale. (until we have a solution for
    // dynamic style scaling, changing this requires resetting
    // Style + calling this again)
    style.ScaleAllSizes(main_scale);
    // Set initial font scale. (using
    // io.ConfigDpiScaleFonts=true makes this unnecessary. We
    // leave both here for documentation purpose)
    style.FontScaleDpi = main_scale;
    io.ConfigDpiScaleFonts =
        true;  // [Experimental] Automatically overwrite style.FontScaleDpi in
               // Begin() when Monitor DPI changes. This will scale fonts but
               // _NOT_ scale sizes/padding for now.
    io.ConfigDpiScaleViewports =
        true;  // [Experimental] Scale Dear ImGui and Platform Windows when
               // Monitor DPI changes.

    // When viewports are enabled we tweak WindowRounding/WindowBg so platform
    // windows can look identical to regular ones.
    if ( io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable ) {
        style.WindowRounding              = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForVulkan(window_handle, true);

    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.ApiVersion                = VK_API_VERSION_1_4;
    init_info.Instance                  = m_vkInstance;
    init_info.PhysicalDevice            = m_vkPhysicalDevice;
    init_info.QueueFamily    = m_queueFamilyIndices.graphicsQueueIndex.value();
    init_info.Device         = m_vkLogicalDevice;
    init_info.Queue          = m_LogicDeviceGraphicsQueue;
    init_info.PipelineCache  = nullptr;
    init_info.DescriptorPool = m_vkRenderer->m_vkDescriptorPool;
    init_info.MinImageCount  = 2;  // 根据你的 swapchain 设置
    init_info.ImageCount     = m_swapchain->m_vkImageBuffers.size();
    init_info.PipelineInfoMain.RenderPass  = m_vkRenderPass->getRenderPass();
    init_info.PipelineInfoMain.Subpass     = 0;
    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.Allocator                    = nullptr;
    init_info.CheckVkResultFn              = check_vk_result;

    // 执行初始化imgui-vulkan
    ImGui_ImplVulkan_Init(&init_info);

    // Load Fonts
    // - If fonts are not explicitly loaded, Dear ImGui will call
    // AddFontDefault() to select an embedded font: either
    // AddFontDefaultVector() or AddFontDefaultBitmap().
    //   This selection is based on (style.FontSizeBase * style.FontScaleMain *
    //   style.FontScaleDpi) reaching a small threshold.
    // - You can load multiple fonts and use ImGui::PushFont()/PopFont() to
    // select them.
    // - If a file cannot be loaded, AddFont functions will return a nullptr.
    // Please handle those errors in your code (e.g. use an assertion, display
    // an error and quit).
    // - Read 'docs/FONTS.md' for more instructions and details.
    // - Use '#define IMGUI_ENABLE_FREETYPE' in your imconfig file to use
    // FreeType for higher quality font rendering.
    // - Remember that in C/C++ if you want to include a backslash \ in a string
    // literal you need to write a double backslash \\ !
    // style.FontSizeBase = 20.0f;
    // io.Fonts->AddFontDefaultVector();
    // io.Fonts->AddFontDefaultBitmap();
    // io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\segoeui.ttf");
    // io.Fonts->AddFontFromFileTTF("../../misc/fonts/DroidSans.ttf");
    // io.Fonts->AddFontFromFileTTF("../../misc/fonts/Roboto-Medium.ttf");
    // io.Fonts->AddFontFromFileTTF("../../misc/fonts/Cousine-Regular.ttf");
    // ImFont* font =
    // io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\ArialUni.ttf");
    // IM_ASSERT(font != nullptr);

    auto asciiFontPath = Config::SkinManager::instance().getFontPath("ascii");
    XINFO("asciiFontPath: {}", asciiFontPath.generic_string());

    ImFont* font =
        io.Fonts->AddFontFromFileTTF(asciiFontPath.generic_string().c_str());
    XINFO("ImGui Vulkan backend initialized.");
}

/**
 * @brief 设置imgui样式包
 */
void VKContext::setImguiStyle()
{
    // AdobeInspired style by nexacopic from ImThemes
    ImGuiStyle& style = ImGui::GetStyle();

    style.Alpha                            = 1.0f;
    style.DisabledAlpha                    = 0.6f;
    style.WindowPadding                    = ImVec2(8.0f, 8.0f);
    style.WindowRounding                   = 4.0f;
    style.WindowBorderSize                 = 1.0f;
    style.WindowMinSize                    = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign                 = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition         = ImGuiDir_None;
    style.ChildRounding                    = 4.0f;
    style.ChildBorderSize                  = 1.0f;
    style.PopupRounding                    = 4.0f;
    style.PopupBorderSize                  = 1.0f;
    style.FramePadding                     = ImVec2(4.0f, 3.0f);
    style.FrameRounding                    = 4.0f;
    style.FrameBorderSize                  = 1.0f;
    style.ItemSpacing                      = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing                 = ImVec2(4.0f, 4.0f);
    style.CellPadding                      = ImVec2(4.0f, 2.0f);
    style.IndentSpacing                    = 21.0f;
    style.ColumnsMinSpacing                = 6.0f;
    style.ScrollbarSize                    = 14.0f;
    style.ScrollbarRounding                = 4.0f;
    style.GrabMinSize                      = 10.0f;
    style.GrabRounding                     = 20.0f;
    style.TabRounding                      = 4.0f;
    style.TabBorderSize                    = 1.0f;
    style.TabCloseButtonMinWidthSelected   = 0.0f;
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    style.Colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4(0.49803922f, 0.49803922f, 0.49803922f, 1.0f);
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.11372549f, 0.11372549f, 0.11372549f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.078431375f, 0.078431375f, 0.078431375f, 0.94f);
    style.Colors[ImGuiCol_Border]       = ImVec4(1.0f, 1.0f, 1.0f, 0.16309011f);
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.08627451f, 0.08627451f, 0.08627451f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.15294118f, 0.15294118f, 0.15294118f, 1.0f);
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.1882353f, 0.1882353f, 0.1882353f, 1.0f);
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.11372549f, 0.11372549f, 0.11372549f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.105882354f, 0.105882354f, 0.105882354f, 1.0f);
    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.0f, 0.0f, 0.0f, 0.51f);
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.11372549f, 0.11372549f, 0.11372549f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.019607844f, 0.019607844f, 0.019607844f, 0.53f);
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.30980393f, 0.30980393f, 0.30980393f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.40784314f, 0.40784314f, 0.40784314f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.50980395f, 0.50980395f, 0.50980395f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.8784314f, 0.8784314f, 0.8784314f, 1.0f);
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.98039216f, 0.98039216f, 0.98039216f, 1.0f);
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.14901961f, 0.14901961f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.24705882f, 0.24705882f, 0.24705882f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.32941177f, 0.32941177f, 0.32941177f, 1.0f);
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.9764706f, 0.9764706f, 0.9764706f, 0.30980393f);
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.9764706f, 0.9764706f, 0.9764706f, 0.8f);
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.9764706f, 0.9764706f, 0.9764706f, 1.0f);
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.42745098f, 0.42745098f, 0.49803922f, 0.5f);
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.7490196f, 0.7490196f, 0.7490196f, 0.78039217f);
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.7490196f, 0.7490196f, 0.7490196f, 1.0f);
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.9764706f, 0.9764706f, 0.9764706f, 0.2f);
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.9372549f, 0.9372549f, 0.9372549f, 0.67058825f);
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.9764706f, 0.9764706f, 0.9764706f, 0.9490196f);
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.22352941f, 0.22352941f, 0.22352941f, 0.8627451f);
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.32156864f, 0.32156864f, 0.32156864f, 0.8f);
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.27450982f, 0.27450982f, 0.27450982f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.14509805f, 0.14509805f, 0.14509805f, 0.972549f);
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.42352942f, 0.42352942f, 0.42352942f, 1.0f);
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.60784316f, 0.60784316f, 0.60784316f, 1.0f);
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(1.0f, 0.42745098f, 0.34901962f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.8980392f, 0.69803923f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(1.0f, 0.6f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.1882353f, 0.1882353f, 0.2f, 1.0f);
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.30980393f, 0.30980393f, 0.34901962f, 1.0f);
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(0.22745098f, 0.22745098f, 0.24705882f, 1.0f);
    style.Colors[ImGuiCol_TableRowBg]    = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.35f);
    style.Colors[ImGuiCol_DragDropTarget] = ImVec4(1.0f, 1.0f, 0.0f, 0.9f);
    style.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    style.Colors[ImGuiCol_ModalWindowDimBg]  = ImVec4(0.8f, 0.8f, 0.8f, 0.35f);
}

}  // namespace MMM::Graphic
