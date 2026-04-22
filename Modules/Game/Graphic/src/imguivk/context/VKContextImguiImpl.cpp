#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "graphic/glfw/window/NativeWindow.h"
#include "graphic/imguivk/VKContext.h"
#include "imgui_impl_glfw.h"
#include "implot.h"
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
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    (void)io;
    // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Enable Gamepad Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    // Enable Docking
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // No Auto Change Cursor
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    // 禁用保存
    // io.ConfigFlags |= ImGuiConfigFlags_NoKeyboard;

    // Enable Multi-Viewport / Platform
    // Windows
    // io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    // io.ConfigViewportsNoAutoMerge = true;
    // io.ConfigViewportsNoTaskBarIcon = true;

    // Setup Dear ImGui style
    // ImGui::StyleColorsDark();
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

    auto& skinMgr         = Config::SkinManager::instance();
    auto  asciiFontPath   = skinMgr.getFontPath("ascii");
    auto  chineseFontPath = skinMgr.getFontPath("cjk");
    XINFO("asciiFontPath: {}", asciiFontPath.generic_string());

    // 辅助 Lambda: 加载并合并 CJK 字体
    auto loadFontWithSize = [&](const std::string& key, float size) {
        ImFontConfig config;
        config.OversampleH = 2;
        config.OversampleV = 2;

        // 加载基础 ASCII 字体
        ImFont* font = io.Fonts->AddFontFromFileTTF(
            asciiFontPath.generic_string().c_str(), size, &config);

        if ( font ) {
            // 配置合并参数加载 CJK 字体
            ImFontConfig mergeConfig;
            mergeConfig.MergeMode  = true;
            mergeConfig.PixelSnapH = true;
            const ImWchar* ranges  = io.Fonts->GetGlyphRangesChineseFull();

            io.Fonts->AddFontFromFileTTF(
                chineseFontPath.generic_string().c_str(),
                size,
                &mergeConfig,
                ranges);

            skinMgr.setFont(key, font);
        }
        return font;
    };

    // 从皮肤配置中读取各个场景的字体大小
    auto getFontSize = [&](const std::string& key, float defaultSize) {
        std::string val = skinMgr.getLayoutConfig("fontsize." + key);
        return val.empty() ? defaultSize : std::stof(val);
    };

    // 1. 加载默认字体 (也是 content 字体)
    float contentSize = getFontSize("content", 14.0f);
    loadFontWithSize("content", contentSize);

    // 2. 加载其他范围的字体
    float titleFontSize = getFontSize("title", 20.0f);
    XINFO("Loading title font with size: {}", titleFontSize);
    loadFontWithSize("title", titleFontSize);

    loadFontWithSize("menu", getFontSize("menu", 16.0f));
    loadFontWithSize("filemanager", getFontSize("filemanager", 14.0f));
    loadFontWithSize("side_bar", getFontSize("side_bar", 16.0f));
    loadFontWithSize("setting_internal",
                     getFontSize("setting_internal", 14.0f));

    // 重新设置拖拽回调，确保在 ImGui 初始化后仍然有效
    glfwSetDropCallback(window_handle, NativeWindow::GLFW_DropCallback);

    XINFO("ImGui Vulkan backend initialized.");
}

// --- Added applyTheme definition earlier

