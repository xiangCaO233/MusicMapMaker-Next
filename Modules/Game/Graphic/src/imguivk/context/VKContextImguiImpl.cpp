#include "config/AppConfig.h"
#include "config/AppPaths.h"
#include "config/Utf8Path.h"
#include "config/fonticon/NerdFontData.h"
#include "config/skin/SkinConfig.h"
#include "event/core/EventBus.h"
#include "event/ui/ClearColorUpdateEvent.h"
#include "graphic/glfw/window/NativeWindow.h"
#include "graphic/imguivk/VKContext.h"
#include "imgui_impl_glfw.h"
#include "implot.h"
#include "log/colorful-log.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

namespace MMM::Graphic
{

/// @brief Wayland 下独立图标字体的视觉缩放，避免 DPI 下方形按钮裁切字形。
static constexpr float WAYLAND_PURE_ICON_VISUAL_SCALE = 0.86f;

/// @brief 获取独立图标字体的运行时视觉缩放。
/// @return Wayland 平台返回修正倍率，其他平台保持 1.0。
static float getPureIconVisualScale()
{
    return glfwGetPlatform() == GLFW_PLATFORM_WAYLAND
               ? WAYLAND_PURE_ICON_VISUAL_SCALE
               : 1.0f;
}

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
    float native_scale = Config::AppConfig::instance().getNativeContentScale();
    float ui_scale     = Config::AppConfig::instance().getUIScale();
    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiStyle& style = ImGui::GetStyle();
    ImGuiIO&    io    = ImGui::GetIO();
    (void)io;
    /// @brief 创建 ImGui ini 所在目录时接收的文件系统错误。
    std::error_code imguiIniDirectoryError;
    std::filesystem::create_directories(Config::AppPaths::configRootPath(),
                                        imguiIniDirectoryError);
    if ( imguiIniDirectoryError ) {
        XWARN("Failed to create ImGui ini directory: {}",
              imguiIniDirectoryError.message());
    }
    /// @brief ImGui ini 文件路径字符串，必须静态保存以满足 ImGui
    /// 指针生命周期要求。
    static const std::string imguiIniPath =
        Config::pathToUtf8(Config::AppPaths::imguiIniFilePath());
    io.IniFilename = imguiIniPath.c_str();
    // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Enable Gamepad Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    // Enable Docking
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // 禁用保存
    // io.ConfigFlags |= ImGuiConfigFlags_NoKeyboard;

    // Enable Multi-Viewport / Platform
    // Windows
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.ConfigViewportsNoAutoMerge        = false;
    io.ConfigViewportsNoTaskBarIcon      = true;
    io.ConfigWindowsMoveFromTitleBarOnly = true;

    // Setup Dear ImGui style
    // ImGui::StyleColorsDark();
    // ImGui::StyleColorsLight();

    // Setup scaling
    // style.ScaleAllSizes(ui_scale); // We don't scale style sizes globally
    // here yet
    style.FontScaleDpi = 1.0f;

    io.ConfigDpiScaleFonts     = false;  // Handled manually below
    io.ConfigDpiScaleViewports = true;

    // When viewports are enabled we tweak WindowRounding/WindowBg so platform
    // windows can look identical to regular ones.

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

    // 重新设置拖拽回调，确保在 ImGui 初始化后仍然有效
    glfwSetDropCallback(window_handle, NativeWindow::GLFW_DropCallback);

    setupFonts();

    applyTheme();
    XDEBUG("ImGui Vulkan backend initialized.");
}

/**
 * @brief 加载 ImGui 运行时字体，并按当前 DPI 建立字体缩放基准。
 * @warning 仅在初始化或低频字体重建时执行，不应进入每帧 UI 热路径。
 */
void VKContext::setupFonts()
{
    ImGuiIO& io        = ImGui::GetIO();
    auto&    settings  = Config::AppConfig::instance().getEditorSettings();
    float native_scale = Config::AppConfig::instance().getNativeContentScale();
    float ui_scale     = Config::AppConfig::instance().getUIScale();
    auto& skinMgr      = Config::SkinManager::instance();
    if ( !std::isfinite(native_scale) || native_scale <= 0.0f ) {
        native_scale = 1.0f;
    }
    if ( !std::isfinite(ui_scale) || ui_scale <= 0.0f ) {
        ui_scale = 1.0f;
    }
    m_fontAtlasBaseScale = ui_scale / native_scale;
    if ( !std::isfinite(m_fontAtlasBaseScale) ||
         m_fontAtlasBaseScale <= 0.0f ) {
        m_fontAtlasBaseScale = 1.0f;
    }
    m_fontAtlasScaleMain = settings.fontSizeMultiplier;
    if ( !std::isfinite(m_fontAtlasScaleMain) ||
         m_fontAtlasScaleMain <= 0.0f ) {
        m_fontAtlasScaleMain = 1.0f;
    }
    m_fontAtlasScaleMain = std::clamp(m_fontAtlasScaleMain, 0.5f, 2.0f);
    ImGui::GetStyle().FontScaleMain = m_fontAtlasScaleMain;

    // --- 字体范围定义 ---
    static const ImWchar ascii_ranges[] = { 0x0020, 0x00FF, 0 };
    static const ImWchar cjk_ranges[]   = { 0x2000, 0x206F, 0x3000, 0x30FF,
                                            0x3105, 0x312D, 0x4E00, 0x9FFF,
                                            0xFF00, 0xFFEF, 0 };
    static const ImWchar nerd_ranges[]  = { 0xE000, 0xF8FF, 0 };

    auto loadFontWithSize = [&](const std::string& key, float size) {
        auto& settings      = Config::AppConfig::instance().getEditorSettings();
        auto  asciiFontPath = skinMgr.getFontPath("ascii");
        auto  cjkFontPath   = skinMgr.getFontPath("cjk");

        // 尝试加载偏好 ASCII 字体
        if ( !settings.preferredAsciiFont.empty() &&
             settings.preferredAsciiFont != "Default" ) {
            const auto& asciiFonts = skinMgr.getAsciiFonts();
            auto        it         = std::find_if(
                asciiFonts.begin(), asciiFonts.end(), [&](const auto& pair) {
                    return pair.first == settings.preferredAsciiFont;
                });
            if ( it != asciiFonts.end() ) {
                asciiFontPath = it->second;
            } else if ( std::filesystem::exists(
                            Config::utf8ToPath(settings.preferredAsciiFont)) ) {
                // 如果是绝对路径，说明是外部/系统字体
                asciiFontPath = settings.preferredAsciiFont;
            }
        }

        // 尝试加载偏好 CJK 字体
        if ( !settings.preferredCjkFont.empty() &&
             settings.preferredCjkFont != "Default" ) {
            const auto& cjkFonts = skinMgr.getCjkFonts();
            auto        it       = std::find_if(
                cjkFonts.begin(), cjkFonts.end(), [&](const auto& pair) {
                    return pair.first == settings.preferredCjkFont;
                });
            if ( it != cjkFonts.end() ) {
                cjkFontPath = it->second;
            } else if ( std::filesystem::exists(
                            Config::utf8ToPath(settings.preferredCjkFont)) ) {
                // 如果是绝对路径，说明是外部/系统字体
                cjkFontPath = settings.preferredCjkFont;
            }
        }

        ImFontConfig config;
        // ImGui 1.92 dynamic atlas may rasterize glyphs lazily while rendering.
        // STB's oversample prefilter asserts on some CJK/OpenType glyph edges
        // in debug builds, so keep oversampling disabled for runtime-loaded
        // fonts.
        config.OversampleH = 1;
        config.OversampleV = 1;
        config.PixelSnapH  = true;

        // Load at physical pixel size for sharpness
        float atlasSize = std::max(1.0f, size * native_scale);

        // 1. 加载基础 ASCII 字体 (严格限制范围)
        ImFont* font = io.Fonts->AddFontFromFileTTF(
            Config::pathToUtf8(asciiFontPath).c_str(),
            atlasSize,
            &config,
            ascii_ranges);

        if ( font ) {
            // 2. 配置合并参数加载 CJK 字体 (严格限制范围)
            ImFontConfig mergeConfig;
            mergeConfig.MergeMode   = true;
            mergeConfig.PixelSnapH  = true;
            mergeConfig.OversampleH = 1;
            mergeConfig.OversampleV = 1;

            io.Fonts->AddFontFromFileTTF(
                Config::pathToUtf8(cjkFontPath).c_str(),
                atlasSize,
                &mergeConfig,
                cjk_ranges);

            // 3. 合并嵌入的 NerdFont 图标
            ImFontConfig iconConfig;
            iconConfig.MergeMode            = true;
            iconConfig.PixelSnapH           = true;
            iconConfig.FontDataOwnedByAtlas = false;  // 数据在静态区，不要释放
            iconConfig.OversampleH          = 1;
            iconConfig.OversampleV          = 1;

            // 向上稍微偏移 (-0.05x)，并设置缩放为 0.9x
            // 缩小尺寸后，图标会靠近基准线（显得偏下），因此需要负向偏移来使其在按钮/行内视觉居中
            iconConfig.GlyphOffset.y = -(size * 0.05f) * native_scale;

            io.Fonts->AddFontFromMemoryTTF((void*)Config::g_nerdfont_data,
                                           Config::g_nerdfont_data_size,
                                           atlasSize * 0.9f,
                                           &iconConfig,
                                           nerd_ranges);

            // 固定当前 atlas 生命周期的字体缩放，避免运行时切换动态烘焙尺寸。
            font->Scale = m_fontAtlasBaseScale;

            skinMgr.setFont(key, font);
        }
        return font;
    };

    // 从皮肤配置中读取各个场景的字体大小
    auto getFontSize = [&](const std::string& key, float defaultSize) {
        std::string val = skinMgr.getLayoutConfig("fontsize." + key);
        return val.empty() ? defaultSize : std::stof(val);
    };

    // 加载各个场景的字体
    loadFontWithSize("content", getFontSize("content", 14.0f));
    loadFontWithSize("title", getFontSize("title", 20.0f));
    loadFontWithSize("menu", getFontSize("menu", 16.0f));
    loadFontWithSize("filemanager", getFontSize("filemanager", 14.0f));
    loadFontWithSize("side_bar", getFontSize("side_bar", 16.0f));
    loadFontWithSize("setting_internal",
                     getFontSize("setting_internal", 14.0f));

    // 4. 加载独立的纯图标字体
    // (不合并，专门用于侧边栏、工具栏等不需要随文字缩放的地方)
    {
        ImFontConfig iconConfig;
        iconConfig.PixelSnapH           = true;
        iconConfig.FontDataOwnedByAtlas = false;
        iconConfig.OversampleH          = 1;
        iconConfig.OversampleV          = 1;

        // 独立图标字体保持较小的基础尺寸，避免方形按钮内裁切。
        float size               = 16.0f;
        float atlasSize          = std::max(1.0f, size * native_scale);
        iconConfig.GlyphOffset.y = -(size * 0.05f) * native_scale;

        ImFont* iconFont =
            io.Fonts->AddFontFromMemoryTTF((void*)Config::g_nerdfont_data,
                                           Config::g_nerdfont_data_size,
                                           atlasSize * 0.9f,
                                           &iconConfig,
                                           nerd_ranges);
        if ( iconFont ) {
            iconFont->Scale = m_fontAtlasBaseScale * getPureIconVisualScale();
        }
        skinMgr.setFont("pure_icons", iconFont);
    }
}

void VKContext::rebuildFonts()
{
    XINFO("Hot-reloading ImGui fonts...");
    // 确保设备空闲
    if ( m_vkLogicalDevice.waitIdle() != vk::Result::eSuccess ) {
        XERROR("Failed to wait for device idle during font reload.");
        return;
    }

    ImGuiIO& io = ImGui::GetIO();

    Config::SkinManager::instance().clearRuntimeFonts();

    // 清除旧字体
    io.Fonts->Clear();

    // 重新运行字体加载
    setupFonts();

    XINFO("Fonts rebuilt.");
}

/**
 * @brief 刷新已经加载的 ImGui 运行时字体缩放。
 * @warning 仅响应 DPI 或字体设置变化时调用，不应进入每帧 UI 热路径。
 */
void VKContext::updateFontScales()
{
    ImGui::GetStyle().FontScaleMain = m_fontAtlasScaleMain;

    auto& skinMgr = Config::SkinManager::instance();
    for ( auto& [key, font] : skinMgr.getData().runtimeFonts ) {
        if ( font ) {
            font->Scale = key == "pure_icons"
                              ? m_fontAtlasBaseScale * getPureIconVisualScale()
                              : m_fontAtlasBaseScale;
        }
    }
}

void VKContext::requestFontRebuild()
{
    m_fontRebuildRequested.store(true, std::memory_order_release);
}

void VKContext::checkAndRebuildFonts()
{
    if ( m_fontRebuildRequested.exchange(false, std::memory_order_acq_rel) ) {
        rebuildFonts();
    }
}

// --- Added applyTheme definition earlier

