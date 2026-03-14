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
    init_info.PipelineInfoMain.RenderPass = m_vkRenderPass->m_graphicRenderPass;
    init_info.PipelineInfoMain.Subpass    = 0;
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

}  // namespace MMM::Graphic