void VKContext::applyTheme()
{
    auto& settings     = Config::AppConfig::instance().getEditorSettings();
    auto  appliedTheme = settings.theme;
    if ( appliedTheme == Config::UITheme::Auto ) {
        std::string skinTheme =
            Config::SkinManager::instance().getDefaultTheme();
        if ( skinTheme == "Dark" )
            appliedTheme = Config::UITheme::Dark;
        else if ( skinTheme == "Light" )
            appliedTheme = Config::UITheme::Light;
        else if ( skinTheme == "Classic" )
            appliedTheme = Config::UITheme::Classic;
        else if ( skinTheme == "DeepDark" )
            appliedTheme = Config::UITheme::DeepDark;
        else if ( skinTheme == "Microsoft" )
            appliedTheme = Config::UITheme::Microsoft;
        else if ( skinTheme == "Darcula" )
            appliedTheme = Config::UITheme::Darcula;
        else if ( skinTheme == "Photoshop" )
            appliedTheme = Config::UITheme::Photoshop;
    }

    if ( appliedTheme == Config::UITheme::DeepDark ) {
        setDeepDarkStyle();
    } else if ( appliedTheme == Config::UITheme::Dark ) {
        setDarkStyle();
    } else if ( appliedTheme == Config::UITheme::Light ) {
        setLightStyle();
    } else if ( appliedTheme == Config::UITheme::Classic ) {
        setClassicStyle();
    } else if ( appliedTheme == Config::UITheme::Microsoft ) {
        setMicrosoftStyle();
    } else if ( appliedTheme == Config::UITheme::Darcula ) {
        setDarculaStyle();
    } else if ( appliedTheme == Config::UITheme::Photoshop ) {
        setPhotoshopStyle();
    }
}

/**
 * @brief 设置DeepDark样式
 */
void VKContext::setDeepDarkStyle()
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

void VKContext::setDarkStyle()
{
    // Dark style by dougbinks from ImThemes
    ImGuiStyle& style = ImGui::GetStyle();

    style.Alpha                            = 1.0f;
    style.DisabledAlpha                    = 0.6f;
    style.WindowPadding                    = ImVec2(8.0f, 8.0f);
    style.WindowRounding                   = 0.0f;
    style.WindowBorderSize                 = 1.0f;
    style.WindowMinSize                    = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign                 = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition         = ImGuiDir_Left;
    style.ChildRounding                    = 0.0f;
    style.ChildBorderSize                  = 1.0f;
    style.PopupRounding                    = 0.0f;
    style.PopupBorderSize                  = 1.0f;
    style.FramePadding                     = ImVec2(4.0f, 3.0f);
    style.FrameRounding                    = 0.0f;
    style.FrameBorderSize                  = 0.0f;
    style.ItemSpacing                      = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing                 = ImVec2(4.0f, 4.0f);
    style.CellPadding                      = ImVec2(4.0f, 2.0f);
    style.IndentSpacing                    = 21.0f;
    style.ColumnsMinSpacing                = 6.0f;
    style.ScrollbarSize                    = 14.0f;
    style.ScrollbarRounding                = 9.0f;
    style.GrabMinSize                      = 10.0f;
    style.GrabRounding                     = 0.0f;
    style.TabRounding                      = 4.0f;
    style.TabBorderSize                    = 0.0f;
    style.TabCloseButtonMinWidthSelected   = 0.0f;
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    style.Colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4(0.49803922f, 0.49803922f, 0.49803922f, 1.0f);
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.05882353f, 0.05882353f, 0.05882353f, 0.94f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.078431375f, 0.078431375f, 0.078431375f, 0.94f);
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.42745098f, 0.42745098f, 0.49803922f, 0.5f);
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.15686275f, 0.28627452f, 0.47843137f, 0.54f);
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.4f);
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.67f);
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.039215688f, 0.039215688f, 0.039215688f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.15686275f, 0.28627452f, 0.47843137f, 1.0f);
    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.0f, 0.0f, 0.0f, 0.51f);
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.13725491f, 0.13725491f, 0.13725491f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.019607844f, 0.019607844f, 0.019607844f, 0.53f);
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.30980393f, 0.30980393f, 0.30980393f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.40784314f, 0.40784314f, 0.40784314f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.50980395f, 0.50980395f, 0.50980395f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.23921569f, 0.5176471f, 0.8784314f, 1.0f);
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.4f);
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.05882353f, 0.5294118f, 0.9764706f, 1.0f);
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.31f);
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.8f);
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.42745098f, 0.42745098f, 0.49803922f, 0.5f);
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.09803922f, 0.4f, 0.7490196f, 0.78f);
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.09803922f, 0.4f, 0.7490196f, 1.0f);
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.2f);
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.67f);
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.95f);
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.1764706f, 0.34901962f, 0.5764706f, 0.862f);
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.8f);
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.19607843f, 0.40784314f, 0.6784314f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.06666667f, 0.101960786f, 0.14509805f, 0.9724f);
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.13333334f, 0.25882354f, 0.42352942f, 1.0f);
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