void VKContext::applyTheme()
{
    auto& settings    = Config::AppConfig::instance().getEditorSettings();
    ImGui::GetStyle() = ImGuiStyle();
    auto appliedTheme = settings.theme;
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
        else if ( skinTheme == "Unreal" )
            appliedTheme = Config::UITheme::Unreal;
        else if ( skinTheme == "Gold" )
            appliedTheme = Config::UITheme::Gold;
        else if ( skinTheme == "RoundedVisualStudio" )
            appliedTheme = Config::UITheme::RoundedVisualStudio;
        else if ( skinTheme == "SonicRiders" )
            appliedTheme = Config::UITheme::SonicRiders;
        else if ( skinTheme == "DarkRuda" )
            appliedTheme = Config::UITheme::DarkRuda;
        else if ( skinTheme == "SoftCherry" )
            appliedTheme = Config::UITheme::SoftCherry;
        else if ( skinTheme == "Enemymouse" )
            appliedTheme = Config::UITheme::Enemymouse;
        else if ( skinTheme == "DiscordDark" )
            appliedTheme = Config::UITheme::DiscordDark;
        else if ( skinTheme == "Comfy" )
            appliedTheme = Config::UITheme::Comfy;
        else if ( skinTheme == "PurpleComfy" )
            appliedTheme = Config::UITheme::PurpleComfy;
        else if ( skinTheme == "FutureDark" )
            appliedTheme = Config::UITheme::FutureDark;
        else if ( skinTheme == "CleanDark" )
            appliedTheme = Config::UITheme::CleanDark;
        else if ( skinTheme == "Moonlight" )
            appliedTheme = Config::UITheme::Moonlight;
        else if ( skinTheme == "Cecilia" || skinTheme == "MmmDefault" )
            appliedTheme = Config::UITheme::Cecilia;
        else if ( skinTheme == "ComfortableLight" )
            appliedTheme = Config::UITheme::ComfortableLight;
        else if ( skinTheme == "HazyDark" )
            appliedTheme = Config::UITheme::HazyDark;
        else if ( skinTheme == "Everforest" )
            appliedTheme = Config::UITheme::Everforest;
        else if ( skinTheme == "Windark" )
            appliedTheme = Config::UITheme::Windark;
        else if ( skinTheme == "Rest" )
            appliedTheme = Config::UITheme::Rest;
        else if ( skinTheme == "ComfortableDarkCyan" )
            appliedTheme = Config::UITheme::ComfortableDarkCyan;
        else if ( skinTheme == "KazamCherry" )
            appliedTheme = Config::UITheme::KazamCherry;
    }

    switch ( appliedTheme ) {
    case Config::UITheme::DeepDark: setDeepDarkStyle(); break;
    case Config::UITheme::Dark: setDarkStyle(); break;
    case Config::UITheme::Light: setLightStyle(); break;
    case Config::UITheme::Classic: setClassicStyle(); break;
    case Config::UITheme::Microsoft: setMicrosoftStyle(); break;
    case Config::UITheme::Darcula: setDarculaStyle(); break;
    case Config::UITheme::Photoshop: setPhotoshopStyle(); break;
    case Config::UITheme::Unreal: setUnrealStyle(); break;
    case Config::UITheme::Gold: setGoldStyle(); break;
    case Config::UITheme::RoundedVisualStudio:
        setRoundedVisualStudioStyle();
        break;
    case Config::UITheme::SonicRiders: setSonicRidersStyle(); break;
    case Config::UITheme::DarkRuda: setDarkRudaStyle(); break;
    case Config::UITheme::SoftCherry: setSoftCherryStyle(); break;
    case Config::UITheme::Enemymouse: setEnemymouseStyle(); break;
    case Config::UITheme::DiscordDark: setDiscordDarkStyle(); break;
    case Config::UITheme::Comfy: setComfyStyle(); break;
    case Config::UITheme::PurpleComfy: setPurpleComfyStyle(); break;
    case Config::UITheme::FutureDark: setFutureDarkStyle(); break;
    case Config::UITheme::CleanDark: setCleanDarkStyle(); break;
    case Config::UITheme::Moonlight: setMoonlightStyle(); break;
    case Config::UITheme::Cecilia: setCeciliaStyle(); break;
    case Config::UITheme::ComfortableLight: setComfortableLightStyle(); break;
    case Config::UITheme::HazyDark: setHazyDarkStyle(); break;
    case Config::UITheme::Everforest: setEverforestStyle(); break;
    case Config::UITheme::Windark: setWindarkStyle(); break;
    case Config::UITheme::Rest: setRestStyle(); break;
    case Config::UITheme::ComfortableDarkCyan:
        setComfortableDarkCyanStyle();
        break;
    case Config::UITheme::KazamCherry: setKazamCherryStyle(); break;
    default: setDeepDarkStyle(); break;
    }

    // 应用全局缩放 (注意：ScaleAllSizes 是增量修改，但由于各 setStyle
    // 函数都会重置 style，所以这里直接应用是安全的)
    ImGuiStyle& style = ImGui::GetStyle();
    float dpiScale    = Config::AppConfig::instance().getWindowContentScale();
    auto& aes         = settings.aesthetics;

    style.ScaleAllSizes(settings.uiScaleMultiplier);
    style.FontScaleMain = m_fontAtlasScaleMain;

    // 应用审美设置 (防止 setStyle 重置了这些值)
    style.WindowRounding = std::floor(aes.windowRounding * dpiScale);
    style.ChildRounding  = style.WindowRounding;
    style.FrameRounding  = std::floor(aes.frameRounding * dpiScale);
    style.PopupRounding  = style.WindowRounding;
    style.TabRounding    = style.FrameRounding;
    style.ItemSpacing    = { std::floor(aes.itemSpacing * dpiScale),
                             std::floor(aes.itemSpacing * dpiScale) };
    style.WindowPadding  = { std::floor(aes.windowPadding * dpiScale),
                             std::floor(aes.windowPadding * dpiScale) };

    // 发布清屏颜色更新事件，同步 Vulkan 背景色与 UI 菜单栏背景色
    ImVec4 barBg = ImGui::GetStyle().Colors[ImGuiCol_MenuBarBg];
    Event::ClearColorUpdateEvent clearEvt;
    clearEvt.clear_color_value = { barBg.x, barBg.y, barBg.z, barBg.w };
    Event::EventBus::instance().publish(clearEvt);
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

void VKContext::setUnrealStyle()
{
    // Unreal style by dev0-1 from ImThemes
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
    style.Colors[ImGuiCol_ChildBg] = ImVec4(1.0f, 1.0f, 1.0f, 0.0f);
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.078431375f, 0.078431375f, 0.078431375f, 0.94f);
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.42745098f, 0.42745098f, 0.49803922f, 0.5f);
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.2f, 0.20784314f, 0.21960784f, 0.54f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.4f, 0.4f, 0.4f, 0.4f);
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.1764706f, 0.1764706f, 0.1764706f, 0.67f);
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.039215688f, 0.039215688f, 0.039215688f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.28627452f, 0.28627452f, 0.28627452f, 1.0f);
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
        ImVec4(0.9372549f, 0.9372549f, 0.9372549f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.50980395f, 0.50980395f, 0.50980395f, 1.0f);
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.85882354f, 0.85882354f, 0.85882354f, 1.0f);
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.4392157f, 0.4392157f, 0.4392157f, 0.4f);
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.45882353f, 0.46666667f, 0.47843137f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.41960785f, 0.41960785f, 0.41960785f, 1.0f);
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.69803923f, 0.69803923f, 0.69803923f, 0.31f);
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.69803923f, 0.69803923f, 0.69803923f, 0.8f);
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.47843137f, 0.49803922f, 0.5176471f, 1.0f);
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.42745098f, 0.42745098f, 0.49803922f, 0.5f);
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.7176471f, 0.7176471f, 0.7176471f, 0.78f);
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.50980395f, 0.50980395f, 0.50980395f, 1.0f);
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.9098039f, 0.9098039f, 0.9098039f, 0.25f);
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.80784315f, 0.80784315f, 0.80784315f, 0.67f);
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.45882353f, 0.45882353f, 0.45882353f, 0.95f);
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
        ImVec4(0.7294118f, 0.6f, 0.14901961f, 1.0f);
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
        ImVec4(0.8666667f, 0.8666667f, 0.8666667f, 0.35f);
    style.Colors[ImGuiCol_DragDropTarget] = ImVec4(1.0f, 1.0f, 0.0f, 0.9f);
    style.Colors[ImGuiCol_NavHighlight]   = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    style.Colors[ImGuiCol_ModalWindowDimBg]  = ImVec4(0.8f, 0.8f, 0.8f, 0.35f);
}

void VKContext::setGoldStyle()
{
    // Gold style by CookiePLMonster from ImThemes
    ImGuiStyle& style = ImGui::GetStyle();

    style.Alpha                            = 1.0f;
    style.DisabledAlpha                    = 0.6f;
    style.WindowPadding                    = ImVec2(8.0f, 8.0f);
    style.WindowRounding                   = 4.0f;
    style.WindowBorderSize                 = 1.0f;
    style.WindowMinSize                    = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign                 = ImVec2(1.0f, 0.5f);
    style.WindowMenuButtonPosition         = ImGuiDir_Right;
    style.ChildRounding                    = 0.0f;
    style.ChildBorderSize                  = 1.0f;
    style.PopupRounding                    = 4.0f;
    style.PopupBorderSize                  = 1.0f;
    style.FramePadding                     = ImVec2(4.0f, 2.0f);
    style.FrameRounding                    = 4.0f;
    style.FrameBorderSize                  = 0.0f;
    style.ItemSpacing                      = ImVec2(10.0f, 2.0f);
    style.ItemInnerSpacing                 = ImVec2(4.0f, 4.0f);
    style.CellPadding                      = ImVec2(4.0f, 2.0f);
    style.IndentSpacing                    = 12.0f;
    style.ColumnsMinSpacing                = 6.0f;
    style.ScrollbarSize                    = 10.0f;
    style.ScrollbarRounding                = 6.0f;
    style.GrabMinSize                      = 10.0f;
    style.GrabRounding                     = 4.0f;
    style.TabRounding                      = 4.0f;
    style.TabBorderSize                    = 0.0f;
    style.TabCloseButtonMinWidthSelected   = 0.0f;
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    style.Colors[ImGuiCol_Text] =
        ImVec4(0.91764706f, 0.91764706f, 0.91764706f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4(0.4392157f, 0.4392157f, 0.4392157f, 1.0f);
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.05882353f, 0.05882353f, 0.05882353f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.078431375f, 0.078431375f, 0.078431375f, 0.94f);
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.50980395f, 0.35686275f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.10980392f, 0.10980392f, 0.10980392f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.50980395f, 0.35686275f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.7764706f, 0.54901963f, 0.20784314f, 1.0f);
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.50980395f, 0.35686275f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.9098039f, 0.6392157f, 0.12941177f, 1.0f);
    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.0f, 0.0f, 0.0f, 0.51f);
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.10980392f, 0.10980392f, 0.10980392f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.05882353f, 0.05882353f, 0.05882353f, 0.53f);
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.20784314f, 0.20784314f, 0.20784314f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.46666667f, 0.46666667f, 0.46666667f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.80784315f, 0.827451f, 0.80784315f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.7764706f, 0.54901963f, 0.20784314f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.9098039f, 0.6392157f, 0.12941177f, 1.0f);
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.9098039f, 0.6392157f, 0.12941177f, 1.0f);
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.50980395f, 0.35686275f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.9098039f, 0.6392157f, 0.12941177f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.7764706f, 0.54901963f, 0.20784314f, 1.0f);
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.50980395f, 0.35686275f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.9098039f, 0.6392157f, 0.12941177f, 1.0f);
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.92941177f, 0.64705884f, 0.13725491f, 1.0f);
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.20784314f, 0.20784314f, 0.20784314f, 1.0f);
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.9098039f, 0.6392157f, 0.12941177f, 1.0f);
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.7764706f, 0.54901963f, 0.20784314f, 1.0f);
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.20784314f, 0.20784314f, 0.20784314f, 1.0f);
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.9098039f, 0.6392157f, 0.12941177f, 1.0f);
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.7764706f, 0.54901963f, 0.20784314f, 1.0f);
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.50980395f, 0.35686275f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.9098039f, 0.6392157f, 0.12941177f, 1.0f);
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.7764706f, 0.54901963f, 0.20784314f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.06666667f, 0.09803922f, 0.14901961f, 0.97f);
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.13725491f, 0.25882354f, 0.41960785f, 1.0f);
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

void VKContext::setRoundedVisualStudioStyle()
{
    // Rounded Visual Studio style by RedNicStone from ImThemes
    ImGuiStyle& style = ImGui::GetStyle();

    style.Alpha                            = 1.0f;
    style.DisabledAlpha                    = 0.6f;
    style.WindowPadding                    = ImVec2(8.0f, 8.0f);
    style.WindowRounding                   = 4.0f;
    style.WindowBorderSize                 = 0.0f;
    style.WindowMinSize                    = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign                 = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition         = ImGuiDir_Left;
    style.ChildRounding                    = 0.0f;
    style.ChildBorderSize                  = 1.0f;
    style.PopupRounding                    = 4.0f;
    style.PopupBorderSize                  = 1.0f;
    style.FramePadding                     = ImVec2(4.0f, 3.0f);
    style.FrameRounding                    = 2.5f;
    style.FrameBorderSize                  = 0.0f;
    style.ItemSpacing                      = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing                 = ImVec2(4.0f, 4.0f);
    style.CellPadding                      = ImVec2(4.0f, 2.0f);
    style.IndentSpacing                    = 21.0f;
    style.ColumnsMinSpacing                = 6.0f;
    style.ScrollbarSize                    = 11.0f;
    style.ScrollbarRounding                = 2.5f;
    style.GrabMinSize                      = 10.0f;
    style.GrabRounding                     = 2.0f;
    style.TabRounding                      = 3.5f;
    style.TabBorderSize                    = 0.0f;
    style.TabCloseButtonMinWidthSelected   = 0.0f;
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    style.Colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4(0.5921569f, 0.5921569f, 0.5921569f, 1.0f);
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.14509805f, 0.14509805f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] =
        ImVec4(0.14509805f, 0.14509805f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.14509805f, 0.14509805f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.30588236f, 0.30588236f, 0.30588236f, 1.0f);
    style.Colors[ImGuiCol_BorderShadow] =
        ImVec4(0.30588236f, 0.30588236f, 0.30588236f, 1.0f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.2f, 0.2f, 0.21568628f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.11372549f, 0.5921569f, 0.9254902f, 1.0f);
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.0f, 0.46666667f, 0.78431374f, 1.0f);
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.14509805f, 0.14509805f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.14509805f, 0.14509805f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_TitleBgCollapsed] =
        ImVec4(0.14509805f, 0.14509805f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_MenuBarBg]   = ImVec4(0.2f, 0.2f, 0.21568628f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.2f, 0.2f, 0.21568628f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.32156864f, 0.32156864f, 0.33333334f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.3529412f, 0.3529412f, 0.37254903f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.3529412f, 0.3529412f, 0.37254903f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.0f, 0.46666667f, 0.78431374f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.11372549f, 0.5921569f, 0.9254902f, 1.0f);
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.0f, 0.46666667f, 0.78431374f, 1.0f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.2f, 0.2f, 0.21568628f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.11372549f, 0.5921569f, 0.9254902f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.11372549f, 0.5921569f, 0.9254902f, 1.0f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.2f, 0.2f, 0.21568628f, 1.0f);
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.11372549f, 0.5921569f, 0.9254902f, 1.0f);
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.0f, 0.46666667f, 0.78431374f, 1.0f);
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.30588236f, 0.30588236f, 0.30588236f, 1.0f);
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.30588236f, 0.30588236f, 0.30588236f, 1.0f);
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.30588236f, 0.30588236f, 0.30588236f, 1.0f);
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.14509805f, 0.14509805f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.2f, 0.2f, 0.21568628f, 1.0f);
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.32156864f, 0.32156864f, 0.33333334f, 1.0f);
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.14509805f, 0.14509805f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.11372549f, 0.5921569f, 0.9254902f, 1.0f);
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.0f, 0.46666667f, 0.78431374f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.14509805f, 0.14509805f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.0f, 0.46666667f, 0.78431374f, 1.0f);
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.0f, 0.46666667f, 0.78431374f, 1.0f);
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(0.11372549f, 0.5921569f, 0.9254902f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.0f, 0.46666667f, 0.78431374f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(0.11372549f, 0.5921569f, 0.9254902f, 1.0f);
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.1882353f, 0.1882353f, 0.2f, 1.0f);
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.30980393f, 0.30980393f, 0.34901962f, 1.0f);
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(0.22745098f, 0.22745098f, 0.24705882f, 1.0f);
    style.Colors[ImGuiCol_TableRowBg]    = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(0.0f, 0.46666667f, 0.78431374f, 1.0f);
    style.Colors[ImGuiCol_DragDropTarget] =
        ImVec4(0.14509805f, 0.14509805f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.14509805f, 0.14509805f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    style.Colors[ImGuiCol_ModalWindowDimBg] =
        ImVec4(0.14509805f, 0.14509805f, 0.14901961f, 1.0f);
}

void VKContext::setSonicRidersStyle()
{
    // Sonic Riders style by Sewer56 from ImThemes
    ImGuiStyle& style = ImGui::GetStyle();

    style.Alpha                            = 1.0f;
    style.DisabledAlpha                    = 0.6f;
    style.WindowPadding                    = ImVec2(8.0f, 8.0f);
    style.WindowRounding                   = 0.0f;
    style.WindowBorderSize                 = 0.0f;
    style.WindowMinSize                    = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign                 = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition         = ImGuiDir_Left;
    style.ChildRounding                    = 0.0f;
    style.ChildBorderSize                  = 1.0f;
    style.PopupRounding                    = 0.0f;
    style.PopupBorderSize                  = 0.0f;
    style.FramePadding                     = ImVec2(4.0f, 3.0f);
    style.FrameRounding                    = 4.0f;
    style.FrameBorderSize                  = 0.0f;
    style.ItemSpacing                      = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing                 = ImVec2(4.0f, 4.0f);
    style.CellPadding                      = ImVec2(4.0f, 2.0f);
    style.IndentSpacing                    = 21.0f;
    style.ColumnsMinSpacing                = 6.0f;
    style.ScrollbarSize                    = 14.0f;
    style.ScrollbarRounding                = 9.0f;
    style.GrabMinSize                      = 10.0f;
    style.GrabRounding                     = 4.0f;
    style.TabRounding                      = 4.0f;
    style.TabBorderSize                    = 0.0f;
    style.TabCloseButtonMinWidthSelected   = 0.0f;
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    style.Colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4(0.7294118f, 0.7490196f, 0.7372549f, 1.0f);
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.08627451f, 0.08627451f, 0.08627451f, 0.94f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.078431375f, 0.078431375f, 0.078431375f, 0.94f);
    style.Colors[ImGuiCol_Border]       = ImVec4(0.2f, 0.2f, 0.2f, 0.5f);
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.70980394f, 0.3882353f, 0.3882353f, 0.54f);
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.8392157f, 0.65882355f, 0.65882355f, 0.4f);
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.8392157f, 0.65882355f, 0.65882355f, 0.67f);
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.46666667f, 0.21960784f, 0.21960784f, 0.67f);
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.46666667f, 0.21960784f, 0.21960784f, 1.0f);
    style.Colors[ImGuiCol_TitleBgCollapsed] =
        ImVec4(0.46666667f, 0.21960784f, 0.21960784f, 0.67f);
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.3372549f, 0.15686275f, 0.15686275f, 1.0f);
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
        ImVec4(0.70980394f, 0.3882353f, 0.3882353f, 1.0f);
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.8392157f, 0.65882355f, 0.65882355f, 1.0f);
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.46666667f, 0.21960784f, 0.21960784f, 0.65f);
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.70980394f, 0.3882353f, 0.3882353f, 0.65f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.2f, 0.2f, 0.2f, 0.5f);
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.70980394f, 0.3882353f, 0.3882353f, 0.54f);
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.8392157f, 0.65882355f, 0.65882355f, 0.65f);
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.8392157f, 0.65882355f, 0.65882355f, 0.0f);
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.42745098f, 0.42745098f, 0.49803922f, 0.5f);
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.70980394f, 0.3882353f, 0.3882353f, 0.54f);
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.70980394f, 0.3882353f, 0.3882353f, 0.54f);
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.70980394f, 0.3882353f, 0.3882353f, 0.54f);
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.8392157f, 0.65882355f, 0.65882355f, 0.66f);
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.8392157f, 0.65882355f, 0.65882355f, 0.66f);
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.70980394f, 0.3882353f, 0.3882353f, 0.54f);
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.8392157f, 0.65882355f, 0.65882355f, 0.66f);
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.8392157f, 0.65882355f, 0.65882355f, 0.66f);
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.06666667f, 0.09803922f, 0.14901961f, 0.97f);
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.13725491f, 0.25882354f, 0.41960785f, 1.0f);
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
        ImVec4(0.40784314f, 0.40784314f, 0.40784314f, 1.0f);
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    style.Colors[ImGuiCol_ModalWindowDimBg]  = ImVec4(0.8f, 0.8f, 0.8f, 0.35f);
}

void VKContext::setDarkRudaStyle()
{
    // Dark Ruda style by Raikiri from ImThemes
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
    style.FrameRounding                    = 4.0f;
    style.FrameBorderSize                  = 0.0f;
    style.ItemSpacing                      = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing                 = ImVec2(4.0f, 4.0f);
    style.CellPadding                      = ImVec2(4.0f, 2.0f);
    style.IndentSpacing                    = 21.0f;
    style.ColumnsMinSpacing                = 6.0f;
    style.ScrollbarSize                    = 14.0f;
    style.ScrollbarRounding                = 9.0f;
    style.GrabMinSize                      = 10.0f;
    style.GrabRounding                     = 4.0f;
    style.TabRounding                      = 4.0f;
    style.TabBorderSize                    = 0.0f;
    style.TabCloseButtonMinWidthSelected   = 0.0f;
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    style.Colors[ImGuiCol_Text] =
        ImVec4(0.9490196f, 0.95686275f, 0.9764706f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4(0.35686275f, 0.41960785f, 0.46666667f, 1.0f);
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.10980392f, 0.14901961f, 0.16862746f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] =
        ImVec4(0.14901961f, 0.1764706f, 0.21960784f, 1.0f);
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.078431375f, 0.078431375f, 0.078431375f, 0.94f);
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.078431375f, 0.09803922f, 0.11764706f, 1.0f);
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.2f, 0.24705882f, 0.28627452f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.11764706f, 0.2f, 0.2784314f, 1.0f);
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.08627451f, 0.11764706f, 0.13725491f, 1.0f);
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.08627451f, 0.11764706f, 0.13725491f, 0.65f);
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.078431375f, 0.09803922f, 0.11764706f, 1.0f);
    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.0f, 0.0f, 0.0f, 0.51f);
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.14901961f, 0.1764706f, 0.21960784f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.019607844f, 0.019607844f, 0.019607844f, 0.39f);
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.2f, 0.24705882f, 0.28627452f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.1764706f, 0.21960784f, 0.24705882f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.08627451f, 0.20784314f, 0.30980393f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.2784314f, 0.5568628f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.2784314f, 0.5568628f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.36862746f, 0.60784316f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.2f, 0.24705882f, 0.28627452f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.2784314f, 0.5568628f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.05882353f, 0.5294118f, 0.9764706f, 1.0f);
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.2f, 0.24705882f, 0.28627452f, 0.55f);
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.8f);
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.2f, 0.24705882f, 0.28627452f, 1.0f);
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.09803922f, 0.4f, 0.7490196f, 0.78f);
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.09803922f, 0.4f, 0.7490196f, 1.0f);
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.25f);
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.67f);
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.95f);
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.10980392f, 0.14901961f, 0.16862746f, 1.0f);
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.8f);
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.2f, 0.24705882f, 0.28627452f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.10980392f, 0.14901961f, 0.16862746f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.10980392f, 0.14901961f, 0.16862746f, 1.0f);
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

void VKContext::setSoftCherryStyle()
{
    // Soft Cherry style by Patitotective from ImThemes
    ImGuiStyle& style = ImGui::GetStyle();

    style.Alpha                            = 1.0f;
    style.DisabledAlpha                    = 0.4f;
    style.WindowPadding                    = ImVec2(10.0f, 10.0f);
    style.WindowRounding                   = 4.0f;
    style.WindowBorderSize                 = 0.0f;
    style.WindowMinSize                    = ImVec2(50.0f, 50.0f);
    style.WindowTitleAlign                 = ImVec2(0.5f, 0.5f);
    style.WindowMenuButtonPosition         = ImGuiDir_Left;
    style.ChildRounding                    = 0.0f;
    style.ChildBorderSize                  = 1.0f;
    style.PopupRounding                    = 1.0f;
    style.PopupBorderSize                  = 1.0f;
    style.FramePadding                     = ImVec2(5.0f, 3.0f);
    style.FrameRounding                    = 3.0f;
    style.FrameBorderSize                  = 0.0f;
    style.ItemSpacing                      = ImVec2(6.0f, 6.0f);
    style.ItemInnerSpacing                 = ImVec2(3.0f, 2.0f);
    style.CellPadding                      = ImVec2(3.0f, 3.0f);
    style.IndentSpacing                    = 6.0f;
    style.ColumnsMinSpacing                = 6.0f;
    style.ScrollbarSize                    = 13.0f;
    style.ScrollbarRounding                = 16.0f;
    style.GrabMinSize                      = 20.0f;
    style.GrabRounding                     = 4.0f;
    style.TabRounding                      = 4.0f;
    style.TabBorderSize                    = 1.0f;
    style.TabCloseButtonMinWidthSelected   = 0.0f;
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    style.Colors[ImGuiCol_Text] =
        ImVec4(0.85882354f, 0.92941177f, 0.8862745f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4(0.52156866f, 0.54901963f, 0.53333336f, 1.0f);
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.12941177f, 0.13725491f, 0.16862746f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] =
        ImVec4(0.14901961f, 0.15686275f, 0.1882353f, 1.0f);
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.2f, 0.21960784f, 0.26666668f, 1.0f);
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.13725491f, 0.11372549f, 0.13333334f, 1.0f);
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.16862746f, 0.18431373f, 0.23137255f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.23137255f, 0.2f, 0.27058825f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.5019608f, 0.07450981f, 0.25490198f, 1.0f);
    style.Colors[ImGuiCol_TitleBgCollapsed] =
        ImVec4(0.2f, 0.21960784f, 0.26666668f, 1.0f);
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.2f, 0.21960784f, 0.26666668f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.23921569f, 0.23921569f, 0.21960784f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.3882353f, 0.3882353f, 0.37254903f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.69411767f, 0.69411767f, 0.6862745f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.69411767f, 0.69411767f, 0.6862745f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.65882355f, 0.13725491f, 0.1764706f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.6509804f, 0.14901961f, 0.34509805f, 1.0f);
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.70980394f, 0.21960784f, 0.26666668f, 1.0f);
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.6509804f, 0.14901961f, 0.34509805f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.6509804f, 0.14901961f, 0.34509805f, 1.0f);
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.5019608f, 0.07450981f, 0.25490198f, 1.0f);
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.42745098f, 0.42745098f, 0.49803922f, 1.0f);
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.09803922f, 0.4f, 0.7490196f, 1.0f);
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.09803922f, 0.4f, 0.7490196f, 1.0f);
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.6509804f, 0.14901961f, 0.34509805f, 1.0f);
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.1764706f, 0.34901962f, 0.5764706f, 1.0f);
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.19607843f, 0.40784314f, 0.6784314f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.06666667f, 0.101960786f, 0.14509805f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.13333334f, 0.25882354f, 0.42352942f, 1.0f);
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.85882354f, 0.92941177f, 0.8862745f, 1.0f);
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.30980393f, 0.7764706f, 0.19607843f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.1882353f, 0.1882353f, 0.2f, 1.0f);
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.30980393f, 0.30980393f, 0.34901962f, 1.0f);
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(0.22745098f, 0.22745098f, 0.24705882f, 1.0f);
    style.Colors[ImGuiCol_TableRowBg]    = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(0.38431373f, 0.627451f, 0.91764706f, 1.0f);
    style.Colors[ImGuiCol_DragDropTarget] = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
    style.Colors[ImGuiCol_ModalWindowDimBg]  = ImVec4(0.8f, 0.8f, 0.8f, 0.3f);
}