void VKContext::setLightStyle()
{
    // Light style by dougbinks from ImThemes
    ImGuiStyle& style = ImGui::GetStyle();

    style.Alpha                            = 1.0f;
    style.DisabledAlpha                    = 0.6f;
    style.WindowPadding                    = ImVec2(8.0f, 8.0f);
    style.WindowRounding                   = 0.0f;
    style.WindowBorderSize                 = 1.0f;
    style.WindowMinSize                    = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign                 = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition         = ImGuiDir_Left;
    style.ChildRounding                    = 0.0f;
    style.ChildBorderSize                  = 1.0f;
    style.PopupRounding                    = 0.0f;
    style.PopupBorderSize                  = 1.0f;
    style.FramePadding                     = ImVec2(4.0f, 3.0f);
    style.FrameRounding                    = 0.0f;
    style.FrameBorderSize                  = 0.0f;
    style.ItemSpacing                      = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing                 = ImVec2(4.0f, 4.0f);
    style.CellPadding                      = ImVec2(4.0f, 2.0f);
    style.IndentSpacing                    = 21.0f;
    style.ColumnsMinSpacing                = 6.0f;
    style.ScrollbarSize                    = 14.0f;
    style.ScrollbarRounding                = 9.0f;
    style.GrabMinSize                      = 10.0f;
    style.GrabRounding                     = 0.0f;
    style.TabRounding                      = 4.0f;
    style.TabBorderSize                    = 0.0f;
    style.TabCloseButtonMinWidthSelected   = 0.0f;
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    style.Colors[ImGuiCol_Text]         = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.9372549f, 0.9372549f, 0.9372549f, 1.0f);
    style.Colors[ImGuiCol_ChildBg]      = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_PopupBg]      = ImVec4(1.0f, 1.0f, 1.0f, 0.98f);
    style.Colors[ImGuiCol_Border]       = ImVec4(0.0f, 0.0f, 0.0f, 0.3f);
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_FrameBg]      = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.4f);
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.67f);
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.95686275f, 0.95686275f, 0.95686275f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.81960785f, 0.81960785f, 0.81960785f, 1.0f);
    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(1.0f, 1.0f, 1.0f, 0.51f);
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.85882354f, 0.85882354f, 0.85882354f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.9764706f, 0.9764706f, 0.9764706f, 0.53f);
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.6862745f, 0.6862745f, 0.6862745f, 0.8f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.4862745f, 0.4862745f, 0.4862745f, 0.8f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.4862745f, 0.4862745f, 0.4862745f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.78f);
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.45882353f, 0.5372549f, 0.8f, 0.6f);
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.4f);
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.05882353f, 0.5294118f, 0.9764706f, 1.0f);
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.31f);
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.8f);
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.3882353f, 0.3882353f, 0.3882353f, 0.62f);
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.13725491f, 0.4392157f, 0.8f, 0.78f);
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.13725491f, 0.4392157f, 0.8f, 1.0f);
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.34901962f, 0.34901962f, 0.34901962f, 0.17f);
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.67f);
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.95f);
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.7607843f, 0.79607844f, 0.8352941f, 0.931f);
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.8f);
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.5921569f, 0.7254902f, 0.88235295f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.91764706f, 0.9254902f, 0.93333334f, 0.9862f);
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.7411765f, 0.81960785f, 0.9137255f, 1.0f);
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.3882353f, 0.3882353f, 0.3882353f, 1.0f);
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(1.0f, 0.42745098f, 0.34901962f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.8980392f, 0.69803923f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(1.0f, 0.44705883f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.7764706f, 0.8666667f, 0.9764706f, 1.0f);
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.5686275f, 0.5686275f, 0.6392157f, 1.0f);
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(0.6784314f, 0.6784314f, 0.7372549f, 1.0f);
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_TableRowBgAlt] =
        ImVec4(0.29803923f, 0.29803923f, 0.29803923f, 0.09f);
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.35f);
    style.Colors[ImGuiCol_DragDropTarget] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.95f);
    style.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.8f);
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(0.69803923f, 0.69803923f, 0.69803923f, 0.7f);
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.2f, 0.2f, 0.2f, 0.2f);
    style.Colors[ImGuiCol_ModalWindowDimBg]  = ImVec4(0.2f, 0.2f, 0.2f, 0.35f);
}