void VKContext::setEnemymouseStyle()
{
    // Enemymouse style by enemymouse from ImThemes
    ImGuiStyle& style = ImGui::GetStyle();

    style.Alpha                            = 1.0f;
    style.DisabledAlpha                    = 0.6f;
    style.WindowPadding                    = ImVec2(8.0f, 8.0f);
    style.WindowRounding                   = 3.0f;
    style.WindowBorderSize                 = 1.0f;
    style.WindowMinSize                    = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign                 = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition         = ImGuiDir_Left;
    style.ChildRounding                    = 3.0f;
    style.ChildBorderSize                  = 1.0f;
    style.PopupRounding                    = 0.0f;
    style.PopupBorderSize                  = 1.0f;
    style.FramePadding                     = ImVec2(4.0f, 3.0f);
    style.FrameRounding                    = 3.0f;
    style.FrameBorderSize                  = 0.0f;
    style.ItemSpacing                      = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing                 = ImVec2(4.0f, 4.0f);
    style.CellPadding                      = ImVec2(4.0f, 2.0f);
    style.IndentSpacing                    = 21.0f;
    style.ColumnsMinSpacing                = 6.0f;
    style.ScrollbarSize                    = 14.0f;
    style.ScrollbarRounding                = 9.0f;
    style.GrabMinSize                      = 20.0f;
    style.GrabRounding                     = 1.0f;
    style.TabRounding                      = 4.0f;
    style.TabBorderSize                    = 0.0f;
    style.TabCloseButtonMinWidthSelected   = 0.0f;
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    style.Colors[ImGuiCol_Text]         = ImVec4(0.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.0f, 0.4f, 0.40784314f, 1.0f);
    style.Colors[ImGuiCol_WindowBg]     = ImVec4(0.0f, 0.0f, 0.0f, 0.83f);
    style.Colors[ImGuiCol_ChildBg]      = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.15686275f, 0.23921569f, 0.21960784f, 0.6f);
    style.Colors[ImGuiCol_Border]       = ImVec4(0.0f, 1.0f, 1.0f, 0.65f);
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_FrameBg]      = ImVec4(0.4392157f, 0.8f, 0.8f, 0.18f);
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.4392157f, 0.8f, 0.8f, 0.27f);
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.4392157f, 0.80784315f, 0.85882354f, 0.66f);
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.13725491f, 0.1764706f, 0.20784314f, 0.73f);
    style.Colors[ImGuiCol_TitleBgActive]    = ImVec4(0.0f, 1.0f, 1.0f, 0.27f);
    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.0f, 0.0f, 0.0f, 0.54f);
    style.Colors[ImGuiCol_MenuBarBg]        = ImVec4(0.0f, 0.0f, 0.0f, 0.2f);
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.21960784f, 0.28627452f, 0.29803923f, 0.71f);
    style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.0f, 1.0f, 1.0f, 0.44f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.0f, 1.0f, 1.0f, 0.74f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_CheckMark]        = ImVec4(0.0f, 1.0f, 1.0f, 0.68f);
    style.Colors[ImGuiCol_SliderGrab]       = ImVec4(0.0f, 1.0f, 1.0f, 0.36f);
    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.0f, 1.0f, 1.0f, 0.76f);
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.0f, 0.64705884f, 0.64705884f, 0.46f);
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.007843138f, 1.0f, 1.0f, 0.43f);
    style.Colors[ImGuiCol_ButtonActive]  = ImVec4(0.0f, 1.0f, 1.0f, 0.62f);
    style.Colors[ImGuiCol_Header]        = ImVec4(0.0f, 1.0f, 1.0f, 0.33f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.0f, 1.0f, 1.0f, 0.42f);
    style.Colors[ImGuiCol_HeaderActive]  = ImVec4(0.0f, 1.0f, 1.0f, 0.54f);
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.0f, 0.49803922f, 0.49803922f, 0.33f);
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.0f, 0.49803922f, 0.49803922f, 0.47f);
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.0f, 0.69803923f, 0.69803923f, 1.0f);
    style.Colors[ImGuiCol_ResizeGrip]        = ImVec4(0.0f, 1.0f, 1.0f, 0.54f);
    style.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.0f, 1.0f, 1.0f, 0.74f);
    style.Colors[ImGuiCol_ResizeGripActive]  = ImVec4(0.0f, 1.0f, 1.0f, 1.0f);
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
    style.Colors[ImGuiCol_PlotLines]        = ImVec4(0.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_PlotLinesHovered] = ImVec4(0.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogram]    = ImVec4(0.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(0.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.1882353f, 0.1882353f, 0.2f, 1.0f);
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.30980393f, 0.30980393f, 0.34901962f, 1.0f);
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(0.22745098f, 0.22745098f, 0.24705882f, 1.0f);
    style.Colors[ImGuiCol_TableRowBg]     = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_TableRowBgAlt]  = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.0f, 1.0f, 1.0f, 0.22f);
    style.Colors[ImGuiCol_DragDropTarget] = ImVec4(1.0f, 1.0f, 0.0f, 0.9f);
    style.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    style.Colors[ImGuiCol_ModalWindowDimBg] =
        ImVec4(0.039215688f, 0.09803922f, 0.08627451f, 0.51f);
}

void VKContext::setDiscordDarkStyle()
{
    // Discord (Dark) style by BttrDrgn from ImThemes
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
    style.ScrollbarRounding                = 0.0f;
    style.GrabMinSize                      = 10.0f;
    style.GrabRounding                     = 0.0f;
    style.TabRounding                      = 0.0f;
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
        ImVec4(0.21176471f, 0.22352941f, 0.24705882f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] =
        ImVec4(0.18431373f, 0.19215687f, 0.21176471f, 1.0f);
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.078431375f, 0.078431375f, 0.078431375f, 0.94f);
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.42745098f, 0.42745098f, 0.49803922f, 0.5f);
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.30980393f, 0.32941177f, 0.36078432f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.30980393f, 0.32941177f, 0.36078432f, 1.0f);
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.34509805f, 0.39607844f, 0.9490196f, 1.0f);
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.18431373f, 0.19215687f, 0.21176471f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.1254902f, 0.13333334f, 0.14509805f, 1.0f);
    style.Colors[ImGuiCol_TitleBgCollapsed] =
        ImVec4(0.1254902f, 0.13333334f, 0.14509805f, 1.0f);
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.1254902f, 0.13333334f, 0.14509805f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.019607844f, 0.019607844f, 0.019607844f, 0.53f);
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.30980393f, 0.30980393f, 0.30980393f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.40784314f, 0.40784314f, 0.40784314f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.50980395f, 0.50980395f, 0.50980395f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.23137255f, 0.64705884f, 0.3647059f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab]       = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.30980393f, 0.32941177f, 0.36078432f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.40784314f, 0.42745098f, 0.4509804f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.1254902f, 0.13333334f, 0.14509805f, 1.0f);
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.30980393f, 0.32941177f, 0.36078432f, 1.0f);
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.40784314f, 0.42745098f, 0.4509804f, 1.0f);
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.40784314f, 0.42745098f, 0.4509804f, 1.0f);
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
        ImVec4(0.18431373f, 0.19215687f, 0.21176471f, 1.0f);
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.23529412f, 0.24705882f, 0.27058825f, 1.0f);
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.25882354f, 0.27450982f, 0.3019608f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.06666667f, 0.101960786f, 0.14509805f, 0.9724f);
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.13333334f, 0.25882354f, 0.42352942f, 1.0f);
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.60784316f, 0.60784316f, 0.60784316f, 1.0f);
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(0.34509805f, 0.39607844f, 0.9490196f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.34509805f, 0.39607844f, 0.9490196f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(0.36078432f, 0.4f, 0.42745098f, 1.0f);
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.1882353f, 0.1882353f, 0.2f, 1.0f);
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.30980393f, 0.30980393f, 0.34901962f, 1.0f);
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(0.22745098f, 0.22745098f, 0.24705882f, 1.0f);
    style.Colors[ImGuiCol_TableRowBg]    = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(0.050980393f, 0.41960785f, 0.85882354f, 1.0f);
    style.Colors[ImGuiCol_DragDropTarget] =
        ImVec4(0.34509805f, 0.39607844f, 0.9490196f, 1.0f);
    style.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    style.Colors[ImGuiCol_ModalWindowDimBg]  = ImVec4(0.8f, 0.8f, 0.8f, 0.35f);
}

void VKContext::setComfyStyle()
{
    // Comfy style by Giuseppe from ImThemes
    ImGuiStyle& style = ImGui::GetStyle();

    style.Alpha                            = 1.0f;
    style.DisabledAlpha                    = 0.1f;
    style.WindowPadding                    = ImVec2(8.0f, 8.0f);
    style.WindowRounding                   = 10.0f;
    style.WindowBorderSize                 = 0.0f;
    style.WindowMinSize                    = ImVec2(30.0f, 30.0f);
    style.WindowTitleAlign                 = ImVec2(0.5f, 0.5f);
    style.WindowMenuButtonPosition         = ImGuiDir_Right;
    style.ChildRounding                    = 5.0f;
    style.ChildBorderSize                  = 1.0f;
    style.PopupRounding                    = 10.0f;
    style.PopupBorderSize                  = 0.0f;
    style.FramePadding                     = ImVec2(5.0f, 3.5f);
    style.FrameRounding                    = 5.0f;
    style.FrameBorderSize                  = 0.0f;
    style.ItemSpacing                      = ImVec2(5.0f, 4.0f);
    style.ItemInnerSpacing                 = ImVec2(5.0f, 5.0f);
    style.CellPadding                      = ImVec2(4.0f, 2.0f);
    style.IndentSpacing                    = 5.0f;
    style.ColumnsMinSpacing                = 5.0f;
    style.ScrollbarSize                    = 15.0f;
    style.ScrollbarRounding                = 9.0f;
    style.GrabMinSize                      = 15.0f;
    style.GrabRounding                     = 5.0f;
    style.TabRounding                      = 5.0f;
    style.TabBorderSize                    = 0.0f;
    style.TabCloseButtonMinWidthSelected   = 0.0f;
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    style.Colors[ImGuiCol_Text]         = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(1.0f, 1.0f, 1.0f, 0.360515f);
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.09803922f, 0.09803922f, 0.09803922f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(1.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.09803922f, 0.09803922f, 0.09803922f, 1.0f);
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.42352942f, 0.38039216f, 0.57254905f, 0.5493562f);
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.15686275f, 0.15686275f, 0.15686275f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.38039216f, 0.42352942f, 0.57254905f, 0.54901963f);
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.61960787f, 0.5764706f, 0.76862746f, 0.54901963f);
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.09803922f, 0.09803922f, 0.09803922f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.09803922f, 0.09803922f, 0.09803922f, 1.0f);
    style.Colors[ImGuiCol_TitleBgCollapsed] =
        ImVec4(0.25882354f, 0.25882354f, 0.25882354f, 0.0f);
    style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.15686275f, 0.15686275f, 0.15686275f, 0.0f);
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.15686275f, 0.15686275f, 0.15686275f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.23529412f, 0.23529412f, 0.23529412f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.29411766f, 0.29411766f, 0.29411766f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.29411766f, 0.29411766f, 0.29411766f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.61960787f, 0.5764706f, 0.76862746f, 0.54901963f);
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.8156863f, 0.77254903f, 0.9647059f, 0.54901963f);
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.61960787f, 0.5764706f, 0.76862746f, 0.54901963f);
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.7372549f, 0.69411767f, 0.8862745f, 0.54901963f);
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.8156863f, 0.77254903f, 0.9647059f, 0.54901963f);
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.61960787f, 0.5764706f, 0.76862746f, 0.54901963f);
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.7372549f, 0.69411767f, 0.8862745f, 0.54901963f);
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.8156863f, 0.77254903f, 0.9647059f, 0.54901963f);
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.61960787f, 0.5764706f, 0.76862746f, 0.54901963f);
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.7372549f, 0.69411767f, 0.8862745f, 0.54901963f);
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.8156863f, 0.77254903f, 0.9647059f, 0.54901963f);
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.61960787f, 0.5764706f, 0.76862746f, 0.54901963f);
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.7372549f, 0.69411767f, 0.8862745f, 0.54901963f);
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.8156863f, 0.77254903f, 0.9647059f, 0.54901963f);
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.61960787f, 0.5764706f, 0.76862746f, 0.54901963f);
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.7372549f, 0.69411767f, 0.8862745f, 0.54901963f);
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.8156863f, 0.77254903f, 0.9647059f, 0.54901963f);
    style.Colors[ImGuiCol_TabUnfocused] = ImVec4(0.0f, 0.4509804f, 1.0f, 0.0f);
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.13333334f, 0.25882354f, 0.42352942f, 0.0f);
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.29411766f, 0.29411766f, 0.29411766f, 1.0f);
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(0.7372549f, 0.69411767f, 0.8862745f, 0.54901963f);
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.61960787f, 0.5764706f, 0.76862746f, 0.54901963f);
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(0.7372549f, 0.69411767f, 0.8862745f, 0.54901963f);
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.1882353f, 0.1882353f, 0.2f, 1.0f);
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.42352942f, 0.38039216f, 0.57254905f, 0.54901963f);
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(0.42352942f, 0.38039216f, 0.57254905f, 0.2918455f);
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_TableRowBgAlt] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.03433478f);
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(0.7372549f, 0.69411767f, 0.8862745f, 0.54901963f);
    style.Colors[ImGuiCol_DragDropTarget] = ImVec4(1.0f, 1.0f, 0.0f, 0.9f);
    style.Colors[ImGuiCol_NavHighlight]   = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    style.Colors[ImGuiCol_ModalWindowDimBg]  = ImVec4(0.8f, 0.8f, 0.8f, 0.35f);
}