void VKContext::setClassicStyle()
{
    // Classic style by ocornut from ImThemes
    ImGuiStyle& style = ImGui::GetStyle();

    style.Alpha                            = 1.0f;
    style.DisabledAlpha                    = 0.6f;
    style.WindowPadding                    = ImVec2(8.0f, 8.0f);
    style.WindowRounding                   = 0.0f;
    style.WindowBorderSize                 = 1.0f;
    style.WindowMinSize                    = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign                 = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition         = ImGuiDir_Left;
    style.ChildRounding                    = 0.0f;
    style.ChildBorderSize                  = 1.0f;
    style.PopupRounding                    = 0.0f;
    style.PopupBorderSize                  = 1.0f;
    style.FramePadding                     = ImVec2(4.0f, 3.0f);
    style.FrameRounding                    = 0.0f;
    style.FrameBorderSize                  = 0.0f;
    style.ItemSpacing                      = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing                 = ImVec2(4.0f, 4.0f);
    style.CellPadding                      = ImVec2(4.0f, 2.0f);
    style.IndentSpacing                    = 21.0f;
    style.ColumnsMinSpacing                = 6.0f;
    style.ScrollbarSize                    = 14.0f;
    style.ScrollbarRounding                = 9.0f;
    style.GrabMinSize                      = 10.0f;
    style.GrabRounding                     = 0.0f;
    style.TabRounding                      = 4.0f;
    style.TabBorderSize                    = 0.0f;
    style.TabCloseButtonMinWidthSelected   = 0.0f;
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    style.Colors[ImGuiCol_Text] =
        ImVec4(0.8980392f, 0.8980392f, 0.8980392f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
    style.Colors[ImGuiCol_WindowBg]     = ImVec4(0.0f, 0.0f, 0.0f, 0.85f);
    style.Colors[ImGuiCol_ChildBg]      = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.10980392f, 0.10980392f, 0.13725491f, 0.92f);
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.49803922f, 0.49803922f, 0.49803922f, 0.5f);
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.42745098f, 0.42745098f, 0.42745098f, 0.39f);
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.46666667f, 0.46666667f, 0.6862745f, 0.4f);
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.41960785f, 0.40784314f, 0.6392157f, 0.69f);
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.26666668f, 0.26666668f, 0.5372549f, 0.83f);
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.31764707f, 0.31764707f, 0.627451f, 0.87f);
    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.4f, 0.4f, 0.8f, 0.2f);
    style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.4f, 0.4f, 0.54901963f, 0.8f);
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.2f, 0.24705882f, 0.29803923f, 0.6f);
    style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.4f, 0.4f, 0.8f, 0.3f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.4f, 0.4f, 0.8f, 0.4f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.40784314f, 0.3882353f, 0.8f, 0.6f);
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.8980392f, 0.8980392f, 0.8980392f, 0.5f);
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(1.0f, 1.0f, 1.0f, 0.3f);
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.40784314f, 0.3882353f, 0.8f, 0.6f);
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.34901962f, 0.4f, 0.60784316f, 0.62f);
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.4f, 0.47843137f, 0.70980394f, 0.79f);
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.45882353f, 0.5372549f, 0.8f, 1.0f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.4f, 0.4f, 0.8980392f, 0.45f);
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.44705883f, 0.44705883f, 0.8980392f, 0.8f);
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.5294118f, 0.5294118f, 0.8666667f, 0.8f);
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.49803922f, 0.49803922f, 0.49803922f, 0.6f);
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.6f, 0.6f, 0.69803923f, 1.0f);
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.69803923f, 0.69803923f, 0.8980392f, 1.0f);
    style.Colors[ImGuiCol_ResizeGrip] = ImVec4(1.0f, 1.0f, 1.0f, 0.1f);
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.7764706f, 0.81960785f, 1.0f, 0.6f);
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.7764706f, 0.81960785f, 1.0f, 0.9f);
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.33333334f, 0.33333334f, 0.68235296f, 0.786f);
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.44705883f, 0.44705883f, 0.8980392f, 0.8f);
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.40392157f, 0.40392157f, 0.7254902f, 0.842f);
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.28235295f, 0.28235295f, 0.5686275f, 0.8212f);
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.34901962f, 0.34901962f, 0.6509804f, 0.8372f);
    style.Colors[ImGuiCol_PlotLines] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(0.8980392f, 0.69803923f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.8980392f, 0.69803923f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(1.0f, 0.6f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.26666668f, 0.26666668f, 0.3764706f, 1.0f);
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.30980393f, 0.30980393f, 0.44705883f, 1.0f);
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(0.25882354f, 0.25882354f, 0.2784314f, 1.0f);
    style.Colors[ImGuiCol_TableRowBg]     = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_TableRowBgAlt]  = ImVec4(1.0f, 1.0f, 1.0f, 0.07f);
    style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.0f, 0.0f, 1.0f, 0.35f);
    style.Colors[ImGuiCol_DragDropTarget] = ImVec4(1.0f, 1.0f, 0.0f, 0.9f);
    style.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.44705883f, 0.44705883f, 0.8980392f, 0.8f);
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    style.Colors[ImGuiCol_ModalWindowDimBg]  = ImVec4(0.2f, 0.2f, 0.2f, 0.35f);
}

void VKContext::setMicrosoftStyle()
{
    // Microsoft style by usernameiwantedwasalreadytaken from ImThemes
    ImGuiStyle& style = ImGui::GetStyle();

    style.Alpha                            = 1.0f;
    style.DisabledAlpha                    = 0.6f;
    style.WindowPadding                    = ImVec2(4.0f, 6.0f);
    style.WindowRounding                   = 0.0f;
    style.WindowBorderSize                 = 0.0f;
    style.WindowMinSize                    = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign                 = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition         = ImGuiDir_Left;
    style.ChildRounding                    = 0.0f;
    style.ChildBorderSize                  = 1.0f;
    style.PopupRounding                    = 0.0f;
    style.PopupBorderSize                  = 1.0f;
    style.FramePadding                     = ImVec2(8.0f, 6.0f);
    style.FrameRounding                    = 0.0f;
    style.FrameBorderSize                  = 1.0f;
    style.ItemSpacing                      = ImVec2(8.0f, 6.0f);
    style.ItemInnerSpacing                 = ImVec2(8.0f, 6.0f);
    style.CellPadding                      = ImVec2(4.0f, 2.0f);
    style.IndentSpacing                    = 20.0f;
    style.ColumnsMinSpacing                = 6.0f;
    style.ScrollbarSize                    = 20.0f;
    style.ScrollbarRounding                = 0.0f;
    style.GrabMinSize                      = 5.0f;
    style.GrabRounding                     = 0.0f;
    style.TabRounding                      = 4.0f;
    style.TabBorderSize                    = 0.0f;
    style.TabCloseButtonMinWidthSelected   = 0.0f;
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    style.Colors[ImGuiCol_Text] =
        ImVec4(0.09803922f, 0.09803922f, 0.09803922f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4(0.49803922f, 0.49803922f, 0.49803922f, 1.0f);
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.9490196f, 0.9490196f, 0.9490196f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] =
        ImVec4(0.9490196f, 0.9490196f, 0.9490196f, 1.0f);
    style.Colors[ImGuiCol_PopupBg]      = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_Border]       = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_FrameBg]      = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.0f, 0.46666667f, 0.8392157f, 0.2f);
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.0f, 0.46666667f, 0.8392157f, 1.0f);
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.039215688f, 0.039215688f, 0.039215688f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.15686275f, 0.28627452f, 0.47843137f, 1.0f);
    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.0f, 0.0f, 0.0f, 0.51f);
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.85882354f, 0.85882354f, 0.85882354f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.85882354f, 0.85882354f, 0.85882354f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.6862745f, 0.6862745f, 0.6862745f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.0f, 0.0f, 0.0f, 0.2f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.0f, 0.0f, 0.0f, 0.5f);
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.09803922f, 0.09803922f, 0.09803922f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.6862745f, 0.6862745f, 0.6862745f, 1.0f);
    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.0f, 0.0f, 0.0f, 0.5f);
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.85882354f, 0.85882354f, 0.85882354f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.0f, 0.46666667f, 0.8392157f, 0.2f);
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.0f, 0.46666667f, 0.8392157f, 1.0f);
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.85882354f, 0.85882354f, 0.85882354f, 1.0f);
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.0f, 0.46666667f, 0.8392157f, 0.2f);
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.0f, 0.46666667f, 0.8392157f, 1.0f);
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.42745098f, 0.42745098f, 0.49803922f, 0.5f);
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.09803922f, 0.4f, 0.7490196f, 0.78f);
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.09803922f, 0.4f, 0.7490196f, 1.0f);
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.2f);
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.67f);
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.95f);
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.1764706f, 0.34901962f, 0.5764706f, 0.862f);
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.8f);
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.19607843f, 0.40784314f, 0.6784314f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.06666667f, 0.101960786f, 0.14509805f, 0.9724f);
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.13333334f, 0.25882354f, 0.42352942f, 1.0f);
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