void VKContext::setPurpleComfyStyle()
{
    // Purple Comfy style by RegularLunar from ImThemes
    ImGuiStyle& style = ImGui::GetStyle();

    style.Alpha                            = 1.0f;
    style.DisabledAlpha                    = 0.1f;
    style.WindowPadding                    = ImVec2(8.0f, 8.0f);
    style.WindowRounding                   = 10.0f;
    style.WindowBorderSize                 = 0.0f;
    style.WindowMinSize                    = ImVec2(30.0f, 30.0f);
    style.WindowTitleAlign                 = ImVec2(0.5f, 0.5f);
    style.WindowMenuButtonPosition         = ImGuiDir_Right;
    style.ChildRounding                    = 5.0f;
    style.ChildBorderSize                  = 1.0f;
    style.PopupRounding                    = 10.0f;
    style.PopupBorderSize                  = 0.0f;
    style.FramePadding                     = ImVec2(5.0f, 3.5f);
    style.FrameRounding                    = 5.0f;
    style.FrameBorderSize                  = 0.0f;
    style.ItemSpacing                      = ImVec2(5.0f, 4.0f);
    style.ItemInnerSpacing                 = ImVec2(5.0f, 5.0f);
    style.CellPadding                      = ImVec2(4.0f, 2.0f);
    style.IndentSpacing                    = 5.0f;
    style.ColumnsMinSpacing                = 5.0f;
    style.ScrollbarSize                    = 15.0f;
    style.ScrollbarRounding                = 9.0f;
    style.GrabMinSize                      = 15.0f;
    style.GrabRounding                     = 5.0f;
    style.TabRounding                      = 5.0f;
    style.TabBorderSize                    = 0.0f;
    style.TabCloseButtonMinWidthSelected   = 0.0f;
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    style.Colors[ImGuiCol_Text]         = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(1.0f, 1.0f, 1.0f, 0.360515f);
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.09803922f, 0.09803922f, 0.09803922f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(1.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.09803922f, 0.09803922f, 0.09803922f, 1.0f);
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.15686275f, 0.15686275f, 0.15686275f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.38039216f, 0.42352942f, 0.57254905f, 0.54901963f);
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.09803922f, 0.09803922f, 0.09803922f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.09803922f, 0.09803922f, 0.09803922f, 1.0f);
    style.Colors[ImGuiCol_TitleBgCollapsed] =
        ImVec4(0.25882354f, 0.25882354f, 0.25882354f, 0.0f);
    style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.15686275f, 0.15686275f, 0.15686275f, 0.0f);
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.15686275f, 0.15686275f, 0.15686275f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.23529412f, 0.23529412f, 0.23529412f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.29411766f, 0.29411766f, 0.29411766f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    style.Colors[ImGuiCol_TabUnfocused] = ImVec4(0.0f, 0.4509804f, 1.0f, 0.0f);
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.13333334f, 0.25882354f, 0.42352942f, 0.0f);
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.29411766f, 0.29411766f, 0.29411766f, 1.0f);
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(0.7372549f, 0.69411767f, 0.8862745f, 0.54901963f);
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.1882353f, 0.1882353f, 0.2f, 1.0f);
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.2901961f);
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_TableRowBgAlt] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.03433478f);
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    style.Colors[ImGuiCol_DragDropTarget] = ImVec4(1.0f, 1.0f, 0.0f, 0.9f);
    style.Colors[ImGuiCol_NavHighlight]   = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    style.Colors[ImGuiCol_ModalWindowDimBg]  = ImVec4(0.8f, 0.8f, 0.8f, 0.35f);
}

void VKContext::setFutureDarkStyle()
{
    // Future Dark style by rewrking from ImThemes
    ImGuiStyle& style = ImGui::GetStyle();

    style.Alpha                            = 1.0f;
    style.DisabledAlpha                    = 1.0f;
    style.WindowPadding                    = ImVec2(12.0f, 12.0f);
    style.WindowRounding                   = 0.0f;
    style.WindowBorderSize                 = 0.0f;
    style.WindowMinSize                    = ImVec2(20.0f, 20.0f);
    style.WindowTitleAlign                 = ImVec2(0.5f, 0.5f);
    style.WindowMenuButtonPosition         = ImGuiDir_None;
    style.ChildRounding                    = 0.0f;
    style.ChildBorderSize                  = 1.0f;
    style.PopupRounding                    = 0.0f;
    style.PopupBorderSize                  = 1.0f;
    style.FramePadding                     = ImVec2(6.0f, 6.0f);
    style.FrameRounding                    = 0.0f;
    style.FrameBorderSize                  = 0.0f;
    style.ItemSpacing                      = ImVec2(12.0f, 6.0f);
    style.ItemInnerSpacing                 = ImVec2(6.0f, 3.0f);
    style.CellPadding                      = ImVec2(12.0f, 6.0f);
    style.IndentSpacing                    = 20.0f;
    style.ColumnsMinSpacing                = 6.0f;
    style.ScrollbarSize                    = 12.0f;
    style.ScrollbarRounding                = 0.0f;
    style.GrabMinSize                      = 12.0f;
    style.GrabRounding                     = 0.0f;
    style.TabRounding                      = 0.0f;
    style.TabBorderSize                    = 0.0f;
    style.TabCloseButtonMinWidthSelected   = 0.0f;
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    style.Colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4(0.27450982f, 0.31764707f, 0.4509804f, 1.0f);
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.15686275f, 0.16862746f, 0.19215687f, 1.0f);
    style.Colors[ImGuiCol_BorderShadow] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.15686275f, 0.16862746f, 0.19215687f, 1.0f);
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.23529412f, 0.21568628f, 0.59607846f, 1.0f);
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    style.Colors[ImGuiCol_TitleBgCollapsed] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.09803922f, 0.105882354f, 0.12156863f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.15686275f, 0.16862746f, 0.19215687f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.49803922f, 0.5137255f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.49803922f, 0.5137255f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.5372549f, 0.5529412f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.19607843f, 0.1764706f, 0.54509807f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.23529412f, 0.21568628f, 0.59607846f, 1.0f);
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.19607843f, 0.1764706f, 0.54509807f, 1.0f);
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.23529412f, 0.21568628f, 0.59607846f, 1.0f);
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.15686275f, 0.18431373f, 0.2509804f, 1.0f);
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.15686275f, 0.18431373f, 0.2509804f, 1.0f);
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.15686275f, 0.18431373f, 0.2509804f, 1.0f);
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.19607843f, 0.1764706f, 0.54509807f, 1.0f);
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.23529412f, 0.21568628f, 0.59607846f, 1.0f);
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.09803922f, 0.105882354f, 0.12156863f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.52156866f, 0.6f, 0.7019608f, 1.0f);
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(0.039215688f, 0.98039216f, 0.98039216f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(1.0f, 0.2901961f, 0.59607846f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(0.99607843f, 0.4745098f, 0.69803923f, 1.0f);
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    style.Colors[ImGuiCol_TableBorderLight] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_TableRowBg] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_TableRowBgAlt] =
        ImVec4(0.09803922f, 0.105882354f, 0.12156863f, 1.0f);
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(0.23529412f, 0.21568628f, 0.59607846f, 1.0f);
    style.Colors[ImGuiCol_DragDropTarget] =
        ImVec4(0.49803922f, 0.5137255f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.49803922f, 0.5137255f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(0.49803922f, 0.5137255f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_NavWindowingDimBg] =
        ImVec4(0.19607843f, 0.1764706f, 0.54509807f, 0.5019608f);
    style.Colors[ImGuiCol_ModalWindowDimBg] =
        ImVec4(0.19607843f, 0.1764706f, 0.54509807f, 0.5019608f);
}

void VKContext::setCleanDarkStyle()
{
    // Clean Dark/Red style by ImBritish from ImThemes
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
    style.FrameBorderSize                  = 1.0f;
    style.ItemSpacing                      = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing                 = ImVec2(4.0f, 4.0f);
    style.CellPadding                      = ImVec2(4.0f, 2.0f);
    style.IndentSpacing                    = 21.0f;
    style.ColumnsMinSpacing                = 6.0f;
    style.ScrollbarSize                    = 14.0f;
    style.ScrollbarRounding                = 9.0f;
    style.GrabMinSize                      = 10.0f;
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
        ImVec4(0.7294118f, 0.7490196f, 0.7372549f, 1.0f);
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.08627451f, 0.08627451f, 0.08627451f, 0.94f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.078431375f, 0.078431375f, 0.078431375f, 0.94f);
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.13725491f, 0.13725491f, 0.13725491f, 1.0f);
    style.Colors[ImGuiCol_BorderShadow] =
        ImVec4(0.13725491f, 0.13725491f, 0.13725491f, 1.0f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.54f);
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.1764706f, 0.1764706f, 0.1764706f, 0.4f);
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.21568628f, 0.21568628f, 0.21568628f, 0.67f);
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.13725491f, 0.13725491f, 0.13725491f, 0.65236056f);
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.13725491f, 0.13725491f, 0.13725491f, 1.0f);
    style.Colors[ImGuiCol_TitleBgCollapsed] =
        ImVec4(0.13725491f, 0.13725491f, 0.13725491f, 0.67f);
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.21568628f, 0.21568628f, 0.21568628f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.019607844f, 0.019607844f, 0.019607844f, 0.53f);
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.30980393f, 0.30980393f, 0.30980393f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.40784314f, 0.40784314f, 0.40784314f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.50980395f, 0.50980395f, 0.50980395f, 1.0f);
    style.Colors[ImGuiCol_CheckMark]  = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(1.0f, 0.38039216f, 0.38039216f, 1.0f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.0f, 0.0f, 0.0f, 0.5411765f);
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.1764706f, 0.1764706f, 0.1764706f, 0.4f);
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.21568628f, 0.21568628f, 0.21568628f, 0.67058825f);
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.21568628f, 0.21568628f, 0.21568628f, 1.0f);
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.27058825f, 0.27058825f, 0.27058825f, 1.0f);
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.3529412f, 0.3529412f, 0.3529412f, 1.0f);
    style.Colors[ImGuiCol_Separator]        = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_SeparatorHovered] = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(1.0f, 0.32941177f, 0.32941177f, 1.0f);
    style.Colors[ImGuiCol_ResizeGrip] = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(1.0f, 0.4862745f, 0.4862745f, 1.0f);
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(1.0f, 0.4862745f, 0.4862745f, 1.0f);
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.21960784f, 0.21960784f, 0.21960784f, 1.0f);
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.2901961f, 0.2901961f, 0.2901961f, 1.0f);
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.1764706f, 0.1764706f, 0.1764706f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.14901961f, 0.06666667f, 0.06666667f, 0.97f);
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.40392157f, 0.15294118f, 0.15294118f, 1.0f);
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.60784316f, 0.60784316f, 0.60784316f, 1.0f);
    style.Colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogram] = ImVec4(0.8980392f, 0.0f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(0.3647059f, 0.0f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.3019608f, 0.3019608f, 0.3019608f, 1.0f);
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.13725491f, 0.13725491f, 0.13725491f, 1.0f);
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(0.13725491f, 0.13725491f, 0.13725491f, 1.0f);
    style.Colors[ImGuiCol_TableRowBg]    = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(0.2627451f, 0.63529414f, 0.8784314f, 0.43776822f);
    style.Colors[ImGuiCol_DragDropTarget] =
        ImVec4(0.46666667f, 0.18431373f, 0.18431373f, 0.9656652f);
    style.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.40784314f, 0.40784314f, 0.40784314f, 1.0f);
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    style.Colors[ImGuiCol_ModalWindowDimBg]  = ImVec4(0.8f, 0.8f, 0.8f, 0.35f);
}

void VKContext::setMoonlightStyle()
{
    // Moonlight style by Madam-Herta from ImThemes
    ImGuiStyle& style = ImGui::GetStyle();

    style.Alpha                            = 1.0f;
    style.DisabledAlpha                    = 1.0f;
    style.WindowPadding                    = ImVec2(12.0f, 12.0f);
    style.WindowRounding                   = 11.5f;
    style.WindowBorderSize                 = 0.0f;
    style.WindowMinSize                    = ImVec2(20.0f, 20.0f);
    style.WindowTitleAlign                 = ImVec2(0.5f, 0.5f);
    style.WindowMenuButtonPosition         = ImGuiDir_Right;
    style.ChildRounding                    = 0.0f;
    style.ChildBorderSize                  = 1.0f;
    style.PopupRounding                    = 0.0f;
    style.PopupBorderSize                  = 1.0f;
    style.FramePadding                     = ImVec2(20.0f, 3.4f);
    style.FrameRounding                    = 11.9f;
    style.FrameBorderSize                  = 0.0f;
    style.ItemSpacing                      = ImVec2(4.3f, 5.5f);
    style.ItemInnerSpacing                 = ImVec2(7.1f, 1.8f);
    style.CellPadding                      = ImVec2(12.1f, 9.2f);
    style.IndentSpacing                    = 0.0f;
    style.ColumnsMinSpacing                = 4.9f;
    style.ScrollbarSize                    = 11.6f;
    style.ScrollbarRounding                = 15.9f;
    style.GrabMinSize                      = 3.7f;
    style.GrabRounding                     = 20.0f;
    style.TabRounding                      = 0.0f;
    style.TabBorderSize                    = 0.0f;
    style.TabCloseButtonMinWidthSelected   = 0.0f;
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    style.Colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4(0.27450982f, 0.31764707f, 0.4509804f, 1.0f);
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] =
        ImVec4(0.09411765f, 0.101960786f, 0.11764706f, 1.0f);
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.15686275f, 0.16862746f, 0.19215687f, 1.0f);
    style.Colors[ImGuiCol_BorderShadow] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.11372549f, 0.1254902f, 0.15294118f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.15686275f, 0.16862746f, 0.19215687f, 1.0f);
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.15686275f, 0.16862746f, 0.19215687f, 1.0f);
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    style.Colors[ImGuiCol_TitleBgCollapsed] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.09803922f, 0.105882354f, 0.12156863f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.15686275f, 0.16862746f, 0.19215687f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.972549f, 1.0f, 0.49803922f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.972549f, 1.0f, 0.49803922f, 1.0f);
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(1.0f, 0.79607844f, 0.49803922f, 1.0f);
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.18039216f, 0.1882353f, 0.19607843f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.15294118f, 0.15294118f, 0.15294118f, 1.0f);
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.14117648f, 0.16470589f, 0.20784314f, 1.0f);
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.105882354f, 0.105882354f, 0.105882354f, 1.0f);
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.12941177f, 0.14901961f, 0.19215687f, 1.0f);
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.15686275f, 0.18431373f, 0.2509804f, 1.0f);
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.15686275f, 0.18431373f, 0.2509804f, 1.0f);
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.14509805f, 0.14509805f, 0.14509805f, 1.0f);
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.972549f, 1.0f, 0.49803922f, 1.0f);
    style.Colors[ImGuiCol_ResizeGripActive] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.1254902f, 0.27450982f, 0.57254905f, 1.0f);
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.52156866f, 0.6f, 0.7019608f, 1.0f);
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(0.039215688f, 0.98039216f, 0.98039216f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.88235295f, 0.79607844f, 0.56078434f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(0.95686275f, 0.95686275f, 0.95686275f, 1.0f);
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    style.Colors[ImGuiCol_TableBorderLight] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_TableRowBg] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_TableRowBgAlt] =
        ImVec4(0.09803922f, 0.105882354f, 0.12156863f, 1.0f);
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(0.9372549f, 0.9372549f, 0.9372549f, 1.0f);
    style.Colors[ImGuiCol_DragDropTarget] =
        ImVec4(0.49803922f, 0.5137255f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.26666668f, 0.2901961f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(0.49803922f, 0.5137255f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_NavWindowingDimBg] =
        ImVec4(0.19607843f, 0.1764706f, 0.54509807f, 0.5019608f);
    style.Colors[ImGuiCol_ModalWindowDimBg] =
        ImVec4(0.19607843f, 0.1764706f, 0.54509807f, 0.5019608f);
}