void VKContext::setDarculaStyle()
{
    // Darcula style by ice1000 from ImThemes
    ImGuiStyle& style = ImGui::GetStyle();

    style.Alpha                            = 1.0f;
    style.DisabledAlpha                    = 0.6f;
    style.WindowPadding                    = ImVec2(8.0f, 8.0f);
    style.WindowRounding                   = 5.3f;
    style.WindowBorderSize                 = 1.0f;
    style.WindowMinSize                    = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign                 = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition         = ImGuiDir_Left;
    style.ChildRounding                    = 0.0f;
    style.ChildBorderSize                  = 1.0f;
    style.PopupRounding                    = 0.0f;
    style.PopupBorderSize                  = 1.0f;
    style.FramePadding                     = ImVec2(4.0f, 3.0f);
    style.FrameRounding                    = 2.3f;
    style.FrameBorderSize                  = 1.0f;
    style.ItemSpacing                      = ImVec2(8.0f, 6.5f);
    style.ItemInnerSpacing                 = ImVec2(4.0f, 4.0f);
    style.CellPadding                      = ImVec2(4.0f, 2.0f);
    style.IndentSpacing                    = 21.0f;
    style.ColumnsMinSpacing                = 6.0f;
    style.ScrollbarSize                    = 14.0f;
    style.ScrollbarRounding                = 5.0f;
    style.GrabMinSize                      = 10.0f;
    style.GrabRounding                     = 2.3f;
    style.TabRounding                      = 4.0f;
    style.TabBorderSize                    = 0.0f;
    style.TabCloseButtonMinWidthSelected   = 0.0f;
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    style.Colors[ImGuiCol_Text] =
        ImVec4(0.73333335f, 0.73333335f, 0.73333335f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4(0.34509805f, 0.34509805f, 0.34509805f, 1.0f);
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.23529412f, 0.24705882f, 0.25490198f, 0.94f);
    style.Colors[ImGuiCol_ChildBg] =
        ImVec4(0.23529412f, 0.24705882f, 0.25490198f, 0.0f);
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.23529412f, 0.24705882f, 0.25490198f, 0.94f);
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.33333334f, 0.33333334f, 0.33333334f, 0.5f);
    style.Colors[ImGuiCol_BorderShadow] =
        ImVec4(0.15686275f, 0.15686275f, 0.15686275f, 0.0f);
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.16862746f, 0.16862746f, 0.16862746f, 0.54f);
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.4509804f, 0.6745098f, 0.99607843f, 0.67f);
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.47058824f, 0.47058824f, 0.47058824f, 0.67f);
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.039215688f, 0.039215688f, 0.039215688f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.0f, 0.0f, 0.0f, 0.51f);
    style.Colors[ImGuiCol_TitleBgCollapsed] =
        ImVec4(0.15686275f, 0.28627452f, 0.47843137f, 1.0f);
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.27058825f, 0.28627452f, 0.2901961f, 0.8f);
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.27058825f, 0.28627452f, 0.2901961f, 0.6f);
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.21960784f, 0.30980393f, 0.41960785f, 0.51f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.21960784f, 0.30980393f, 0.41960785f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.13725491f, 0.19215687f, 0.2627451f, 0.91f);
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.8980392f, 0.8980392f, 0.8980392f, 0.83f);
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.69803923f, 0.69803923f, 0.69803923f, 0.62f);
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.29803923f, 0.29803923f, 0.29803923f, 0.84f);
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.33333334f, 0.3529412f, 0.36078432f, 0.49f);
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.21960784f, 0.30980393f, 0.41960785f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.13725491f, 0.19215687f, 0.2627451f, 1.0f);
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.33333334f, 0.3529412f, 0.36078432f, 0.53f);
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.4509804f, 0.6745098f, 0.99607843f, 0.67f);
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.47058824f, 0.47058824f, 0.47058824f, 0.67f);
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.3137255f, 0.3137255f, 0.3137255f, 1.0f);
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.3137255f, 0.3137255f, 0.3137255f, 1.0f);
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.3137255f, 0.3137255f, 0.3137255f, 1.0f);
    style.Colors[ImGuiCol_ResizeGrip]        = ImVec4(1.0f, 1.0f, 1.0f, 0.85f);
    style.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.6f);
    style.Colors[ImGuiCol_ResizeGripActive]  = ImVec4(1.0f, 1.0f, 1.0f, 0.9f);
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.1764706f, 0.34901962f, 0.5764706f, 0.862f);
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.8f);
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.19607843f, 0.40784314f, 0.6784314f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.06666667f, 0.101960786f, 0.14509805f, 0.9724f);
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.13333334f, 0.25882354f, 0.42352942f, 1.0f);
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
        ImVec4(0.18431373f, 0.39607844f, 0.7921569f, 0.9f);
    style.Colors[ImGuiCol_DragDropTarget] = ImVec4(1.0f, 1.0f, 0.0f, 0.9f);
    style.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    style.Colors[ImGuiCol_ModalWindowDimBg]  = ImVec4(0.8f, 0.8f, 0.8f, 0.35f);
}