/**
 * @brief 设置 Cecilia 塞西莉娅配色派生样式。
 */
void VKContext::setCeciliaStyle()
{
    setMoonlightStyle();

    ImGuiStyle& style = ImGui::GetStyle();

    auto skinColor = [](const std::string& key, ImVec4 fallback) {
        const auto& colors = Config::SkinManager::instance().getData().colors;
        if ( auto it = colors.find(key); it != colors.end() ) {
            return ImVec4(it->second.r, it->second.g, it->second.b, fallback.w);
        }
        return fallback;
    };

    const ImVec4 nodeColor =
        skinColor("note_node", ImVec4(0.9922f, 0.9255f, 0.5608f, 1.0f));
    auto rgb = [](int r, int g, int b) {
        return ImVec4(static_cast<float>(r) / 255.0f,
                      static_cast<float>(g) / 255.0f,
                      static_cast<float>(b) / 255.0f,
                      1.0f);
    };

    const ImVec4 titleTextColor     = rgb(0x36, 0x26, 0x28);
    const ImVec4 detailTextColor    = rgb(0x5B, 0x48, 0x43);
    const ImVec4 baseBgColor        = rgb(0xE8, 0xDF, 0xD6);
    const ImVec4 viewBgColor        = rgb(0xEE, 0xE7, 0xDF);
    const ImVec4 progressBgColor    = rgb(0xF7, 0xF1, 0xEA);
    const ImVec4 popupBgColor       = rgb(0xD2, 0xC4, 0xB8);
    const ImVec4 borderColor        = rgb(0xCA, 0xB7, 0xA4);
    const ImVec4 typeTextColor      = rgb(0x87, 0x9F, 0x4B);
    const ImVec4 itemGroupColor     = rgb(0xC7, 0x61, 0x62);
    const ImVec4 progressColor      = rgb(0xAF, 0x5F, 0x39);
    const ImVec4 fullProgressColor  = rgb(0xF7, 0x86, 0x80);
    const ImVec4 hairColor          = rgb(0xB9, 0xC0, 0xAE);
    const ImVec4 deepHairColor      = rgb(0x8B, 0x98, 0x87);
    const ImVec4 titleBgColor       = rgb(0xA4, 0xAD, 0x9F);
    const ImVec4 titleBgActiveColor = rgb(0x95, 0xA2, 0x91);
    const ImVec4 windowBorderColor  = rgb(0xAC, 0x9B, 0x8C);
    const ImVec4 shadowWarmColor    = rgb(0xB9, 0xAA, 0x9C);
    const ImVec4 panelHoverColor    = rgb(0xE1, 0xD7, 0xCD);
    const ImVec4 cloakColor         = rgb(0x13, 0x1B, 0x29);

    style.Colors[ImGuiCol_Text]         = detailTextColor;
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(
        detailTextColor.x, detailTextColor.y, detailTextColor.z, 0.6200f);
    style.Colors[ImGuiCol_WindowBg] = baseBgColor;
    style.Colors[ImGuiCol_ChildBg]  = viewBgColor;
    style.Colors[ImGuiCol_PopupBg]  = popupBgColor;
    style.Colors[ImGuiCol_Border]   = windowBorderColor;
    style.Colors[ImGuiCol_BorderShadow] =
        ImVec4(cloakColor.x, cloakColor.y, cloakColor.z, 0.1600f);
    style.Colors[ImGuiCol_FrameBg]              = progressBgColor;
    style.Colors[ImGuiCol_FrameBgHovered]       = panelHoverColor;
    style.Colors[ImGuiCol_FrameBgActive]        = borderColor;
    style.Colors[ImGuiCol_TitleBg]              = titleBgColor;
    style.Colors[ImGuiCol_TitleBgActive]        = titleBgActiveColor;
    style.Colors[ImGuiCol_TitleBgCollapsed]     = hairColor;
    style.Colors[ImGuiCol_MenuBarBg]            = deepHairColor;
    style.Colors[ImGuiCol_ScrollbarBg]          = viewBgColor;
    style.Colors[ImGuiCol_ScrollbarGrab]        = borderColor;
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = shadowWarmColor;
    style.Colors[ImGuiCol_ScrollbarGrabActive]  = progressColor;
    style.Colors[ImGuiCol_CheckMark]            = typeTextColor;
    style.Colors[ImGuiCol_SliderGrab]           = progressColor;
    style.Colors[ImGuiCol_SliderGrabActive]     = fullProgressColor;
    style.Colors[ImGuiCol_Button]               = viewBgColor;
    style.Colors[ImGuiCol_ButtonHovered]        = panelHoverColor;
    style.Colors[ImGuiCol_ButtonActive]         = borderColor;
    style.Colors[ImGuiCol_Header]               = borderColor;
    style.Colors[ImGuiCol_HeaderHovered]        = hairColor;
    style.Colors[ImGuiCol_HeaderActive]         = deepHairColor;
    style.Colors[ImGuiCol_Separator]            = borderColor;
    style.Colors[ImGuiCol_SeparatorHovered]     = hairColor;
    style.Colors[ImGuiCol_SeparatorActive]      = deepHairColor;
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(borderColor.x, borderColor.y, borderColor.z, 0.5000f);
    style.Colors[ImGuiCol_ResizeGripHovered]   = hairColor;
    style.Colors[ImGuiCol_ResizeGripActive]    = deepHairColor;
    style.Colors[ImGuiCol_Tab]                 = panelHoverColor;
    style.Colors[ImGuiCol_TabHovered]          = hairColor;
    style.Colors[ImGuiCol_TabActive]           = progressBgColor;
    style.Colors[ImGuiCol_TabSelectedOverline] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_TabUnfocused]        = baseBgColor;
    style.Colors[ImGuiCol_TabUnfocusedActive]  = panelHoverColor;
    style.Colors[ImGuiCol_TabDimmedSelectedOverline] =
        ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_PlotLines]            = borderColor;
    style.Colors[ImGuiCol_PlotLinesHovered]     = typeTextColor;
    style.Colors[ImGuiCol_PlotHistogram]        = titleTextColor;
    style.Colors[ImGuiCol_PlotHistogramHovered] = nodeColor;
    style.Colors[ImGuiCol_TableHeaderBg]        = panelHoverColor;
    style.Colors[ImGuiCol_TableBorderStrong]    = borderColor;
    style.Colors[ImGuiCol_TableBorderLight]     = panelHoverColor;
    style.Colors[ImGuiCol_TableRowBg]           = viewBgColor;
    style.Colors[ImGuiCol_TableRowBgAlt]        = panelHoverColor;
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(itemGroupColor.x, itemGroupColor.y, itemGroupColor.z, 0.3000f);
    style.Colors[ImGuiCol_DragDropTarget]        = fullProgressColor;
    style.Colors[ImGuiCol_NavHighlight]          = titleTextColor;
    style.Colors[ImGuiCol_NavWindowingHighlight] = titleTextColor;
    style.Colors[ImGuiCol_NavWindowingDimBg] =
        ImVec4(cloakColor.x, cloakColor.y, cloakColor.z, 0.2500f);
    style.Colors[ImGuiCol_ModalWindowDimBg] =
        ImVec4(cloakColor.x, cloakColor.y, cloakColor.z, 0.3000f);
    style.Colors[ImGuiCol_DockingPreview] =
        ImVec4(typeTextColor.x, typeTextColor.y, typeTextColor.z, 0.3200f);
    style.Colors[ImGuiCol_DockingEmptyBg] = baseBgColor;
    style.Colors[ImGuiCol_TextLink]       = titleTextColor;

    style.WindowBorderSize = 1.0f;
}

void VKContext::setComfortableLightStyle()
{
    // Comfortable Light Orange style by SouthCraftX from ImThemes
    ImGuiStyle& style = ImGui::GetStyle();

    style.Alpha                            = 1.0f;
    style.DisabledAlpha                    = 1.0f;
    style.WindowPadding                    = ImVec2(20.0f, 20.0f);
    style.WindowRounding                   = 11.5f;
    style.WindowBorderSize                 = 0.0f;
    style.WindowMinSize                    = ImVec2(20.0f, 20.0f);
    style.WindowTitleAlign                 = ImVec2(0.5f, 0.5f);
    style.WindowMenuButtonPosition         = ImGuiDir_None;
    style.ChildRounding                    = 20.0f;
    style.ChildBorderSize                  = 1.0f;
    style.PopupRounding                    = 17.4f;
    style.PopupBorderSize                  = 1.0f;
    style.FramePadding                     = ImVec2(20.0f, 3.4f);
    style.FrameRounding                    = 11.9f;
    style.FrameBorderSize                  = 0.0f;
    style.ItemSpacing                      = ImVec2(8.9f, 13.4f);
    style.ItemInnerSpacing                 = ImVec2(7.1f, 1.8f);
    style.CellPadding                      = ImVec2(12.1f, 9.2f);
    style.IndentSpacing                    = 0.0f;
    style.ColumnsMinSpacing                = 8.7f;
    style.ScrollbarSize                    = 11.6f;
    style.ScrollbarRounding                = 15.9f;
    style.GrabMinSize                      = 3.7f;
    style.GrabRounding                     = 20.0f;
    style.TabRounding                      = 9.8f;
    style.TabBorderSize                    = 0.0f;
    style.TabCloseButtonMinWidthSelected   = 0.0f;
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    style.Colors[ImGuiCol_Text] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4(0.7254902f, 0.68235296f, 0.54901963f, 1.0f);
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.92156863f, 0.9137255f, 0.8980392f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] =
        ImVec4(0.90588236f, 0.8980392f, 0.88235295f, 1.0f);
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.92156863f, 0.9137255f, 0.8980392f, 1.0f);
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.84313726f, 0.83137256f, 0.80784315f, 1.0f);
    style.Colors[ImGuiCol_BorderShadow] =
        ImVec4(0.92156863f, 0.9137255f, 0.8980392f, 1.0f);
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.8862745f, 0.8745098f, 0.84705883f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.84313726f, 0.83137256f, 0.80784315f, 1.0f);
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.84313726f, 0.83137256f, 0.80784315f, 1.0f);
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.9529412f, 0.94509804f, 0.92941177f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.9529412f, 0.94509804f, 0.92941177f, 1.0f);
    style.Colors[ImGuiCol_TitleBgCollapsed] =
        ImVec4(0.92156863f, 0.9137255f, 0.8980392f, 1.0f);
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.9019608f, 0.89411765f, 0.8784314f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.9529412f, 0.94509804f, 0.92941177f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.88235295f, 0.8666667f, 0.8509804f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.84313726f, 0.83137256f, 0.80784315f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.88235295f, 0.8666667f, 0.8509804f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.96862745f, 0.050980393f, 0.15686275f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.9647059f, 0.8f, 0.02745098f, 1.0f);
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.96862745f, 0.5882353f, 0.03529412f, 1.0f);
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.88235295f, 0.8666667f, 0.8509804f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.81960785f, 0.8117647f, 0.8039216f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.84705883f, 0.84705883f, 0.84705883f, 1.0f);
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.85882354f, 0.8352941f, 0.7921569f, 1.0f);
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.89411765f, 0.89411765f, 0.89411765f, 1.0f);
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.92156863f, 0.9137255f, 0.8980392f, 1.0f);
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.87058824f, 0.8509804f, 0.80784315f, 1.0f);
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.84313726f, 0.8156863f, 0.7490196f, 1.0f);
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.84313726f, 0.8156863f, 0.7490196f, 1.0f);
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.85490197f, 0.85490197f, 0.85490197f, 1.0f);
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.96862745f, 0.050980393f, 0.15686275f, 1.0f);
    style.Colors[ImGuiCol_ResizeGripActive] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.92156863f, 0.9137255f, 0.8980392f, 1.0f);
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.88235295f, 0.8666667f, 0.8509804f, 1.0f);
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.88235295f, 0.8666667f, 0.8509804f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.92156863f, 0.9137255f, 0.8980392f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.8745098f, 0.7254902f, 0.42745098f, 1.0f);
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.47843137f, 0.4f, 0.29803923f, 1.0f);
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(0.9607843f, 0.019607844f, 0.11764706f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.90588236f, 0.6627451f, 0.30980393f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(0.6392157f, 0.39607844f, 0.043137256f, 1.0f);
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.9529412f, 0.94509804f, 0.92941177f, 1.0f);
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.9529412f, 0.94509804f, 0.92941177f, 1.0f);
    style.Colors[ImGuiCol_TableBorderLight] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_TableRowBg] =
        ImVec4(0.88235295f, 0.8666667f, 0.8509804f, 1.0f);
    style.Colors[ImGuiCol_TableRowBgAlt] =
        ImVec4(0.9019608f, 0.89411765f, 0.8784314f, 1.0f);
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(0.0627451f, 0.0627451f, 0.0627451f, 1.0f);
    style.Colors[ImGuiCol_DragDropTarget] =
        ImVec4(0.5019608f, 0.4862745f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.73333335f, 0.70980394f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(0.5019608f, 0.4862745f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_NavWindowingDimBg] =
        ImVec4(0.8039216f, 0.8235294f, 0.45490196f, 0.502f);
    style.Colors[ImGuiCol_ModalWindowDimBg] =
        ImVec4(0.8039216f, 0.8235294f, 0.45490196f, 0.502f);
}

void VKContext::setHazyDarkStyle()
{
    // Hazy Dark style by kaitabuchi314 from ImThemes
    ImGuiStyle& style = ImGui::GetStyle();

    style.Alpha                            = 1.0f;
    style.DisabledAlpha                    = 0.6f;
    style.WindowPadding                    = ImVec2(5.5f, 8.3f);
    style.WindowRounding                   = 4.5f;
    style.WindowBorderSize                 = 1.0f;
    style.WindowMinSize                    = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign                 = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition         = ImGuiDir_Left;
    style.ChildRounding                    = 3.2f;
    style.ChildBorderSize                  = 1.0f;
    style.PopupRounding                    = 2.7f;
    style.PopupBorderSize                  = 1.0f;
    style.FramePadding                     = ImVec2(4.0f, 3.0f);
    style.FrameRounding                    = 2.4f;
    style.FrameBorderSize                  = 0.0f;
    style.ItemSpacing                      = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing                 = ImVec2(4.0f, 4.0f);
    style.CellPadding                      = ImVec2(4.0f, 2.0f);
    style.IndentSpacing                    = 21.0f;
    style.ColumnsMinSpacing                = 6.0f;
    style.ScrollbarSize                    = 14.0f;
    style.ScrollbarRounding                = 9.0f;
    style.GrabMinSize                      = 10.0f;
    style.GrabRounding                     = 3.2f;
    style.TabRounding                      = 3.5f;
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
        ImVec4(0.05882353f, 0.05882353f, 0.05882353f, 0.94f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.078431375f, 0.078431375f, 0.078431375f, 0.94f);
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.42745098f, 0.42745098f, 0.49803922f, 0.5f);
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.13725491f, 0.17254902f, 0.22745098f, 0.54f);
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.21176471f, 0.25490198f, 0.3019608f, 0.4f);
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.043137256f, 0.047058824f, 0.047058824f, 0.67f);
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.039215688f, 0.039215688f, 0.039215688f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.078431375f, 0.08235294f, 0.09019608f, 1.0f);
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
        ImVec4(0.7176471f, 0.78431374f, 0.84313726f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.47843137f, 0.5254902f, 0.57254905f, 1.0f);
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.2901961f, 0.31764707f, 0.3529412f, 1.0f);
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.14901961f, 0.16078432f, 0.1764706f, 0.4f);
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.13725491f, 0.14509805f, 0.15686275f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.078431375f, 0.08627451f, 0.09019608f, 1.0f);
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.19607843f, 0.21568628f, 0.23921569f, 0.31f);
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.16470589f, 0.1764706f, 0.19215687f, 0.8f);
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.07450981f, 0.08235294f, 0.09019608f, 1.0f);
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.42745098f, 0.42745098f, 0.49803922f, 0.5f);
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.23921569f, 0.3254902f, 0.42352942f, 0.78f);
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.27450982f, 0.38039216f, 0.49803922f, 1.0f);
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.2901961f, 0.32941177f, 0.3764706f, 0.2f);
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.23921569f, 0.29803923f, 0.36862746f, 0.67f);
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.16470589f, 0.1764706f, 0.1882353f, 0.95f);
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.11764706f, 0.1254902f, 0.13333334f, 0.862f);
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.32941177f, 0.40784314f, 0.5019608f, 0.8f);
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.24313726f, 0.24705882f, 0.25490198f, 1.0f);
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

void VKContext::setEverforestStyle()
{
    // Everforest style by DestroyerDarkNess from ImThemes
    ImGuiStyle& style = ImGui::GetStyle();

    style.Alpha                            = 1.0f;
    style.DisabledAlpha                    = 0.6f;
    style.WindowPadding                    = ImVec2(6.0f, 3.0f);
    style.WindowRounding                   = 6.0f;
    style.WindowBorderSize                 = 1.0f;
    style.WindowMinSize                    = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign                 = ImVec2(0.5f, 0.5f);
    style.WindowMenuButtonPosition         = ImGuiDir_Left;
    style.ChildRounding                    = 0.0f;
    style.ChildBorderSize                  = 1.0f;
    style.PopupRounding                    = 0.0f;
    style.PopupBorderSize                  = 1.0f;
    style.FramePadding                     = ImVec2(5.0f, 1.0f);
    style.FrameRounding                    = 3.0f;
    style.FrameBorderSize                  = 1.0f;
    style.ItemSpacing                      = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing                 = ImVec2(4.0f, 4.0f);
    style.CellPadding                      = ImVec2(4.0f, 2.0f);
    style.IndentSpacing                    = 21.0f;
    style.ColumnsMinSpacing                = 6.0f;
    style.ScrollbarSize                    = 13.0f;
    style.ScrollbarRounding                = 16.0f;
    style.GrabMinSize                      = 20.0f;
    style.GrabRounding                     = 2.0f;
    style.TabRounding                      = 4.0f;
    style.TabBorderSize                    = 1.0f;
    style.TabCloseButtonMinWidthSelected   = 0.0f;
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    style.Colors[ImGuiCol_Text] =
        ImVec4(0.8745098f, 0.87058824f, 0.8392157f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4(0.58431375f, 0.57254905f, 0.52156866f, 1.0f);
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.23529412f, 0.21960784f, 0.21176471f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] =
        ImVec4(0.23529412f, 0.21960784f, 0.21176471f, 1.0f);
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.23529412f, 0.21960784f, 0.21176471f, 1.0f);
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.3137255f, 0.28627452f, 0.27058825f, 1.0f);
    style.Colors[ImGuiCol_BorderShadow] =
        ImVec4(0.23529412f, 0.21960784f, 0.21176471f, 0.0f);
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.3137255f, 0.28627452f, 0.27058825f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.4f, 0.36078432f, 0.32941177f, 1.0f);
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.4862745f, 0.43529412f, 0.39215687f, 1.0f);
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.23529412f, 0.21960784f, 0.21176471f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.3137255f, 0.28627452f, 0.27058825f, 1.0f);
    style.Colors[ImGuiCol_TitleBgCollapsed] =
        ImVec4(0.23529412f, 0.21960784f, 0.21176471f, 1.0f);
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.3137255f, 0.28627452f, 0.27058825f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.23529412f, 0.21960784f, 0.21176471f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.4862745f, 0.43529412f, 0.39215687f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.4f, 0.36078432f, 0.32941177f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.3137255f, 0.28627452f, 0.27058825f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.59607846f, 0.5921569f, 0.101960786f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.59607846f, 0.5921569f, 0.101960786f, 1.0f);
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.7411765f, 0.7176471f, 0.41960785f, 1.0f);
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.4f, 0.36078432f, 0.32941177f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.4862745f, 0.43529412f, 0.39215687f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.7411765f, 0.7176471f, 0.41960785f, 1.0f);
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.4f, 0.36078432f, 0.32941177f, 1.0f);
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.4862745f, 0.43529412f, 0.39215687f, 1.0f);
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.7411765f, 0.7176471f, 0.41960785f, 1.0f);
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.7411765f, 0.7176471f, 0.41960785f, 1.0f);
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.4862745f, 0.43529412f, 0.39215687f, 1.0f);
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.7411765f, 0.7176471f, 0.41960785f, 1.0f);
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.4f, 0.36078432f, 0.32941177f, 1.0f);
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.4862745f, 0.43529412f, 0.39215687f, 1.0f);
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.7411765f, 0.7176471f, 0.41960785f, 1.0f);
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.3137255f, 0.28627452f, 0.27058825f, 1.0f);
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.4f, 0.36078432f, 0.32941177f, 1.0f);
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.4862745f, 0.43529412f, 0.39215687f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.23529412f, 0.21960784f, 0.21176471f, 0.972549f);
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.3137255f, 0.28627452f, 0.27058825f, 1.0f);
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.7411765f, 0.7176471f, 0.41960785f, 1.0f);
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(0.8392157f, 0.7490196f, 0.4f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.7411765f, 0.7176471f, 0.41960785f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(0.8392157f, 0.7490196f, 0.4f, 1.0f);
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.8392157f, 0.7490196f, 0.4f, 0.60944206f);
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.30980393f, 0.30980393f, 0.34901962f, 1.0f);
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(0.22745098f, 0.22745098f, 0.24705882f, 1.0f);
    style.Colors[ImGuiCol_TableRowBg]    = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(0.8392157f, 0.7490196f, 0.4f, 0.43137255f);
    style.Colors[ImGuiCol_DragDropTarget] =
        ImVec4(0.8392157f, 0.7490196f, 0.4f, 0.9019608f);
    style.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.23529412f, 0.21960784f, 0.21176471f, 1.0f);
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    style.Colors[ImGuiCol_ModalWindowDimBg]  = ImVec4(0.8f, 0.8f, 0.8f, 0.35f);
}

void VKContext::setWindarkStyle()
{
    // Windark style by DestroyerDarkNess from ImThemes
    ImGuiStyle& style = ImGui::GetStyle();

    style.Alpha                            = 1.0f;
    style.DisabledAlpha                    = 0.6f;
    style.WindowPadding                    = ImVec2(8.0f, 8.0f);
    style.WindowRounding                   = 8.4f;
    style.WindowBorderSize                 = 1.0f;
    style.WindowMinSize                    = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign                 = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition         = ImGuiDir_Right;
    style.ChildRounding                    = 3.0f;
    style.ChildBorderSize                  = 1.0f;
    style.PopupRounding                    = 3.0f;
    style.PopupBorderSize                  = 1.0f;
    style.FramePadding                     = ImVec2(4.0f, 3.0f);
    style.FrameRounding                    = 3.0f;
    style.FrameBorderSize                  = 1.0f;
    style.ItemSpacing                      = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing                 = ImVec2(4.0f, 4.0f);
    style.CellPadding                      = ImVec2(4.0f, 2.0f);
    style.IndentSpacing                    = 21.0f;
    style.ColumnsMinSpacing                = 6.0f;
    style.ScrollbarSize                    = 5.6f;
    style.ScrollbarRounding                = 18.0f;
    style.GrabMinSize                      = 10.0f;
    style.GrabRounding                     = 3.0f;
    style.TabRounding                      = 3.0f;
    style.TabBorderSize                    = 0.0f;
    style.TabCloseButtonMinWidthSelected   = 0.0f;
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    style.Colors[ImGuiCol_Text]         = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.1254902f, 0.1254902f, 0.1254902f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] =
        ImVec4(0.1254902f, 0.1254902f, 0.1254902f, 1.0f);
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.16862746f, 0.16862746f, 0.16862746f, 1.0f);
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.2509804f, 0.2509804f, 0.2509804f, 1.0f);
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.16862746f, 0.16862746f, 0.16862746f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.21568628f, 0.21568628f, 0.21568628f, 1.0f);
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.2509804f, 0.2509804f, 0.2509804f, 1.0f);
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.1254902f, 0.1254902f, 0.1254902f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.16862746f, 0.16862746f, 0.16862746f, 1.0f);
    style.Colors[ImGuiCol_TitleBgCollapsed] =
        ImVec4(0.1254902f, 0.1254902f, 0.1254902f, 1.0f);
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.16862746f, 0.16862746f, 0.16862746f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.1254902f, 0.1254902f, 0.1254902f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.2509804f, 0.2509804f, 0.2509804f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.3019608f, 0.3019608f, 0.3019608f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.34901962f, 0.34901962f, 0.34901962f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.0f, 0.47058824f, 0.84313726f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.0f, 0.47058824f, 0.84313726f, 1.0f);
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.0f, 0.32941177f, 0.6f, 1.0f);
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.16862746f, 0.16862746f, 0.16862746f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.21568628f, 0.21568628f, 0.21568628f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.2509804f, 0.2509804f, 0.2509804f, 1.0f);
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.21568628f, 0.21568628f, 0.21568628f, 1.0f);
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.2509804f, 0.2509804f, 0.2509804f, 1.0f);
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.3019608f, 0.3019608f, 0.3019608f, 1.0f);
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.21568628f, 0.21568628f, 0.21568628f, 1.0f);
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.2509804f, 0.2509804f, 0.2509804f, 1.0f);
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.3019608f, 0.3019608f, 0.3019608f, 1.0f);
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.21568628f, 0.21568628f, 0.21568628f, 1.0f);
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.2509804f, 0.2509804f, 0.2509804f, 1.0f);
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.3019608f, 0.3019608f, 0.3019608f, 1.0f);
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.16862746f, 0.16862746f, 0.16862746f, 1.0f);
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.21568628f, 0.21568628f, 0.21568628f, 1.0f);
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.2509804f, 0.2509804f, 0.2509804f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.16862746f, 0.16862746f, 0.16862746f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.21568628f, 0.21568628f, 0.21568628f, 1.0f);
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.0f, 0.47058824f, 0.84313726f, 1.0f);
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(0.0f, 0.32941177f, 0.6f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.0f, 0.47058824f, 0.84313726f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(0.0f, 0.32941177f, 0.6f, 1.0f);
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.1882353f, 0.1882353f, 0.2f, 1.0f);
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.30980393f, 0.30980393f, 0.34901962f, 1.0f);
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(0.22745098f, 0.22745098f, 0.24705882f, 1.0f);
    style.Colors[ImGuiCol_TableRowBg]    = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(0.0f, 0.47058824f, 0.84313726f, 1.0f);
    style.Colors[ImGuiCol_DragDropTarget] = ImVec4(1.0f, 1.0f, 0.0f, 0.9f);
    style.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    style.Colors[ImGuiCol_ModalWindowDimBg]  = ImVec4(0.8f, 0.8f, 0.8f, 0.35f);
}