void VKContext::setPhotoshopStyle()
{
    // Photoshop style by Derydoca from ImThemes
    ImGuiStyle& style = ImGui::GetStyle();

    style.Alpha                            = 1.0f;
    style.DisabledAlpha                    = 0.6f;
    style.WindowPadding                    = ImVec2(8.0f, 8.0f);
    style.WindowRounding                   = 4.0f;
    style.WindowBorderSize                 = 1.0f;
    style.WindowMinSize                    = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign                 = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition         = ImGuiDir_Left;
    style.ChildRounding                    = 4.0f;
    style.ChildBorderSize                  = 1.0f;
    style.PopupRounding                    = 2.0f;
    style.PopupBorderSize                  = 1.0f;
    style.FramePadding                     = ImVec2(4.0f, 3.0f);
    style.FrameRounding                    = 2.0f;
    style.FrameBorderSize                  = 1.0f;
    style.ItemSpacing                      = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing                 = ImVec2(4.0f, 4.0f);
    style.CellPadding                      = ImVec2(4.0f, 2.0f);
    style.IndentSpacing                    = 21.0f;
    style.ColumnsMinSpacing                = 6.0f;
    style.ScrollbarSize                    = 13.0f;
    style.ScrollbarRounding                = 12.0f;
    style.GrabMinSize                      = 7.0f;
    style.GrabRounding                     = 0.0f;
    style.TabRounding                      = 0.0f;
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
        ImVec4(0.1764706f, 0.1764706f, 0.1764706f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] =
        ImVec4(0.2784314f, 0.2784314f, 0.2784314f, 0.0f);
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.30980393f, 0.30980393f, 0.30980393f, 1.0f);
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.2627451f, 0.2627451f, 0.2627451f, 1.0f);
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.15686275f, 0.15686275f, 0.15686275f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.2f, 0.2f, 0.2f, 1.0f);
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.2784314f, 0.2784314f, 0.2784314f, 1.0f);
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.14509805f, 0.14509805f, 0.14509805f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.14509805f, 0.14509805f, 0.14509805f, 1.0f);
    style.Colors[ImGuiCol_TitleBgCollapsed] =
        ImVec4(0.14509805f, 0.14509805f, 0.14509805f, 1.0f);
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.19215687f, 0.19215687f, 0.19215687f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.15686275f, 0.15686275f, 0.15686275f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.27450982f, 0.27450982f, 0.27450982f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.29803923f, 0.29803923f, 0.29803923f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(1.0f, 0.3882353f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.3882353f, 0.3882353f, 0.3882353f, 1.0f);
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(1.0f, 0.3882353f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_Button]        = ImVec4(1.0f, 1.0f, 1.0f, 0.0f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.156f);
    style.Colors[ImGuiCol_ButtonActive]  = ImVec4(1.0f, 1.0f, 1.0f, 0.391f);
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.30980393f, 0.30980393f, 0.30980393f, 1.0f);
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.46666667f, 0.46666667f, 0.46666667f, 1.0f);
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.46666667f, 0.46666667f, 0.46666667f, 1.0f);
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.2627451f, 0.2627451f, 0.2627451f, 1.0f);
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.3882353f, 0.3882353f, 0.3882353f, 1.0f);
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(1.0f, 0.3882353f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_ResizeGrip]        = ImVec4(1.0f, 1.0f, 1.0f, 0.25f);
    style.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.67f);
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(1.0f, 0.3882353f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.09411765f, 0.09411765f, 0.09411765f, 1.0f);
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.34901962f, 0.34901962f, 0.34901962f, 1.0f);
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.19215687f, 0.19215687f, 0.19215687f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.09411765f, 0.09411765f, 0.09411765f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.19215687f, 0.19215687f, 0.19215687f, 1.0f);
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.46666667f, 0.46666667f, 0.46666667f, 1.0f);
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(1.0f, 0.3882353f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.58431375f, 0.58431375f, 0.58431375f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(1.0f, 0.3882353f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.1882353f, 0.1882353f, 0.2f, 1.0f);
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.30980393f, 0.30980393f, 0.34901962f, 1.0f);
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(0.22745098f, 0.22745098f, 0.24705882f, 1.0f);
    style.Colors[ImGuiCol_TableRowBg]     = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_TableRowBgAlt]  = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(1.0f, 1.0f, 1.0f, 0.156f);
    style.Colors[ImGuiCol_DragDropTarget] =
        ImVec4(1.0f, 0.3882353f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_NavHighlight] = ImVec4(1.0f, 0.3882353f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 0.3882353f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.586f);
    style.Colors[ImGuiCol_ModalWindowDimBg]  = ImVec4(0.0f, 0.0f, 0.0f, 0.586f);
}

}  // namespace MMM::Graphic