void VKContext::setRestStyle()
{
    // Rest style by AaronBeardless from ImThemes
    ImGuiStyle& style = ImGui::GetStyle();

    style.Alpha                            = 1.0f;
    style.DisabledAlpha                    = 0.5f;
    style.WindowPadding                    = ImVec2(13.0f, 10.0f);
    style.WindowRounding                   = 0.0f;
    style.WindowBorderSize                 = 1.0f;
    style.WindowMinSize                    = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign                 = ImVec2(0.5f, 0.5f);
    style.WindowMenuButtonPosition         = ImGuiDir_Right;
    style.ChildRounding                    = 3.0f;
    style.ChildBorderSize                  = 1.0f;
    style.PopupRounding                    = 5.0f;
    style.PopupBorderSize                  = 1.0f;
    style.FramePadding                     = ImVec2(20.0f, 8.1f);
    style.FrameRounding                    = 2.0f;
    style.FrameBorderSize                  = 0.0f;
    style.ItemSpacing                      = ImVec2(3.0f, 3.0f);
    style.ItemInnerSpacing                 = ImVec2(3.0f, 8.0f);
    style.CellPadding                      = ImVec2(6.0f, 14.1f);
    style.IndentSpacing                    = 0.0f;
    style.ColumnsMinSpacing                = 10.0f;
    style.ScrollbarSize                    = 10.0f;
    style.ScrollbarRounding                = 2.0f;
    style.GrabMinSize                      = 12.1f;
    style.GrabRounding                     = 1.0f;
    style.TabRounding                      = 2.0f;
    style.TabBorderSize                    = 0.0f;
    style.TabCloseButtonMinWidthSelected   = 0.0f;
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    style.Colors[ImGuiCol_Text] =
        ImVec4(0.98039216f, 0.98039216f, 0.98039216f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4(0.49803922f, 0.49803922f, 0.49803922f, 1.0f);
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.09411765f, 0.09411765f, 0.09411765f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] =
        ImVec4(0.15686275f, 0.15686275f, 0.15686275f, 1.0f);
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.09411765f, 0.09411765f, 0.09411765f, 1.0f);
    style.Colors[ImGuiCol_Border]       = ImVec4(1.0f, 1.0f, 1.0f, 0.09803922f);
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_FrameBg]      = ImVec4(1.0f, 1.0f, 1.0f, 0.09803922f);
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.15686275f);
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.0f, 0.0f, 0.0f, 0.047058824f);
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.11764706f, 0.11764706f, 0.11764706f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.15686275f, 0.15686275f, 0.15686275f, 1.0f);
    style.Colors[ImGuiCol_TitleBgCollapsed] =
        ImVec4(0.11764706f, 0.11764706f, 0.11764706f, 1.0f);
    style.Colors[ImGuiCol_MenuBarBg]   = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.10980392f);
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.39215687f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.47058824f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.0f, 0.0f, 0.0f, 0.09803922f);
    style.Colors[ImGuiCol_CheckMark]  = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(1.0f, 1.0f, 1.0f, 0.39215687f);
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.3137255f);
    style.Colors[ImGuiCol_Button] = ImVec4(1.0f, 1.0f, 1.0f, 0.09803922f);
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.15686275f);
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.0f, 0.0f, 0.0f, 0.047058824f);
    style.Colors[ImGuiCol_Header] = ImVec4(1.0f, 1.0f, 1.0f, 0.09803922f);
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.15686275f);
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.0f, 0.0f, 0.0f, 0.047058824f);
    style.Colors[ImGuiCol_Separator] = ImVec4(1.0f, 1.0f, 1.0f, 0.15686275f);
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.23529412f);
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.23529412f);
    style.Colors[ImGuiCol_ResizeGrip] = ImVec4(1.0f, 1.0f, 1.0f, 0.15686275f);
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.23529412f);
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.23529412f);
    style.Colors[ImGuiCol_Tab]          = ImVec4(1.0f, 1.0f, 1.0f, 0.09803922f);
    style.Colors[ImGuiCol_TabHovered]   = ImVec4(1.0f, 1.0f, 1.0f, 0.15686275f);
    style.Colors[ImGuiCol_TabActive]    = ImVec4(1.0f, 1.0f, 1.0f, 0.3137255f);
    style.Colors[ImGuiCol_TabUnfocused] = ImVec4(0.0f, 0.0f, 0.0f, 0.15686275f);
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.23529412f);
    style.Colors[ImGuiCol_PlotLines] = ImVec4(1.0f, 1.0f, 1.0f, 0.3529412f);
    style.Colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogram] = ImVec4(1.0f, 1.0f, 1.0f, 0.3529412f);
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.15686275f, 0.15686275f, 0.15686275f, 1.0f);
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.3137255f);
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.19607843f);
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_TableRowBgAlt] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.019607844f);
    style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_DragDropTarget] =
        ImVec4(0.16862746f, 0.23137255f, 0.5372549f, 1.0f);
    style.Colors[ImGuiCol_NavHighlight] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    style.Colors[ImGuiCol_ModalWindowDimBg] =
        ImVec4(0.0f, 0.0f, 0.0f, 0.5647059f);
}

void VKContext::setComfortableDarkCyanStyle()
{
    // Comfortable Dark Cyan style by SouthCraftX from ImThemes
    ImGuiStyle& style = ImGui::GetStyle();

    style.Alpha                            = 1.0f;
    style.DisabledAlpha                    = 1.0f;
    style.WindowPadding                    = ImVec2(20.0f, 20.0f);
    style.WindowRounding                   = 11.5f;
    style.WindowBorderSize                 = 0.0f;
    style.WindowMinSize                    = ImVec2(20.0f, 20.0f);
    style.WindowTitleAlign                 = ImVec2(0.5f, 0.5f);
    style.WindowMenuButtonPosition         = ImGuiDir_None;
    style.ChildRounding                    = 20.0f;
    style.ChildBorderSize                  = 1.0f;
    style.PopupRounding                    = 17.4f;
    style.PopupBorderSize                  = 1.0f;
    style.FramePadding                     = ImVec2(20.0f, 3.4f);
    style.FrameRounding                    = 11.9f;
    style.FrameBorderSize                  = 0.0f;
    style.ItemSpacing                      = ImVec2(8.9f, 13.4f);
    style.ItemInnerSpacing                 = ImVec2(7.1f, 1.8f);
    style.CellPadding                      = ImVec2(12.1f, 9.2f);
    style.IndentSpacing                    = 0.0f;
    style.ColumnsMinSpacing                = 8.7f;
    style.ScrollbarSize                    = 11.6f;
    style.ScrollbarRounding                = 15.9f;
    style.GrabMinSize                      = 3.7f;
    style.GrabRounding                     = 20.0f;
    style.TabRounding                      = 9.8f;
    style.TabBorderSize                    = 0.0f;
    style.TabCloseButtonMinWidthSelected   = 0.0f;
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    style.Colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4(0.27450982f, 0.31764707f, 0.4509804f, 1.0f);
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] =
        ImVec4(0.09411765f, 0.101960786f, 0.11764706f, 1.0f);
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.15686275f, 0.16862746f, 0.19215687f, 1.0f);
    style.Colors[ImGuiCol_BorderShadow] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.11372549f, 0.1254902f, 0.15294118f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.15686275f, 0.16862746f, 0.19215687f, 1.0f);
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.15686275f, 0.16862746f, 0.19215687f, 1.0f);
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    style.Colors[ImGuiCol_TitleBgCollapsed] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.09803922f, 0.105882354f, 0.12156863f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.15686275f, 0.16862746f, 0.19215687f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.03137255f, 0.9490196f, 0.84313726f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.03137255f, 0.9490196f, 0.84313726f, 1.0f);
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.6f, 0.9647059f, 0.03137255f, 1.0f);
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.18039216f, 0.1882353f, 0.19607843f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.15294118f, 0.15294118f, 0.15294118f, 1.0f);
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.14117648f, 0.16470589f, 0.20784314f, 1.0f);
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.105882354f, 0.105882354f, 0.105882354f, 1.0f);
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.12941177f, 0.14901961f, 0.19215687f, 1.0f);
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.15686275f, 0.18431373f, 0.2509804f, 1.0f);
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.15686275f, 0.18431373f, 0.2509804f, 1.0f);
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.14509805f, 0.14509805f, 0.14509805f, 1.0f);
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.03137255f, 0.9490196f, 0.84313726f, 1.0f);
    style.Colors[ImGuiCol_ResizeGripActive] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.1254902f, 0.27450982f, 0.57254905f, 1.0f);
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.52156866f, 0.6f, 0.7019608f, 1.0f);
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(0.039215688f, 0.98039216f, 0.98039216f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.03137255f, 0.9490196f, 0.84313726f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(0.15686275f, 0.18431373f, 0.2509804f, 1.0f);
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    style.Colors[ImGuiCol_TableBorderLight] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_TableRowBg] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    style.Colors[ImGuiCol_TableRowBgAlt] =
        ImVec4(0.09803922f, 0.105882354f, 0.12156863f, 1.0f);
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(0.9372549f, 0.9372549f, 0.9372549f, 1.0f);
    style.Colors[ImGuiCol_DragDropTarget] =
        ImVec4(0.49803922f, 0.5137255f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.26666668f, 0.2901961f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(0.49803922f, 0.5137255f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_NavWindowingDimBg] =
        ImVec4(0.19607843f, 0.1764706f, 0.54509807f, 0.5019608f);
    style.Colors[ImGuiCol_ModalWindowDimBg] =
        ImVec4(0.19607843f, 0.1764706f, 0.54509807f, 0.5019608f);
}

void VKContext::setKazamCherryStyle()
{
    // Kazam's Cherry style by coyoteclan from ImThemes
    ImGuiStyle& style = ImGui::GetStyle();

    style.Alpha                            = 1.0f;
    style.DisabledAlpha                    = 0.6f;
    style.WindowPadding                    = ImVec2(6.0f, 3.0f);
    style.WindowRounding                   = 0.0f;
    style.WindowBorderSize                 = 1.0f;
    style.WindowMinSize                    = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign                 = ImVec2(0.5f, 0.5f);
    style.WindowMenuButtonPosition         = ImGuiDir_Left;
    style.ChildRounding                    = 0.0f;
    style.ChildBorderSize                  = 1.0f;
    style.PopupRounding                    = 0.0f;
    style.PopupBorderSize                  = 1.0f;
    style.FramePadding                     = ImVec2(5.0f, 5.0f);
    style.FrameRounding                    = 1.0f;
    style.FrameBorderSize                  = 0.0f;
    style.ItemSpacing                      = ImVec2(7.0f, 1.0f);
    style.ItemInnerSpacing                 = ImVec2(1.0f, 1.0f);
    style.CellPadding                      = ImVec2(4.0f, 4.0f);
    style.IndentSpacing                    = 6.0f;
    style.ColumnsMinSpacing                = 6.0f;
    style.ScrollbarSize                    = 13.0f;
    style.ScrollbarRounding                = 16.0f;
    style.GrabMinSize                      = 20.0f;
    style.GrabRounding                     = 2.0f;
    style.TabRounding                      = 2.0f;
    style.TabBorderSize                    = 0.0f;
    style.TabCloseButtonMinWidthSelected   = 0.0f;
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    style.Colors[ImGuiCol_Text] =
        ImVec4(0.85882354f, 0.92941177f, 0.8862745f, 0.88f);
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4(0.85882354f, 0.92941177f, 0.8862745f, 0.28f);
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.12941177f, 0.13725491f, 0.16862746f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.2f, 0.21960784f, 0.26666668f, 0.9f);
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.5372549f, 0.47843137f, 0.25490198f, 0.162f);
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.2f, 0.21960784f, 0.26666668f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 0.78f);
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.23137255f, 0.2f, 0.27058825f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.5019608f, 0.07450981f, 0.25490198f, 1.0f);
    style.Colors[ImGuiCol_TitleBgCollapsed] =
        ImVec4(0.2f, 0.21960784f, 0.26666668f, 0.75f);
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.2f, 0.21960784f, 0.26666668f, 0.47f);
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.2f, 0.21960784f, 0.26666668f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.08627451f, 0.14901961f, 0.15686275f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 0.78f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.70980394f, 0.21960784f, 0.26666668f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.46666667f, 0.76862746f, 0.827451f, 0.14f);
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.70980394f, 0.21960784f, 0.26666668f, 1.0f);
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.46666667f, 0.76862746f, 0.827451f, 0.14f);
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 0.86f);
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 0.76f);
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 0.86f);
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.5019608f, 0.07450981f, 0.25490198f, 1.0f);
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.42745098f, 0.42745098f, 0.49803922f, 0.5f);
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.5176471f, 0.21960784f, 0.3372549f, 0.88412017f);
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.46666667f, 0.76862746f, 0.827451f, 0.04f);
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 0.78f);
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.70980394f, 0.21960784f, 0.26666668f, 0.82832617f);
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.70980394f, 0.21960784f, 0.26666668f, 1.0f);
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.70980394f, 0.21960784f, 0.26666668f, 1.0f);
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.06666667f, 0.101960786f, 0.14509805f, 0.9724f);
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.85882354f, 0.92941177f, 0.8862745f, 0.63f);
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.85882354f, 0.92941177f, 0.8862745f, 0.63f);
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.1882353f, 0.1882353f, 0.2f, 1.0f);
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.30980393f, 0.30980393f, 0.34901962f, 1.0f);
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(0.22745098f, 0.22745098f, 0.24705882f, 1.0f);
    style.Colors[ImGuiCol_TableRowBg]    = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 0.43f);
    style.Colors[ImGuiCol_DragDropTarget] = ImVec4(1.0f, 1.0f, 0.0f, 0.9f);
    style.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    style.Colors[ImGuiCol_ModalWindowDimBg]  = ImVec4(0.8f, 0.8f, 0.8f, 0.35f);
}

}  // namespace MMM::Graphic
