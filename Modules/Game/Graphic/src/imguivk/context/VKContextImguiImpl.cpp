#include "config/AppConfig.h"
#include "config/AppPaths.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "event/core/EventBus.h"
#include "event/ui/ClearColorUpdateEvent.h"
#include "font/SystemFontResolver.h"
#include "graphic/glfw/window/NativeWindow.h"
#include "graphic/imguivk/VKContext.h"
#include "graphic/imguivk/VKRenderPass.h"
#include "graphic/imguivk/VKRenderer.h"
#include "graphic/imguivk/VKSwapchain.h"
#include "graphic/system/SystemTheme.h"
#include "graphic/theme/ImGuiThemeRegistry.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"
#include "implot.h"
#include "log/colorful-log.h"
#include "mmm/SafeParse.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace MMM::Graphic
{

/// @brief 获取当前上下文持有的内置与插件主题注册表。
/// @return 与 VKContext 生命周期一致的只读注册表引用。
/// @warning 设置/UI 查询路径：只解引用已在上下文初始化阶段创建的 owning
/// pointer。
const ImGuiThemeRegistry& VKContext::getThemeRegistry() const
{
    return *m_themeRegistry;
}

/// @brief Wayland 下独立图标字体的视觉缩放，避免 DPI 下方形按钮裁切字形。
static constexpr float WAYLAND_PURE_ICON_VISUAL_SCALE = 0.86f;

/// @brief 获取独立图标字体的运行时视觉缩放。
///
/// Wayland 的字体 DPI 与 framebuffer 缩放组合会让方形工具按钮中的图标略大，因此
/// 只对 pure_icons 字体应用视觉修正；文本合并图标仍沿用正文缩放。
///
/// @return Wayland 平台返回修正倍率，其他平台保持 1.0。
/// @warning 字体缩放更新路径：仅查询 GLFW backend，不访问文件系统或字体 atlas。
static float getPureIconVisualScale()
{
    // GLFW 可在 Linux 运行时选择 X11 或 Wayland，不能只依赖编译期宏。
    return glfwGetPlatform() == GLFW_PLATFORM_WAYLAND
               ? WAYLAND_PURE_ICON_VISUAL_SCALE
               : 1.0f;
}

/// @brief 无异常解析字体布局浮点配置。
///
/// 解析只接受非空、至少消费一个字符、可表示为有限正 float
/// 的数值前缀。任何错误、 NaN/Inf、零或负值都回退调用方默认字号。
///
/// @param value 配置字符串。
/// @param fallback 解析失败时的默认值。
/// @return 解析成功的有限浮点数或默认值。
static float parseFontLayoutFloat(std::string_view value, float fallback)
{
    // 空配置表示皮肤没有覆盖该场景字号。
    if ( value.empty() ) return fallback;

    // SafeParse 通过结果对象表达错误，不在字体加载路径使用异常。
    const auto  result = Internal::parseFloatingPrefix(value);
    const float parsed = static_cast<float>(result.value);
    // double 到 float 的窄化可能产生非有限值，因此同时验证最终表示。
    if ( result.error == std::errc{} && result.parsedLength != 0 &&
         std::isfinite(parsed) && parsed > 0.0f ) {
        return parsed;
    }
    // 配置错误不阻止整个字体 atlas 建立，使用场景默认值继续加载。
    return fallback;
}

/// @brief 无异常判断文件系统路径是否存在。
///
/// 使用 error_code
/// 重载屏蔽权限、编码或路径状态错误，调用者把任何失败都视为不可用
/// 字体候选并继续使用皮肤默认路径。
///
/// @param path 待检查路径。
/// @return 路径存在且检查过程无错误时返回 true。
static bool pathExistsNoError(const std::filesystem::path& path)
{
    // error_code 必须在 exists 返回后一起检查，避免把查询失败误判为存在。
    std::error_code filesystemError;
    return std::filesystem::exists(path, filesystemError) && !filesystemError;
}

/// @brief 应用 DeepDark 深色内置主题的布局与颜色参数。
/// @param style 接收完整主题覆盖的 ImGui 样式对象。
static void applyDeepDarkStyle(ImGuiStyle& style);
/// @brief 内置 Dark 主题样式。
/**
 * @brief 应用 Dark 深色内置主题的布局与颜色参数。
 * @param style 接收完整主题覆盖的 ImGui 样式对象。
 */
static void applyDarkStyle(ImGuiStyle& style);
/// @brief 内置 Light 主题样式。
/**
 * @brief 应用 Light 浅色内置主题的布局与颜色参数。
 * @param style 接收完整主题覆盖的 ImGui 样式对象。
 */
static void applyLightStyle(ImGuiStyle& style);
/// @brief 内置 IVM 经典 Windows 工具软件主题样式。
/**
 * @brief 应用 IVM 浅色扩展内置主题的布局与颜色参数。
 * @param style 接收完整主题覆盖的 ImGui 样式对象。
 */
static void applyIvmStyle(ImGuiStyle& style);
/// @brief 内置 Classic 主题样式。
/**
 * @brief 应用 Classic 经典内置主题的布局与颜色参数。
 * @param style 接收完整主题覆盖的 ImGui 样式对象。
 */
static void applyClassicStyle(ImGuiStyle& style);
/// @brief 内置 Microsoft 主题样式。
/**
 * @brief 应用 Microsoft内置主题的布局与颜色参数。
 * @param style 接收完整主题覆盖的 ImGui 样式对象。
 */
static void applyMicrosoftStyle(ImGuiStyle& style);
/// @brief 内置 Darcula 主题样式。
/**
 * @brief 应用 Darcula内置主题的布局与颜色参数。
 * @param style 接收完整主题覆盖的 ImGui 样式对象。
 */
static void applyDarculaStyle(ImGuiStyle& style);
/// @brief 内置 Photoshop 主题样式。
/**
 * @brief 应用 Photoshop内置主题的布局与颜色参数。
 * @param style 接收完整主题覆盖的 ImGui 样式对象。
 */
static void applyPhotoshopStyle(ImGuiStyle& style);
/// @brief 内置 Unreal 主题样式。
/**
 * @brief 应用 Unreal内置主题的布局与颜色参数。
 * @param style 接收完整主题覆盖的 ImGui 样式对象。
 */
static void applyUnrealStyle(ImGuiStyle& style);
/// @brief 内置 Gold 主题样式。
/**
 * @brief 应用 Gold 金色内置主题的布局与颜色参数。
 * @param style 接收完整主题覆盖的 ImGui 样式对象。
 */
static void applyGoldStyle(ImGuiStyle& style);
/// @brief 内置 RoundedVisualStudio 主题样式。
/**
 * @brief 应用 Rounded Visual Studio内置主题的布局与颜色参数。
 * @param style 接收完整主题覆盖的 ImGui 样式对象。
 */
static void applyRoundedVisualStudioStyle(ImGuiStyle& style);
/// @brief 内置 SonicRiders 主题样式。
/**
 * @brief 应用 Sonic Riders内置主题的布局与颜色参数。
 * @param style 接收完整主题覆盖的 ImGui 样式对象。
 */
static void applySonicRidersStyle(ImGuiStyle& style);
/// @brief 内置 DarkRuda 主题样式。
/**
 * @brief 应用 Dark Ruda内置主题的布局与颜色参数。
 * @param style 接收完整主题覆盖的 ImGui 样式对象。
 */
static void applyDarkRudaStyle(ImGuiStyle& style);
/// @brief 内置 SoftCherry 主题样式。
/**
 * @brief 应用 Soft Cherry内置主题的布局与颜色参数。
 * @param style 接收完整主题覆盖的 ImGui 样式对象。
 */
static void applySoftCherryStyle(ImGuiStyle& style);
/// @brief 内置 Enemymouse 主题样式。
/**
 * @brief 应用 Enemymouse内置主题的布局与颜色参数。
 * @param style 接收完整主题覆盖的 ImGui 样式对象。
 */
static void applyEnemymouseStyle(ImGuiStyle& style);
/// @brief 内置 DiscordDark 主题样式。
/**
 * @brief 应用 Discord Dark内置主题的布局与颜色参数。
 * @param style 接收完整主题覆盖的 ImGui 样式对象。
 */
static void applyDiscordDarkStyle(ImGuiStyle& style);
/// @brief 内置 Comfy 主题样式。
/**
 * @brief 应用 Comfy内置主题的布局与颜色参数。
 * @param style 接收完整主题覆盖的 ImGui 样式对象。
 */
static void applyComfyStyle(ImGuiStyle& style);
/// @brief 内置 PurpleComfy 主题样式。
/**
 * @brief 应用 Purple Comfy内置主题的布局与颜色参数。
 * @param style 接收完整主题覆盖的 ImGui 样式对象。
 */
static void applyPurpleComfyStyle(ImGuiStyle& style);
/// @brief 内置 FutureDark 主题样式。
/**
 * @brief 应用 Future Dark内置主题的布局与颜色参数。
 * @param style 接收完整主题覆盖的 ImGui 样式对象。
 */
static void applyFutureDarkStyle(ImGuiStyle& style);
/// @brief 内置 CleanDark 主题样式。
/**
 * @brief 应用 Clean Dark内置主题的布局与颜色参数。
 * @param style 接收完整主题覆盖的 ImGui 样式对象。
 */
static void applyCleanDarkStyle(ImGuiStyle& style);
/// @brief 内置 Moonlight 主题样式。
/**
 * @brief 应用 Moonlight内置主题的布局与颜色参数。
 * @param style 接收完整主题覆盖的 ImGui 样式对象。
 */
static void applyMoonlightStyle(ImGuiStyle& style);
/// @brief 内置 Cecilia 主题样式。
/**
 * @brief 应用 Cecilia 动态内置主题的布局与颜色参数。
 * @param style 接收完整主题覆盖的 ImGui 样式对象。
 */
static void applyCeciliaStyle(ImGuiStyle& style);
/// @brief 内置 ComfortableLight 主题样式。
/**
 * @brief 应用 Comfortable Light内置主题的布局与颜色参数。
 * @param style 接收完整主题覆盖的 ImGui 样式对象。
 */
static void applyComfortableLightStyle(ImGuiStyle& style);
/// @brief 内置 HazyDark 主题样式。
/**
 * @brief 应用 Hazy Dark内置主题的布局与颜色参数。
 * @param style 接收完整主题覆盖的 ImGui 样式对象。
 */
static void applyHazyDarkStyle(ImGuiStyle& style);
/// @brief 内置 Everforest 主题样式。
/**
 * @brief 应用 Everforest内置主题的布局与颜色参数。
 * @param style 接收完整主题覆盖的 ImGui 样式对象。
 */
static void applyEverforestStyle(ImGuiStyle& style);
/// @brief 内置 Windark 主题样式。
/**
 * @brief 应用 Windark内置主题的布局与颜色参数。
 * @param style 接收完整主题覆盖的 ImGui 样式对象。
 */
static void applyWindarkStyle(ImGuiStyle& style);
/// @brief 内置 Rest 主题样式。
/**
 * @brief 应用 Rest内置主题的布局与颜色参数。
 * @param style 接收完整主题覆盖的 ImGui 样式对象。
 */
static void applyRestStyle(ImGuiStyle& style);
/// @brief 内置 ComfortableDarkCyan 主题样式。
/**
 * @brief 应用 Comfortable Dark Cyan内置主题的布局与颜色参数。
 * @param style 接收完整主题覆盖的 ImGui 样式对象。
 */
static void applyComfortableDarkCyanStyle(ImGuiStyle& style);
/// @brief 内置 KazamCherry 主题样式。
/**
 * @brief 应用 Kazam Cherry内置主题的布局与颜色参数。
 * @param style 接收完整主题覆盖的 ImGui 样式对象。
 */
static void applyKazamCherryStyle(ImGuiStyle& style);

/// @brief 把 ImGui Vulkan backend 回调的失败结果写入项目日志。
/// @param err backend 返回的原始 VkResult。
/// @warning Vulkan backend 回调路径：成功快速返回；失败只记录错误，不抛出异常。
static void check_vk_result(VkResult err)
{
    // VK_SUCCESS 是 backend 常态，不产生每帧日志噪声。
    if ( err == VK_SUCCESS ) return;
    XERROR("[vulkan] Error: VkResult = {}", static_cast<uint32_t>(err));
}

/// @brief 初始化 ImGui GLFW/Vulkan 后端。
///
/// 创建 ImGui/ImPlot context，配置 ini、导航、Docking/Multi-Viewport 与 DPI
/// 策略， 再把现有 Vulkan instance/device/queue/render pass/descriptor pool
/// 注入官方 backend。
/// 最后根据窗口资源模式建立启动字体或完整皮肤字体和插件主题。
///
/// @param windowHandle 已创建并与当前 Vulkan surface 对应的 GLFW 窗口。
/// @param mode Bootstrap 只加载系统字体和最小主题；Application
/// 加载完整运行资源。
/// @warning 应用启动低频路径：创建全局 GUI context、访问配置目录、初始化
/// backend 并加载字体/Lua 插件，必须在渲染线程首次 NewFrame 之前调用一次。
void VKContext::imguiVulkanInit(GLFWwindow*          windowHandle,
                                VKWindowResourceMode mode)
{
    // 版本检查必须先于 context 创建，捕获头文件与链接库 ABI 不匹配。
    IMGUI_CHECKVERSION();
    // ImPlot 依赖有效 ImGui context，按 ImGui 后创建、析构时反向销毁。
    ImGui::CreateContext();
    ImPlot::CreateContext();
    // style/io 引用在 context 生命周期内稳定，初始化阶段不跨线程共享。
    ImGuiStyle& style = ImGui::GetStyle();
    ImGuiIO&    io    = ImGui::GetIO();
    (void)io;
    // 使用 error_code 创建配置目录，失败只禁用/影响 ini
    // 持久化，不中断渲染初始化。
    std::error_code imguiIniDirectoryError;
    std::filesystem::create_directories(Config::AppPaths::configRootPath(),
                                        imguiIniDirectoryError);
    if ( imguiIniDirectoryError ) {
        XWARN("Failed to create ImGui ini directory: {}",
              imguiIniDirectoryError.message());
    }
    // ImGui 只保存 const char*，静态 string 必须覆盖整个 context 生命周期。
    static const std::string imguiIniPath =
        Config::pathToUtf8(Config::AppPaths::imguiIniFilePath());
    io.IniFilename = imguiIniPath.c_str();
    // 键盘和手柄导航在首次 NewFrame 前固定，避免运行中改变 backend 输入契约。
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    // Docking/Multi-Viewport 必须在首次 NewFrame 前确定；Bootstrap
    // 只使用主视口， 提前启用不会主动创建平台窗口。
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    // 允许平台窗口自动并回主视口，但不为每个 viewport 创建独立任务栏图标。
    io.ConfigViewportsNoAutoMerge   = false;
    io.ConfigViewportsNoTaskBarIcon = true;
    // 自绘窗口只允许从标题栏区域移动，避免内容空白处误拖动。
    io.ConfigWindowsMoveFromTitleBarOnly = true;

    // 字体和样式缩放由项目显式控制，禁用 ImGui 自动字体 DPI
    // 修改以避免双重缩放。
    style.FontScaleDpi = 1.0f;

    io.ConfigDpiScaleFonts = false;  // 下方手动处理字体缩放。
    // 平台 viewport 的系统 DPI 仍交由 backend 跟踪，窗口尺寸与 framebuffer
    // 保持一致。
    io.ConfigDpiScaleViewports = true;

    // GLFW backend 安装输入 callback；下方会恢复本项目必须保留的 iconify/drop
    // 入口。
    ImGui_ImplGlfw_InitForVulkan(windowHandle, true);

    // Vulkan backend 只借用 VKContext/VKRenderer 拥有的句柄，不接管其生命周期。
    ImGui_ImplVulkan_InitInfo init_info = {};
    // API 版本必须与实例创建版本兼容，backend 据此选择 Vulkan 能力路径。
    init_info.ApiVersion     = VK_API_VERSION_1_4;
    init_info.Instance       = m_vkInstance;
    init_info.PhysicalDevice = m_vkPhysicalDevice;
    // ImGui draw 上传与渲染使用 graphics queue family 和对应 queue。
    init_info.QueueFamily   = m_queueFamilyIndices.graphicsQueueIndex.value();
    init_info.Device        = m_vkLogicalDevice;
    init_info.Queue         = m_LogicDeviceGraphicsQueue;
    init_info.PipelineCache = nullptr;
    // Descriptor pool 由 VKRenderer 统一拥有，纹理注册也从同一 pool 分配。
    init_info.DescriptorPool = m_vkRenderer->m_vkDescriptorPool;
    // 双缓冲是 backend 支持下限，实际 image count 与当前 swapchain image
    // 数一致。
    init_info.MinImageCount = 2;
    init_info.ImageCount    = m_swapchain->m_vkImageBuffers.size();
    // 主 pipeline 必须与 VKContext 当前交换链 render pass/subpass/sample count
    // 兼容。
    init_info.PipelineInfoMain.RenderPass  = m_vkRenderPass->getRenderPass();
    init_info.PipelineInfoMain.Subpass     = 0;
    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    // 使用默认 Vulkan allocator；错误经 check_vk_result 统一进入项目日志。
    init_info.Allocator       = nullptr;
    init_info.CheckVkResultFn = check_vk_result;

    // backend 初始化必须在字体纹理或任何 ImGui draw command 创建之前完成。
    ImGui_ImplVulkan_Init(&init_info);

    // ImGui GLFW backend 会安装自身
    // callback；重新设置本项目入口以保留窗口状态和 文件拖放桥接，其他输入
    // callback 由 backend chaining 处理。
    glfwSetWindowIconifyCallback(windowHandle,
                                 NativeWindow::GLFW_IconifyCallback);
    glfwSetDropCallback(windowHandle, NativeWindow::GLFW_DropCallback);

    if ( mode == VKWindowResourceMode::Bootstrap ) {
        // 启动窗口不能依赖尚未同步的皮肤资源或 Lua 插件。
        setupBootstrapFonts();
        applyBootstrapTheme();
    } else {
        // 正式窗口加载皮肤字体后再注册/应用内置与插件主题。
        setupFonts();
        reloadPlugins();
    }
    XDEBUG("ImGui Vulkan backend initialized.");
}

/// @brief 加载启动期系统首选字体。
///
/// 根据 native/ui scale 建立 atlas
/// 像素尺寸与运行显示尺寸的比例，按系统字体解析器 返回顺序加载首个字体并把后续
/// face 合并到同一 atlas。没有可用系统字体时回退 ImGui 默认字体，Bootstrap
/// 阶段不依赖皮肤字体资源。
///
/// @warning 启动低频路径：枚举系统字体并读取字体文件，只在 Bootstrap context
/// 初始化时调用。
void VKContext::setupBootstrapFonts()
{
    // 非 2 次幂高度减少系统 CJK 字体 atlas 的无效空白。
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;

    // nativeScale 决定 raster 像素，uiScale 决定逻辑显示比例。
    float nativeScale = Config::AppConfig::instance().getNativeContentScale();
    float uiScale     = Config::AppConfig::instance().getUIScale();
    // 配置来自平台探测，异常值必须在参与字号乘除前归一化。
    if ( !std::isfinite(nativeScale) || nativeScale <= 0.0f ) {
        nativeScale = 1.0f;
    }
    if ( !std::isfinite(uiScale) || uiScale <= 0.0f ) {
        uiScale = 1.0f;
    }

    // 字体以 nativeScale 烘焙，再用 baseScale 抵消 backend 已处理部分并匹配
    // UI。
    m_fontAtlasBaseScale = uiScale / nativeScale;
    if ( !std::isfinite(m_fontAtlasBaseScale) ||
         m_fontAtlasBaseScale <= 0.0f ) {
        m_fontAtlasBaseScale = 1.0f;
    }
    // Bootstrap 不应用用户字体倍率，避免加载界面受尚未完成的设置资源影响。
    m_fontAtlasScaleMain              = 1.0f;
    ImGui::GetStyle().FontScaleMain   = 1.0f;
    constexpr float bootstrapFontSize = 17.0f;
    // Vulkan/ImGui 字体字号必须保持正像素值。
    const float atlasSize = std::max(1.0f, bootstrapFontSize * nativeScale);

    // 第一个成功字体成为默认字体，后续系统候选使用 MergeMode 补充缺失字形。
    ImFont* bootstrapFont = nullptr;
    for ( const auto& systemFont : Font::resolvePreferredSystemFonts() ) {
        if ( systemFont.m_filePath.empty() ) continue;

        // faceIndex 支持 TTC/OTC 集合；负索引钳制为第一个 face。
        ImFontConfig fontConfig;
        fontConfig.FontNo = std::max(systemFont.m_faceIndex, 0);
        // 只有已有基础字体时才合并，否则当前候选创建 atlas 首个字体对象。
        fontConfig.MergeMode   = bootstrapFont != nullptr;
        fontConfig.OversampleH = 1;
        fontConfig.OversampleV = 1;
        fontConfig.PixelSnapH  = true;

        // 动态 atlas 只保留文件引用/字体配置，失败候选不阻止继续尝试下一项。
        ImFont* loadedFont = io.Fonts->AddFontFromFileTTF(
            Config::pathToUtf8(systemFont.m_filePath).c_str(),
            atlasSize,
            &fontConfig);
        if ( !loadedFont ) {
            XWARN("Failed to load preferred system font: {}",
                  Config::pathToUtf8(systemFont.m_filePath));
            continue;
        }

        if ( !bootstrapFont ) {
            // 默认字体指针和 Scale 只设置一次，合并 face
            // 不创建新的业务默认选择。
            bootstrapFont        = loadedFont;
            io.FontDefault       = bootstrapFont;
            bootstrapFont->Scale = m_fontAtlasBaseScale;
        }
        XINFO("Loaded startup system font: {} (face {})",
              Config::pathToUtf8(systemFont.m_filePath),
              systemFont.m_faceIndex);
    }

    if ( !bootstrapFont ) {
        // 所有系统候选失败时，ImGui 内置压缩字体保证启动错误界面仍可显示。
        bootstrapFont = io.Fonts->AddFontDefault();
        if ( bootstrapFont ) {
            bootstrapFont->Scale = m_fontAtlasBaseScale;
            io.FontDefault       = bootstrapFont;
        }
        XWARN("No preferred system font was resolved; using ImGui default");
    }
}

/// @brief 应用不依赖皮肤资源的启动期最小样式。
///
/// 从 ImGui 默认 Dark palette 建立圆角、间距和少量关键颜色，只使用 AppConfig 的
/// UI scale，不访问主题插件或 SkinManager 色板。正式 context
/// 会在资源加载后替换。
///
/// @warning 启动低频路径：重置并缩放全局 ImGuiStyle，只在 Bootstrap
/// 初始化调用。
void VKContext::applyBootstrapTheme()
{
    // 先恢复全新 ImGuiStyle，避免复用 context 时残留其他主题字段。
    ImGuiStyle& style = ImGui::GetStyle();
    style             = ImGuiStyle();
    ImGui::StyleColorsDark(&style);

    // 启动样式不允许缩小到 1 以下，确保错误提示和操作控件可读。
    const float uiScale =
        std::max(Config::AppConfig::instance().getUIScale(), 1.0f);
    // 先统一缩放默认字段，再覆盖项目启动界面的特定圆角和间距。
    style.ScaleAllSizes(uiScale);
    style.FontScaleDpi     = 1.0f;
    style.WindowRounding   = 12.0f * uiScale;
    style.ChildRounding    = 12.0f * uiScale;
    style.FrameRounding    = 7.0f * uiScale;
    style.PopupRounding    = 10.0f * uiScale;
    style.GrabRounding     = 7.0f * uiScale;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize  = 1.0f;
    // 框体内边距决定输入框和按钮的文字基线及垂直触达面积。
    style.FramePadding = ImVec2(12.0f * uiScale, 8.0f * uiScale);
    // 项目间距约束同级控件节奏，也是多数自动布局的基础步长。
    style.ItemSpacing = ImVec2(10.0f * uiScale, 10.0f * uiScale);
    // 窗口内边距同时决定内容密度和停靠面板的呼吸空间。
    style.WindowPadding = ImVec2(24.0f * uiScale, 22.0f * uiScale);

    // 深色中性背景与蓝色操作强调保证尚未加载皮肤时的基本层级对比。
    // 窗口底色奠定整套主题明度，后续容器和控件颜色均以此为参照。
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.035f, 0.043f, 0.065f, 1.0f);
    // 子窗口背景可选择透明继承，避免嵌套区域产生多余色块。
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.075f, 0.086f, 0.12f, 0.98f);
    // 边框色用于低成本描绘控件和容器边界，透明度决定强调强度。
    style.Colors[ImGuiCol_Border] = ImVec4(0.20f, 0.24f, 0.34f, 0.8f);
    // 框体常态是输入和选择控件的静止基面，也是交互渐变的起点。
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.14f, 0.20f, 1.0f);
    // 柱状图使用独立数据色，避免与按钮等交互强调色混淆。
    style.Colors[ImGuiCol_PlotHistogram] = ImVec4(0.31f, 0.57f, 0.96f, 1.0f);
    // 按钮常态建立操作面的基础层级，为悬浮和按下反馈预留变化空间。
    style.Colors[ImGuiCol_Button] = ImVec4(0.18f, 0.36f, 0.68f, 1.0f);
    // 按钮悬浮态只表达指向，不应与已按下状态混淆。
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.24f, 0.46f, 0.84f, 1.0f);
    // 按钮激活态提供按下确认，需要成为三态中最明确的反馈。
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.15f, 0.31f, 0.62f, 1.0f);
}

/// @brief 加载完整运行时 ASCII、CJK、合并图标与独立图标字体。
///
/// 字体选择优先用户偏好名称，其次允许外部绝对路径，最后使用皮肤默认资源。每个
/// 业务场景建立独立基础字体并合并 CJK/图标 face；atlas 按 native DPI 像素加载，
/// Font::Scale 再恢复 UI 逻辑比例。
///
/// @warning 初始化或低频字体重建路径：访问配置/字体文件并向动态 atlas 添加多个
/// face，禁止进入每帧 UI 热路径。
void VKContext::setupFonts()
{
    // 本轮所有字体共享同一 atlas 与 SkinManager runtimeFonts 注册表。
    ImGuiIO& io        = ImGui::GetIO();
    auto&    settings  = Config::AppConfig::instance().getEditorSettings();
    float native_scale = Config::AppConfig::instance().getNativeContentScale();
    float ui_scale     = Config::AppConfig::instance().getUIScale();
    auto& skinMgr      = Config::SkinManager::instance();
    // 放宽 atlas 高度到非 2 次幂并设置最小尺寸，降低多场景 CJK 字体频繁扩容。
    io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;
    io.Fonts->TexMinWidth  = std::max(io.Fonts->TexMinWidth, 2048);
    io.Fonts->TexMinHeight = std::max(io.Fonts->TexMinHeight, 1024);

    // 平台/用户缩放在参与 atlas 计算前必须归一化为有限正值。
    if ( !std::isfinite(native_scale) || native_scale <= 0.0f ) {
        native_scale = 1.0f;
    }
    if ( !std::isfinite(ui_scale) || ui_scale <= 0.0f ) {
        ui_scale = 1.0f;
    }
    // baseScale 把按物理像素烘焙的字体转换回项目 UI 逻辑尺寸。
    m_fontAtlasBaseScale = ui_scale / native_scale;
    if ( !std::isfinite(m_fontAtlasBaseScale) ||
         m_fontAtlasBaseScale <= 0.0f ) {
        m_fontAtlasBaseScale = 1.0f;
    }
    // FontScaleMain 是用户对全部正文的二次倍率，限制到设置 UI 支持范围。
    m_fontAtlasScaleMain = settings.fontSizeMultiplier;
    if ( !std::isfinite(m_fontAtlasScaleMain) ||
         m_fontAtlasScaleMain <= 0.0f ) {
        m_fontAtlasScaleMain = 1.0f;
    }
    m_fontAtlasScaleMain = std::clamp(m_fontAtlasScaleMain, 0.5f, 2.0f);
    ImGui::GetStyle().FontScaleMain = m_fontAtlasScaleMain;

    // 每次调用创建一个业务字体栈：ASCII 基础 + CJK 合并 + 图标合并。
    auto loadFontWithSize = [&](const std::string& key, float size) {
        // 在 lambda 内重新取 settings 引用，保持偏好读取与当前 AppConfig
        // 实例一致。
        auto& settings      = Config::AppConfig::instance().getEditorSettings();
        auto  asciiFontPath = skinMgr.getFontPath("ascii");
        auto  cjkFontPath   = skinMgr.getFontPath("cjk");
        auto  iconFontPath  = skinMgr.getFontPath("icons");

        // 偏好值先按皮肤已注册显示名查找，再把存在的路径解释为外部字体。
        if ( !settings.preferredAsciiFont.empty() &&
             settings.preferredAsciiFont != "Default" ) {
            const auto& asciiFonts = skinMgr.getAsciiFonts();
            auto        it         = std::find_if(
                asciiFonts.begin(), asciiFonts.end(), [&](const auto& pair) {
                    return pair.first == settings.preferredAsciiFont;
                });
            if ( it != asciiFonts.end() ) {
                // 皮肤字体映射提供规范化资源路径。
                asciiFontPath = it->second;
            } else if ( pathExistsNoError(
                            Config::utf8ToPath(settings.preferredAsciiFont)) ) {
                // 未命中名称但路径存在时允许直接选择外部/系统字体。
                asciiFontPath = settings.preferredAsciiFont;
            }
        }

        // CJK 偏好遵循与 ASCII 相同的名称优先、路径后备规则。
        if ( !settings.preferredCjkFont.empty() &&
             settings.preferredCjkFont != "Default" ) {
            const auto& cjkFonts = skinMgr.getCjkFonts();
            auto        it       = std::find_if(
                cjkFonts.begin(), cjkFonts.end(), [&](const auto& pair) {
                    return pair.first == settings.preferredCjkFont;
                });
            if ( it != cjkFonts.end() ) {
                cjkFontPath = it->second;
            } else if ( pathExistsNoError(
                            Config::utf8ToPath(settings.preferredCjkFont)) ) {
                // 如果是绝对路径，说明是外部/系统字体
                cjkFontPath = settings.preferredCjkFont;
            }
        }

        // 基础字体不使用 MergeMode，返回的 ImFont 是业务侧保存和缩放的主对象。
        ImFontConfig config;
        // ImGui dynamic atlas 可能延迟烘焙；STB oversample 在部分 CJK/OpenType
        // 边缘会触发断言，因此所有 face 固定单采样并启用像素对齐。
        config.OversampleH = 1;
        config.OversampleV = 1;
        config.PixelSnapH  = true;

        // 按原生 DPI 像素加载以保持清晰，显示时再用 Font::Scale 抵消。
        float atlasSize = std::max(1.0f, size * native_scale);

        // ASCII 基础必须成功后才合并其他 face，避免 MergeMode 没有目标字体。
        ImFont* font = io.Fonts->AddFontFromFileTTF(
            Config::pathToUtf8(asciiFontPath).c_str(), atlasSize, &config);

        if ( font ) {
            // CJK face 合并到当前基础字体，由动态 atlas 按实际字符需求烘焙。
            ImFontConfig mergeConfig;
            mergeConfig.MergeMode   = true;
            mergeConfig.PixelSnapH  = true;
            mergeConfig.OversampleH = 1;
            mergeConfig.OversampleV = 1;

            io.Fonts->AddFontFromFileTTF(
                Config::pathToUtf8(cjkFontPath).c_str(),
                atlasSize,
                &mergeConfig);

            // 图标 face 使用皮肤资源并合并到相同
            // ImFont，文本和图标可在同一字符串。
            ImFontConfig iconConfig;
            iconConfig.MergeMode   = true;
            iconConfig.PixelSnapH  = true;
            iconConfig.OversampleH = 1;
            iconConfig.OversampleV = 1;

            // 图标缩小为正文 0.9 倍后略向上移，使方形按钮和行内基线视觉居中。
            iconConfig.GlyphOffset.y = -(size * 0.05f) * native_scale;

            io.Fonts->AddFontFromFileTTF(
                Config::pathToUtf8(iconFontPath).c_str(),
                atlasSize * 0.9f,
                &iconConfig);

            // Scale 只改变显示大小，不要求动态 atlas 重新选择 raster 像素尺寸。
            font->Scale = m_fontAtlasBaseScale;

            // SkinManager 只保存观察指针，实际字体由 ImGui atlas 拥有。
            skinMgr.setFont(key, font);
        }
        return font;
    };

    // 字号配置允许皮肤字符串覆盖，非法值按每场景默认值恢复。
    auto getFontSize = [&](const std::string& key, float defaultSize) {
        std::string val = skinMgr.getLayoutConfig("fontsize." + key);
        return parseFontLayoutFloat(val, defaultSize);
    };

    // 各 key 对应 UI 不同层级，保持独立 ImFont 以支持皮肤定制字号。
    loadFontWithSize("content", getFontSize("content", 14.0f));
    loadFontWithSize("title", getFontSize("title", 20.0f));
    loadFontWithSize("menu", getFontSize("menu", 16.0f));
    loadFontWithSize("filemanager", getFontSize("filemanager", 14.0f));
    loadFontWithSize("side_bar", getFontSize("side_bar", 16.0f));
    loadFontWithSize("setting_internal",
                     getFontSize("setting_internal", 14.0f));

    // pure_icons 不与正文合并，供侧边栏/工具栏方形按钮独立选择和视觉缩放。
    {
        // 与合并图标相同地禁用 oversample 并启用像素对齐。
        ImFontConfig iconConfig;
        iconConfig.PixelSnapH  = true;
        iconConfig.OversampleH = 1;
        iconConfig.OversampleV = 1;

        // 固定较小基础尺寸与轻微负 Y 偏移，避免按钮内部裁切或视觉下沉。
        float size               = 16.0f;
        float atlasSize          = std::max(1.0f, size * native_scale);
        iconConfig.GlyphOffset.y = -(size * 0.05f) * native_scale;

        ImFont* iconFont = io.Fonts->AddFontFromFileTTF(
            Config::pathToUtf8(skinMgr.getFontPath("icons")).c_str(),
            atlasSize * 0.9f,
            &iconConfig);
        if ( iconFont ) {
            // Wayland 额外应用视觉修正，其他平台只使用 atlas base scale。
            iconFont->Scale = m_fontAtlasBaseScale * getPureIconVisualScale();
        }
        // 加载失败也注册 nullptr，让消费者显式走字体后备而非沿用旧指针。
        skinMgr.setFont("pure_icons", iconFont);
    }
}

/// @brief 在 GPU 空闲边界内清除并重新建立完整 ImGui 字体 atlas。
///
/// 先等待所有可能引用旧字体纹理的命令完成，清 SkinManager 观察指针和 ImGui
/// atlas， 再复用 setupFonts 加载当前皮肤/设置。backend 的动态 atlas
/// 会在后续帧处理纹理。
///
/// @warning 低频字体热重载路径：内部 device.waitIdle
/// 且访问字体文件，只能响应明确 重建请求，禁止从每帧无条件调用。
void VKContext::rebuildFonts()
{
    // 日志标记资源热重载边界，便于定位字体文件和 Vulkan 同步问题。
    XINFO("Hot-reloading ImGui fonts...");
    // 旧 atlas 纹理或 ImFont 可能仍被在途 draw data 引用，清理前必须 device
    // idle。
    if ( m_vkLogicalDevice.waitIdle() != vk::Result::eSuccess ) {
        XERROR("Failed to wait for device idle during font reload.");
        return;
    }

    // wait 成功后当前线程可安全修改全局 atlas。
    ImGuiIO& io = ImGui::GetIO();

    // 先清除 SkinManager 的非 owning ImFont 指针，避免 atlas Clear
    // 后出现悬空引用。
    Config::SkinManager::instance().clearRuntimeFonts();

    // 默认字体指针属于 atlas，必须在 Clear 前归零。
    io.FontDefault = nullptr;
    io.Fonts->Clear();

    // setupFonts 重新计算 DPI/用户倍率并注册所有业务字体 key。
    setupFonts();

    XINFO("Fonts rebuilt.");
}

/// @brief 不重建 atlas 地刷新已加载字体的运行显示倍率。
///
/// FontScaleMain 承载用户全局字号倍率；每个 runtime font 恢复 atlas base
/// scale， pure_icons 再叠加 Wayland 视觉修正。函数不改变字体文件或 raster
/// 尺寸。
///
/// @warning 仅响应 DPI 或字体设置变化时调用；会遍历
/// runtimeFonts，但不访问文件或 等待 GPU，不应进入每帧 UI 热路径。
void VKContext::updateFontScales()
{
    // 样式字段影响 ImGui 当前主字体倍率，与每个 ImFont 的 atlas base scale
    // 分离。
    ImGui::GetStyle().FontScaleMain = m_fontAtlasScaleMain;

    // runtimeFonts 由 setupFonts 建立，值为 ImGui atlas 拥有的观察指针。
    auto& skinMgr = Config::SkinManager::instance();
    for ( auto& [key, font] : skinMgr.getData().runtimeFonts ) {
        if ( font ) {
            // 独立图标修正不能应用到合并正文，否则文本字号会随平台改变。
            font->Scale = key == "pure_icons"
                              ? m_fontAtlasBaseScale * getPureIconVisualScale()
                              : m_fontAtlasBaseScale;
        }
    }
}

/// @brief 跨线程请求在渲染安全点重建字体。
///
/// 只发布单向脏位，不携带设置数据；调用方先更新配置，渲染线程消费后读取最新版。
///
/// @warning 原子写入路径：可由 UI/资源线程调用，使用 release 与消费端 acquire
/// 配对。
void VKContext::requestFontRebuild()
{
    m_fontRebuildRequested.store(true, std::memory_order_release);
}

/// @brief 在渲染线程安全点消费一次字体重建请求。
///
/// exchange 同时清除脏位并获得请求前配置写入；多个请求在重建前合并为一次。
///
/// @warning 渲染准备热路径：每帧只执行一次原子 exchange；命中后会进入阻塞式低频
/// rebuildFonts，调用位置必须位于允许 device idle 的资源维护阶段。
void VKContext::checkAndRebuildFonts()
{
    // acq_rel 既消费 release 发布的数据，也保证同一时刻只有一次重建取得 true。
    if ( m_fontRebuildRequested.exchange(false, std::memory_order_acq_rel) ) {
        rebuildFonts();
    }
}

/// @brief 在 Auto 主题模式下限频检测系统深浅色变化并重应用主题。
///
/// 非 Auto 模式立即清除已应用系统主题标记。Auto 模式最多每秒调用一次平台探测，
/// 只有结果与 m_appliedSystemTheme 不同才执行完整 applyTheme。
///
/// @warning UI 更新路径：每帧可调用，但常态仅比较时间点；系统探测和主题应用限频
/// 到一秒一次，不执行固定时长阻塞等待。
void VKContext::checkAndApplySystemTheme()
{
    // 显式主题不应受系统外观变化影响。
    const auto& settings = Config::AppConfig::instance().getEditorSettings();
    if ( settings.theme != Config::UI_THEME_AUTO_ID ) {
        m_appliedSystemTheme = SystemTheme::Unknown;
        return;
    }

    // steady_clock 不受系统时间校准影响，适合非阻塞轮询节流。
    const auto now = std::chrono::steady_clock::now();
    if ( now < m_nextSystemThemeCheck ) return;
    // 先推进下一检查点，applyTheme 内部若触发其他状态也不会重复探测。
    m_nextSystemThemeCheck = now + std::chrono::seconds(1);

    // refreshSystemTheme 更新平台缓存，只有实际变化才重置整套 ImGuiStyle。
    const SystemTheme systemTheme = refreshSystemTheme();
    if ( systemTheme != m_appliedSystemTheme ) {
        applyTheme();
    }
}

/// @brief 幂等注册项目提供的全部内置 ImGui 主题。
///
/// 每个主题以自身 ID 作为显示名，来源固定 BuiltIn、无
/// base/sourcePath，并保存对应 apply 函数。重复调用通过 registry.contains
/// 跳过已有项，供 reload/apply 安全复用。
///
/// @warning 初始化/插件重载低频路径：会分配主题对象，禁止每帧调用。
void VKContext::registerBuiltInThemes()
{
    // 局部 helper 集中构造内置主题元数据和失败日志。
    auto registerTheme = [this](const char*               id,
                                ImGuiTheme::ApplyFunction applyFunction) {
        // 幂等检查保留首次注册顺序，不重复创建 owning 对象。
        if ( m_themeRegistry->contains(id) ) return;
        // registry 再验证来源、空 base、ID 字符集和唯一性。
        if ( !m_themeRegistry->registerBuiltInTheme(
                 std::make_unique<ImGuiTheme>(id,
                                              id,
                                              ImGuiThemeOrigin::BuiltIn,
                                              std::string(),
                                              std::filesystem::path(),
                                              std::move(applyFunction))) ) {
            XERROR("Failed to register built-in ImGui theme: {}", id);
        }
    };

    // 注册顺序同时决定设置界面的稳定展示顺序。
    registerTheme("DeepDark", applyDeepDarkStyle);
    registerTheme("Dark", applyDarkStyle);
    registerTheme("Light", applyLightStyle);
    registerTheme("IVM", applyIvmStyle);
    registerTheme("Classic", applyClassicStyle);
    registerTheme("Microsoft", applyMicrosoftStyle);
    registerTheme("Darcula", applyDarculaStyle);
    registerTheme("Photoshop", applyPhotoshopStyle);
    registerTheme("Unreal", applyUnrealStyle);
    registerTheme("Gold", applyGoldStyle);
    registerTheme("RoundedVisualStudio", applyRoundedVisualStudioStyle);
    registerTheme("SonicRiders", applySonicRidersStyle);
    registerTheme("DarkRuda", applyDarkRudaStyle);
    registerTheme("SoftCherry", applySoftCherryStyle);
    registerTheme("Enemymouse", applyEnemymouseStyle);
    registerTheme("DiscordDark", applyDiscordDarkStyle);
    registerTheme("Comfy", applyComfyStyle);
    registerTheme("PurpleComfy", applyPurpleComfyStyle);
    registerTheme("FutureDark", applyFutureDarkStyle);
    registerTheme("CleanDark", applyCleanDarkStyle);
    registerTheme("Moonlight", applyMoonlightStyle);
    registerTheme("Cecilia", applyCeciliaStyle);
    registerTheme("ComfortableLight", applyComfortableLightStyle);
    registerTheme("HazyDark", applyHazyDarkStyle);
    registerTheme("Everforest", applyEverforestStyle);
    registerTheme("Windark", applyWindarkStyle);
    registerTheme("Rest", applyRestStyle);
    registerTheme("ComfortableDarkCyan", applyComfortableDarkCyanStyle);
    registerTheme("KazamCherry", applyKazamCherryStyle);
}

/// @brief 重载主题插件并立即按当前设置重应用主题。
/// @return 插件目录扫描、加载和错误统计。
/// @warning 显式资源重载路径：会访问文件系统、执行
/// Lua、分配主题并修改全局样式。
ThemePluginReloadResult VKContext::reloadPlugins()
{
    // 内置基底必须在解析插件主题继承关系前存在。
    registerBuiltInThemes();
    const auto& settings = Config::AppConfig::instance().getEditorSettings();
    // disabledPluginIds 决定本轮完全跳过执行的插件文件。
    ThemePluginReloadResult result = m_themeRegistry->reloadThemePlugins(
        Config::AppPaths::themePluginsRootPath(), settings.disabledPluginIds);
    // 清除旧插件后当前主题 ID 可能失效，统一走 applyTheme 的回退策略。
    applyTheme();
    return result;
}

/// @brief 修改单个已发现主题插件的启用状态、保存配置并重载全部插件。
///
/// enabled=true 删除所有匹配禁用 ID；false 只追加一次并排序，保持配置确定性。
/// 即使配置保存失败也执行 reload，使当前进程状态反映用户刚才的选择。
///
/// @param pluginId 最近一次扫描中存在的稳定插件 ID。
/// @param enabled 目标启用状态。
/// @return AppConfig 持久化是否成功；未知插件直接返回 false。
/// @warning 设置操作低频路径：修改配置、写文件并完整重载 Lua 插件。
bool VKContext::setPluginEnabled(std::string_view pluginId, bool enabled)
{
    // 只允许操作已发现插件，防止配置积累任意无效 ID。
    if ( !m_themeRegistry->findPlugin(pluginId) ) return false;

    auto& disabledPluginIds =
        Config::AppConfig::instance().getEditorSettings().disabledPluginIds;
    if ( enabled ) {
        // std::erase 同时清理历史重复项，恢复规范化禁用列表。
        std::erase(disabledPluginIds, pluginId);
    } else if ( std::find(disabledPluginIds.begin(),
                          disabledPluginIds.end(),
                          pluginId) == disabledPluginIds.end() ) {
        // 新禁用项复制 string_view 内容后排序，持久化输出不依赖交互顺序。
        disabledPluginIds.emplace_back(pluginId);
        std::sort(disabledPluginIds.begin(), disabledPluginIds.end());
    }

    // 保存结果独立返回；运行时仍继续 reload 以响应用户操作。
    const bool saved = Config::AppConfig::instance().save();
    reloadPlugins();
    return saved;
}

/// @brief 解析当前显式/Auto 主题、应用注册表样式并叠加用户审美与 DPI 设置。
///
/// Auto 模式把系统深浅色映射为皮肤默认主题，MmmDefault 再映射内置 Cecilia。主题
/// 无效时回退 DeepDark。注册表应用后统一缩放尺寸、恢复用户圆角/间距，最后发布
/// 菜单栏背景对应的 Vulkan clear color。
///
/// @warning 主题切换低频路径：重置全局 ImGuiStyle
/// 并发布事件，不得每帧无条件调用。
void VKContext::applyTheme()
{
    // 主题 registry 在首次调用或插件清理后都必须保有全部内置 fallback。
    registerBuiltInThemes();
    auto&       settings = Config::AppConfig::instance().getEditorSettings();
    std::string appliedThemeId = settings.theme;
    if ( appliedThemeId == Config::UI_THEME_AUTO_ID ) {
        // Auto 使用平台缓存结果，并记录本次已应用值供限频检查比较。
        const SystemTheme systemTheme = getSystemTheme();
        m_appliedSystemTheme          = systemTheme;
        // 未明确 Dark 的状态按 Light 选择，保持启动时可预测后备。
        const Config::SkinThemeAppearance appearance =
            systemTheme == SystemTheme::Dark
                ? Config::SkinThemeAppearance::Dark
                : Config::SkinThemeAppearance::Light;
        // 皮肤可为深浅外观分别声明主题 ID。
        const std::string& skinTheme =
            Config::SkinManager::instance().getDefaultTheme(appearance);
        // 皮肤抽象默认名不在 ImGui registry 中，对应项目内置 Cecilia。
        appliedThemeId =
            skinTheme == "MmmDefault" ? std::string("Cecilia") : skinTheme;
    } else {
        // 显式选择时清除系统主题比较状态。
        m_appliedSystemTheme = SystemTheme::Unknown;
    }

    // registry 会从全新 ImGuiStyle 应用内置或“内置基底 + 插件 patch”。
    if ( !m_themeRegistry->applyTheme(appliedThemeId, ImGui::GetStyle()) ) {
        XWARN("Unknown or invalid ImGui theme '{}'; falling back to DeepDark",
              appliedThemeId);
        // DeepDark 由上方幂等注册保证存在，作为最终视觉后备。
        m_themeRegistry->applyTheme("DeepDark", ImGui::GetStyle());
    }

    // registry 每次从默认 style 开始，ScaleAllSizes 不会在多次切换中累积。
    ImGuiStyle& style = ImGui::GetStyle();
    float dpiScale    = Config::AppConfig::instance().getWindowContentScale();
    auto& aes         = settings.aesthetics;

    // 用户 UI 倍率应用于所有主题定义的尺寸，字体主倍率单独恢复。
    style.ScaleAllSizes(settings.uiScaleMultiplier);
    style.FontScaleMain = m_fontAtlasScaleMain;

    // 审美设置拥有最终优先级，按窗口内容 DPI 取整以保持像素边界稳定。
    style.WindowRounding = std::floor(aes.windowRounding * dpiScale);
    style.ChildRounding  = style.WindowRounding;
    style.FrameRounding  = std::floor(aes.frameRounding * dpiScale);
    style.PopupRounding  = style.WindowRounding;
    style.TabRounding    = style.FrameRounding;
    // 项目间距约束同级控件节奏，也是多数自动布局的基础步长。
    style.ItemSpacing = { std::floor(aes.itemSpacing * dpiScale),
                          std::floor(aes.itemSpacing * dpiScale) };
    // 窗口内边距同时决定内容密度和停靠面板的呼吸空间。
    style.WindowPadding = { std::floor(aes.windowPadding * dpiScale),
                            std::floor(aes.windowPadding * dpiScale) };

    // Vulkan clear color 跟随最终 MenuBarBg，而不是未缩放/未覆盖的主题中间值。
    ImVec4 barBg = ImGui::GetStyle().Colors[ImGuiCol_MenuBarBg];
    Event::ClearColorUpdateEvent clearEvt;
    clearEvt.clear_color_value = { barBg.x, barBg.y, barBg.z, barBg.w };
    Event::EventBus::instance().publish(clearEvt);
}

static void applyDeepDarkStyle(ImGuiStyle& style)
{
    // AdobeInspired 样式，来源为 ImThemes 的 nexacopic 配色。

    // 全局透明度会乘到整套颜色上，主题在此保持最终合成基准。
    style.Alpha = 1.0f;
    // 禁用态通过统一衰减与可交互控件拉开层级，无需逐项改写颜色。
    style.DisabledAlpha = 0.6f;
    // 窗口内边距同时决定内容密度和停靠面板的呼吸空间。
    style.WindowPadding            = ImVec2(8.0f, 8.0f);
    style.WindowRounding           = 4.0f;
    style.WindowBorderSize         = 1.0f;
    style.WindowMinSize            = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign         = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.ChildRounding            = 4.0f;
    style.ChildBorderSize          = 1.0f;
    style.PopupRounding            = 4.0f;
    style.PopupBorderSize          = 1.0f;
    // 框体内边距决定输入框和按钮的文字基线及垂直触达面积。
    style.FramePadding    = ImVec2(4.0f, 3.0f);
    style.FrameRounding   = 4.0f;
    style.FrameBorderSize = 1.0f;
    // 项目间距约束同级控件节奏，也是多数自动布局的基础步长。
    style.ItemSpacing      = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
    // 单元格留白保证表格密集展示时文字和分隔线仍有安全距离。
    style.CellPadding       = ImVec2(4.0f, 2.0f);
    style.IndentSpacing     = 21.0f;
    style.ColumnsMinSpacing = 6.0f;
    // 滚动条宽度在可点击性与内容占用之间维持主题取舍。
    style.ScrollbarSize     = 14.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabMinSize       = 10.0f;
    style.GrabRounding      = 20.0f;
    style.TabRounding       = 4.0f;
    style.TabBorderSize     = 1.0f;
    // 选中标签的关闭按钮阈值控制窄页签中操作入口的保留策略。
    style.TabCloseButtonMinWidthSelected = 0.0f;
    // 未选中标签单独设阈值，避免拥挤时关闭图标喧宾夺主。
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    // 正文色承担主要信息层，必须与窗口和框体背景保持足够对比。
    style.Colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // 弱化文字用于不可用或次要信息，但仍需在主背景上可辨识。
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4(0.49803922f, 0.49803922f, 0.49803922f, 1.0f);
    // 窗口底色奠定整套主题明度，后续容器和控件颜色均以此为参照。
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.11372549f, 0.11372549f, 0.11372549f, 1.0f);
    // 子窗口背景可选择透明继承，避免嵌套区域产生多余色块。
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 弹窗背景需要遮住下层内容，同时以透明度保留空间关系。
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.078431375f, 0.078431375f, 0.078431375f, 0.94f);
    // 边框色用于低成本描绘控件和容器边界，透明度决定强调强度。
    style.Colors[ImGuiCol_Border] = ImVec4(1.0f, 1.0f, 1.0f, 0.16309011f);
    // 边框阴影独立配置，以便主题明确选择平面或浮起效果。
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 框体常态是输入和选择控件的静止基面，也是交互渐变的起点。
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.08627451f, 0.08627451f, 0.08627451f, 1.0f);
    // 框体悬浮色只提示可交互性，应比激活态更克制。
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.15294118f, 0.15294118f, 0.15294118f, 1.0f);
    // 框体激活色确认当前操作焦点，并与悬浮态形成连续反馈。
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.1882353f, 0.1882353f, 0.1882353f, 1.0f);
    // 非活动标题栏降低存在感，让当前编辑窗口更容易被识别。
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.11372549f, 0.11372549f, 0.11372549f, 1.0f);
    // 活动标题栏提高层级，承担当前窗口焦点的主要视觉提示。
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.105882354f, 0.105882354f, 0.105882354f, 1.0f);
    // 折叠标题仍保留窗口身份，但用透明度减少对内容的干扰。
    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.0f, 0.0f, 0.0f, 0.51f);
    // 菜单栏底色连接窗口标题与内容区，避免形成突兀的横向色带。
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.11372549f, 0.11372549f, 0.11372549f, 1.0f);
    // 滚动轨道应融入容器背景，使抓手成为滚动位置的主要提示。
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.019607844f, 0.019607844f, 0.019607844f, 0.53f);
    // 滚动抓手常态需持续可见，又不能盖过正文和主要操作。
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.30980393f, 0.30980393f, 0.30980393f, 1.0f);
    // 滚动抓手悬浮态提高对比，提示鼠标已经进入可拖动区域。
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.40784314f, 0.40784314f, 0.40784314f, 1.0f);
    // 滚动抓手激活态确认持续拖动，颜色变化需强于悬浮态。
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.50980395f, 0.50980395f, 0.50980395f, 1.0f);
    // 勾选标记承载布尔结果，应从框体背景中清晰跳出。
    style.Colors[ImGuiCol_CheckMark] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // 滑块抓手常态标记当前值位置，并与轨道保持明度区分。
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.8784314f, 0.8784314f, 0.8784314f, 1.0f);
    // 滑块活动抓手强调正在调整的数值，释放后回落至常态。
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.98039216f, 0.98039216f, 0.98039216f, 1.0f);
    // 按钮常态建立操作面的基础层级，为悬浮和按下反馈预留变化空间。
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.14901961f, 0.14901961f, 0.14901961f, 1.0f);
    // 按钮悬浮态只表达指向，不应与已按下状态混淆。
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.24705882f, 0.24705882f, 0.24705882f, 1.0f);
    // 按钮激活态提供按下确认，需要成为三态中最明确的反馈。
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.32941177f, 0.32941177f, 0.32941177f, 1.0f);
    // Header 常态同时服务树节点、列表选项和折叠区标题。
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.9764706f, 0.9764706f, 0.9764706f, 0.30980393f);
    // Header 悬浮态覆盖较大行区域，应控制亮度避免产生闪烁感。
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.9764706f, 0.9764706f, 0.9764706f, 0.8f);
    // Header 激活态标识当前选择或按压结果，与按钮反馈保持一致。
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.9764706f, 0.9764706f, 0.9764706f, 1.0f);
    // 分隔线以低对比划分区域，不与可拖动边缘争夺注意力。
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.42745098f, 0.42745098f, 0.49803922f, 0.5f);
    // 可交互分隔线在悬浮时增强，提示其支持调整布局。
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.7490196f, 0.7490196f, 0.7490196f, 0.78039217f);
    // 分隔线拖动期间使用最强状态，持续反馈布局调整操作。
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.7490196f, 0.7490196f, 0.7490196f, 1.0f);
    // 缩放抓手常态保持含蓄，避免遮挡窗口角落内容。
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.9764706f, 0.9764706f, 0.9764706f, 0.2f);
    // 缩放抓手悬浮态显露可调整方向，强化边角命中提示。
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.9372549f, 0.9372549f, 0.9372549f, 0.67058825f);
    // 缩放抓手激活态维持拖动反馈，直到尺寸调整结束。
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.9764706f, 0.9764706f, 0.9764706f, 0.9490196f);
    // 普通标签以中等层级承载页面入口，不抢占当前标签的焦点。
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.22352941f, 0.22352941f, 0.22352941f, 0.8627451f);
    // 标签悬浮色提前提示可切换目标，并需兼容已选中标签的叠加状态。
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.32156864f, 0.32156864f, 0.32156864f, 0.8f);
    // 活动标签与内容面板建立连续关系，明确当前可见页面。
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.27450982f, 0.27450982f, 0.27450982f, 1.0f);
    // 失焦停靠节点的普通标签进一步降级，突出当前活动窗口组。
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.14509805f, 0.14509805f, 0.14509805f, 0.972549f);
    // 失焦组仍需标识其已选页面，因此保留受抑制的活动态。
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.42352942f, 0.42352942f, 0.42352942f, 1.0f);
    // 折线图常态颜色优先保证细线在背景上的连续可读性。
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.60784316f, 0.60784316f, 0.60784316f, 1.0f);
    // 折线悬浮态采用明显强调色，便于定位密集曲线中的目标。
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(1.0f, 0.42745098f, 0.34901962f, 1.0f);
    // 柱状图使用独立数据色，避免与按钮等交互强调色混淆。
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.8980392f, 0.69803923f, 0.0f, 1.0f);
    // 柱状图悬浮色强化当前样本，但仍需保留原数据系列归属。
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(1.0f, 0.6f, 0.0f, 1.0f);
    // 表头底色固定列语义区域，与数据行建立稳定的纵向起点。
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.1882353f, 0.1882353f, 0.2f, 1.0f);
    // 强表格边框用于外沿和表头分界，承担主要结构线。
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.30980393f, 0.30980393f, 0.34901962f, 1.0f);
    // 弱表格边框只分隔普通单元格，降低密集网格的视觉噪声。
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(0.22745098f, 0.22745098f, 0.24705882f, 1.0f);
    // 基础行背景允许透明继承，使表格自然嵌入所在面板。
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 交替行仅施加轻微差异，帮助横向追踪而不破坏整体底色。
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    // 文本选区背景必须兼顾选中辨识度与前景文字可读性。
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.35f);
    // 拖放目标使用高辨识强调色，明确释放操作将作用的位置。
    style.Colors[ImGuiCol_DragDropTarget] = ImVec4(1.0f, 1.0f, 0.0f, 0.9f);
    // 键盘和手柄导航高亮独立于鼠标悬浮态，避免输入来源含混。
    style.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    // 窗口导航高亮标识待切换窗口，并覆盖多种面板底色。
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    // 窗口切换期间的遮罩压低非目标内容，但仍保留场景上下文。
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    // 模态遮罩明确阻断下层交互，透明度同时维持来源窗口可见。
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.35f);
}

static void applyDarkStyle(ImGuiStyle& style)
{
    // Dark 样式，来源为 ImThemes 的 dougbinks 配色。

    // 全局透明度会乘到整套颜色上，主题在此保持最终合成基准。
    style.Alpha = 1.0f;
    // 禁用态通过统一衰减与可交互控件拉开层级，无需逐项改写颜色。
    style.DisabledAlpha = 0.6f;
    // 窗口内边距同时决定内容密度和停靠面板的呼吸空间。
    style.WindowPadding            = ImVec2(8.0f, 8.0f);
    style.WindowRounding           = 0.0f;
    style.WindowBorderSize         = 1.0f;
    style.WindowMinSize            = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign         = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_Left;
    style.ChildRounding            = 0.0f;
    style.ChildBorderSize          = 1.0f;
    style.PopupRounding            = 0.0f;
    style.PopupBorderSize          = 1.0f;
    // 框体内边距决定输入框和按钮的文字基线及垂直触达面积。
    style.FramePadding    = ImVec2(4.0f, 3.0f);
    style.FrameRounding   = 0.0f;
    style.FrameBorderSize = 0.0f;
    // 项目间距约束同级控件节奏，也是多数自动布局的基础步长。
    style.ItemSpacing      = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
    // 单元格留白保证表格密集展示时文字和分隔线仍有安全距离。
    style.CellPadding       = ImVec2(4.0f, 2.0f);
    style.IndentSpacing     = 21.0f;
    style.ColumnsMinSpacing = 6.0f;
    // 滚动条宽度在可点击性与内容占用之间维持主题取舍。
    style.ScrollbarSize     = 14.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabMinSize       = 10.0f;
    style.GrabRounding      = 0.0f;
    style.TabRounding       = 4.0f;
    style.TabBorderSize     = 0.0f;
    // 选中标签的关闭按钮阈值控制窄页签中操作入口的保留策略。
    style.TabCloseButtonMinWidthSelected = 0.0f;
    // 未选中标签单独设阈值，避免拥挤时关闭图标喧宾夺主。
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    // 正文色承担主要信息层，必须与窗口和框体背景保持足够对比。
    style.Colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // 弱化文字用于不可用或次要信息，但仍需在主背景上可辨识。
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4(0.49803922f, 0.49803922f, 0.49803922f, 1.0f);
    // 窗口底色奠定整套主题明度，后续容器和控件颜色均以此为参照。
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.05882353f, 0.05882353f, 0.05882353f, 0.94f);
    // 子窗口背景可选择透明继承，避免嵌套区域产生多余色块。
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 弹窗背景需要遮住下层内容，同时以透明度保留空间关系。
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.078431375f, 0.078431375f, 0.078431375f, 0.94f);
    // 边框色用于低成本描绘控件和容器边界，透明度决定强调强度。
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.42745098f, 0.42745098f, 0.49803922f, 0.5f);
    // 边框阴影独立配置，以便主题明确选择平面或浮起效果。
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 框体常态是输入和选择控件的静止基面，也是交互渐变的起点。
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.15686275f, 0.28627452f, 0.47843137f, 0.54f);
    // 框体悬浮色只提示可交互性，应比激活态更克制。
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.4f);
    // 框体激活色确认当前操作焦点，并与悬浮态形成连续反馈。
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.67f);
    // 非活动标题栏降低存在感，让当前编辑窗口更容易被识别。
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.039215688f, 0.039215688f, 0.039215688f, 1.0f);
    // 活动标题栏提高层级，承担当前窗口焦点的主要视觉提示。
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.15686275f, 0.28627452f, 0.47843137f, 1.0f);
    // 折叠标题仍保留窗口身份，但用透明度减少对内容的干扰。
    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.0f, 0.0f, 0.0f, 0.51f);
    // 菜单栏底色连接窗口标题与内容区，避免形成突兀的横向色带。
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.13725491f, 0.13725491f, 0.13725491f, 1.0f);
    // 滚动轨道应融入容器背景，使抓手成为滚动位置的主要提示。
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.019607844f, 0.019607844f, 0.019607844f, 0.53f);
    // 滚动抓手常态需持续可见，又不能盖过正文和主要操作。
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.30980393f, 0.30980393f, 0.30980393f, 1.0f);
    // 滚动抓手悬浮态提高对比，提示鼠标已经进入可拖动区域。
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.40784314f, 0.40784314f, 0.40784314f, 1.0f);
    // 滚动抓手激活态确认持续拖动，颜色变化需强于悬浮态。
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.50980395f, 0.50980395f, 0.50980395f, 1.0f);
    // 勾选标记承载布尔结果，应从框体背景中清晰跳出。
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    // 滑块抓手常态标记当前值位置，并与轨道保持明度区分。
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.23921569f, 0.5176471f, 0.8784314f, 1.0f);
    // 滑块活动抓手强调正在调整的数值，释放后回落至常态。
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    // 按钮常态建立操作面的基础层级，为悬浮和按下反馈预留变化空间。
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.4f);
    // 按钮悬浮态只表达指向，不应与已按下状态混淆。
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    // 按钮激活态提供按下确认，需要成为三态中最明确的反馈。
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.05882353f, 0.5294118f, 0.9764706f, 1.0f);
    // Header 常态同时服务树节点、列表选项和折叠区标题。
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.31f);
    // Header 悬浮态覆盖较大行区域，应控制亮度避免产生闪烁感。
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.8f);
    // Header 激活态标识当前选择或按压结果，与按钮反馈保持一致。
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    // 分隔线以低对比划分区域，不与可拖动边缘争夺注意力。
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.42745098f, 0.42745098f, 0.49803922f, 0.5f);
    // 可交互分隔线在悬浮时增强，提示其支持调整布局。
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.09803922f, 0.4f, 0.7490196f, 0.78f);
    // 分隔线拖动期间使用最强状态，持续反馈布局调整操作。
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.09803922f, 0.4f, 0.7490196f, 1.0f);
    // 缩放抓手常态保持含蓄，避免遮挡窗口角落内容。
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.2f);
    // 缩放抓手悬浮态显露可调整方向，强化边角命中提示。
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.67f);
    // 缩放抓手激活态维持拖动反馈，直到尺寸调整结束。
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.95f);
    // 普通标签以中等层级承载页面入口，不抢占当前标签的焦点。
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.1764706f, 0.34901962f, 0.5764706f, 0.862f);
    // 标签悬浮色提前提示可切换目标，并需兼容已选中标签的叠加状态。
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.8f);
    // 活动标签与内容面板建立连续关系，明确当前可见页面。
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.19607843f, 0.40784314f, 0.6784314f, 1.0f);
    // 失焦停靠节点的普通标签进一步降级，突出当前活动窗口组。
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.06666667f, 0.101960786f, 0.14509805f, 0.9724f);
    // 失焦组仍需标识其已选页面，因此保留受抑制的活动态。
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.13333334f, 0.25882354f, 0.42352942f, 1.0f);
    // 折线图常态颜色优先保证细线在背景上的连续可读性。
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.60784316f, 0.60784316f, 0.60784316f, 1.0f);
    // 折线悬浮态采用明显强调色，便于定位密集曲线中的目标。
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(1.0f, 0.42745098f, 0.34901962f, 1.0f);
    // 柱状图使用独立数据色，避免与按钮等交互强调色混淆。
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.8980392f, 0.69803923f, 0.0f, 1.0f);
    // 柱状图悬浮色强化当前样本，但仍需保留原数据系列归属。
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(1.0f, 0.6f, 0.0f, 1.0f);
    // 表头底色固定列语义区域，与数据行建立稳定的纵向起点。
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.1882353f, 0.1882353f, 0.2f, 1.0f);
    // 强表格边框用于外沿和表头分界，承担主要结构线。
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.30980393f, 0.30980393f, 0.34901962f, 1.0f);
    // 弱表格边框只分隔普通单元格，降低密集网格的视觉噪声。
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(0.22745098f, 0.22745098f, 0.24705882f, 1.0f);
    // 基础行背景允许透明继承，使表格自然嵌入所在面板。
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 交替行仅施加轻微差异，帮助横向追踪而不破坏整体底色。
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    // 文本选区背景必须兼顾选中辨识度与前景文字可读性。
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.35f);
    // 拖放目标使用高辨识强调色，明确释放操作将作用的位置。
    style.Colors[ImGuiCol_DragDropTarget] = ImVec4(1.0f, 1.0f, 0.0f, 0.9f);
    // 键盘和手柄导航高亮独立于鼠标悬浮态，避免输入来源含混。
    style.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    // 窗口导航高亮标识待切换窗口，并覆盖多种面板底色。
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    // 窗口切换期间的遮罩压低非目标内容，但仍保留场景上下文。
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    // 模态遮罩明确阻断下层交互，透明度同时维持来源窗口可见。
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.35f);
}

static void applyLightStyle(ImGuiStyle& style)
{
    // Light 样式，来源为 ImThemes 的 dougbinks 配色。

    // 全局透明度会乘到整套颜色上，主题在此保持最终合成基准。
    style.Alpha = 1.0f;
    // 禁用态通过统一衰减与可交互控件拉开层级，无需逐项改写颜色。
    style.DisabledAlpha = 0.6f;
    // 窗口内边距同时决定内容密度和停靠面板的呼吸空间。
    style.WindowPadding            = ImVec2(8.0f, 8.0f);
    style.WindowRounding           = 0.0f;
    style.WindowBorderSize         = 1.0f;
    style.WindowMinSize            = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign         = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_Left;
    style.ChildRounding            = 0.0f;
    style.ChildBorderSize          = 1.0f;
    style.PopupRounding            = 0.0f;
    style.PopupBorderSize          = 1.0f;
    // 框体内边距决定输入框和按钮的文字基线及垂直触达面积。
    style.FramePadding    = ImVec2(4.0f, 3.0f);
    style.FrameRounding   = 0.0f;
    style.FrameBorderSize = 0.0f;
    // 项目间距约束同级控件节奏，也是多数自动布局的基础步长。
    style.ItemSpacing      = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
    // 单元格留白保证表格密集展示时文字和分隔线仍有安全距离。
    style.CellPadding       = ImVec2(4.0f, 2.0f);
    style.IndentSpacing     = 21.0f;
    style.ColumnsMinSpacing = 6.0f;
    // 滚动条宽度在可点击性与内容占用之间维持主题取舍。
    style.ScrollbarSize     = 14.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabMinSize       = 10.0f;
    style.GrabRounding      = 0.0f;
    style.TabRounding       = 4.0f;
    style.TabBorderSize     = 0.0f;
    // 选中标签的关闭按钮阈值控制窄页签中操作入口的保留策略。
    style.TabCloseButtonMinWidthSelected = 0.0f;
    // 未选中标签单独设阈值，避免拥挤时关闭图标喧宾夺主。
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    // 正文色承担主要信息层，必须与窗口和框体背景保持足够对比。
    style.Colors[ImGuiCol_Text] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    // 弱化文字用于不可用或次要信息，但仍需在主背景上可辨识。
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
    // 窗口底色奠定整套主题明度，后续容器和控件颜色均以此为参照。
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.9372549f, 0.9372549f, 0.9372549f, 1.0f);
    // 子窗口背景可选择透明继承，避免嵌套区域产生多余色块。
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 弹窗背景需要遮住下层内容，同时以透明度保留空间关系。
    style.Colors[ImGuiCol_PopupBg] = ImVec4(1.0f, 1.0f, 1.0f, 0.98f);
    // 边框色用于低成本描绘控件和容器边界，透明度决定强调强度。
    style.Colors[ImGuiCol_Border] = ImVec4(0.0f, 0.0f, 0.0f, 0.3f);
    // 边框阴影独立配置，以便主题明确选择平面或浮起效果。
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 框体常态是输入和选择控件的静止基面，也是交互渐变的起点。
    style.Colors[ImGuiCol_FrameBg] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // 框体悬浮色只提示可交互性，应比激活态更克制。
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.4f);
    // 框体激活色确认当前操作焦点，并与悬浮态形成连续反馈。
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.67f);
    // 非活动标题栏降低存在感，让当前编辑窗口更容易被识别。
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.95686275f, 0.95686275f, 0.95686275f, 1.0f);
    // 活动标题栏提高层级，承担当前窗口焦点的主要视觉提示。
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.81960785f, 0.81960785f, 0.81960785f, 1.0f);
    // 折叠标题仍保留窗口身份，但用透明度减少对内容的干扰。
    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(1.0f, 1.0f, 1.0f, 0.51f);
    // 菜单栏底色连接窗口标题与内容区，避免形成突兀的横向色带。
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.85882354f, 0.85882354f, 0.85882354f, 1.0f);
    // 滚动轨道应融入容器背景，使抓手成为滚动位置的主要提示。
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.9764706f, 0.9764706f, 0.9764706f, 0.53f);
    // 滚动抓手常态需持续可见，又不能盖过正文和主要操作。
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.6862745f, 0.6862745f, 0.6862745f, 0.8f);
    // 滚动抓手悬浮态提高对比，提示鼠标已经进入可拖动区域。
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.4862745f, 0.4862745f, 0.4862745f, 0.8f);
    // 滚动抓手激活态确认持续拖动，颜色变化需强于悬浮态。
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.4862745f, 0.4862745f, 0.4862745f, 1.0f);
    // 勾选标记承载布尔结果，应从框体背景中清晰跳出。
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    // 滑块抓手常态标记当前值位置，并与轨道保持明度区分。
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.78f);
    // 滑块活动抓手强调正在调整的数值，释放后回落至常态。
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.45882353f, 0.5372549f, 0.8f, 0.6f);
    // 按钮常态建立操作面的基础层级，为悬浮和按下反馈预留变化空间。
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.4f);
    // 按钮悬浮态只表达指向，不应与已按下状态混淆。
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    // 按钮激活态提供按下确认，需要成为三态中最明确的反馈。
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.05882353f, 0.5294118f, 0.9764706f, 1.0f);
    // Header 常态同时服务树节点、列表选项和折叠区标题。
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.31f);
    // Header 悬浮态覆盖较大行区域，应控制亮度避免产生闪烁感。
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.8f);
    // Header 激活态标识当前选择或按压结果，与按钮反馈保持一致。
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    // 分隔线以低对比划分区域，不与可拖动边缘争夺注意力。
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.3882353f, 0.3882353f, 0.3882353f, 0.62f);
    // 可交互分隔线在悬浮时增强，提示其支持调整布局。
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.13725491f, 0.4392157f, 0.8f, 0.78f);
    // 分隔线拖动期间使用最强状态，持续反馈布局调整操作。
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.13725491f, 0.4392157f, 0.8f, 1.0f);
    // 缩放抓手常态保持含蓄，避免遮挡窗口角落内容。
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.34901962f, 0.34901962f, 0.34901962f, 0.17f);
    // 缩放抓手悬浮态显露可调整方向，强化边角命中提示。
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.67f);
    // 缩放抓手激活态维持拖动反馈，直到尺寸调整结束。
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.95f);
    // 普通标签以中等层级承载页面入口，不抢占当前标签的焦点。
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.7607843f, 0.79607844f, 0.8352941f, 0.931f);
    // 标签悬浮色提前提示可切换目标，并需兼容已选中标签的叠加状态。
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.8f);
    // 活动标签与内容面板建立连续关系，明确当前可见页面。
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.5921569f, 0.7254902f, 0.88235295f, 1.0f);
    // 失焦停靠节点的普通标签进一步降级，突出当前活动窗口组。
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.91764706f, 0.9254902f, 0.93333334f, 0.9862f);
    // 失焦组仍需标识其已选页面，因此保留受抑制的活动态。
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.7411765f, 0.81960785f, 0.9137255f, 1.0f);
    // 折线图常态颜色优先保证细线在背景上的连续可读性。
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.3882353f, 0.3882353f, 0.3882353f, 1.0f);
    // 折线悬浮态采用明显强调色，便于定位密集曲线中的目标。
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(1.0f, 0.42745098f, 0.34901962f, 1.0f);
    // 柱状图使用独立数据色，避免与按钮等交互强调色混淆。
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.8980392f, 0.69803923f, 0.0f, 1.0f);
    // 柱状图悬浮色强化当前样本，但仍需保留原数据系列归属。
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(1.0f, 0.44705883f, 0.0f, 1.0f);
    // 表头底色固定列语义区域，与数据行建立稳定的纵向起点。
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.7764706f, 0.8666667f, 0.9764706f, 1.0f);
    // 强表格边框用于外沿和表头分界，承担主要结构线。
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.5686275f, 0.5686275f, 0.6392157f, 1.0f);
    // 弱表格边框只分隔普通单元格，降低密集网格的视觉噪声。
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(0.6784314f, 0.6784314f, 0.7372549f, 1.0f);
    // 基础行背景允许透明继承，使表格自然嵌入所在面板。
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 交替行仅施加轻微差异，帮助横向追踪而不破坏整体底色。
    style.Colors[ImGuiCol_TableRowBgAlt] =
        ImVec4(0.29803923f, 0.29803923f, 0.29803923f, 0.09f);
    // 文本选区背景必须兼顾选中辨识度与前景文字可读性。
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.35f);
    // 拖放目标使用高辨识强调色，明确释放操作将作用的位置。
    style.Colors[ImGuiCol_DragDropTarget] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.95f);
    // 键盘和手柄导航高亮独立于鼠标悬浮态，避免输入来源含混。
    style.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.8f);
    // 窗口导航高亮标识待切换窗口，并覆盖多种面板底色。
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(0.69803923f, 0.69803923f, 0.69803923f, 0.7f);
    // 窗口切换期间的遮罩压低非目标内容，但仍保留场景上下文。
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.2f, 0.2f, 0.2f, 0.2f);
    // 模态遮罩明确阻断下层交互，透明度同时维持来源窗口可见。
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.2f, 0.2f, 0.2f, 0.35f);
}

/// @brief 设置 IVM 经典 Windows 工具软件样式。
/// @param style 待覆盖的 ImGui 样式。
static void applyIvmStyle(ImGuiStyle& style)
{
    applyLightStyle(style);

    // 全局透明度会乘到整套颜色上，主题在此保持最终合成基准。
    style.Alpha = 1.0f;
    // 禁用态通过统一衰减与可交互控件拉开层级，无需逐项改写颜色。
    style.DisabledAlpha = 0.55f;
    // 窗口内边距同时决定内容密度和停靠面板的呼吸空间。
    style.WindowPadding            = ImVec2(7.0f, 6.0f);
    style.WindowMinSize            = ImVec2(32.0f, 32.0f);
    style.WindowRounding           = 0.0f;
    style.WindowBorderSize         = 1.0f;
    style.WindowBorderHoverPadding = 3.0f;
    style.WindowTitleAlign         = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_Left;
    style.ChildRounding            = 0.0f;
    style.ChildBorderSize          = 1.0f;
    style.PopupRounding            = 0.0f;
    style.PopupBorderSize          = 1.0f;
    // 框体内边距决定输入框和按钮的文字基线及垂直触达面积。
    style.FramePadding    = ImVec2(6.0f, 3.0f);
    style.FrameRounding   = 0.0f;
    style.FrameBorderSize = 1.0f;
    // 项目间距约束同级控件节奏，也是多数自动布局的基础步长。
    style.ItemSpacing      = ImVec2(7.0f, 5.0f);
    style.ItemInnerSpacing = ImVec2(5.0f, 4.0f);
    // 单元格留白保证表格密集展示时文字和分隔线仍有安全距离。
    style.CellPadding   = ImVec2(5.0f, 3.0f);
    style.IndentSpacing = 18.0f;
    // 滚动条宽度在可点击性与内容占用之间维持主题取舍。
    style.ScrollbarSize           = 16.0f;
    style.ScrollbarRounding       = 0.0f;
    style.ScrollbarPadding        = 1.0f;
    style.GrabMinSize             = 12.0f;
    style.GrabRounding            = 0.0f;
    style.TabRounding             = 0.0f;
    style.TabBorderSize           = 1.0f;
    style.TabBarBorderSize        = 1.0f;
    style.TabBarOverlineSize      = 2.0f;
    style.MenuItemRounding        = 0.0f;
    style.SelectableRounding      = 0.0f;
    style.ButtonTextAlign         = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign     = ImVec2(0.0f, 0.0f);
    style.SeparatorSize           = 1.0f;
    style.SeparatorTextBorderSize = 1.0f;
    style.DockingSeparatorSize    = 1.0f;
    style.AntiAliasedLines        = true;
    style.AntiAliasedLinesUseTex  = true;
    style.AntiAliasedFill         = true;

    // 正文色承担主要信息层，必须与窗口和框体背景保持足够对比。
    style.Colors[ImGuiCol_Text] = ImVec4(0.05f, 0.05f, 0.05f, 1.0f);
    // 弱化文字用于不可用或次要信息，但仍需在主背景上可辨识。
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.43f, 0.43f, 0.43f, 1.0f);
    // 窗口底色奠定整套主题明度，后续容器和控件颜色均以此为参照。
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.941f, 0.941f, 0.941f, 1.0f);
    // 子窗口背景可选择透明继承，避免嵌套区域产生多余色块。
    style.Colors[ImGuiCol_ChildBg] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // 弹窗背景需要遮住下层内容，同时以透明度保留空间关系。
    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.965f, 0.965f, 0.965f, 1.0f);
    // 边框色用于低成本描绘控件和容器边界，透明度决定强调强度。
    style.Colors[ImGuiCol_Border] = ImVec4(0.60f, 0.60f, 0.60f, 1.0f);
    // 边框阴影独立配置，以便主题明确选择平面或浮起效果。
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(1.0f, 1.0f, 1.0f, 0.55f);
    // 框体常态是输入和选择控件的静止基面，也是交互渐变的起点。
    style.Colors[ImGuiCol_FrameBg] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // 框体悬浮色只提示可交互性，应比激活态更克制。
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.82f, 0.97f, 0.95f, 1.0f);
    // 框体激活色确认当前操作焦点，并与悬浮态形成连续反馈。
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.66f, 0.92f, 0.89f, 1.0f);
    // 非活动标题栏降低存在感，让当前编辑窗口更容易被识别。
    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.925f, 0.949f, 0.957f, 1.0f);
    // 活动标题栏提高层级，承担当前窗口焦点的主要视觉提示。
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.925f, 0.949f, 0.957f, 1.0f);
    // 折叠标题仍保留窗口身份，但用透明度减少对内容的干扰。
    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.90f, 0.92f, 0.93f, 1.0f);
    // 菜单栏底色连接窗口标题与内容区，避免形成突兀的横向色带。
    style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.949f, 0.949f, 0.949f, 1.0f);
    // 滚动轨道应融入容器背景，使抓手成为滚动位置的主要提示。
    style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.91f, 0.91f, 0.91f, 1.0f);
    // 滚动抓手常态需持续可见，又不能盖过正文和主要操作。
    style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.72f, 0.72f, 0.72f, 1.0f);
    // 滚动抓手悬浮态提高对比，提示鼠标已经进入可拖动区域。
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.61f, 0.61f, 0.61f, 1.0f);
    // 滚动抓手激活态确认持续拖动，颜色变化需强于悬浮态。
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.50f, 0.50f, 0.50f, 1.0f);
    // 勾选标记承载布尔结果，应从框体背景中清晰跳出。
    style.Colors[ImGuiCol_CheckMark] = ImVec4(0.0f, 0.82f, 0.12f, 1.0f);
    // 滑块抓手常态标记当前值位置，并与轨道保持明度区分。
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.08f, 0.75f, 0.70f, 1.0f);
    // 滑块活动抓手强调正在调整的数值，释放后回落至常态。
    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.0f, 0.88f, 0.16f, 1.0f);
    // 按钮常态建立操作面的基础层级，为悬浮和按下反馈预留变化空间。
    style.Colors[ImGuiCol_Button] = ImVec4(0.91f, 0.91f, 0.91f, 1.0f);
    // 按钮悬浮态只表达指向，不应与已按下状态混淆。
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.82f, 0.97f, 0.95f, 1.0f);
    // 按钮激活态提供按下确认，需要成为三态中最明确的反馈。
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.67f, 0.91f, 0.88f, 1.0f);
    // Header 常态同时服务树节点、列表选项和折叠区标题。
    style.Colors[ImGuiCol_Header] = ImVec4(0.91f, 0.82f, 0.92f, 1.0f);
    // Header 悬浮态覆盖较大行区域，应控制亮度避免产生闪烁感。
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.77f, 0.95f, 0.93f, 1.0f);
    // Header 激活态标识当前选择或按压结果，与按钮反馈保持一致。
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.56f, 0.88f, 0.84f, 1.0f);
    // 分隔线以低对比划分区域，不与可拖动边缘争夺注意力。
    style.Colors[ImGuiCol_Separator] = ImVec4(0.63f, 0.63f, 0.63f, 1.0f);
    // 可交互分隔线在悬浮时增强，提示其支持调整布局。
    style.Colors[ImGuiCol_SeparatorHovered] = ImVec4(0.08f, 0.75f, 0.70f, 1.0f);
    // 分隔线拖动期间使用最强状态，持续反馈布局调整操作。
    style.Colors[ImGuiCol_SeparatorActive] = ImVec4(0.0f, 0.82f, 0.12f, 1.0f);
    // 缩放抓手常态保持含蓄，避免遮挡窗口角落内容。
    style.Colors[ImGuiCol_ResizeGrip] = ImVec4(0.55f, 0.55f, 0.55f, 0.20f);
    // 缩放抓手悬浮态显露可调整方向，强化边角命中提示。
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.08f, 0.75f, 0.70f, 0.65f);
    // 缩放抓手激活态维持拖动反馈，直到尺寸调整结束。
    style.Colors[ImGuiCol_ResizeGripActive] = ImVec4(0.0f, 0.82f, 0.12f, 0.90f);
    style.Colors[ImGuiCol_InputTextCursor]  = ImVec4(0.05f, 0.05f, 0.05f, 1.0f);
    // 标签悬浮色提前提示可切换目标，并需兼容已选中标签的叠加状态。
    style.Colors[ImGuiCol_TabHovered] = ImVec4(0.77f, 0.95f, 0.93f, 1.0f);
    // 普通标签以中等层级承载页面入口，不抢占当前标签的焦点。
    style.Colors[ImGuiCol_Tab]         = ImVec4(0.88f, 0.88f, 0.88f, 1.0f);
    style.Colors[ImGuiCol_TabSelected] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    style.Colors[ImGuiCol_TabSelectedOverline] =
        ImVec4(0.0f, 0.82f, 0.12f, 1.0f);
    style.Colors[ImGuiCol_TabDimmed] = ImVec4(0.86f, 0.86f, 0.86f, 1.0f);
    style.Colors[ImGuiCol_TabDimmedSelected] =
        ImVec4(0.92f, 0.92f, 0.92f, 1.0f);
    style.Colors[ImGuiCol_TabDimmedSelectedOverline] =
        ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
    // 停靠预览色指示潜在落点，透明度允许用户同时判断下层内容。
    style.Colors[ImGuiCol_DockingPreview] = ImVec4(0.08f, 0.75f, 0.70f, 0.55f);
    // 空停靠节点使用独立底色，提示这里尚未承载实际窗口。
    style.Colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.86f, 0.86f, 0.86f, 1.0f);
    // 折线图常态颜色优先保证细线在背景上的连续可读性。
    style.Colors[ImGuiCol_PlotLines] = ImVec4(0.15f, 0.15f, 0.15f, 1.0f);
    // 折线悬浮态采用明显强调色，便于定位密集曲线中的目标。
    style.Colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.0f, 0.10f, 0.10f, 1.0f);
    // 柱状图使用独立数据色，避免与按钮等交互强调色混淆。
    style.Colors[ImGuiCol_PlotHistogram] = ImVec4(0.0f, 0.82f, 0.12f, 1.0f);
    // 柱状图悬浮色强化当前样本，但仍需保留原数据系列归属。
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(0.08f, 0.75f, 0.70f, 1.0f);
    // 表头底色固定列语义区域，与数据行建立稳定的纵向起点。
    style.Colors[ImGuiCol_TableHeaderBg] = ImVec4(0.93f, 0.93f, 0.93f, 1.0f);
    // 强表格边框用于外沿和表头分界，承担主要结构线。
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.58f, 0.58f, 0.58f, 1.0f);
    // 弱表格边框只分隔普通单元格，降低密集网格的视觉噪声。
    style.Colors[ImGuiCol_TableBorderLight] = ImVec4(0.76f, 0.76f, 0.76f, 1.0f);
    // 基础行背景允许透明继承，使表格自然嵌入所在面板。
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // 交替行仅施加轻微差异，帮助横向追踪而不破坏整体底色。
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.90f, 0.96f, 0.96f, 0.55f);
    style.Colors[ImGuiCol_TextLink]      = ImVec4(0.0f, 0.40f, 0.38f, 1.0f);
    // 文本选区背景必须兼顾选中辨识度与前景文字可读性。
    style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.08f, 0.75f, 0.70f, 0.35f);
    style.Colors[ImGuiCol_TreeLines]      = ImVec4(0.62f, 0.62f, 0.62f, 1.0f);
    // 拖放目标使用高辨识强调色，明确释放操作将作用的位置。
    style.Colors[ImGuiCol_DragDropTarget]   = ImVec4(0.0f, 0.82f, 0.12f, 1.0f);
    style.Colors[ImGuiCol_DragDropTargetBg] = ImVec4(0.0f, 0.82f, 0.12f, 0.16f);
    style.Colors[ImGuiCol_UnsavedMarker]    = ImVec4(1.0f, 0.12f, 0.12f, 1.0f);
    style.Colors[ImGuiCol_NavCursor]        = ImVec4(0.0f, 0.82f, 0.12f, 1.0f);
    // 窗口导航高亮标识待切换窗口，并覆盖多种面板底色。
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(0.08f, 0.75f, 0.70f, 0.70f);
    // 窗口切换期间的遮罩压低非目标内容，但仍保留场景上下文。
    style.Colors[ImGuiCol_NavWindowingDimBg] =
        ImVec4(0.80f, 0.80f, 0.80f, 0.30f);
    // 模态遮罩明确阻断下层交互，透明度同时维持来源窗口可见。
    style.Colors[ImGuiCol_ModalWindowDimBg] =
        ImVec4(0.78f, 0.78f, 0.78f, 0.55f);
}

static void applyClassicStyle(ImGuiStyle& style)
{
    // Classic 样式，来源为 ImThemes 的 ocornut 配色。

    // 全局透明度会乘到整套颜色上，主题在此保持最终合成基准。
    style.Alpha = 1.0f;
    // 禁用态通过统一衰减与可交互控件拉开层级，无需逐项改写颜色。
    style.DisabledAlpha = 0.6f;
    // 窗口内边距同时决定内容密度和停靠面板的呼吸空间。
    style.WindowPadding            = ImVec2(8.0f, 8.0f);
    style.WindowRounding           = 0.0f;
    style.WindowBorderSize         = 1.0f;
    style.WindowMinSize            = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign         = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_Left;
    style.ChildRounding            = 0.0f;
    style.ChildBorderSize          = 1.0f;
    style.PopupRounding            = 0.0f;
    style.PopupBorderSize          = 1.0f;
    // 框体内边距决定输入框和按钮的文字基线及垂直触达面积。
    style.FramePadding    = ImVec2(4.0f, 3.0f);
    style.FrameRounding   = 0.0f;
    style.FrameBorderSize = 0.0f;
    // 项目间距约束同级控件节奏，也是多数自动布局的基础步长。
    style.ItemSpacing      = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
    // 单元格留白保证表格密集展示时文字和分隔线仍有安全距离。
    style.CellPadding       = ImVec2(4.0f, 2.0f);
    style.IndentSpacing     = 21.0f;
    style.ColumnsMinSpacing = 6.0f;
    // 滚动条宽度在可点击性与内容占用之间维持主题取舍。
    style.ScrollbarSize     = 14.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabMinSize       = 10.0f;
    style.GrabRounding      = 0.0f;
    style.TabRounding       = 4.0f;
    style.TabBorderSize     = 0.0f;
    // 选中标签的关闭按钮阈值控制窄页签中操作入口的保留策略。
    style.TabCloseButtonMinWidthSelected = 0.0f;
    // 未选中标签单独设阈值，避免拥挤时关闭图标喧宾夺主。
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    // 正文色承担主要信息层，必须与窗口和框体背景保持足够对比。
    style.Colors[ImGuiCol_Text] =
        ImVec4(0.8980392f, 0.8980392f, 0.8980392f, 1.0f);
    // 弱化文字用于不可用或次要信息，但仍需在主背景上可辨识。
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
    // 窗口底色奠定整套主题明度，后续容器和控件颜色均以此为参照。
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.85f);
    // 子窗口背景可选择透明继承，避免嵌套区域产生多余色块。
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 弹窗背景需要遮住下层内容，同时以透明度保留空间关系。
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.10980392f, 0.10980392f, 0.13725491f, 0.92f);
    // 边框色用于低成本描绘控件和容器边界，透明度决定强调强度。
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.49803922f, 0.49803922f, 0.49803922f, 0.5f);
    // 边框阴影独立配置，以便主题明确选择平面或浮起效果。
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 框体常态是输入和选择控件的静止基面，也是交互渐变的起点。
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.42745098f, 0.42745098f, 0.42745098f, 0.39f);
    // 框体悬浮色只提示可交互性，应比激活态更克制。
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.46666667f, 0.46666667f, 0.6862745f, 0.4f);
    // 框体激活色确认当前操作焦点，并与悬浮态形成连续反馈。
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.41960785f, 0.40784314f, 0.6392157f, 0.69f);
    // 非活动标题栏降低存在感，让当前编辑窗口更容易被识别。
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.26666668f, 0.26666668f, 0.5372549f, 0.83f);
    // 活动标题栏提高层级，承担当前窗口焦点的主要视觉提示。
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.31764707f, 0.31764707f, 0.627451f, 0.87f);
    // 折叠标题仍保留窗口身份，但用透明度减少对内容的干扰。
    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.4f, 0.4f, 0.8f, 0.2f);
    // 菜单栏底色连接窗口标题与内容区，避免形成突兀的横向色带。
    style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.4f, 0.4f, 0.54901963f, 0.8f);
    // 滚动轨道应融入容器背景，使抓手成为滚动位置的主要提示。
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.2f, 0.24705882f, 0.29803923f, 0.6f);
    // 滚动抓手常态需持续可见，又不能盖过正文和主要操作。
    style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.4f, 0.4f, 0.8f, 0.3f);
    // 滚动抓手悬浮态提高对比，提示鼠标已经进入可拖动区域。
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.4f, 0.4f, 0.8f, 0.4f);
    // 滚动抓手激活态确认持续拖动，颜色变化需强于悬浮态。
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.40784314f, 0.3882353f, 0.8f, 0.6f);
    // 勾选标记承载布尔结果，应从框体背景中清晰跳出。
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.8980392f, 0.8980392f, 0.8980392f, 0.5f);
    // 滑块抓手常态标记当前值位置，并与轨道保持明度区分。
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(1.0f, 1.0f, 1.0f, 0.3f);
    // 滑块活动抓手强调正在调整的数值，释放后回落至常态。
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.40784314f, 0.3882353f, 0.8f, 0.6f);
    // 按钮常态建立操作面的基础层级，为悬浮和按下反馈预留变化空间。
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.34901962f, 0.4f, 0.60784316f, 0.62f);
    // 按钮悬浮态只表达指向，不应与已按下状态混淆。
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.4f, 0.47843137f, 0.70980394f, 0.79f);
    // 按钮激活态提供按下确认，需要成为三态中最明确的反馈。
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.45882353f, 0.5372549f, 0.8f, 1.0f);
    // Header 常态同时服务树节点、列表选项和折叠区标题。
    style.Colors[ImGuiCol_Header] = ImVec4(0.4f, 0.4f, 0.8980392f, 0.45f);
    // Header 悬浮态覆盖较大行区域，应控制亮度避免产生闪烁感。
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.44705883f, 0.44705883f, 0.8980392f, 0.8f);
    // Header 激活态标识当前选择或按压结果，与按钮反馈保持一致。
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.5294118f, 0.5294118f, 0.8666667f, 0.8f);
    // 分隔线以低对比划分区域，不与可拖动边缘争夺注意力。
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.49803922f, 0.49803922f, 0.49803922f, 0.6f);
    // 可交互分隔线在悬浮时增强，提示其支持调整布局。
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.6f, 0.6f, 0.69803923f, 1.0f);
    // 分隔线拖动期间使用最强状态，持续反馈布局调整操作。
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.69803923f, 0.69803923f, 0.8980392f, 1.0f);
    // 缩放抓手常态保持含蓄，避免遮挡窗口角落内容。
    style.Colors[ImGuiCol_ResizeGrip] = ImVec4(1.0f, 1.0f, 1.0f, 0.1f);
    // 缩放抓手悬浮态显露可调整方向，强化边角命中提示。
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.7764706f, 0.81960785f, 1.0f, 0.6f);
    // 缩放抓手激活态维持拖动反馈，直到尺寸调整结束。
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.7764706f, 0.81960785f, 1.0f, 0.9f);
    // 普通标签以中等层级承载页面入口，不抢占当前标签的焦点。
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.33333334f, 0.33333334f, 0.68235296f, 0.786f);
    // 标签悬浮色提前提示可切换目标，并需兼容已选中标签的叠加状态。
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.44705883f, 0.44705883f, 0.8980392f, 0.8f);
    // 活动标签与内容面板建立连续关系，明确当前可见页面。
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.40392157f, 0.40392157f, 0.7254902f, 0.842f);
    // 失焦停靠节点的普通标签进一步降级，突出当前活动窗口组。
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.28235295f, 0.28235295f, 0.5686275f, 0.8212f);
    // 失焦组仍需标识其已选页面，因此保留受抑制的活动态。
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.34901962f, 0.34901962f, 0.6509804f, 0.8372f);
    // 折线图常态颜色优先保证细线在背景上的连续可读性。
    style.Colors[ImGuiCol_PlotLines] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // 折线悬浮态采用明显强调色，便于定位密集曲线中的目标。
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(0.8980392f, 0.69803923f, 0.0f, 1.0f);
    // 柱状图使用独立数据色，避免与按钮等交互强调色混淆。
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.8980392f, 0.69803923f, 0.0f, 1.0f);
    // 柱状图悬浮色强化当前样本，但仍需保留原数据系列归属。
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(1.0f, 0.6f, 0.0f, 1.0f);
    // 表头底色固定列语义区域，与数据行建立稳定的纵向起点。
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.26666668f, 0.26666668f, 0.3764706f, 1.0f);
    // 强表格边框用于外沿和表头分界，承担主要结构线。
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.30980393f, 0.30980393f, 0.44705883f, 1.0f);
    // 弱表格边框只分隔普通单元格，降低密集网格的视觉噪声。
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(0.25882354f, 0.25882354f, 0.2784314f, 1.0f);
    // 基础行背景允许透明继承，使表格自然嵌入所在面板。
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 交替行仅施加轻微差异，帮助横向追踪而不破坏整体底色。
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.07f);
    // 文本选区背景必须兼顾选中辨识度与前景文字可读性。
    style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.0f, 0.0f, 1.0f, 0.35f);
    // 拖放目标使用高辨识强调色，明确释放操作将作用的位置。
    style.Colors[ImGuiCol_DragDropTarget] = ImVec4(1.0f, 1.0f, 0.0f, 0.9f);
    // 键盘和手柄导航高亮独立于鼠标悬浮态，避免输入来源含混。
    style.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.44705883f, 0.44705883f, 0.8980392f, 0.8f);
    // 窗口导航高亮标识待切换窗口，并覆盖多种面板底色。
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    // 窗口切换期间的遮罩压低非目标内容，但仍保留场景上下文。
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    // 模态遮罩明确阻断下层交互，透明度同时维持来源窗口可见。
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.2f, 0.2f, 0.2f, 0.35f);
}

static void applyMicrosoftStyle(ImGuiStyle& style)
{
    // Microsoft 样式，来源为 ImThemes 的 usernameiwantedwasalreadytaken 配色。

    // 全局透明度会乘到整套颜色上，主题在此保持最终合成基准。
    style.Alpha = 1.0f;
    // 禁用态通过统一衰减与可交互控件拉开层级，无需逐项改写颜色。
    style.DisabledAlpha = 0.6f;
    // 窗口内边距同时决定内容密度和停靠面板的呼吸空间。
    style.WindowPadding            = ImVec2(4.0f, 6.0f);
    style.WindowRounding           = 0.0f;
    style.WindowBorderSize         = 0.0f;
    style.WindowMinSize            = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign         = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_Left;
    style.ChildRounding            = 0.0f;
    style.ChildBorderSize          = 1.0f;
    style.PopupRounding            = 0.0f;
    style.PopupBorderSize          = 1.0f;
    // 框体内边距决定输入框和按钮的文字基线及垂直触达面积。
    style.FramePadding    = ImVec2(8.0f, 6.0f);
    style.FrameRounding   = 0.0f;
    style.FrameBorderSize = 1.0f;
    // 项目间距约束同级控件节奏，也是多数自动布局的基础步长。
    style.ItemSpacing      = ImVec2(8.0f, 6.0f);
    style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    // 单元格留白保证表格密集展示时文字和分隔线仍有安全距离。
    style.CellPadding       = ImVec2(4.0f, 2.0f);
    style.IndentSpacing     = 20.0f;
    style.ColumnsMinSpacing = 6.0f;
    // 滚动条宽度在可点击性与内容占用之间维持主题取舍。
    style.ScrollbarSize     = 20.0f;
    style.ScrollbarRounding = 0.0f;
    style.GrabMinSize       = 5.0f;
    style.GrabRounding      = 0.0f;
    style.TabRounding       = 4.0f;
    style.TabBorderSize     = 0.0f;
    // 选中标签的关闭按钮阈值控制窄页签中操作入口的保留策略。
    style.TabCloseButtonMinWidthSelected = 0.0f;
    // 未选中标签单独设阈值，避免拥挤时关闭图标喧宾夺主。
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    // 正文色承担主要信息层，必须与窗口和框体背景保持足够对比。
    style.Colors[ImGuiCol_Text] =
        ImVec4(0.09803922f, 0.09803922f, 0.09803922f, 1.0f);
    // 弱化文字用于不可用或次要信息，但仍需在主背景上可辨识。
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4(0.49803922f, 0.49803922f, 0.49803922f, 1.0f);
    // 窗口底色奠定整套主题明度，后续容器和控件颜色均以此为参照。
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.9490196f, 0.9490196f, 0.9490196f, 1.0f);
    // 子窗口背景可选择透明继承，避免嵌套区域产生多余色块。
    style.Colors[ImGuiCol_ChildBg] =
        ImVec4(0.9490196f, 0.9490196f, 0.9490196f, 1.0f);
    // 弹窗背景需要遮住下层内容，同时以透明度保留空间关系。
    style.Colors[ImGuiCol_PopupBg] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // 边框色用于低成本描绘控件和容器边界，透明度决定强调强度。
    style.Colors[ImGuiCol_Border] = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
    // 边框阴影独立配置，以便主题明确选择平面或浮起效果。
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 框体常态是输入和选择控件的静止基面，也是交互渐变的起点。
    style.Colors[ImGuiCol_FrameBg] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // 框体悬浮色只提示可交互性，应比激活态更克制。
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.0f, 0.46666667f, 0.8392157f, 0.2f);
    // 框体激活色确认当前操作焦点，并与悬浮态形成连续反馈。
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.0f, 0.46666667f, 0.8392157f, 1.0f);
    // 非活动标题栏降低存在感，让当前编辑窗口更容易被识别。
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.039215688f, 0.039215688f, 0.039215688f, 1.0f);
    // 活动标题栏提高层级，承担当前窗口焦点的主要视觉提示。
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.15686275f, 0.28627452f, 0.47843137f, 1.0f);
    // 折叠标题仍保留窗口身份，但用透明度减少对内容的干扰。
    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.0f, 0.0f, 0.0f, 0.51f);
    // 菜单栏底色连接窗口标题与内容区，避免形成突兀的横向色带。
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.85882354f, 0.85882354f, 0.85882354f, 1.0f);
    // 滚动轨道应融入容器背景，使抓手成为滚动位置的主要提示。
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.85882354f, 0.85882354f, 0.85882354f, 1.0f);
    // 滚动抓手常态需持续可见，又不能盖过正文和主要操作。
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.6862745f, 0.6862745f, 0.6862745f, 1.0f);
    // 滚动抓手悬浮态提高对比，提示鼠标已经进入可拖动区域。
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.0f, 0.0f, 0.0f, 0.2f);
    // 滚动抓手激活态确认持续拖动，颜色变化需强于悬浮态。
    style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.0f, 0.0f, 0.0f, 0.5f);
    // 勾选标记承载布尔结果，应从框体背景中清晰跳出。
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.09803922f, 0.09803922f, 0.09803922f, 1.0f);
    // 滑块抓手常态标记当前值位置，并与轨道保持明度区分。
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.6862745f, 0.6862745f, 0.6862745f, 1.0f);
    // 滑块活动抓手强调正在调整的数值，释放后回落至常态。
    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.0f, 0.0f, 0.0f, 0.5f);
    // 按钮常态建立操作面的基础层级，为悬浮和按下反馈预留变化空间。
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.85882354f, 0.85882354f, 0.85882354f, 1.0f);
    // 按钮悬浮态只表达指向，不应与已按下状态混淆。
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.0f, 0.46666667f, 0.8392157f, 0.2f);
    // 按钮激活态提供按下确认，需要成为三态中最明确的反馈。
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.0f, 0.46666667f, 0.8392157f, 1.0f);
    // Header 常态同时服务树节点、列表选项和折叠区标题。
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.85882354f, 0.85882354f, 0.85882354f, 1.0f);
    // Header 悬浮态覆盖较大行区域，应控制亮度避免产生闪烁感。
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.0f, 0.46666667f, 0.8392157f, 0.2f);
    // Header 激活态标识当前选择或按压结果，与按钮反馈保持一致。
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.0f, 0.46666667f, 0.8392157f, 1.0f);
    // 分隔线以低对比划分区域，不与可拖动边缘争夺注意力。
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.42745098f, 0.42745098f, 0.49803922f, 0.5f);
    // 可交互分隔线在悬浮时增强，提示其支持调整布局。
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.09803922f, 0.4f, 0.7490196f, 0.78f);
    // 分隔线拖动期间使用最强状态，持续反馈布局调整操作。
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.09803922f, 0.4f, 0.7490196f, 1.0f);
    // 缩放抓手常态保持含蓄，避免遮挡窗口角落内容。
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.2f);
    // 缩放抓手悬浮态显露可调整方向，强化边角命中提示。
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.67f);
    // 缩放抓手激活态维持拖动反馈，直到尺寸调整结束。
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.95f);
    // 普通标签以中等层级承载页面入口，不抢占当前标签的焦点。
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.1764706f, 0.34901962f, 0.5764706f, 0.862f);
    // 标签悬浮色提前提示可切换目标，并需兼容已选中标签的叠加状态。
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.8f);
    // 活动标签与内容面板建立连续关系，明确当前可见页面。
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.19607843f, 0.40784314f, 0.6784314f, 1.0f);
    // 失焦停靠节点的普通标签进一步降级，突出当前活动窗口组。
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.06666667f, 0.101960786f, 0.14509805f, 0.9724f);
    // 失焦组仍需标识其已选页面，因此保留受抑制的活动态。
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.13333334f, 0.25882354f, 0.42352942f, 1.0f);
    // 折线图常态颜色优先保证细线在背景上的连续可读性。
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.60784316f, 0.60784316f, 0.60784316f, 1.0f);
    // 折线悬浮态采用明显强调色，便于定位密集曲线中的目标。
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(1.0f, 0.42745098f, 0.34901962f, 1.0f);
    // 柱状图使用独立数据色，避免与按钮等交互强调色混淆。
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.8980392f, 0.69803923f, 0.0f, 1.0f);
    // 柱状图悬浮色强化当前样本，但仍需保留原数据系列归属。
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(1.0f, 0.6f, 0.0f, 1.0f);
    // 表头底色固定列语义区域，与数据行建立稳定的纵向起点。
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.1882353f, 0.1882353f, 0.2f, 1.0f);
    // 强表格边框用于外沿和表头分界，承担主要结构线。
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.30980393f, 0.30980393f, 0.34901962f, 1.0f);
    // 弱表格边框只分隔普通单元格，降低密集网格的视觉噪声。
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(0.22745098f, 0.22745098f, 0.24705882f, 1.0f);
    // 基础行背景允许透明继承，使表格自然嵌入所在面板。
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 交替行仅施加轻微差异，帮助横向追踪而不破坏整体底色。
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    // 文本选区背景必须兼顾选中辨识度与前景文字可读性。
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.35f);
    // 拖放目标使用高辨识强调色，明确释放操作将作用的位置。
    style.Colors[ImGuiCol_DragDropTarget] = ImVec4(1.0f, 1.0f, 0.0f, 0.9f);
    // 键盘和手柄导航高亮独立于鼠标悬浮态，避免输入来源含混。
    style.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    // 窗口导航高亮标识待切换窗口，并覆盖多种面板底色。
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    // 窗口切换期间的遮罩压低非目标内容，但仍保留场景上下文。
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    // 模态遮罩明确阻断下层交互，透明度同时维持来源窗口可见。
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.35f);
}

static void applyDarculaStyle(ImGuiStyle& style)
{
    // Darcula 样式，来源为 ImThemes 的 ice1000 配色。

    // 全局透明度会乘到整套颜色上，主题在此保持最终合成基准。
    style.Alpha = 1.0f;
    // 禁用态通过统一衰减与可交互控件拉开层级，无需逐项改写颜色。
    style.DisabledAlpha = 0.6f;
    // 窗口内边距同时决定内容密度和停靠面板的呼吸空间。
    style.WindowPadding            = ImVec2(8.0f, 8.0f);
    style.WindowRounding           = 5.3f;
    style.WindowBorderSize         = 1.0f;
    style.WindowMinSize            = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign         = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_Left;
    style.ChildRounding            = 0.0f;
    style.ChildBorderSize          = 1.0f;
    style.PopupRounding            = 0.0f;
    style.PopupBorderSize          = 1.0f;
    // 框体内边距决定输入框和按钮的文字基线及垂直触达面积。
    style.FramePadding    = ImVec2(4.0f, 3.0f);
    style.FrameRounding   = 2.3f;
    style.FrameBorderSize = 1.0f;
    // 项目间距约束同级控件节奏，也是多数自动布局的基础步长。
    style.ItemSpacing      = ImVec2(8.0f, 6.5f);
    style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
    // 单元格留白保证表格密集展示时文字和分隔线仍有安全距离。
    style.CellPadding       = ImVec2(4.0f, 2.0f);
    style.IndentSpacing     = 21.0f;
    style.ColumnsMinSpacing = 6.0f;
    // 滚动条宽度在可点击性与内容占用之间维持主题取舍。
    style.ScrollbarSize     = 14.0f;
    style.ScrollbarRounding = 5.0f;
    style.GrabMinSize       = 10.0f;
    style.GrabRounding      = 2.3f;
    style.TabRounding       = 4.0f;
    style.TabBorderSize     = 0.0f;
    // 选中标签的关闭按钮阈值控制窄页签中操作入口的保留策略。
    style.TabCloseButtonMinWidthSelected = 0.0f;
    // 未选中标签单独设阈值，避免拥挤时关闭图标喧宾夺主。
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    // 正文色承担主要信息层，必须与窗口和框体背景保持足够对比。
    style.Colors[ImGuiCol_Text] =
        ImVec4(0.73333335f, 0.73333335f, 0.73333335f, 1.0f);
    // 弱化文字用于不可用或次要信息，但仍需在主背景上可辨识。
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4(0.34509805f, 0.34509805f, 0.34509805f, 1.0f);
    // 窗口底色奠定整套主题明度，后续容器和控件颜色均以此为参照。
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.23529412f, 0.24705882f, 0.25490198f, 0.94f);
    // 子窗口背景可选择透明继承，避免嵌套区域产生多余色块。
    style.Colors[ImGuiCol_ChildBg] =
        ImVec4(0.23529412f, 0.24705882f, 0.25490198f, 0.0f);
    // 弹窗背景需要遮住下层内容，同时以透明度保留空间关系。
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.23529412f, 0.24705882f, 0.25490198f, 0.94f);
    // 边框色用于低成本描绘控件和容器边界，透明度决定强调强度。
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.33333334f, 0.33333334f, 0.33333334f, 0.5f);
    // 边框阴影独立配置，以便主题明确选择平面或浮起效果。
    style.Colors[ImGuiCol_BorderShadow] =
        ImVec4(0.15686275f, 0.15686275f, 0.15686275f, 0.0f);
    // 框体常态是输入和选择控件的静止基面，也是交互渐变的起点。
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.16862746f, 0.16862746f, 0.16862746f, 0.54f);
    // 框体悬浮色只提示可交互性，应比激活态更克制。
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.4509804f, 0.6745098f, 0.99607843f, 0.67f);
    // 框体激活色确认当前操作焦点，并与悬浮态形成连续反馈。
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.47058824f, 0.47058824f, 0.47058824f, 0.67f);
    // 非活动标题栏降低存在感，让当前编辑窗口更容易被识别。
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.039215688f, 0.039215688f, 0.039215688f, 1.0f);
    // 活动标题栏提高层级，承担当前窗口焦点的主要视觉提示。
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.0f, 0.0f, 0.0f, 0.51f);
    // 折叠标题仍保留窗口身份，但用透明度减少对内容的干扰。
    style.Colors[ImGuiCol_TitleBgCollapsed] =
        ImVec4(0.15686275f, 0.28627452f, 0.47843137f, 1.0f);
    // 菜单栏底色连接窗口标题与内容区，避免形成突兀的横向色带。
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.27058825f, 0.28627452f, 0.2901961f, 0.8f);
    // 滚动轨道应融入容器背景，使抓手成为滚动位置的主要提示。
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.27058825f, 0.28627452f, 0.2901961f, 0.6f);
    // 滚动抓手常态需持续可见，又不能盖过正文和主要操作。
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.21960784f, 0.30980393f, 0.41960785f, 0.51f);
    // 滚动抓手悬浮态提高对比，提示鼠标已经进入可拖动区域。
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.21960784f, 0.30980393f, 0.41960785f, 1.0f);
    // 滚动抓手激活态确认持续拖动，颜色变化需强于悬浮态。
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.13725491f, 0.19215687f, 0.2627451f, 0.91f);
    // 勾选标记承载布尔结果，应从框体背景中清晰跳出。
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.8980392f, 0.8980392f, 0.8980392f, 0.83f);
    // 滑块抓手常态标记当前值位置，并与轨道保持明度区分。
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.69803923f, 0.69803923f, 0.69803923f, 0.62f);
    // 滑块活动抓手强调正在调整的数值，释放后回落至常态。
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.29803923f, 0.29803923f, 0.29803923f, 0.84f);
    // 按钮常态建立操作面的基础层级，为悬浮和按下反馈预留变化空间。
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.33333334f, 0.3529412f, 0.36078432f, 0.49f);
    // 按钮悬浮态只表达指向，不应与已按下状态混淆。
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.21960784f, 0.30980393f, 0.41960785f, 1.0f);
    // 按钮激活态提供按下确认，需要成为三态中最明确的反馈。
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.13725491f, 0.19215687f, 0.2627451f, 1.0f);
    // Header 常态同时服务树节点、列表选项和折叠区标题。
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.33333334f, 0.3529412f, 0.36078432f, 0.53f);
    // Header 悬浮态覆盖较大行区域，应控制亮度避免产生闪烁感。
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.4509804f, 0.6745098f, 0.99607843f, 0.67f);
    // Header 激活态标识当前选择或按压结果，与按钮反馈保持一致。
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.47058824f, 0.47058824f, 0.47058824f, 0.67f);
    // 分隔线以低对比划分区域，不与可拖动边缘争夺注意力。
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.3137255f, 0.3137255f, 0.3137255f, 1.0f);
    // 可交互分隔线在悬浮时增强，提示其支持调整布局。
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.3137255f, 0.3137255f, 0.3137255f, 1.0f);
    // 分隔线拖动期间使用最强状态，持续反馈布局调整操作。
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.3137255f, 0.3137255f, 0.3137255f, 1.0f);
    // 缩放抓手常态保持含蓄，避免遮挡窗口角落内容。
    style.Colors[ImGuiCol_ResizeGrip] = ImVec4(1.0f, 1.0f, 1.0f, 0.85f);
    // 缩放抓手悬浮态显露可调整方向，强化边角命中提示。
    style.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.6f);
    // 缩放抓手激活态维持拖动反馈，直到尺寸调整结束。
    style.Colors[ImGuiCol_ResizeGripActive] = ImVec4(1.0f, 1.0f, 1.0f, 0.9f);
    // 普通标签以中等层级承载页面入口，不抢占当前标签的焦点。
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.1764706f, 0.34901962f, 0.5764706f, 0.862f);
    // 标签悬浮色提前提示可切换目标，并需兼容已选中标签的叠加状态。
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.8f);
    // 活动标签与内容面板建立连续关系，明确当前可见页面。
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.19607843f, 0.40784314f, 0.6784314f, 1.0f);
    // 失焦停靠节点的普通标签进一步降级，突出当前活动窗口组。
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.06666667f, 0.101960786f, 0.14509805f, 0.9724f);
    // 失焦组仍需标识其已选页面，因此保留受抑制的活动态。
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.13333334f, 0.25882354f, 0.42352942f, 1.0f);
    // 折线图常态颜色优先保证细线在背景上的连续可读性。
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.60784316f, 0.60784316f, 0.60784316f, 1.0f);
    // 折线悬浮态采用明显强调色，便于定位密集曲线中的目标。
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(1.0f, 0.42745098f, 0.34901962f, 1.0f);
    // 柱状图使用独立数据色，避免与按钮等交互强调色混淆。
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.8980392f, 0.69803923f, 0.0f, 1.0f);
    // 柱状图悬浮色强化当前样本，但仍需保留原数据系列归属。
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(1.0f, 0.6f, 0.0f, 1.0f);
    // 表头底色固定列语义区域，与数据行建立稳定的纵向起点。
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.1882353f, 0.1882353f, 0.2f, 1.0f);
    // 强表格边框用于外沿和表头分界，承担主要结构线。
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.30980393f, 0.30980393f, 0.34901962f, 1.0f);
    // 弱表格边框只分隔普通单元格，降低密集网格的视觉噪声。
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(0.22745098f, 0.22745098f, 0.24705882f, 1.0f);
    // 基础行背景允许透明继承，使表格自然嵌入所在面板。
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 交替行仅施加轻微差异，帮助横向追踪而不破坏整体底色。
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    // 文本选区背景必须兼顾选中辨识度与前景文字可读性。
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(0.18431373f, 0.39607844f, 0.7921569f, 0.9f);
    // 拖放目标使用高辨识强调色，明确释放操作将作用的位置。
    style.Colors[ImGuiCol_DragDropTarget] = ImVec4(1.0f, 1.0f, 0.0f, 0.9f);
    // 键盘和手柄导航高亮独立于鼠标悬浮态，避免输入来源含混。
    style.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    // 窗口导航高亮标识待切换窗口，并覆盖多种面板底色。
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    // 窗口切换期间的遮罩压低非目标内容，但仍保留场景上下文。
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    // 模态遮罩明确阻断下层交互，透明度同时维持来源窗口可见。
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.35f);
}

static void applyPhotoshopStyle(ImGuiStyle& style)
{
    // Photoshop 样式，来源为 ImThemes 的 Derydoca 配色。

    // 全局透明度会乘到整套颜色上，主题在此保持最终合成基准。
    style.Alpha = 1.0f;
    // 禁用态通过统一衰减与可交互控件拉开层级，无需逐项改写颜色。
    style.DisabledAlpha = 0.6f;
    // 窗口内边距同时决定内容密度和停靠面板的呼吸空间。
    style.WindowPadding            = ImVec2(8.0f, 8.0f);
    style.WindowRounding           = 4.0f;
    style.WindowBorderSize         = 1.0f;
    style.WindowMinSize            = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign         = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_Left;
    style.ChildRounding            = 4.0f;
    style.ChildBorderSize          = 1.0f;
    style.PopupRounding            = 2.0f;
    style.PopupBorderSize          = 1.0f;
    // 框体内边距决定输入框和按钮的文字基线及垂直触达面积。
    style.FramePadding    = ImVec2(4.0f, 3.0f);
    style.FrameRounding   = 2.0f;
    style.FrameBorderSize = 1.0f;
    // 项目间距约束同级控件节奏，也是多数自动布局的基础步长。
    style.ItemSpacing      = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
    // 单元格留白保证表格密集展示时文字和分隔线仍有安全距离。
    style.CellPadding       = ImVec2(4.0f, 2.0f);
    style.IndentSpacing     = 21.0f;
    style.ColumnsMinSpacing = 6.0f;
    // 滚动条宽度在可点击性与内容占用之间维持主题取舍。
    style.ScrollbarSize     = 13.0f;
    style.ScrollbarRounding = 12.0f;
    style.GrabMinSize       = 7.0f;
    style.GrabRounding      = 0.0f;
    style.TabRounding       = 0.0f;
    style.TabBorderSize     = 1.0f;
    // 选中标签的关闭按钮阈值控制窄页签中操作入口的保留策略。
    style.TabCloseButtonMinWidthSelected = 0.0f;
    // 未选中标签单独设阈值，避免拥挤时关闭图标喧宾夺主。
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    // 正文色承担主要信息层，必须与窗口和框体背景保持足够对比。
    style.Colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // 弱化文字用于不可用或次要信息，但仍需在主背景上可辨识。
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4(0.49803922f, 0.49803922f, 0.49803922f, 1.0f);
    // 窗口底色奠定整套主题明度，后续容器和控件颜色均以此为参照。
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.1764706f, 0.1764706f, 0.1764706f, 1.0f);
    // 子窗口背景可选择透明继承，避免嵌套区域产生多余色块。
    style.Colors[ImGuiCol_ChildBg] =
        ImVec4(0.2784314f, 0.2784314f, 0.2784314f, 0.0f);
    // 弹窗背景需要遮住下层内容，同时以透明度保留空间关系。
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.30980393f, 0.30980393f, 0.30980393f, 1.0f);
    // 边框色用于低成本描绘控件和容器边界，透明度决定强调强度。
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.2627451f, 0.2627451f, 0.2627451f, 1.0f);
    // 边框阴影独立配置，以便主题明确选择平面或浮起效果。
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 框体常态是输入和选择控件的静止基面，也是交互渐变的起点。
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.15686275f, 0.15686275f, 0.15686275f, 1.0f);
    // 框体悬浮色只提示可交互性，应比激活态更克制。
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.2f, 0.2f, 0.2f, 1.0f);
    // 框体激活色确认当前操作焦点，并与悬浮态形成连续反馈。
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.2784314f, 0.2784314f, 0.2784314f, 1.0f);
    // 非活动标题栏降低存在感，让当前编辑窗口更容易被识别。
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.14509805f, 0.14509805f, 0.14509805f, 1.0f);
    // 活动标题栏提高层级，承担当前窗口焦点的主要视觉提示。
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.14509805f, 0.14509805f, 0.14509805f, 1.0f);
    // 折叠标题仍保留窗口身份，但用透明度减少对内容的干扰。
    style.Colors[ImGuiCol_TitleBgCollapsed] =
        ImVec4(0.14509805f, 0.14509805f, 0.14509805f, 1.0f);
    // 菜单栏底色连接窗口标题与内容区，避免形成突兀的横向色带。
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.19215687f, 0.19215687f, 0.19215687f, 1.0f);
    // 滚动轨道应融入容器背景，使抓手成为滚动位置的主要提示。
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.15686275f, 0.15686275f, 0.15686275f, 1.0f);
    // 滚动抓手常态需持续可见，又不能盖过正文和主要操作。
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.27450982f, 0.27450982f, 0.27450982f, 1.0f);
    // 滚动抓手悬浮态提高对比，提示鼠标已经进入可拖动区域。
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.29803923f, 0.29803923f, 0.29803923f, 1.0f);
    // 滚动抓手激活态确认持续拖动，颜色变化需强于悬浮态。
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(1.0f, 0.3882353f, 0.0f, 1.0f);
    // 勾选标记承载布尔结果，应从框体背景中清晰跳出。
    style.Colors[ImGuiCol_CheckMark] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // 滑块抓手常态标记当前值位置，并与轨道保持明度区分。
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.3882353f, 0.3882353f, 0.3882353f, 1.0f);
    // 滑块活动抓手强调正在调整的数值，释放后回落至常态。
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(1.0f, 0.3882353f, 0.0f, 1.0f);
    // 按钮常态建立操作面的基础层级，为悬浮和按下反馈预留变化空间。
    style.Colors[ImGuiCol_Button] = ImVec4(1.0f, 1.0f, 1.0f, 0.0f);
    // 按钮悬浮态只表达指向，不应与已按下状态混淆。
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.156f);
    // 按钮激活态提供按下确认，需要成为三态中最明确的反馈。
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(1.0f, 1.0f, 1.0f, 0.391f);
    // Header 常态同时服务树节点、列表选项和折叠区标题。
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.30980393f, 0.30980393f, 0.30980393f, 1.0f);
    // Header 悬浮态覆盖较大行区域，应控制亮度避免产生闪烁感。
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.46666667f, 0.46666667f, 0.46666667f, 1.0f);
    // Header 激活态标识当前选择或按压结果，与按钮反馈保持一致。
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.46666667f, 0.46666667f, 0.46666667f, 1.0f);
    // 分隔线以低对比划分区域，不与可拖动边缘争夺注意力。
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.2627451f, 0.2627451f, 0.2627451f, 1.0f);
    // 可交互分隔线在悬浮时增强，提示其支持调整布局。
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.3882353f, 0.3882353f, 0.3882353f, 1.0f);
    // 分隔线拖动期间使用最强状态，持续反馈布局调整操作。
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(1.0f, 0.3882353f, 0.0f, 1.0f);
    // 缩放抓手常态保持含蓄，避免遮挡窗口角落内容。
    style.Colors[ImGuiCol_ResizeGrip] = ImVec4(1.0f, 1.0f, 1.0f, 0.25f);
    // 缩放抓手悬浮态显露可调整方向，强化边角命中提示。
    style.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.67f);
    // 缩放抓手激活态维持拖动反馈，直到尺寸调整结束。
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(1.0f, 0.3882353f, 0.0f, 1.0f);
    // 普通标签以中等层级承载页面入口，不抢占当前标签的焦点。
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.09411765f, 0.09411765f, 0.09411765f, 1.0f);
    // 标签悬浮色提前提示可切换目标，并需兼容已选中标签的叠加状态。
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.34901962f, 0.34901962f, 0.34901962f, 1.0f);
    // 活动标签与内容面板建立连续关系，明确当前可见页面。
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.19215687f, 0.19215687f, 0.19215687f, 1.0f);
    // 失焦停靠节点的普通标签进一步降级，突出当前活动窗口组。
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.09411765f, 0.09411765f, 0.09411765f, 1.0f);
    // 失焦组仍需标识其已选页面，因此保留受抑制的活动态。
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.19215687f, 0.19215687f, 0.19215687f, 1.0f);
    // 折线图常态颜色优先保证细线在背景上的连续可读性。
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.46666667f, 0.46666667f, 0.46666667f, 1.0f);
    // 折线悬浮态采用明显强调色，便于定位密集曲线中的目标。
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(1.0f, 0.3882353f, 0.0f, 1.0f);
    // 柱状图使用独立数据色，避免与按钮等交互强调色混淆。
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.58431375f, 0.58431375f, 0.58431375f, 1.0f);
    // 柱状图悬浮色强化当前样本，但仍需保留原数据系列归属。
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(1.0f, 0.3882353f, 0.0f, 1.0f);
    // 表头底色固定列语义区域，与数据行建立稳定的纵向起点。
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.1882353f, 0.1882353f, 0.2f, 1.0f);
    // 强表格边框用于外沿和表头分界，承担主要结构线。
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.30980393f, 0.30980393f, 0.34901962f, 1.0f);
    // 弱表格边框只分隔普通单元格，降低密集网格的视觉噪声。
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(0.22745098f, 0.22745098f, 0.24705882f, 1.0f);
    // 基础行背景允许透明继承，使表格自然嵌入所在面板。
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 交替行仅施加轻微差异，帮助横向追踪而不破坏整体底色。
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    // 文本选区背景必须兼顾选中辨识度与前景文字可读性。
    style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(1.0f, 1.0f, 1.0f, 0.156f);
    // 拖放目标使用高辨识强调色，明确释放操作将作用的位置。
    style.Colors[ImGuiCol_DragDropTarget] =
        ImVec4(1.0f, 0.3882353f, 0.0f, 1.0f);
    // 键盘和手柄导航高亮独立于鼠标悬浮态，避免输入来源含混。
    style.Colors[ImGuiCol_NavHighlight] = ImVec4(1.0f, 0.3882353f, 0.0f, 1.0f);
    // 窗口导航高亮标识待切换窗口，并覆盖多种面板底色。
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 0.3882353f, 0.0f, 1.0f);
    // 窗口切换期间的遮罩压低非目标内容，但仍保留场景上下文。
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.586f);
    // 模态遮罩明确阻断下层交互，透明度同时维持来源窗口可见。
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.586f);
}

static void applyUnrealStyle(ImGuiStyle& style)
{
    // Unreal 样式，来源为 ImThemes 的 dev0-1 配色。

    // 全局透明度会乘到整套颜色上，主题在此保持最终合成基准。
    style.Alpha = 1.0f;
    // 禁用态通过统一衰减与可交互控件拉开层级，无需逐项改写颜色。
    style.DisabledAlpha = 0.6f;
    // 窗口内边距同时决定内容密度和停靠面板的呼吸空间。
    style.WindowPadding            = ImVec2(8.0f, 8.0f);
    style.WindowRounding           = 0.0f;
    style.WindowBorderSize         = 1.0f;
    style.WindowMinSize            = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign         = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_Left;
    style.ChildRounding            = 0.0f;
    style.ChildBorderSize          = 1.0f;
    style.PopupRounding            = 0.0f;
    style.PopupBorderSize          = 1.0f;
    // 框体内边距决定输入框和按钮的文字基线及垂直触达面积。
    style.FramePadding    = ImVec2(4.0f, 3.0f);
    style.FrameRounding   = 0.0f;
    style.FrameBorderSize = 0.0f;
    // 项目间距约束同级控件节奏，也是多数自动布局的基础步长。
    style.ItemSpacing      = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
    // 单元格留白保证表格密集展示时文字和分隔线仍有安全距离。
    style.CellPadding       = ImVec2(4.0f, 2.0f);
    style.IndentSpacing     = 21.0f;
    style.ColumnsMinSpacing = 6.0f;
    // 滚动条宽度在可点击性与内容占用之间维持主题取舍。
    style.ScrollbarSize     = 14.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabMinSize       = 10.0f;
    style.GrabRounding      = 0.0f;
    style.TabRounding       = 4.0f;
    style.TabBorderSize     = 0.0f;
    // 选中标签的关闭按钮阈值控制窄页签中操作入口的保留策略。
    style.TabCloseButtonMinWidthSelected = 0.0f;
    // 未选中标签单独设阈值，避免拥挤时关闭图标喧宾夺主。
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    // 正文色承担主要信息层，必须与窗口和框体背景保持足够对比。
    style.Colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // 弱化文字用于不可用或次要信息，但仍需在主背景上可辨识。
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4(0.49803922f, 0.49803922f, 0.49803922f, 1.0f);
    // 窗口底色奠定整套主题明度，后续容器和控件颜色均以此为参照。
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.05882353f, 0.05882353f, 0.05882353f, 0.94f);
    // 子窗口背景可选择透明继承，避免嵌套区域产生多余色块。
    style.Colors[ImGuiCol_ChildBg] = ImVec4(1.0f, 1.0f, 1.0f, 0.0f);
    // 弹窗背景需要遮住下层内容，同时以透明度保留空间关系。
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.078431375f, 0.078431375f, 0.078431375f, 0.94f);
    // 边框色用于低成本描绘控件和容器边界，透明度决定强调强度。
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.42745098f, 0.42745098f, 0.49803922f, 0.5f);
    // 边框阴影独立配置，以便主题明确选择平面或浮起效果。
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 框体常态是输入和选择控件的静止基面，也是交互渐变的起点。
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.2f, 0.20784314f, 0.21960784f, 0.54f);
    // 框体悬浮色只提示可交互性，应比激活态更克制。
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.4f, 0.4f, 0.4f, 0.4f);
    // 框体激活色确认当前操作焦点，并与悬浮态形成连续反馈。
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.1764706f, 0.1764706f, 0.1764706f, 0.67f);
    // 非活动标题栏降低存在感，让当前编辑窗口更容易被识别。
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.039215688f, 0.039215688f, 0.039215688f, 1.0f);
    // 活动标题栏提高层级，承担当前窗口焦点的主要视觉提示。
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.28627452f, 0.28627452f, 0.28627452f, 1.0f);
    // 折叠标题仍保留窗口身份，但用透明度减少对内容的干扰。
    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.0f, 0.0f, 0.0f, 0.51f);
    // 菜单栏底色连接窗口标题与内容区，避免形成突兀的横向色带。
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.13725491f, 0.13725491f, 0.13725491f, 1.0f);
    // 滚动轨道应融入容器背景，使抓手成为滚动位置的主要提示。
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.019607844f, 0.019607844f, 0.019607844f, 0.53f);
    // 滚动抓手常态需持续可见，又不能盖过正文和主要操作。
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.30980393f, 0.30980393f, 0.30980393f, 1.0f);
    // 滚动抓手悬浮态提高对比，提示鼠标已经进入可拖动区域。
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.40784314f, 0.40784314f, 0.40784314f, 1.0f);
    // 滚动抓手激活态确认持续拖动，颜色变化需强于悬浮态。
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.50980395f, 0.50980395f, 0.50980395f, 1.0f);
    // 勾选标记承载布尔结果，应从框体背景中清晰跳出。
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.9372549f, 0.9372549f, 0.9372549f, 1.0f);
    // 滑块抓手常态标记当前值位置，并与轨道保持明度区分。
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.50980395f, 0.50980395f, 0.50980395f, 1.0f);
    // 滑块活动抓手强调正在调整的数值，释放后回落至常态。
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.85882354f, 0.85882354f, 0.85882354f, 1.0f);
    // 按钮常态建立操作面的基础层级，为悬浮和按下反馈预留变化空间。
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.4392157f, 0.4392157f, 0.4392157f, 0.4f);
    // 按钮悬浮态只表达指向，不应与已按下状态混淆。
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.45882353f, 0.46666667f, 0.47843137f, 1.0f);
    // 按钮激活态提供按下确认，需要成为三态中最明确的反馈。
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.41960785f, 0.41960785f, 0.41960785f, 1.0f);
    // Header 常态同时服务树节点、列表选项和折叠区标题。
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.69803923f, 0.69803923f, 0.69803923f, 0.31f);
    // Header 悬浮态覆盖较大行区域，应控制亮度避免产生闪烁感。
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.69803923f, 0.69803923f, 0.69803923f, 0.8f);
    // Header 激活态标识当前选择或按压结果，与按钮反馈保持一致。
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.47843137f, 0.49803922f, 0.5176471f, 1.0f);
    // 分隔线以低对比划分区域，不与可拖动边缘争夺注意力。
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.42745098f, 0.42745098f, 0.49803922f, 0.5f);
    // 可交互分隔线在悬浮时增强，提示其支持调整布局。
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.7176471f, 0.7176471f, 0.7176471f, 0.78f);
    // 分隔线拖动期间使用最强状态，持续反馈布局调整操作。
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.50980395f, 0.50980395f, 0.50980395f, 1.0f);
    // 缩放抓手常态保持含蓄，避免遮挡窗口角落内容。
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.9098039f, 0.9098039f, 0.9098039f, 0.25f);
    // 缩放抓手悬浮态显露可调整方向，强化边角命中提示。
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.80784315f, 0.80784315f, 0.80784315f, 0.67f);
    // 缩放抓手激活态维持拖动反馈，直到尺寸调整结束。
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.45882353f, 0.45882353f, 0.45882353f, 0.95f);
    // 普通标签以中等层级承载页面入口，不抢占当前标签的焦点。
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.1764706f, 0.34901962f, 0.5764706f, 0.862f);
    // 标签悬浮色提前提示可切换目标，并需兼容已选中标签的叠加状态。
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.8f);
    // 活动标签与内容面板建立连续关系，明确当前可见页面。
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.19607843f, 0.40784314f, 0.6784314f, 1.0f);
    // 失焦停靠节点的普通标签进一步降级，突出当前活动窗口组。
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.06666667f, 0.101960786f, 0.14509805f, 0.9724f);
    // 失焦组仍需标识其已选页面，因此保留受抑制的活动态。
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.13333334f, 0.25882354f, 0.42352942f, 1.0f);
    // 折线图常态颜色优先保证细线在背景上的连续可读性。
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.60784316f, 0.60784316f, 0.60784316f, 1.0f);
    // 折线悬浮态采用明显强调色，便于定位密集曲线中的目标。
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(1.0f, 0.42745098f, 0.34901962f, 1.0f);
    // 柱状图使用独立数据色，避免与按钮等交互强调色混淆。
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.7294118f, 0.6f, 0.14901961f, 1.0f);
    // 柱状图悬浮色强化当前样本，但仍需保留原数据系列归属。
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(1.0f, 0.6f, 0.0f, 1.0f);
    // 表头底色固定列语义区域，与数据行建立稳定的纵向起点。
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.1882353f, 0.1882353f, 0.2f, 1.0f);
    // 强表格边框用于外沿和表头分界，承担主要结构线。
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.30980393f, 0.30980393f, 0.34901962f, 1.0f);
    // 弱表格边框只分隔普通单元格，降低密集网格的视觉噪声。
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(0.22745098f, 0.22745098f, 0.24705882f, 1.0f);
    // 基础行背景允许透明继承，使表格自然嵌入所在面板。
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 交替行仅施加轻微差异，帮助横向追踪而不破坏整体底色。
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    // 文本选区背景必须兼顾选中辨识度与前景文字可读性。
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(0.8666667f, 0.8666667f, 0.8666667f, 0.35f);
    // 拖放目标使用高辨识强调色，明确释放操作将作用的位置。
    style.Colors[ImGuiCol_DragDropTarget] = ImVec4(1.0f, 1.0f, 0.0f, 0.9f);
    // 键盘和手柄导航高亮独立于鼠标悬浮态，避免输入来源含混。
    style.Colors[ImGuiCol_NavHighlight] = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
    // 窗口导航高亮标识待切换窗口，并覆盖多种面板底色。
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    // 窗口切换期间的遮罩压低非目标内容，但仍保留场景上下文。
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    // 模态遮罩明确阻断下层交互，透明度同时维持来源窗口可见。
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.35f);
}

static void applyGoldStyle(ImGuiStyle& style)
{
    // Gold 样式，来源为 ImThemes 的 CookiePLMonster 配色。

    // 全局透明度会乘到整套颜色上，主题在此保持最终合成基准。
    style.Alpha = 1.0f;
    // 禁用态通过统一衰减与可交互控件拉开层级，无需逐项改写颜色。
    style.DisabledAlpha = 0.6f;
    // 窗口内边距同时决定内容密度和停靠面板的呼吸空间。
    style.WindowPadding            = ImVec2(8.0f, 8.0f);
    style.WindowRounding           = 4.0f;
    style.WindowBorderSize         = 1.0f;
    style.WindowMinSize            = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign         = ImVec2(1.0f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_Right;
    style.ChildRounding            = 0.0f;
    style.ChildBorderSize          = 1.0f;
    style.PopupRounding            = 4.0f;
    style.PopupBorderSize          = 1.0f;
    // 框体内边距决定输入框和按钮的文字基线及垂直触达面积。
    style.FramePadding    = ImVec2(4.0f, 2.0f);
    style.FrameRounding   = 4.0f;
    style.FrameBorderSize = 0.0f;
    // 项目间距约束同级控件节奏，也是多数自动布局的基础步长。
    style.ItemSpacing      = ImVec2(10.0f, 2.0f);
    style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
    // 单元格留白保证表格密集展示时文字和分隔线仍有安全距离。
    style.CellPadding       = ImVec2(4.0f, 2.0f);
    style.IndentSpacing     = 12.0f;
    style.ColumnsMinSpacing = 6.0f;
    // 滚动条宽度在可点击性与内容占用之间维持主题取舍。
    style.ScrollbarSize     = 10.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabMinSize       = 10.0f;
    style.GrabRounding      = 4.0f;
    style.TabRounding       = 4.0f;
    style.TabBorderSize     = 0.0f;
    // 选中标签的关闭按钮阈值控制窄页签中操作入口的保留策略。
    style.TabCloseButtonMinWidthSelected = 0.0f;
    // 未选中标签单独设阈值，避免拥挤时关闭图标喧宾夺主。
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    // 正文色承担主要信息层，必须与窗口和框体背景保持足够对比。
    style.Colors[ImGuiCol_Text] =
        ImVec4(0.91764706f, 0.91764706f, 0.91764706f, 1.0f);
    // 弱化文字用于不可用或次要信息，但仍需在主背景上可辨识。
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4(0.4392157f, 0.4392157f, 0.4392157f, 1.0f);
    // 窗口底色奠定整套主题明度，后续容器和控件颜色均以此为参照。
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.05882353f, 0.05882353f, 0.05882353f, 1.0f);
    // 子窗口背景可选择透明继承，避免嵌套区域产生多余色块。
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 弹窗背景需要遮住下层内容，同时以透明度保留空间关系。
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.078431375f, 0.078431375f, 0.078431375f, 0.94f);
    // 边框色用于低成本描绘控件和容器边界，透明度决定强调强度。
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.50980395f, 0.35686275f, 0.14901961f, 1.0f);
    // 边框阴影独立配置，以便主题明确选择平面或浮起效果。
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 框体常态是输入和选择控件的静止基面，也是交互渐变的起点。
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.10980392f, 0.10980392f, 0.10980392f, 1.0f);
    // 框体悬浮色只提示可交互性，应比激活态更克制。
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.50980395f, 0.35686275f, 0.14901961f, 1.0f);
    // 框体激活色确认当前操作焦点，并与悬浮态形成连续反馈。
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.7764706f, 0.54901963f, 0.20784314f, 1.0f);
    // 非活动标题栏降低存在感，让当前编辑窗口更容易被识别。
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.50980395f, 0.35686275f, 0.14901961f, 1.0f);
    // 活动标题栏提高层级，承担当前窗口焦点的主要视觉提示。
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.9098039f, 0.6392157f, 0.12941177f, 1.0f);
    // 折叠标题仍保留窗口身份，但用透明度减少对内容的干扰。
    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.0f, 0.0f, 0.0f, 0.51f);
    // 菜单栏底色连接窗口标题与内容区，避免形成突兀的横向色带。
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.10980392f, 0.10980392f, 0.10980392f, 1.0f);
    // 滚动轨道应融入容器背景，使抓手成为滚动位置的主要提示。
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.05882353f, 0.05882353f, 0.05882353f, 0.53f);
    // 滚动抓手常态需持续可见，又不能盖过正文和主要操作。
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.20784314f, 0.20784314f, 0.20784314f, 1.0f);
    // 滚动抓手悬浮态提高对比，提示鼠标已经进入可拖动区域。
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.46666667f, 0.46666667f, 0.46666667f, 1.0f);
    // 滚动抓手激活态确认持续拖动，颜色变化需强于悬浮态。
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.80784315f, 0.827451f, 0.80784315f, 1.0f);
    // 勾选标记承载布尔结果，应从框体背景中清晰跳出。
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.7764706f, 0.54901963f, 0.20784314f, 1.0f);
    // 滑块抓手常态标记当前值位置，并与轨道保持明度区分。
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.9098039f, 0.6392157f, 0.12941177f, 1.0f);
    // 滑块活动抓手强调正在调整的数值，释放后回落至常态。
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.9098039f, 0.6392157f, 0.12941177f, 1.0f);
    // 按钮常态建立操作面的基础层级，为悬浮和按下反馈预留变化空间。
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.50980395f, 0.35686275f, 0.14901961f, 1.0f);
    // 按钮悬浮态只表达指向，不应与已按下状态混淆。
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.9098039f, 0.6392157f, 0.12941177f, 1.0f);
    // 按钮激活态提供按下确认，需要成为三态中最明确的反馈。
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.7764706f, 0.54901963f, 0.20784314f, 1.0f);
    // Header 常态同时服务树节点、列表选项和折叠区标题。
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.50980395f, 0.35686275f, 0.14901961f, 1.0f);
    // Header 悬浮态覆盖较大行区域，应控制亮度避免产生闪烁感。
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.9098039f, 0.6392157f, 0.12941177f, 1.0f);
    // Header 激活态标识当前选择或按压结果，与按钮反馈保持一致。
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.92941177f, 0.64705884f, 0.13725491f, 1.0f);
    // 分隔线以低对比划分区域，不与可拖动边缘争夺注意力。
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.20784314f, 0.20784314f, 0.20784314f, 1.0f);
    // 可交互分隔线在悬浮时增强，提示其支持调整布局。
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.9098039f, 0.6392157f, 0.12941177f, 1.0f);
    // 分隔线拖动期间使用最强状态，持续反馈布局调整操作。
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.7764706f, 0.54901963f, 0.20784314f, 1.0f);
    // 缩放抓手常态保持含蓄，避免遮挡窗口角落内容。
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.20784314f, 0.20784314f, 0.20784314f, 1.0f);
    // 缩放抓手悬浮态显露可调整方向，强化边角命中提示。
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.9098039f, 0.6392157f, 0.12941177f, 1.0f);
    // 缩放抓手激活态维持拖动反馈，直到尺寸调整结束。
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.7764706f, 0.54901963f, 0.20784314f, 1.0f);
    // 普通标签以中等层级承载页面入口，不抢占当前标签的焦点。
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.50980395f, 0.35686275f, 0.14901961f, 1.0f);
    // 标签悬浮色提前提示可切换目标，并需兼容已选中标签的叠加状态。
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.9098039f, 0.6392157f, 0.12941177f, 1.0f);
    // 活动标签与内容面板建立连续关系，明确当前可见页面。
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.7764706f, 0.54901963f, 0.20784314f, 1.0f);
    // 失焦停靠节点的普通标签进一步降级，突出当前活动窗口组。
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.06666667f, 0.09803922f, 0.14901961f, 0.97f);
    // 失焦组仍需标识其已选页面，因此保留受抑制的活动态。
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.13725491f, 0.25882354f, 0.41960785f, 1.0f);
    // 折线图常态颜色优先保证细线在背景上的连续可读性。
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.60784316f, 0.60784316f, 0.60784316f, 1.0f);
    // 折线悬浮态采用明显强调色，便于定位密集曲线中的目标。
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(1.0f, 0.42745098f, 0.34901962f, 1.0f);
    // 柱状图使用独立数据色，避免与按钮等交互强调色混淆。
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.8980392f, 0.69803923f, 0.0f, 1.0f);
    // 柱状图悬浮色强化当前样本，但仍需保留原数据系列归属。
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(1.0f, 0.6f, 0.0f, 1.0f);
    // 表头底色固定列语义区域，与数据行建立稳定的纵向起点。
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.1882353f, 0.1882353f, 0.2f, 1.0f);
    // 强表格边框用于外沿和表头分界，承担主要结构线。
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.30980393f, 0.30980393f, 0.34901962f, 1.0f);
    // 弱表格边框只分隔普通单元格，降低密集网格的视觉噪声。
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(0.22745098f, 0.22745098f, 0.24705882f, 1.0f);
    // 基础行背景允许透明继承，使表格自然嵌入所在面板。
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 交替行仅施加轻微差异，帮助横向追踪而不破坏整体底色。
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    // 文本选区背景必须兼顾选中辨识度与前景文字可读性。
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.35f);
    // 拖放目标使用高辨识强调色，明确释放操作将作用的位置。
    style.Colors[ImGuiCol_DragDropTarget] = ImVec4(1.0f, 1.0f, 0.0f, 0.9f);
    // 键盘和手柄导航高亮独立于鼠标悬浮态，避免输入来源含混。
    style.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    // 窗口导航高亮标识待切换窗口，并覆盖多种面板底色。
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    // 窗口切换期间的遮罩压低非目标内容，但仍保留场景上下文。
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    // 模态遮罩明确阻断下层交互，透明度同时维持来源窗口可见。
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.35f);
}

static void applyRoundedVisualStudioStyle(ImGuiStyle& style)
{
    // Rounded Visual Studio 样式，来源为 ImThemes 的 RedNicStone 配色。

    // 全局透明度会乘到整套颜色上，主题在此保持最终合成基准。
    style.Alpha = 1.0f;
    // 禁用态通过统一衰减与可交互控件拉开层级，无需逐项改写颜色。
    style.DisabledAlpha = 0.6f;
    // 窗口内边距同时决定内容密度和停靠面板的呼吸空间。
    style.WindowPadding            = ImVec2(8.0f, 8.0f);
    style.WindowRounding           = 4.0f;
    style.WindowBorderSize         = 0.0f;
    style.WindowMinSize            = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign         = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_Left;
    style.ChildRounding            = 0.0f;
    style.ChildBorderSize          = 1.0f;
    style.PopupRounding            = 4.0f;
    style.PopupBorderSize          = 1.0f;
    // 框体内边距决定输入框和按钮的文字基线及垂直触达面积。
    style.FramePadding    = ImVec2(4.0f, 3.0f);
    style.FrameRounding   = 2.5f;
    style.FrameBorderSize = 0.0f;
    // 项目间距约束同级控件节奏，也是多数自动布局的基础步长。
    style.ItemSpacing      = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
    // 单元格留白保证表格密集展示时文字和分隔线仍有安全距离。
    style.CellPadding       = ImVec2(4.0f, 2.0f);
    style.IndentSpacing     = 21.0f;
    style.ColumnsMinSpacing = 6.0f;
    // 滚动条宽度在可点击性与内容占用之间维持主题取舍。
    style.ScrollbarSize     = 11.0f;
    style.ScrollbarRounding = 2.5f;
    style.GrabMinSize       = 10.0f;
    style.GrabRounding      = 2.0f;
    style.TabRounding       = 3.5f;
    style.TabBorderSize     = 0.0f;
    // 选中标签的关闭按钮阈值控制窄页签中操作入口的保留策略。
    style.TabCloseButtonMinWidthSelected = 0.0f;
    // 未选中标签单独设阈值，避免拥挤时关闭图标喧宾夺主。
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    // 正文色承担主要信息层，必须与窗口和框体背景保持足够对比。
    style.Colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // 弱化文字用于不可用或次要信息，但仍需在主背景上可辨识。
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4(0.5921569f, 0.5921569f, 0.5921569f, 1.0f);
    // 窗口底色奠定整套主题明度，后续容器和控件颜色均以此为参照。
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.14509805f, 0.14509805f, 0.14901961f, 1.0f);
    // 子窗口背景可选择透明继承，避免嵌套区域产生多余色块。
    style.Colors[ImGuiCol_ChildBg] =
        ImVec4(0.14509805f, 0.14509805f, 0.14901961f, 1.0f);
    // 弹窗背景需要遮住下层内容，同时以透明度保留空间关系。
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.14509805f, 0.14509805f, 0.14901961f, 1.0f);
    // 边框色用于低成本描绘控件和容器边界，透明度决定强调强度。
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.30588236f, 0.30588236f, 0.30588236f, 1.0f);
    // 边框阴影独立配置，以便主题明确选择平面或浮起效果。
    style.Colors[ImGuiCol_BorderShadow] =
        ImVec4(0.30588236f, 0.30588236f, 0.30588236f, 1.0f);
    // 框体常态是输入和选择控件的静止基面，也是交互渐变的起点。
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.2f, 0.2f, 0.21568628f, 1.0f);
    // 框体悬浮色只提示可交互性，应比激活态更克制。
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.11372549f, 0.5921569f, 0.9254902f, 1.0f);
    // 框体激活色确认当前操作焦点，并与悬浮态形成连续反馈。
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.0f, 0.46666667f, 0.78431374f, 1.0f);
    // 非活动标题栏降低存在感，让当前编辑窗口更容易被识别。
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.14509805f, 0.14509805f, 0.14901961f, 1.0f);
    // 活动标题栏提高层级，承担当前窗口焦点的主要视觉提示。
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.14509805f, 0.14509805f, 0.14901961f, 1.0f);
    // 折叠标题仍保留窗口身份，但用透明度减少对内容的干扰。
    style.Colors[ImGuiCol_TitleBgCollapsed] =
        ImVec4(0.14509805f, 0.14509805f, 0.14901961f, 1.0f);
    // 菜单栏底色连接窗口标题与内容区，避免形成突兀的横向色带。
    style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.2f, 0.2f, 0.21568628f, 1.0f);
    // 滚动轨道应融入容器背景，使抓手成为滚动位置的主要提示。
    style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.2f, 0.2f, 0.21568628f, 1.0f);
    // 滚动抓手常态需持续可见，又不能盖过正文和主要操作。
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.32156864f, 0.32156864f, 0.33333334f, 1.0f);
    // 滚动抓手悬浮态提高对比，提示鼠标已经进入可拖动区域。
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.3529412f, 0.3529412f, 0.37254903f, 1.0f);
    // 滚动抓手激活态确认持续拖动，颜色变化需强于悬浮态。
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.3529412f, 0.3529412f, 0.37254903f, 1.0f);
    // 勾选标记承载布尔结果，应从框体背景中清晰跳出。
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.0f, 0.46666667f, 0.78431374f, 1.0f);
    // 滑块抓手常态标记当前值位置，并与轨道保持明度区分。
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.11372549f, 0.5921569f, 0.9254902f, 1.0f);
    // 滑块活动抓手强调正在调整的数值，释放后回落至常态。
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.0f, 0.46666667f, 0.78431374f, 1.0f);
    // 按钮常态建立操作面的基础层级，为悬浮和按下反馈预留变化空间。
    style.Colors[ImGuiCol_Button] = ImVec4(0.2f, 0.2f, 0.21568628f, 1.0f);
    // 按钮悬浮态只表达指向，不应与已按下状态混淆。
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.11372549f, 0.5921569f, 0.9254902f, 1.0f);
    // 按钮激活态提供按下确认，需要成为三态中最明确的反馈。
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.11372549f, 0.5921569f, 0.9254902f, 1.0f);
    // Header 常态同时服务树节点、列表选项和折叠区标题。
    style.Colors[ImGuiCol_Header] = ImVec4(0.2f, 0.2f, 0.21568628f, 1.0f);
    // Header 悬浮态覆盖较大行区域，应控制亮度避免产生闪烁感。
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.11372549f, 0.5921569f, 0.9254902f, 1.0f);
    // Header 激活态标识当前选择或按压结果，与按钮反馈保持一致。
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.0f, 0.46666667f, 0.78431374f, 1.0f);
    // 分隔线以低对比划分区域，不与可拖动边缘争夺注意力。
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.30588236f, 0.30588236f, 0.30588236f, 1.0f);
    // 可交互分隔线在悬浮时增强，提示其支持调整布局。
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.30588236f, 0.30588236f, 0.30588236f, 1.0f);
    // 分隔线拖动期间使用最强状态，持续反馈布局调整操作。
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.30588236f, 0.30588236f, 0.30588236f, 1.0f);
    // 缩放抓手常态保持含蓄，避免遮挡窗口角落内容。
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.14509805f, 0.14509805f, 0.14901961f, 1.0f);
    // 缩放抓手悬浮态显露可调整方向，强化边角命中提示。
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.2f, 0.2f, 0.21568628f, 1.0f);
    // 缩放抓手激活态维持拖动反馈，直到尺寸调整结束。
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.32156864f, 0.32156864f, 0.33333334f, 1.0f);
    // 普通标签以中等层级承载页面入口，不抢占当前标签的焦点。
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.14509805f, 0.14509805f, 0.14901961f, 1.0f);
    // 标签悬浮色提前提示可切换目标，并需兼容已选中标签的叠加状态。
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.11372549f, 0.5921569f, 0.9254902f, 1.0f);
    // 活动标签与内容面板建立连续关系，明确当前可见页面。
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.0f, 0.46666667f, 0.78431374f, 1.0f);
    // 失焦停靠节点的普通标签进一步降级，突出当前活动窗口组。
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.14509805f, 0.14509805f, 0.14901961f, 1.0f);
    // 失焦组仍需标识其已选页面，因此保留受抑制的活动态。
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.0f, 0.46666667f, 0.78431374f, 1.0f);
    // 折线图常态颜色优先保证细线在背景上的连续可读性。
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.0f, 0.46666667f, 0.78431374f, 1.0f);
    // 折线悬浮态采用明显强调色，便于定位密集曲线中的目标。
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(0.11372549f, 0.5921569f, 0.9254902f, 1.0f);
    // 柱状图使用独立数据色，避免与按钮等交互强调色混淆。
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.0f, 0.46666667f, 0.78431374f, 1.0f);
    // 柱状图悬浮色强化当前样本，但仍需保留原数据系列归属。
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(0.11372549f, 0.5921569f, 0.9254902f, 1.0f);
    // 表头底色固定列语义区域，与数据行建立稳定的纵向起点。
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.1882353f, 0.1882353f, 0.2f, 1.0f);
    // 强表格边框用于外沿和表头分界，承担主要结构线。
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.30980393f, 0.30980393f, 0.34901962f, 1.0f);
    // 弱表格边框只分隔普通单元格，降低密集网格的视觉噪声。
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(0.22745098f, 0.22745098f, 0.24705882f, 1.0f);
    // 基础行背景允许透明继承，使表格自然嵌入所在面板。
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 交替行仅施加轻微差异，帮助横向追踪而不破坏整体底色。
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    // 文本选区背景必须兼顾选中辨识度与前景文字可读性。
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(0.0f, 0.46666667f, 0.78431374f, 1.0f);
    // 拖放目标使用高辨识强调色，明确释放操作将作用的位置。
    style.Colors[ImGuiCol_DragDropTarget] =
        ImVec4(0.14509805f, 0.14509805f, 0.14901961f, 1.0f);
    // 键盘和手柄导航高亮独立于鼠标悬浮态，避免输入来源含混。
    style.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.14509805f, 0.14509805f, 0.14901961f, 1.0f);
    // 窗口导航高亮标识待切换窗口，并覆盖多种面板底色。
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    // 窗口切换期间的遮罩压低非目标内容，但仍保留场景上下文。
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    // 模态遮罩明确阻断下层交互，透明度同时维持来源窗口可见。
    style.Colors[ImGuiCol_ModalWindowDimBg] =
        ImVec4(0.14509805f, 0.14509805f, 0.14901961f, 1.0f);
}

static void applySonicRidersStyle(ImGuiStyle& style)
{
    // Sonic Riders 样式，来源为 ImThemes 的 Sewer56 配色。

    // 全局透明度会乘到整套颜色上，主题在此保持最终合成基准。
    style.Alpha = 1.0f;
    // 禁用态通过统一衰减与可交互控件拉开层级，无需逐项改写颜色。
    style.DisabledAlpha = 0.6f;
    // 窗口内边距同时决定内容密度和停靠面板的呼吸空间。
    style.WindowPadding            = ImVec2(8.0f, 8.0f);
    style.WindowRounding           = 0.0f;
    style.WindowBorderSize         = 0.0f;
    style.WindowMinSize            = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign         = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_Left;
    style.ChildRounding            = 0.0f;
    style.ChildBorderSize          = 1.0f;
    style.PopupRounding            = 0.0f;
    style.PopupBorderSize          = 0.0f;
    // 框体内边距决定输入框和按钮的文字基线及垂直触达面积。
    style.FramePadding    = ImVec2(4.0f, 3.0f);
    style.FrameRounding   = 4.0f;
    style.FrameBorderSize = 0.0f;
    // 项目间距约束同级控件节奏，也是多数自动布局的基础步长。
    style.ItemSpacing      = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
    // 单元格留白保证表格密集展示时文字和分隔线仍有安全距离。
    style.CellPadding       = ImVec2(4.0f, 2.0f);
    style.IndentSpacing     = 21.0f;
    style.ColumnsMinSpacing = 6.0f;
    // 滚动条宽度在可点击性与内容占用之间维持主题取舍。
    style.ScrollbarSize     = 14.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabMinSize       = 10.0f;
    style.GrabRounding      = 4.0f;
    style.TabRounding       = 4.0f;
    style.TabBorderSize     = 0.0f;
    // 选中标签的关闭按钮阈值控制窄页签中操作入口的保留策略。
    style.TabCloseButtonMinWidthSelected = 0.0f;
    // 未选中标签单独设阈值，避免拥挤时关闭图标喧宾夺主。
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    // 正文色承担主要信息层，必须与窗口和框体背景保持足够对比。
    style.Colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // 弱化文字用于不可用或次要信息，但仍需在主背景上可辨识。
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4(0.7294118f, 0.7490196f, 0.7372549f, 1.0f);
    // 窗口底色奠定整套主题明度，后续容器和控件颜色均以此为参照。
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.08627451f, 0.08627451f, 0.08627451f, 0.94f);
    // 子窗口背景可选择透明继承，避免嵌套区域产生多余色块。
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 弹窗背景需要遮住下层内容，同时以透明度保留空间关系。
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.078431375f, 0.078431375f, 0.078431375f, 0.94f);
    // 边框色用于低成本描绘控件和容器边界，透明度决定强调强度。
    style.Colors[ImGuiCol_Border] = ImVec4(0.2f, 0.2f, 0.2f, 0.5f);
    // 边框阴影独立配置，以便主题明确选择平面或浮起效果。
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 框体常态是输入和选择控件的静止基面，也是交互渐变的起点。
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.70980394f, 0.3882353f, 0.3882353f, 0.54f);
    // 框体悬浮色只提示可交互性，应比激活态更克制。
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.8392157f, 0.65882355f, 0.65882355f, 0.4f);
    // 框体激活色确认当前操作焦点，并与悬浮态形成连续反馈。
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.8392157f, 0.65882355f, 0.65882355f, 0.67f);
    // 非活动标题栏降低存在感，让当前编辑窗口更容易被识别。
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.46666667f, 0.21960784f, 0.21960784f, 0.67f);
    // 活动标题栏提高层级，承担当前窗口焦点的主要视觉提示。
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.46666667f, 0.21960784f, 0.21960784f, 1.0f);
    // 折叠标题仍保留窗口身份，但用透明度减少对内容的干扰。
    style.Colors[ImGuiCol_TitleBgCollapsed] =
        ImVec4(0.46666667f, 0.21960784f, 0.21960784f, 0.67f);
    // 菜单栏底色连接窗口标题与内容区，避免形成突兀的横向色带。
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.3372549f, 0.15686275f, 0.15686275f, 1.0f);
    // 滚动轨道应融入容器背景，使抓手成为滚动位置的主要提示。
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.019607844f, 0.019607844f, 0.019607844f, 0.53f);
    // 滚动抓手常态需持续可见，又不能盖过正文和主要操作。
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.30980393f, 0.30980393f, 0.30980393f, 1.0f);
    // 滚动抓手悬浮态提高对比，提示鼠标已经进入可拖动区域。
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.40784314f, 0.40784314f, 0.40784314f, 1.0f);
    // 滚动抓手激活态确认持续拖动，颜色变化需强于悬浮态。
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.50980395f, 0.50980395f, 0.50980395f, 1.0f);
    // 勾选标记承载布尔结果，应从框体背景中清晰跳出。
    style.Colors[ImGuiCol_CheckMark] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // 滑块抓手常态标记当前值位置，并与轨道保持明度区分。
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.70980394f, 0.3882353f, 0.3882353f, 1.0f);
    // 滑块活动抓手强调正在调整的数值，释放后回落至常态。
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.8392157f, 0.65882355f, 0.65882355f, 1.0f);
    // 按钮常态建立操作面的基础层级，为悬浮和按下反馈预留变化空间。
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.46666667f, 0.21960784f, 0.21960784f, 0.65f);
    // 按钮悬浮态只表达指向，不应与已按下状态混淆。
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.70980394f, 0.3882353f, 0.3882353f, 0.65f);
    // 按钮激活态提供按下确认，需要成为三态中最明确的反馈。
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.2f, 0.2f, 0.2f, 0.5f);
    // Header 常态同时服务树节点、列表选项和折叠区标题。
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.70980394f, 0.3882353f, 0.3882353f, 0.54f);
    // Header 悬浮态覆盖较大行区域，应控制亮度避免产生闪烁感。
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.8392157f, 0.65882355f, 0.65882355f, 0.65f);
    // Header 激活态标识当前选择或按压结果，与按钮反馈保持一致。
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.8392157f, 0.65882355f, 0.65882355f, 0.0f);
    // 分隔线以低对比划分区域，不与可拖动边缘争夺注意力。
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.42745098f, 0.42745098f, 0.49803922f, 0.5f);
    // 可交互分隔线在悬浮时增强，提示其支持调整布局。
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.70980394f, 0.3882353f, 0.3882353f, 0.54f);
    // 分隔线拖动期间使用最强状态，持续反馈布局调整操作。
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.70980394f, 0.3882353f, 0.3882353f, 0.54f);
    // 缩放抓手常态保持含蓄，避免遮挡窗口角落内容。
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.70980394f, 0.3882353f, 0.3882353f, 0.54f);
    // 缩放抓手悬浮态显露可调整方向，强化边角命中提示。
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.8392157f, 0.65882355f, 0.65882355f, 0.66f);
    // 缩放抓手激活态维持拖动反馈，直到尺寸调整结束。
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.8392157f, 0.65882355f, 0.65882355f, 0.66f);
    // 普通标签以中等层级承载页面入口，不抢占当前标签的焦点。
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.70980394f, 0.3882353f, 0.3882353f, 0.54f);
    // 标签悬浮色提前提示可切换目标，并需兼容已选中标签的叠加状态。
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.8392157f, 0.65882355f, 0.65882355f, 0.66f);
    // 活动标签与内容面板建立连续关系，明确当前可见页面。
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.8392157f, 0.65882355f, 0.65882355f, 0.66f);
    // 失焦停靠节点的普通标签进一步降级，突出当前活动窗口组。
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.06666667f, 0.09803922f, 0.14901961f, 0.97f);
    // 失焦组仍需标识其已选页面，因此保留受抑制的活动态。
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.13725491f, 0.25882354f, 0.41960785f, 1.0f);
    // 折线图常态颜色优先保证细线在背景上的连续可读性。
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.60784316f, 0.60784316f, 0.60784316f, 1.0f);
    // 折线悬浮态采用明显强调色，便于定位密集曲线中的目标。
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(1.0f, 0.42745098f, 0.34901962f, 1.0f);
    // 柱状图使用独立数据色，避免与按钮等交互强调色混淆。
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.8980392f, 0.69803923f, 0.0f, 1.0f);
    // 柱状图悬浮色强化当前样本，但仍需保留原数据系列归属。
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(1.0f, 0.6f, 0.0f, 1.0f);
    // 表头底色固定列语义区域，与数据行建立稳定的纵向起点。
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.1882353f, 0.1882353f, 0.2f, 1.0f);
    // 强表格边框用于外沿和表头分界，承担主要结构线。
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.30980393f, 0.30980393f, 0.34901962f, 1.0f);
    // 弱表格边框只分隔普通单元格，降低密集网格的视觉噪声。
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(0.22745098f, 0.22745098f, 0.24705882f, 1.0f);
    // 基础行背景允许透明继承，使表格自然嵌入所在面板。
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 交替行仅施加轻微差异，帮助横向追踪而不破坏整体底色。
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    // 文本选区背景必须兼顾选中辨识度与前景文字可读性。
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.35f);
    // 拖放目标使用高辨识强调色，明确释放操作将作用的位置。
    style.Colors[ImGuiCol_DragDropTarget] = ImVec4(1.0f, 1.0f, 0.0f, 0.9f);
    // 键盘和手柄导航高亮独立于鼠标悬浮态，避免输入来源含混。
    style.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.40784314f, 0.40784314f, 0.40784314f, 1.0f);
    // 窗口导航高亮标识待切换窗口，并覆盖多种面板底色。
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    // 窗口切换期间的遮罩压低非目标内容，但仍保留场景上下文。
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    // 模态遮罩明确阻断下层交互，透明度同时维持来源窗口可见。
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.35f);
}

static void applyDarkRudaStyle(ImGuiStyle& style)
{
    // Dark Ruda 样式，来源为 ImThemes 的 Raikiri 配色。

    // 全局透明度会乘到整套颜色上，主题在此保持最终合成基准。
    style.Alpha = 1.0f;
    // 禁用态通过统一衰减与可交互控件拉开层级，无需逐项改写颜色。
    style.DisabledAlpha = 0.6f;
    // 窗口内边距同时决定内容密度和停靠面板的呼吸空间。
    style.WindowPadding            = ImVec2(8.0f, 8.0f);
    style.WindowRounding           = 0.0f;
    style.WindowBorderSize         = 1.0f;
    style.WindowMinSize            = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign         = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_Left;
    style.ChildRounding            = 0.0f;
    style.ChildBorderSize          = 1.0f;
    style.PopupRounding            = 0.0f;
    style.PopupBorderSize          = 1.0f;
    // 框体内边距决定输入框和按钮的文字基线及垂直触达面积。
    style.FramePadding    = ImVec2(4.0f, 3.0f);
    style.FrameRounding   = 4.0f;
    style.FrameBorderSize = 0.0f;
    // 项目间距约束同级控件节奏，也是多数自动布局的基础步长。
    style.ItemSpacing      = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
    // 单元格留白保证表格密集展示时文字和分隔线仍有安全距离。
    style.CellPadding       = ImVec2(4.0f, 2.0f);
    style.IndentSpacing     = 21.0f;
    style.ColumnsMinSpacing = 6.0f;
    // 滚动条宽度在可点击性与内容占用之间维持主题取舍。
    style.ScrollbarSize     = 14.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabMinSize       = 10.0f;
    style.GrabRounding      = 4.0f;
    style.TabRounding       = 4.0f;
    style.TabBorderSize     = 0.0f;
    // 选中标签的关闭按钮阈值控制窄页签中操作入口的保留策略。
    style.TabCloseButtonMinWidthSelected = 0.0f;
    // 未选中标签单独设阈值，避免拥挤时关闭图标喧宾夺主。
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    // 正文色承担主要信息层，必须与窗口和框体背景保持足够对比。
    style.Colors[ImGuiCol_Text] =
        ImVec4(0.9490196f, 0.95686275f, 0.9764706f, 1.0f);
    // 弱化文字用于不可用或次要信息，但仍需在主背景上可辨识。
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4(0.35686275f, 0.41960785f, 0.46666667f, 1.0f);
    // 窗口底色奠定整套主题明度，后续容器和控件颜色均以此为参照。
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.10980392f, 0.14901961f, 0.16862746f, 1.0f);
    // 子窗口背景可选择透明继承，避免嵌套区域产生多余色块。
    style.Colors[ImGuiCol_ChildBg] =
        ImVec4(0.14901961f, 0.1764706f, 0.21960784f, 1.0f);
    // 弹窗背景需要遮住下层内容，同时以透明度保留空间关系。
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.078431375f, 0.078431375f, 0.078431375f, 0.94f);
    // 边框色用于低成本描绘控件和容器边界，透明度决定强调强度。
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.078431375f, 0.09803922f, 0.11764706f, 1.0f);
    // 边框阴影独立配置，以便主题明确选择平面或浮起效果。
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 框体常态是输入和选择控件的静止基面，也是交互渐变的起点。
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.2f, 0.24705882f, 0.28627452f, 1.0f);
    // 框体悬浮色只提示可交互性，应比激活态更克制。
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.11764706f, 0.2f, 0.2784314f, 1.0f);
    // 框体激活色确认当前操作焦点，并与悬浮态形成连续反馈。
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.08627451f, 0.11764706f, 0.13725491f, 1.0f);
    // 非活动标题栏降低存在感，让当前编辑窗口更容易被识别。
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.08627451f, 0.11764706f, 0.13725491f, 0.65f);
    // 活动标题栏提高层级，承担当前窗口焦点的主要视觉提示。
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.078431375f, 0.09803922f, 0.11764706f, 1.0f);
    // 折叠标题仍保留窗口身份，但用透明度减少对内容的干扰。
    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.0f, 0.0f, 0.0f, 0.51f);
    // 菜单栏底色连接窗口标题与内容区，避免形成突兀的横向色带。
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.14901961f, 0.1764706f, 0.21960784f, 1.0f);
    // 滚动轨道应融入容器背景，使抓手成为滚动位置的主要提示。
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.019607844f, 0.019607844f, 0.019607844f, 0.39f);
    // 滚动抓手常态需持续可见，又不能盖过正文和主要操作。
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.2f, 0.24705882f, 0.28627452f, 1.0f);
    // 滚动抓手悬浮态提高对比，提示鼠标已经进入可拖动区域。
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.1764706f, 0.21960784f, 0.24705882f, 1.0f);
    // 滚动抓手激活态确认持续拖动，颜色变化需强于悬浮态。
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.08627451f, 0.20784314f, 0.30980393f, 1.0f);
    // 勾选标记承载布尔结果，应从框体背景中清晰跳出。
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.2784314f, 0.5568628f, 1.0f, 1.0f);
    // 滑块抓手常态标记当前值位置，并与轨道保持明度区分。
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.2784314f, 0.5568628f, 1.0f, 1.0f);
    // 滑块活动抓手强调正在调整的数值，释放后回落至常态。
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.36862746f, 0.60784316f, 1.0f, 1.0f);
    // 按钮常态建立操作面的基础层级，为悬浮和按下反馈预留变化空间。
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.2f, 0.24705882f, 0.28627452f, 1.0f);
    // 按钮悬浮态只表达指向，不应与已按下状态混淆。
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.2784314f, 0.5568628f, 1.0f, 1.0f);
    // 按钮激活态提供按下确认，需要成为三态中最明确的反馈。
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.05882353f, 0.5294118f, 0.9764706f, 1.0f);
    // Header 常态同时服务树节点、列表选项和折叠区标题。
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.2f, 0.24705882f, 0.28627452f, 0.55f);
    // Header 悬浮态覆盖较大行区域，应控制亮度避免产生闪烁感。
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.8f);
    // Header 激活态标识当前选择或按压结果，与按钮反馈保持一致。
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    // 分隔线以低对比划分区域，不与可拖动边缘争夺注意力。
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.2f, 0.24705882f, 0.28627452f, 1.0f);
    // 可交互分隔线在悬浮时增强，提示其支持调整布局。
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.09803922f, 0.4f, 0.7490196f, 0.78f);
    // 分隔线拖动期间使用最强状态，持续反馈布局调整操作。
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.09803922f, 0.4f, 0.7490196f, 1.0f);
    // 缩放抓手常态保持含蓄，避免遮挡窗口角落内容。
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.25f);
    // 缩放抓手悬浮态显露可调整方向，强化边角命中提示。
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.67f);
    // 缩放抓手激活态维持拖动反馈，直到尺寸调整结束。
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.95f);
    // 普通标签以中等层级承载页面入口，不抢占当前标签的焦点。
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.10980392f, 0.14901961f, 0.16862746f, 1.0f);
    // 标签悬浮色提前提示可切换目标，并需兼容已选中标签的叠加状态。
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.8f);
    // 活动标签与内容面板建立连续关系，明确当前可见页面。
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.2f, 0.24705882f, 0.28627452f, 1.0f);
    // 失焦停靠节点的普通标签进一步降级，突出当前活动窗口组。
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.10980392f, 0.14901961f, 0.16862746f, 1.0f);
    // 失焦组仍需标识其已选页面，因此保留受抑制的活动态。
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.10980392f, 0.14901961f, 0.16862746f, 1.0f);
    // 折线图常态颜色优先保证细线在背景上的连续可读性。
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.60784316f, 0.60784316f, 0.60784316f, 1.0f);
    // 折线悬浮态采用明显强调色，便于定位密集曲线中的目标。
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(1.0f, 0.42745098f, 0.34901962f, 1.0f);
    // 柱状图使用独立数据色，避免与按钮等交互强调色混淆。
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.8980392f, 0.69803923f, 0.0f, 1.0f);
    // 柱状图悬浮色强化当前样本，但仍需保留原数据系列归属。
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(1.0f, 0.6f, 0.0f, 1.0f);
    // 表头底色固定列语义区域，与数据行建立稳定的纵向起点。
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.1882353f, 0.1882353f, 0.2f, 1.0f);
    // 强表格边框用于外沿和表头分界，承担主要结构线。
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.30980393f, 0.30980393f, 0.34901962f, 1.0f);
    // 弱表格边框只分隔普通单元格，降低密集网格的视觉噪声。
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(0.22745098f, 0.22745098f, 0.24705882f, 1.0f);
    // 基础行背景允许透明继承，使表格自然嵌入所在面板。
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 交替行仅施加轻微差异，帮助横向追踪而不破坏整体底色。
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    // 文本选区背景必须兼顾选中辨识度与前景文字可读性。
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.35f);
    // 拖放目标使用高辨识强调色，明确释放操作将作用的位置。
    style.Colors[ImGuiCol_DragDropTarget] = ImVec4(1.0f, 1.0f, 0.0f, 0.9f);
    // 键盘和手柄导航高亮独立于鼠标悬浮态，避免输入来源含混。
    style.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    // 窗口导航高亮标识待切换窗口，并覆盖多种面板底色。
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    // 窗口切换期间的遮罩压低非目标内容，但仍保留场景上下文。
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    // 模态遮罩明确阻断下层交互，透明度同时维持来源窗口可见。
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.35f);
}

static void applySoftCherryStyle(ImGuiStyle& style)
{
    // Soft Cherry 样式，来源为 ImThemes 的 Patitotective 配色。

    // 全局透明度会乘到整套颜色上，主题在此保持最终合成基准。
    style.Alpha = 1.0f;
    // 禁用态通过统一衰减与可交互控件拉开层级，无需逐项改写颜色。
    style.DisabledAlpha = 0.4f;
    // 窗口内边距同时决定内容密度和停靠面板的呼吸空间。
    style.WindowPadding            = ImVec2(10.0f, 10.0f);
    style.WindowRounding           = 4.0f;
    style.WindowBorderSize         = 0.0f;
    style.WindowMinSize            = ImVec2(50.0f, 50.0f);
    style.WindowTitleAlign         = ImVec2(0.5f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_Left;
    style.ChildRounding            = 0.0f;
    style.ChildBorderSize          = 1.0f;
    style.PopupRounding            = 1.0f;
    style.PopupBorderSize          = 1.0f;
    // 框体内边距决定输入框和按钮的文字基线及垂直触达面积。
    style.FramePadding    = ImVec2(5.0f, 3.0f);
    style.FrameRounding   = 3.0f;
    style.FrameBorderSize = 0.0f;
    // 项目间距约束同级控件节奏，也是多数自动布局的基础步长。
    style.ItemSpacing      = ImVec2(6.0f, 6.0f);
    style.ItemInnerSpacing = ImVec2(3.0f, 2.0f);
    // 单元格留白保证表格密集展示时文字和分隔线仍有安全距离。
    style.CellPadding       = ImVec2(3.0f, 3.0f);
    style.IndentSpacing     = 6.0f;
    style.ColumnsMinSpacing = 6.0f;
    // 滚动条宽度在可点击性与内容占用之间维持主题取舍。
    style.ScrollbarSize     = 13.0f;
    style.ScrollbarRounding = 16.0f;
    style.GrabMinSize       = 20.0f;
    style.GrabRounding      = 4.0f;
    style.TabRounding       = 4.0f;
    style.TabBorderSize     = 1.0f;
    // 选中标签的关闭按钮阈值控制窄页签中操作入口的保留策略。
    style.TabCloseButtonMinWidthSelected = 0.0f;
    // 未选中标签单独设阈值，避免拥挤时关闭图标喧宾夺主。
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    // 正文色承担主要信息层，必须与窗口和框体背景保持足够对比。
    style.Colors[ImGuiCol_Text] =
        ImVec4(0.85882354f, 0.92941177f, 0.8862745f, 1.0f);
    // 弱化文字用于不可用或次要信息，但仍需在主背景上可辨识。
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4(0.52156866f, 0.54901963f, 0.53333336f, 1.0f);
    // 窗口底色奠定整套主题明度，后续容器和控件颜色均以此为参照。
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.12941177f, 0.13725491f, 0.16862746f, 1.0f);
    // 子窗口背景可选择透明继承，避免嵌套区域产生多余色块。
    style.Colors[ImGuiCol_ChildBg] =
        ImVec4(0.14901961f, 0.15686275f, 0.1882353f, 1.0f);
    // 弹窗背景需要遮住下层内容，同时以透明度保留空间关系。
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.2f, 0.21960784f, 0.26666668f, 1.0f);
    // 边框色用于低成本描绘控件和容器边界，透明度决定强调强度。
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.13725491f, 0.11372549f, 0.13333334f, 1.0f);
    // 边框阴影独立配置，以便主题明确选择平面或浮起效果。
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    // 框体常态是输入和选择控件的静止基面，也是交互渐变的起点。
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.16862746f, 0.18431373f, 0.23137255f, 1.0f);
    // 框体悬浮色只提示可交互性，应比激活态更克制。
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    // 框体激活色确认当前操作焦点，并与悬浮态形成连续反馈。
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    // 非活动标题栏降低存在感，让当前编辑窗口更容易被识别。
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.23137255f, 0.2f, 0.27058825f, 1.0f);
    // 活动标题栏提高层级，承担当前窗口焦点的主要视觉提示。
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.5019608f, 0.07450981f, 0.25490198f, 1.0f);
    // 折叠标题仍保留窗口身份，但用透明度减少对内容的干扰。
    style.Colors[ImGuiCol_TitleBgCollapsed] =
        ImVec4(0.2f, 0.21960784f, 0.26666668f, 1.0f);
    // 菜单栏底色连接窗口标题与内容区，避免形成突兀的横向色带。
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.2f, 0.21960784f, 0.26666668f, 1.0f);
    // 滚动轨道应融入容器背景，使抓手成为滚动位置的主要提示。
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.23921569f, 0.23921569f, 0.21960784f, 1.0f);
    // 滚动抓手常态需持续可见，又不能盖过正文和主要操作。
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.3882353f, 0.3882353f, 0.37254903f, 1.0f);
    // 滚动抓手悬浮态提高对比，提示鼠标已经进入可拖动区域。
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.69411767f, 0.69411767f, 0.6862745f, 1.0f);
    // 滚动抓手激活态确认持续拖动，颜色变化需强于悬浮态。
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.69411767f, 0.69411767f, 0.6862745f, 1.0f);
    // 勾选标记承载布尔结果，应从框体背景中清晰跳出。
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.65882355f, 0.13725491f, 0.1764706f, 1.0f);
    // 滑块抓手常态标记当前值位置，并与轨道保持明度区分。
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.6509804f, 0.14901961f, 0.34509805f, 1.0f);
    // 滑块活动抓手强调正在调整的数值，释放后回落至常态。
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.70980394f, 0.21960784f, 0.26666668f, 1.0f);
    // 按钮常态建立操作面的基础层级，为悬浮和按下反馈预留变化空间。
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.6509804f, 0.14901961f, 0.34509805f, 1.0f);
    // 按钮悬浮态只表达指向，不应与已按下状态混淆。
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    // 按钮激活态提供按下确认，需要成为三态中最明确的反馈。
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    // Header 常态同时服务树节点、列表选项和折叠区标题。
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    // Header 悬浮态覆盖较大行区域，应控制亮度避免产生闪烁感。
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.6509804f, 0.14901961f, 0.34509805f, 1.0f);
    // Header 激活态标识当前选择或按压结果，与按钮反馈保持一致。
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.5019608f, 0.07450981f, 0.25490198f, 1.0f);
    // 分隔线以低对比划分区域，不与可拖动边缘争夺注意力。
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.42745098f, 0.42745098f, 0.49803922f, 1.0f);
    // 可交互分隔线在悬浮时增强，提示其支持调整布局。
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.09803922f, 0.4f, 0.7490196f, 1.0f);
    // 分隔线拖动期间使用最强状态，持续反馈布局调整操作。
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.09803922f, 0.4f, 0.7490196f, 1.0f);
    // 缩放抓手常态保持含蓄，避免遮挡窗口角落内容。
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.6509804f, 0.14901961f, 0.34509805f, 1.0f);
    // 缩放抓手悬浮态显露可调整方向，强化边角命中提示。
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    // 缩放抓手激活态维持拖动反馈，直到尺寸调整结束。
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    // 普通标签以中等层级承载页面入口，不抢占当前标签的焦点。
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.1764706f, 0.34901962f, 0.5764706f, 1.0f);
    // 标签悬浮色提前提示可切换目标，并需兼容已选中标签的叠加状态。
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    // 活动标签与内容面板建立连续关系，明确当前可见页面。
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.19607843f, 0.40784314f, 0.6784314f, 1.0f);
    // 失焦停靠节点的普通标签进一步降级，突出当前活动窗口组。
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.06666667f, 0.101960786f, 0.14509805f, 1.0f);
    // 失焦组仍需标识其已选页面，因此保留受抑制的活动态。
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.13333334f, 0.25882354f, 0.42352942f, 1.0f);
    // 折线图常态颜色优先保证细线在背景上的连续可读性。
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.85882354f, 0.92941177f, 0.8862745f, 1.0f);
    // 折线悬浮态采用明显强调色，便于定位密集曲线中的目标。
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    // 柱状图使用独立数据色，避免与按钮等交互强调色混淆。
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.30980393f, 0.7764706f, 0.19607843f, 1.0f);
    // 柱状图悬浮色强化当前样本，但仍需保留原数据系列归属。
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    // 表头底色固定列语义区域，与数据行建立稳定的纵向起点。
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.1882353f, 0.1882353f, 0.2f, 1.0f);
    // 强表格边框用于外沿和表头分界，承担主要结构线。
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.30980393f, 0.30980393f, 0.34901962f, 1.0f);
    // 弱表格边框只分隔普通单元格，降低密集网格的视觉噪声。
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(0.22745098f, 0.22745098f, 0.24705882f, 1.0f);
    // 基础行背景允许透明继承，使表格自然嵌入所在面板。
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    // 交替行仅施加轻微差异，帮助横向追踪而不破坏整体底色。
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // 文本选区背景必须兼顾选中辨识度与前景文字可读性。
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(0.38431373f, 0.627451f, 0.91764706f, 1.0f);
    // 拖放目标使用高辨识强调色，明确释放操作将作用的位置。
    style.Colors[ImGuiCol_DragDropTarget] = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
    // 键盘和手柄导航高亮独立于鼠标悬浮态，避免输入来源含混。
    style.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    // 窗口导航高亮标识待切换窗口，并覆盖多种面板底色。
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // 窗口切换期间的遮罩压低非目标内容，但仍保留场景上下文。
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
    // 模态遮罩明确阻断下层交互，透明度同时维持来源窗口可见。
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.3f);
}

static void applyEnemymouseStyle(ImGuiStyle& style)
{
    // Enemymouse 样式，来源为 ImThemes 的 enemymouse 配色。

    // 全局透明度会乘到整套颜色上，主题在此保持最终合成基准。
    style.Alpha = 1.0f;
    // 禁用态通过统一衰减与可交互控件拉开层级，无需逐项改写颜色。
    style.DisabledAlpha = 0.6f;
    // 窗口内边距同时决定内容密度和停靠面板的呼吸空间。
    style.WindowPadding            = ImVec2(8.0f, 8.0f);
    style.WindowRounding           = 3.0f;
    style.WindowBorderSize         = 1.0f;
    style.WindowMinSize            = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign         = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_Left;
    style.ChildRounding            = 3.0f;
    style.ChildBorderSize          = 1.0f;
    style.PopupRounding            = 0.0f;
    style.PopupBorderSize          = 1.0f;
    // 框体内边距决定输入框和按钮的文字基线及垂直触达面积。
    style.FramePadding    = ImVec2(4.0f, 3.0f);
    style.FrameRounding   = 3.0f;
    style.FrameBorderSize = 0.0f;
    // 项目间距约束同级控件节奏，也是多数自动布局的基础步长。
    style.ItemSpacing      = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
    // 单元格留白保证表格密集展示时文字和分隔线仍有安全距离。
    style.CellPadding       = ImVec2(4.0f, 2.0f);
    style.IndentSpacing     = 21.0f;
    style.ColumnsMinSpacing = 6.0f;
    // 滚动条宽度在可点击性与内容占用之间维持主题取舍。
    style.ScrollbarSize     = 14.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabMinSize       = 20.0f;
    style.GrabRounding      = 1.0f;
    style.TabRounding       = 4.0f;
    style.TabBorderSize     = 0.0f;
    // 选中标签的关闭按钮阈值控制窄页签中操作入口的保留策略。
    style.TabCloseButtonMinWidthSelected = 0.0f;
    // 未选中标签单独设阈值，避免拥挤时关闭图标喧宾夺主。
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    // 正文色承担主要信息层，必须与窗口和框体背景保持足够对比。
    style.Colors[ImGuiCol_Text] = ImVec4(0.0f, 1.0f, 1.0f, 1.0f);
    // 弱化文字用于不可用或次要信息，但仍需在主背景上可辨识。
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.0f, 0.4f, 0.40784314f, 1.0f);
    // 窗口底色奠定整套主题明度，后续容器和控件颜色均以此为参照。
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.83f);
    // 子窗口背景可选择透明继承，避免嵌套区域产生多余色块。
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 弹窗背景需要遮住下层内容，同时以透明度保留空间关系。
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.15686275f, 0.23921569f, 0.21960784f, 0.6f);
    // 边框色用于低成本描绘控件和容器边界，透明度决定强调强度。
    style.Colors[ImGuiCol_Border] = ImVec4(0.0f, 1.0f, 1.0f, 0.65f);
    // 边框阴影独立配置，以便主题明确选择平面或浮起效果。
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 框体常态是输入和选择控件的静止基面，也是交互渐变的起点。
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.4392157f, 0.8f, 0.8f, 0.18f);
    // 框体悬浮色只提示可交互性，应比激活态更克制。
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.4392157f, 0.8f, 0.8f, 0.27f);
    // 框体激活色确认当前操作焦点，并与悬浮态形成连续反馈。
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.4392157f, 0.80784315f, 0.85882354f, 0.66f);
    // 非活动标题栏降低存在感，让当前编辑窗口更容易被识别。
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.13725491f, 0.1764706f, 0.20784314f, 0.73f);
    // 活动标题栏提高层级，承担当前窗口焦点的主要视觉提示。
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.0f, 1.0f, 1.0f, 0.27f);
    // 折叠标题仍保留窗口身份，但用透明度减少对内容的干扰。
    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.0f, 0.0f, 0.0f, 0.54f);
    // 菜单栏底色连接窗口标题与内容区，避免形成突兀的横向色带。
    style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.2f);
    // 滚动轨道应融入容器背景，使抓手成为滚动位置的主要提示。
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.21960784f, 0.28627452f, 0.29803923f, 0.71f);
    // 滚动抓手常态需持续可见，又不能盖过正文和主要操作。
    style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.0f, 1.0f, 1.0f, 0.44f);
    // 滚动抓手悬浮态提高对比，提示鼠标已经进入可拖动区域。
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.0f, 1.0f, 1.0f, 0.74f);
    // 滚动抓手激活态确认持续拖动，颜色变化需强于悬浮态。
    style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.0f, 1.0f, 1.0f, 1.0f);
    // 勾选标记承载布尔结果，应从框体背景中清晰跳出。
    style.Colors[ImGuiCol_CheckMark] = ImVec4(0.0f, 1.0f, 1.0f, 0.68f);
    // 滑块抓手常态标记当前值位置，并与轨道保持明度区分。
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.0f, 1.0f, 1.0f, 0.36f);
    // 滑块活动抓手强调正在调整的数值，释放后回落至常态。
    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.0f, 1.0f, 1.0f, 0.76f);
    // 按钮常态建立操作面的基础层级，为悬浮和按下反馈预留变化空间。
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.0f, 0.64705884f, 0.64705884f, 0.46f);
    // 按钮悬浮态只表达指向，不应与已按下状态混淆。
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.007843138f, 1.0f, 1.0f, 0.43f);
    // 按钮激活态提供按下确认，需要成为三态中最明确的反馈。
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.0f, 1.0f, 1.0f, 0.62f);
    // Header 常态同时服务树节点、列表选项和折叠区标题。
    style.Colors[ImGuiCol_Header] = ImVec4(0.0f, 1.0f, 1.0f, 0.33f);
    // Header 悬浮态覆盖较大行区域，应控制亮度避免产生闪烁感。
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.0f, 1.0f, 1.0f, 0.42f);
    // Header 激活态标识当前选择或按压结果，与按钮反馈保持一致。
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.0f, 1.0f, 1.0f, 0.54f);
    // 分隔线以低对比划分区域，不与可拖动边缘争夺注意力。
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.0f, 0.49803922f, 0.49803922f, 0.33f);
    // 可交互分隔线在悬浮时增强，提示其支持调整布局。
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.0f, 0.49803922f, 0.49803922f, 0.47f);
    // 分隔线拖动期间使用最强状态，持续反馈布局调整操作。
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.0f, 0.69803923f, 0.69803923f, 1.0f);
    // 缩放抓手常态保持含蓄，避免遮挡窗口角落内容。
    style.Colors[ImGuiCol_ResizeGrip] = ImVec4(0.0f, 1.0f, 1.0f, 0.54f);
    // 缩放抓手悬浮态显露可调整方向，强化边角命中提示。
    style.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.0f, 1.0f, 1.0f, 0.74f);
    // 缩放抓手激活态维持拖动反馈，直到尺寸调整结束。
    style.Colors[ImGuiCol_ResizeGripActive] = ImVec4(0.0f, 1.0f, 1.0f, 1.0f);
    // 普通标签以中等层级承载页面入口，不抢占当前标签的焦点。
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.1764706f, 0.34901962f, 0.5764706f, 0.862f);
    // 标签悬浮色提前提示可切换目标，并需兼容已选中标签的叠加状态。
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.8f);
    // 活动标签与内容面板建立连续关系，明确当前可见页面。
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.19607843f, 0.40784314f, 0.6784314f, 1.0f);
    // 失焦停靠节点的普通标签进一步降级，突出当前活动窗口组。
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.06666667f, 0.101960786f, 0.14509805f, 0.9724f);
    // 失焦组仍需标识其已选页面，因此保留受抑制的活动态。
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.13333334f, 0.25882354f, 0.42352942f, 1.0f);
    // 折线图常态颜色优先保证细线在背景上的连续可读性。
    style.Colors[ImGuiCol_PlotLines] = ImVec4(0.0f, 1.0f, 1.0f, 1.0f);
    // 折线悬浮态采用明显强调色，便于定位密集曲线中的目标。
    style.Colors[ImGuiCol_PlotLinesHovered] = ImVec4(0.0f, 1.0f, 1.0f, 1.0f);
    // 柱状图使用独立数据色，避免与按钮等交互强调色混淆。
    style.Colors[ImGuiCol_PlotHistogram] = ImVec4(0.0f, 1.0f, 1.0f, 1.0f);
    // 柱状图悬浮色强化当前样本，但仍需保留原数据系列归属。
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(0.0f, 1.0f, 1.0f, 1.0f);
    // 表头底色固定列语义区域，与数据行建立稳定的纵向起点。
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.1882353f, 0.1882353f, 0.2f, 1.0f);
    // 强表格边框用于外沿和表头分界，承担主要结构线。
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.30980393f, 0.30980393f, 0.34901962f, 1.0f);
    // 弱表格边框只分隔普通单元格，降低密集网格的视觉噪声。
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(0.22745098f, 0.22745098f, 0.24705882f, 1.0f);
    // 基础行背景允许透明继承，使表格自然嵌入所在面板。
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 交替行仅施加轻微差异，帮助横向追踪而不破坏整体底色。
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    // 文本选区背景必须兼顾选中辨识度与前景文字可读性。
    style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.0f, 1.0f, 1.0f, 0.22f);
    // 拖放目标使用高辨识强调色，明确释放操作将作用的位置。
    style.Colors[ImGuiCol_DragDropTarget] = ImVec4(1.0f, 1.0f, 0.0f, 0.9f);
    // 键盘和手柄导航高亮独立于鼠标悬浮态，避免输入来源含混。
    style.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    // 窗口导航高亮标识待切换窗口，并覆盖多种面板底色。
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    // 窗口切换期间的遮罩压低非目标内容，但仍保留场景上下文。
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    // 模态遮罩明确阻断下层交互，透明度同时维持来源窗口可见。
    style.Colors[ImGuiCol_ModalWindowDimBg] =
        ImVec4(0.039215688f, 0.09803922f, 0.08627451f, 0.51f);
}

static void applyDiscordDarkStyle(ImGuiStyle& style)
{
    // Discord (Dark) 样式，来源为 ImThemes 的 BttrDrgn 配色。

    // 全局透明度会乘到整套颜色上，主题在此保持最终合成基准。
    style.Alpha = 1.0f;
    // 禁用态通过统一衰减与可交互控件拉开层级，无需逐项改写颜色。
    style.DisabledAlpha = 0.6f;
    // 窗口内边距同时决定内容密度和停靠面板的呼吸空间。
    style.WindowPadding            = ImVec2(8.0f, 8.0f);
    style.WindowRounding           = 0.0f;
    style.WindowBorderSize         = 1.0f;
    style.WindowMinSize            = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign         = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_Left;
    style.ChildRounding            = 0.0f;
    style.ChildBorderSize          = 1.0f;
    style.PopupRounding            = 0.0f;
    style.PopupBorderSize          = 1.0f;
    // 框体内边距决定输入框和按钮的文字基线及垂直触达面积。
    style.FramePadding    = ImVec2(4.0f, 3.0f);
    style.FrameRounding   = 0.0f;
    style.FrameBorderSize = 0.0f;
    // 项目间距约束同级控件节奏，也是多数自动布局的基础步长。
    style.ItemSpacing      = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
    // 单元格留白保证表格密集展示时文字和分隔线仍有安全距离。
    style.CellPadding       = ImVec2(4.0f, 2.0f);
    style.IndentSpacing     = 21.0f;
    style.ColumnsMinSpacing = 6.0f;
    // 滚动条宽度在可点击性与内容占用之间维持主题取舍。
    style.ScrollbarSize     = 14.0f;
    style.ScrollbarRounding = 0.0f;
    style.GrabMinSize       = 10.0f;
    style.GrabRounding      = 0.0f;
    style.TabRounding       = 0.0f;
    style.TabBorderSize     = 0.0f;
    // 选中标签的关闭按钮阈值控制窄页签中操作入口的保留策略。
    style.TabCloseButtonMinWidthSelected = 0.0f;
    // 未选中标签单独设阈值，避免拥挤时关闭图标喧宾夺主。
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    // 正文色承担主要信息层，必须与窗口和框体背景保持足够对比。
    style.Colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // 弱化文字用于不可用或次要信息，但仍需在主背景上可辨识。
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4(0.49803922f, 0.49803922f, 0.49803922f, 1.0f);
    // 窗口底色奠定整套主题明度，后续容器和控件颜色均以此为参照。
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.21176471f, 0.22352941f, 0.24705882f, 1.0f);
    // 子窗口背景可选择透明继承，避免嵌套区域产生多余色块。
    style.Colors[ImGuiCol_ChildBg] =
        ImVec4(0.18431373f, 0.19215687f, 0.21176471f, 1.0f);
    // 弹窗背景需要遮住下层内容，同时以透明度保留空间关系。
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.078431375f, 0.078431375f, 0.078431375f, 0.94f);
    // 边框色用于低成本描绘控件和容器边界，透明度决定强调强度。
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.42745098f, 0.42745098f, 0.49803922f, 0.5f);
    // 边框阴影独立配置，以便主题明确选择平面或浮起效果。
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 框体常态是输入和选择控件的静止基面，也是交互渐变的起点。
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.30980393f, 0.32941177f, 0.36078432f, 1.0f);
    // 框体悬浮色只提示可交互性，应比激活态更克制。
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.30980393f, 0.32941177f, 0.36078432f, 1.0f);
    // 框体激活色确认当前操作焦点，并与悬浮态形成连续反馈。
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.34509805f, 0.39607844f, 0.9490196f, 1.0f);
    // 非活动标题栏降低存在感，让当前编辑窗口更容易被识别。
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.18431373f, 0.19215687f, 0.21176471f, 1.0f);
    // 活动标题栏提高层级，承担当前窗口焦点的主要视觉提示。
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.1254902f, 0.13333334f, 0.14509805f, 1.0f);
    // 折叠标题仍保留窗口身份，但用透明度减少对内容的干扰。
    style.Colors[ImGuiCol_TitleBgCollapsed] =
        ImVec4(0.1254902f, 0.13333334f, 0.14509805f, 1.0f);
    // 菜单栏底色连接窗口标题与内容区，避免形成突兀的横向色带。
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.1254902f, 0.13333334f, 0.14509805f, 1.0f);
    // 滚动轨道应融入容器背景，使抓手成为滚动位置的主要提示。
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.019607844f, 0.019607844f, 0.019607844f, 0.53f);
    // 滚动抓手常态需持续可见，又不能盖过正文和主要操作。
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.30980393f, 0.30980393f, 0.30980393f, 1.0f);
    // 滚动抓手悬浮态提高对比，提示鼠标已经进入可拖动区域。
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.40784314f, 0.40784314f, 0.40784314f, 1.0f);
    // 滚动抓手激活态确认持续拖动，颜色变化需强于悬浮态。
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.50980395f, 0.50980395f, 0.50980395f, 1.0f);
    // 勾选标记承载布尔结果，应从框体背景中清晰跳出。
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.23137255f, 0.64705884f, 0.3647059f, 1.0f);
    // 滑块抓手常态标记当前值位置，并与轨道保持明度区分。
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // 滑块活动抓手强调正在调整的数值，释放后回落至常态。
    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // 按钮常态建立操作面的基础层级，为悬浮和按下反馈预留变化空间。
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.30980393f, 0.32941177f, 0.36078432f, 1.0f);
    // 按钮悬浮态只表达指向，不应与已按下状态混淆。
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.40784314f, 0.42745098f, 0.4509804f, 1.0f);
    // 按钮激活态提供按下确认，需要成为三态中最明确的反馈。
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.1254902f, 0.13333334f, 0.14509805f, 1.0f);
    // Header 常态同时服务树节点、列表选项和折叠区标题。
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.30980393f, 0.32941177f, 0.36078432f, 1.0f);
    // Header 悬浮态覆盖较大行区域，应控制亮度避免产生闪烁感。
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.40784314f, 0.42745098f, 0.4509804f, 1.0f);
    // Header 激活态标识当前选择或按压结果，与按钮反馈保持一致。
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.40784314f, 0.42745098f, 0.4509804f, 1.0f);
    // 分隔线以低对比划分区域，不与可拖动边缘争夺注意力。
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.42745098f, 0.42745098f, 0.49803922f, 0.5f);
    // 可交互分隔线在悬浮时增强，提示其支持调整布局。
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.09803922f, 0.4f, 0.7490196f, 0.78f);
    // 分隔线拖动期间使用最强状态，持续反馈布局调整操作。
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.09803922f, 0.4f, 0.7490196f, 1.0f);
    // 缩放抓手常态保持含蓄，避免遮挡窗口角落内容。
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.2f);
    // 缩放抓手悬浮态显露可调整方向，强化边角命中提示。
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.67f);
    // 缩放抓手激活态维持拖动反馈，直到尺寸调整结束。
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.95f);
    // 普通标签以中等层级承载页面入口，不抢占当前标签的焦点。
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.18431373f, 0.19215687f, 0.21176471f, 1.0f);
    // 标签悬浮色提前提示可切换目标，并需兼容已选中标签的叠加状态。
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.23529412f, 0.24705882f, 0.27058825f, 1.0f);
    // 活动标签与内容面板建立连续关系，明确当前可见页面。
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.25882354f, 0.27450982f, 0.3019608f, 1.0f);
    // 失焦停靠节点的普通标签进一步降级，突出当前活动窗口组。
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.06666667f, 0.101960786f, 0.14509805f, 0.9724f);
    // 失焦组仍需标识其已选页面，因此保留受抑制的活动态。
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.13333334f, 0.25882354f, 0.42352942f, 1.0f);
    // 折线图常态颜色优先保证细线在背景上的连续可读性。
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.60784316f, 0.60784316f, 0.60784316f, 1.0f);
    // 折线悬浮态采用明显强调色，便于定位密集曲线中的目标。
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(0.34509805f, 0.39607844f, 0.9490196f, 1.0f);
    // 柱状图使用独立数据色，避免与按钮等交互强调色混淆。
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.34509805f, 0.39607844f, 0.9490196f, 1.0f);
    // 柱状图悬浮色强化当前样本，但仍需保留原数据系列归属。
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(0.36078432f, 0.4f, 0.42745098f, 1.0f);
    // 表头底色固定列语义区域，与数据行建立稳定的纵向起点。
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.1882353f, 0.1882353f, 0.2f, 1.0f);
    // 强表格边框用于外沿和表头分界，承担主要结构线。
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.30980393f, 0.30980393f, 0.34901962f, 1.0f);
    // 弱表格边框只分隔普通单元格，降低密集网格的视觉噪声。
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(0.22745098f, 0.22745098f, 0.24705882f, 1.0f);
    // 基础行背景允许透明继承，使表格自然嵌入所在面板。
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 交替行仅施加轻微差异，帮助横向追踪而不破坏整体底色。
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    // 文本选区背景必须兼顾选中辨识度与前景文字可读性。
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(0.050980393f, 0.41960785f, 0.85882354f, 1.0f);
    // 拖放目标使用高辨识强调色，明确释放操作将作用的位置。
    style.Colors[ImGuiCol_DragDropTarget] =
        ImVec4(0.34509805f, 0.39607844f, 0.9490196f, 1.0f);
    // 键盘和手柄导航高亮独立于鼠标悬浮态，避免输入来源含混。
    style.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    // 窗口导航高亮标识待切换窗口，并覆盖多种面板底色。
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    // 窗口切换期间的遮罩压低非目标内容，但仍保留场景上下文。
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    // 模态遮罩明确阻断下层交互，透明度同时维持来源窗口可见。
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.35f);
}

static void applyComfyStyle(ImGuiStyle& style)
{
    // Comfy 样式，来源为 ImThemes 的 Giuseppe 配色。

    // 全局透明度会乘到整套颜色上，主题在此保持最终合成基准。
    style.Alpha = 1.0f;
    // 禁用态通过统一衰减与可交互控件拉开层级，无需逐项改写颜色。
    style.DisabledAlpha = 0.1f;
    // 窗口内边距同时决定内容密度和停靠面板的呼吸空间。
    style.WindowPadding            = ImVec2(8.0f, 8.0f);
    style.WindowRounding           = 10.0f;
    style.WindowBorderSize         = 0.0f;
    style.WindowMinSize            = ImVec2(30.0f, 30.0f);
    style.WindowTitleAlign         = ImVec2(0.5f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_Right;
    style.ChildRounding            = 5.0f;
    style.ChildBorderSize          = 1.0f;
    style.PopupRounding            = 10.0f;
    style.PopupBorderSize          = 0.0f;
    // 框体内边距决定输入框和按钮的文字基线及垂直触达面积。
    style.FramePadding    = ImVec2(5.0f, 3.5f);
    style.FrameRounding   = 5.0f;
    style.FrameBorderSize = 0.0f;
    // 项目间距约束同级控件节奏，也是多数自动布局的基础步长。
    style.ItemSpacing      = ImVec2(5.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(5.0f, 5.0f);
    // 单元格留白保证表格密集展示时文字和分隔线仍有安全距离。
    style.CellPadding       = ImVec2(4.0f, 2.0f);
    style.IndentSpacing     = 5.0f;
    style.ColumnsMinSpacing = 5.0f;
    // 滚动条宽度在可点击性与内容占用之间维持主题取舍。
    style.ScrollbarSize     = 15.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabMinSize       = 15.0f;
    style.GrabRounding      = 5.0f;
    style.TabRounding       = 5.0f;
    style.TabBorderSize     = 0.0f;
    // 选中标签的关闭按钮阈值控制窄页签中操作入口的保留策略。
    style.TabCloseButtonMinWidthSelected = 0.0f;
    // 未选中标签单独设阈值，避免拥挤时关闭图标喧宾夺主。
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    // 正文色承担主要信息层，必须与窗口和框体背景保持足够对比。
    style.Colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // 弱化文字用于不可用或次要信息，但仍需在主背景上可辨识。
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(1.0f, 1.0f, 1.0f, 0.360515f);
    // 窗口底色奠定整套主题明度，后续容器和控件颜色均以此为参照。
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.09803922f, 0.09803922f, 0.09803922f, 1.0f);
    // 子窗口背景可选择透明继承，避免嵌套区域产生多余色块。
    style.Colors[ImGuiCol_ChildBg] = ImVec4(1.0f, 0.0f, 0.0f, 0.0f);
    // 弹窗背景需要遮住下层内容，同时以透明度保留空间关系。
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.09803922f, 0.09803922f, 0.09803922f, 1.0f);
    // 边框色用于低成本描绘控件和容器边界，透明度决定强调强度。
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.42352942f, 0.38039216f, 0.57254905f, 0.5493562f);
    // 边框阴影独立配置，以便主题明确选择平面或浮起效果。
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 框体常态是输入和选择控件的静止基面，也是交互渐变的起点。
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.15686275f, 0.15686275f, 0.15686275f, 1.0f);
    // 框体悬浮色只提示可交互性，应比激活态更克制。
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.38039216f, 0.42352942f, 0.57254905f, 0.54901963f);
    // 框体激活色确认当前操作焦点，并与悬浮态形成连续反馈。
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.61960787f, 0.5764706f, 0.76862746f, 0.54901963f);
    // 非活动标题栏降低存在感，让当前编辑窗口更容易被识别。
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.09803922f, 0.09803922f, 0.09803922f, 1.0f);
    // 活动标题栏提高层级，承担当前窗口焦点的主要视觉提示。
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.09803922f, 0.09803922f, 0.09803922f, 1.0f);
    // 折叠标题仍保留窗口身份，但用透明度减少对内容的干扰。
    style.Colors[ImGuiCol_TitleBgCollapsed] =
        ImVec4(0.25882354f, 0.25882354f, 0.25882354f, 0.0f);
    // 菜单栏底色连接窗口标题与内容区，避免形成突兀的横向色带。
    style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 滚动轨道应融入容器背景，使抓手成为滚动位置的主要提示。
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.15686275f, 0.15686275f, 0.15686275f, 0.0f);
    // 滚动抓手常态需持续可见，又不能盖过正文和主要操作。
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.15686275f, 0.15686275f, 0.15686275f, 1.0f);
    // 滚动抓手悬浮态提高对比，提示鼠标已经进入可拖动区域。
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.23529412f, 0.23529412f, 0.23529412f, 1.0f);
    // 滚动抓手激活态确认持续拖动，颜色变化需强于悬浮态。
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.29411766f, 0.29411766f, 0.29411766f, 1.0f);
    // 勾选标记承载布尔结果，应从框体背景中清晰跳出。
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.29411766f, 0.29411766f, 0.29411766f, 1.0f);
    // 滑块抓手常态标记当前值位置，并与轨道保持明度区分。
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.61960787f, 0.5764706f, 0.76862746f, 0.54901963f);
    // 滑块活动抓手强调正在调整的数值，释放后回落至常态。
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.8156863f, 0.77254903f, 0.9647059f, 0.54901963f);
    // 按钮常态建立操作面的基础层级，为悬浮和按下反馈预留变化空间。
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.61960787f, 0.5764706f, 0.76862746f, 0.54901963f);
    // 按钮悬浮态只表达指向，不应与已按下状态混淆。
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.7372549f, 0.69411767f, 0.8862745f, 0.54901963f);
    // 按钮激活态提供按下确认，需要成为三态中最明确的反馈。
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.8156863f, 0.77254903f, 0.9647059f, 0.54901963f);
    // Header 常态同时服务树节点、列表选项和折叠区标题。
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.61960787f, 0.5764706f, 0.76862746f, 0.54901963f);
    // Header 悬浮态覆盖较大行区域，应控制亮度避免产生闪烁感。
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.7372549f, 0.69411767f, 0.8862745f, 0.54901963f);
    // Header 激活态标识当前选择或按压结果，与按钮反馈保持一致。
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.8156863f, 0.77254903f, 0.9647059f, 0.54901963f);
    // 分隔线以低对比划分区域，不与可拖动边缘争夺注意力。
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.61960787f, 0.5764706f, 0.76862746f, 0.54901963f);
    // 可交互分隔线在悬浮时增强，提示其支持调整布局。
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.7372549f, 0.69411767f, 0.8862745f, 0.54901963f);
    // 分隔线拖动期间使用最强状态，持续反馈布局调整操作。
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.8156863f, 0.77254903f, 0.9647059f, 0.54901963f);
    // 缩放抓手常态保持含蓄，避免遮挡窗口角落内容。
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.61960787f, 0.5764706f, 0.76862746f, 0.54901963f);
    // 缩放抓手悬浮态显露可调整方向，强化边角命中提示。
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.7372549f, 0.69411767f, 0.8862745f, 0.54901963f);
    // 缩放抓手激活态维持拖动反馈，直到尺寸调整结束。
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.8156863f, 0.77254903f, 0.9647059f, 0.54901963f);
    // 普通标签以中等层级承载页面入口，不抢占当前标签的焦点。
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.61960787f, 0.5764706f, 0.76862746f, 0.54901963f);
    // 标签悬浮色提前提示可切换目标，并需兼容已选中标签的叠加状态。
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.7372549f, 0.69411767f, 0.8862745f, 0.54901963f);
    // 活动标签与内容面板建立连续关系，明确当前可见页面。
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.8156863f, 0.77254903f, 0.9647059f, 0.54901963f);
    // 失焦停靠节点的普通标签进一步降级，突出当前活动窗口组。
    style.Colors[ImGuiCol_TabUnfocused] = ImVec4(0.0f, 0.4509804f, 1.0f, 0.0f);
    // 失焦组仍需标识其已选页面，因此保留受抑制的活动态。
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.13333334f, 0.25882354f, 0.42352942f, 0.0f);
    // 折线图常态颜色优先保证细线在背景上的连续可读性。
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.29411766f, 0.29411766f, 0.29411766f, 1.0f);
    // 折线悬浮态采用明显强调色，便于定位密集曲线中的目标。
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(0.7372549f, 0.69411767f, 0.8862745f, 0.54901963f);
    // 柱状图使用独立数据色，避免与按钮等交互强调色混淆。
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.61960787f, 0.5764706f, 0.76862746f, 0.54901963f);
    // 柱状图悬浮色强化当前样本，但仍需保留原数据系列归属。
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(0.7372549f, 0.69411767f, 0.8862745f, 0.54901963f);
    // 表头底色固定列语义区域，与数据行建立稳定的纵向起点。
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.1882353f, 0.1882353f, 0.2f, 1.0f);
    // 强表格边框用于外沿和表头分界，承担主要结构线。
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.42352942f, 0.38039216f, 0.57254905f, 0.54901963f);
    // 弱表格边框只分隔普通单元格，降低密集网格的视觉噪声。
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(0.42352942f, 0.38039216f, 0.57254905f, 0.2918455f);
    // 基础行背景允许透明继承，使表格自然嵌入所在面板。
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 交替行仅施加轻微差异，帮助横向追踪而不破坏整体底色。
    style.Colors[ImGuiCol_TableRowBgAlt] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.03433478f);
    // 文本选区背景必须兼顾选中辨识度与前景文字可读性。
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(0.7372549f, 0.69411767f, 0.8862745f, 0.54901963f);
    // 拖放目标使用高辨识强调色，明确释放操作将作用的位置。
    style.Colors[ImGuiCol_DragDropTarget] = ImVec4(1.0f, 1.0f, 0.0f, 0.9f);
    // 键盘和手柄导航高亮独立于鼠标悬浮态，避免输入来源含混。
    style.Colors[ImGuiCol_NavHighlight] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    // 窗口导航高亮标识待切换窗口，并覆盖多种面板底色。
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    // 窗口切换期间的遮罩压低非目标内容，但仍保留场景上下文。
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    // 模态遮罩明确阻断下层交互，透明度同时维持来源窗口可见。
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.35f);
}

static void applyPurpleComfyStyle(ImGuiStyle& style)
{
    // Purple Comfy 样式，来源为 ImThemes 的 RegularLunar 配色。

    // 全局透明度会乘到整套颜色上，主题在此保持最终合成基准。
    style.Alpha = 1.0f;
    // 禁用态通过统一衰减与可交互控件拉开层级，无需逐项改写颜色。
    style.DisabledAlpha = 0.1f;
    // 窗口内边距同时决定内容密度和停靠面板的呼吸空间。
    style.WindowPadding            = ImVec2(8.0f, 8.0f);
    style.WindowRounding           = 10.0f;
    style.WindowBorderSize         = 0.0f;
    style.WindowMinSize            = ImVec2(30.0f, 30.0f);
    style.WindowTitleAlign         = ImVec2(0.5f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_Right;
    style.ChildRounding            = 5.0f;
    style.ChildBorderSize          = 1.0f;
    style.PopupRounding            = 10.0f;
    style.PopupBorderSize          = 0.0f;
    // 框体内边距决定输入框和按钮的文字基线及垂直触达面积。
    style.FramePadding    = ImVec2(5.0f, 3.5f);
    style.FrameRounding   = 5.0f;
    style.FrameBorderSize = 0.0f;
    // 项目间距约束同级控件节奏，也是多数自动布局的基础步长。
    style.ItemSpacing      = ImVec2(5.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(5.0f, 5.0f);
    // 单元格留白保证表格密集展示时文字和分隔线仍有安全距离。
    style.CellPadding       = ImVec2(4.0f, 2.0f);
    style.IndentSpacing     = 5.0f;
    style.ColumnsMinSpacing = 5.0f;
    // 滚动条宽度在可点击性与内容占用之间维持主题取舍。
    style.ScrollbarSize     = 15.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabMinSize       = 15.0f;
    style.GrabRounding      = 5.0f;
    style.TabRounding       = 5.0f;
    style.TabBorderSize     = 0.0f;
    // 选中标签的关闭按钮阈值控制窄页签中操作入口的保留策略。
    style.TabCloseButtonMinWidthSelected = 0.0f;
    // 未选中标签单独设阈值，避免拥挤时关闭图标喧宾夺主。
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    // 正文色承担主要信息层，必须与窗口和框体背景保持足够对比。
    style.Colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // 弱化文字用于不可用或次要信息，但仍需在主背景上可辨识。
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(1.0f, 1.0f, 1.0f, 0.360515f);
    // 窗口底色奠定整套主题明度，后续容器和控件颜色均以此为参照。
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.09803922f, 0.09803922f, 0.09803922f, 1.0f);
    // 子窗口背景可选择透明继承，避免嵌套区域产生多余色块。
    style.Colors[ImGuiCol_ChildBg] = ImVec4(1.0f, 0.0f, 0.0f, 0.0f);
    // 弹窗背景需要遮住下层内容，同时以透明度保留空间关系。
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.09803922f, 0.09803922f, 0.09803922f, 1.0f);
    // 边框色用于低成本描绘控件和容器边界，透明度决定强调强度。
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    // 边框阴影独立配置，以便主题明确选择平面或浮起效果。
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 框体常态是输入和选择控件的静止基面，也是交互渐变的起点。
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.15686275f, 0.15686275f, 0.15686275f, 1.0f);
    // 框体悬浮色只提示可交互性，应比激活态更克制。
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.38039216f, 0.42352942f, 0.57254905f, 0.54901963f);
    // 框体激活色确认当前操作焦点，并与悬浮态形成连续反馈。
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    // 非活动标题栏降低存在感，让当前编辑窗口更容易被识别。
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.09803922f, 0.09803922f, 0.09803922f, 1.0f);
    // 活动标题栏提高层级，承担当前窗口焦点的主要视觉提示。
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.09803922f, 0.09803922f, 0.09803922f, 1.0f);
    // 折叠标题仍保留窗口身份，但用透明度减少对内容的干扰。
    style.Colors[ImGuiCol_TitleBgCollapsed] =
        ImVec4(0.25882354f, 0.25882354f, 0.25882354f, 0.0f);
    // 菜单栏底色连接窗口标题与内容区，避免形成突兀的横向色带。
    style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 滚动轨道应融入容器背景，使抓手成为滚动位置的主要提示。
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.15686275f, 0.15686275f, 0.15686275f, 0.0f);
    // 滚动抓手常态需持续可见，又不能盖过正文和主要操作。
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.15686275f, 0.15686275f, 0.15686275f, 1.0f);
    // 滚动抓手悬浮态提高对比，提示鼠标已经进入可拖动区域。
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.23529412f, 0.23529412f, 0.23529412f, 1.0f);
    // 滚动抓手激活态确认持续拖动，颜色变化需强于悬浮态。
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.29411766f, 0.29411766f, 0.29411766f, 1.0f);
    // 勾选标记承载布尔结果，应从框体背景中清晰跳出。
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    // 滑块抓手常态标记当前值位置，并与轨道保持明度区分。
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    // 滑块活动抓手强调正在调整的数值，释放后回落至常态。
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    // 按钮常态建立操作面的基础层级，为悬浮和按下反馈预留变化空间。
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    // 按钮悬浮态只表达指向，不应与已按下状态混淆。
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    // 按钮激活态提供按下确认，需要成为三态中最明确的反馈。
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    // Header 常态同时服务树节点、列表选项和折叠区标题。
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    // Header 悬浮态覆盖较大行区域，应控制亮度避免产生闪烁感。
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    // Header 激活态标识当前选择或按压结果，与按钮反馈保持一致。
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    // 分隔线以低对比划分区域，不与可拖动边缘争夺注意力。
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    // 可交互分隔线在悬浮时增强，提示其支持调整布局。
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    // 分隔线拖动期间使用最强状态，持续反馈布局调整操作。
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    // 缩放抓手常态保持含蓄，避免遮挡窗口角落内容。
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    // 缩放抓手悬浮态显露可调整方向，强化边角命中提示。
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    // 缩放抓手激活态维持拖动反馈，直到尺寸调整结束。
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    // 普通标签以中等层级承载页面入口，不抢占当前标签的焦点。
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    // 标签悬浮色提前提示可切换目标，并需兼容已选中标签的叠加状态。
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    // 活动标签与内容面板建立连续关系，明确当前可见页面。
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    // 失焦停靠节点的普通标签进一步降级，突出当前活动窗口组。
    style.Colors[ImGuiCol_TabUnfocused] = ImVec4(0.0f, 0.4509804f, 1.0f, 0.0f);
    // 失焦组仍需标识其已选页面，因此保留受抑制的活动态。
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.13333334f, 0.25882354f, 0.42352942f, 0.0f);
    // 折线图常态颜色优先保证细线在背景上的连续可读性。
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.29411766f, 0.29411766f, 0.29411766f, 1.0f);
    // 折线悬浮态采用明显强调色，便于定位密集曲线中的目标。
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    // 柱状图使用独立数据色，避免与按钮等交互强调色混淆。
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    // 柱状图悬浮色强化当前样本，但仍需保留原数据系列归属。
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(0.7372549f, 0.69411767f, 0.8862745f, 0.54901963f);
    // 表头底色固定列语义区域，与数据行建立稳定的纵向起点。
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.1882353f, 0.1882353f, 0.2f, 1.0f);
    // 强表格边框用于外沿和表头分界，承担主要结构线。
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    // 弱表格边框只分隔普通单元格，降低密集网格的视觉噪声。
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.2901961f);
    // 基础行背景允许透明继承，使表格自然嵌入所在面板。
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 交替行仅施加轻微差异，帮助横向追踪而不破坏整体底色。
    style.Colors[ImGuiCol_TableRowBgAlt] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.03433478f);
    // 文本选区背景必须兼顾选中辨识度与前景文字可读性。
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(0.5019608f, 0.3019608f, 1.0f, 0.54901963f);
    // 拖放目标使用高辨识强调色，明确释放操作将作用的位置。
    style.Colors[ImGuiCol_DragDropTarget] = ImVec4(1.0f, 1.0f, 0.0f, 0.9f);
    // 键盘和手柄导航高亮独立于鼠标悬浮态，避免输入来源含混。
    style.Colors[ImGuiCol_NavHighlight] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    // 窗口导航高亮标识待切换窗口，并覆盖多种面板底色。
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    // 窗口切换期间的遮罩压低非目标内容，但仍保留场景上下文。
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    // 模态遮罩明确阻断下层交互，透明度同时维持来源窗口可见。
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.35f);
}

static void applyFutureDarkStyle(ImGuiStyle& style)
{
    // Future Dark 样式，来源为 ImThemes 的 rewrking 配色。

    // 全局透明度会乘到整套颜色上，主题在此保持最终合成基准。
    style.Alpha = 1.0f;
    // 禁用态通过统一衰减与可交互控件拉开层级，无需逐项改写颜色。
    style.DisabledAlpha = 1.0f;
    // 窗口内边距同时决定内容密度和停靠面板的呼吸空间。
    style.WindowPadding            = ImVec2(12.0f, 12.0f);
    style.WindowRounding           = 0.0f;
    style.WindowBorderSize         = 0.0f;
    style.WindowMinSize            = ImVec2(20.0f, 20.0f);
    style.WindowTitleAlign         = ImVec2(0.5f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.ChildRounding            = 0.0f;
    style.ChildBorderSize          = 1.0f;
    style.PopupRounding            = 0.0f;
    style.PopupBorderSize          = 1.0f;
    // 框体内边距决定输入框和按钮的文字基线及垂直触达面积。
    style.FramePadding    = ImVec2(6.0f, 6.0f);
    style.FrameRounding   = 0.0f;
    style.FrameBorderSize = 0.0f;
    // 项目间距约束同级控件节奏，也是多数自动布局的基础步长。
    style.ItemSpacing      = ImVec2(12.0f, 6.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 3.0f);
    // 单元格留白保证表格密集展示时文字和分隔线仍有安全距离。
    style.CellPadding       = ImVec2(12.0f, 6.0f);
    style.IndentSpacing     = 20.0f;
    style.ColumnsMinSpacing = 6.0f;
    // 滚动条宽度在可点击性与内容占用之间维持主题取舍。
    style.ScrollbarSize     = 12.0f;
    style.ScrollbarRounding = 0.0f;
    style.GrabMinSize       = 12.0f;
    style.GrabRounding      = 0.0f;
    style.TabRounding       = 0.0f;
    style.TabBorderSize     = 0.0f;
    // 选中标签的关闭按钮阈值控制窄页签中操作入口的保留策略。
    style.TabCloseButtonMinWidthSelected = 0.0f;
    // 未选中标签单独设阈值，避免拥挤时关闭图标喧宾夺主。
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    // 正文色承担主要信息层，必须与窗口和框体背景保持足够对比。
    style.Colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // 弱化文字用于不可用或次要信息，但仍需在主背景上可辨识。
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4(0.27450982f, 0.31764707f, 0.4509804f, 1.0f);
    // 窗口底色奠定整套主题明度，后续容器和控件颜色均以此为参照。
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    // 子窗口背景可选择透明继承，避免嵌套区域产生多余色块。
    style.Colors[ImGuiCol_ChildBg] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    // 弹窗背景需要遮住下层内容，同时以透明度保留空间关系。
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    // 边框色用于低成本描绘控件和容器边界，透明度决定强调强度。
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.15686275f, 0.16862746f, 0.19215687f, 1.0f);
    // 边框阴影独立配置，以便主题明确选择平面或浮起效果。
    style.Colors[ImGuiCol_BorderShadow] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    // 框体常态是输入和选择控件的静止基面，也是交互渐变的起点。
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    // 框体悬浮色只提示可交互性，应比激活态更克制。
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.15686275f, 0.16862746f, 0.19215687f, 1.0f);
    // 框体激活色确认当前操作焦点，并与悬浮态形成连续反馈。
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.23529412f, 0.21568628f, 0.59607846f, 1.0f);
    // 非活动标题栏降低存在感，让当前编辑窗口更容易被识别。
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    // 活动标题栏提高层级，承担当前窗口焦点的主要视觉提示。
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    // 折叠标题仍保留窗口身份，但用透明度减少对内容的干扰。
    style.Colors[ImGuiCol_TitleBgCollapsed] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    // 菜单栏底色连接窗口标题与内容区，避免形成突兀的横向色带。
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.09803922f, 0.105882354f, 0.12156863f, 1.0f);
    // 滚动轨道应融入容器背景，使抓手成为滚动位置的主要提示。
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    // 滚动抓手常态需持续可见，又不能盖过正文和主要操作。
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    // 滚动抓手悬浮态提高对比，提示鼠标已经进入可拖动区域。
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.15686275f, 0.16862746f, 0.19215687f, 1.0f);
    // 滚动抓手激活态确认持续拖动，颜色变化需强于悬浮态。
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    // 勾选标记承载布尔结果，应从框体背景中清晰跳出。
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.49803922f, 0.5137255f, 1.0f, 1.0f);
    // 滑块抓手常态标记当前值位置，并与轨道保持明度区分。
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.49803922f, 0.5137255f, 1.0f, 1.0f);
    // 滑块活动抓手强调正在调整的数值，释放后回落至常态。
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.5372549f, 0.5529412f, 1.0f, 1.0f);
    // 按钮常态建立操作面的基础层级，为悬浮和按下反馈预留变化空间。
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    // 按钮悬浮态只表达指向，不应与已按下状态混淆。
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.19607843f, 0.1764706f, 0.54509807f, 1.0f);
    // 按钮激活态提供按下确认，需要成为三态中最明确的反馈。
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.23529412f, 0.21568628f, 0.59607846f, 1.0f);
    // Header 常态同时服务树节点、列表选项和折叠区标题。
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    // Header 悬浮态覆盖较大行区域，应控制亮度避免产生闪烁感。
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.19607843f, 0.1764706f, 0.54509807f, 1.0f);
    // Header 激活态标识当前选择或按压结果，与按钮反馈保持一致。
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.23529412f, 0.21568628f, 0.59607846f, 1.0f);
    // 分隔线以低对比划分区域，不与可拖动边缘争夺注意力。
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.15686275f, 0.18431373f, 0.2509804f, 1.0f);
    // 可交互分隔线在悬浮时增强，提示其支持调整布局。
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.15686275f, 0.18431373f, 0.2509804f, 1.0f);
    // 分隔线拖动期间使用最强状态，持续反馈布局调整操作。
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.15686275f, 0.18431373f, 0.2509804f, 1.0f);
    // 缩放抓手常态保持含蓄，避免遮挡窗口角落内容。
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    // 缩放抓手悬浮态显露可调整方向，强化边角命中提示。
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.19607843f, 0.1764706f, 0.54509807f, 1.0f);
    // 缩放抓手激活态维持拖动反馈，直到尺寸调整结束。
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.23529412f, 0.21568628f, 0.59607846f, 1.0f);
    // 普通标签以中等层级承载页面入口，不抢占当前标签的焦点。
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    // 标签悬浮色提前提示可切换目标，并需兼容已选中标签的叠加状态。
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    // 活动标签与内容面板建立连续关系，明确当前可见页面。
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.09803922f, 0.105882354f, 0.12156863f, 1.0f);
    // 失焦停靠节点的普通标签进一步降级，突出当前活动窗口组。
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    // 失焦组仍需标识其已选页面，因此保留受抑制的活动态。
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    // 折线图常态颜色优先保证细线在背景上的连续可读性。
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.52156866f, 0.6f, 0.7019608f, 1.0f);
    // 折线悬浮态采用明显强调色，便于定位密集曲线中的目标。
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(0.039215688f, 0.98039216f, 0.98039216f, 1.0f);
    // 柱状图使用独立数据色，避免与按钮等交互强调色混淆。
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(1.0f, 0.2901961f, 0.59607846f, 1.0f);
    // 柱状图悬浮色强化当前样本，但仍需保留原数据系列归属。
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(0.99607843f, 0.4745098f, 0.69803923f, 1.0f);
    // 表头底色固定列语义区域，与数据行建立稳定的纵向起点。
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    // 强表格边框用于外沿和表头分界，承担主要结构线。
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    // 弱表格边框只分隔普通单元格，降低密集网格的视觉噪声。
    style.Colors[ImGuiCol_TableBorderLight] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    // 基础行背景允许透明继承，使表格自然嵌入所在面板。
    style.Colors[ImGuiCol_TableRowBg] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    // 交替行仅施加轻微差异，帮助横向追踪而不破坏整体底色。
    style.Colors[ImGuiCol_TableRowBgAlt] =
        ImVec4(0.09803922f, 0.105882354f, 0.12156863f, 1.0f);
    // 文本选区背景必须兼顾选中辨识度与前景文字可读性。
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(0.23529412f, 0.21568628f, 0.59607846f, 1.0f);
    // 拖放目标使用高辨识强调色，明确释放操作将作用的位置。
    style.Colors[ImGuiCol_DragDropTarget] =
        ImVec4(0.49803922f, 0.5137255f, 1.0f, 1.0f);
    // 键盘和手柄导航高亮独立于鼠标悬浮态，避免输入来源含混。
    style.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.49803922f, 0.5137255f, 1.0f, 1.0f);
    // 窗口导航高亮标识待切换窗口，并覆盖多种面板底色。
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(0.49803922f, 0.5137255f, 1.0f, 1.0f);
    // 窗口切换期间的遮罩压低非目标内容，但仍保留场景上下文。
    style.Colors[ImGuiCol_NavWindowingDimBg] =
        ImVec4(0.19607843f, 0.1764706f, 0.54509807f, 0.5019608f);
    // 模态遮罩明确阻断下层交互，透明度同时维持来源窗口可见。
    style.Colors[ImGuiCol_ModalWindowDimBg] =
        ImVec4(0.19607843f, 0.1764706f, 0.54509807f, 0.5019608f);
}

static void applyCleanDarkStyle(ImGuiStyle& style)
{
    // Clean Dark/Red 样式，来源为 ImThemes 的 ImBritish 配色。

    // 全局透明度会乘到整套颜色上，主题在此保持最终合成基准。
    style.Alpha = 1.0f;
    // 禁用态通过统一衰减与可交互控件拉开层级，无需逐项改写颜色。
    style.DisabledAlpha = 0.6f;
    // 窗口内边距同时决定内容密度和停靠面板的呼吸空间。
    style.WindowPadding            = ImVec2(8.0f, 8.0f);
    style.WindowRounding           = 0.0f;
    style.WindowBorderSize         = 1.0f;
    style.WindowMinSize            = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign         = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_Left;
    style.ChildRounding            = 0.0f;
    style.ChildBorderSize          = 1.0f;
    style.PopupRounding            = 0.0f;
    style.PopupBorderSize          = 1.0f;
    // 框体内边距决定输入框和按钮的文字基线及垂直触达面积。
    style.FramePadding    = ImVec2(4.0f, 3.0f);
    style.FrameRounding   = 0.0f;
    style.FrameBorderSize = 1.0f;
    // 项目间距约束同级控件节奏，也是多数自动布局的基础步长。
    style.ItemSpacing      = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
    // 单元格留白保证表格密集展示时文字和分隔线仍有安全距离。
    style.CellPadding       = ImVec2(4.0f, 2.0f);
    style.IndentSpacing     = 21.0f;
    style.ColumnsMinSpacing = 6.0f;
    // 滚动条宽度在可点击性与内容占用之间维持主题取舍。
    style.ScrollbarSize     = 14.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabMinSize       = 10.0f;
    style.GrabRounding      = 0.0f;
    style.TabRounding       = 0.0f;
    style.TabBorderSize     = 1.0f;
    // 选中标签的关闭按钮阈值控制窄页签中操作入口的保留策略。
    style.TabCloseButtonMinWidthSelected = 0.0f;
    // 未选中标签单独设阈值，避免拥挤时关闭图标喧宾夺主。
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    // 正文色承担主要信息层，必须与窗口和框体背景保持足够对比。
    style.Colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // 弱化文字用于不可用或次要信息，但仍需在主背景上可辨识。
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4(0.7294118f, 0.7490196f, 0.7372549f, 1.0f);
    // 窗口底色奠定整套主题明度，后续容器和控件颜色均以此为参照。
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.08627451f, 0.08627451f, 0.08627451f, 0.94f);
    // 子窗口背景可选择透明继承，避免嵌套区域产生多余色块。
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 弹窗背景需要遮住下层内容，同时以透明度保留空间关系。
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.078431375f, 0.078431375f, 0.078431375f, 0.94f);
    // 边框色用于低成本描绘控件和容器边界，透明度决定强调强度。
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.13725491f, 0.13725491f, 0.13725491f, 1.0f);
    // 边框阴影独立配置，以便主题明确选择平面或浮起效果。
    style.Colors[ImGuiCol_BorderShadow] =
        ImVec4(0.13725491f, 0.13725491f, 0.13725491f, 1.0f);
    // 框体常态是输入和选择控件的静止基面，也是交互渐变的起点。
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.54f);
    // 框体悬浮色只提示可交互性，应比激活态更克制。
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.1764706f, 0.1764706f, 0.1764706f, 0.4f);
    // 框体激活色确认当前操作焦点，并与悬浮态形成连续反馈。
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.21568628f, 0.21568628f, 0.21568628f, 0.67f);
    // 非活动标题栏降低存在感，让当前编辑窗口更容易被识别。
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.13725491f, 0.13725491f, 0.13725491f, 0.65236056f);
    // 活动标题栏提高层级，承担当前窗口焦点的主要视觉提示。
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.13725491f, 0.13725491f, 0.13725491f, 1.0f);
    // 折叠标题仍保留窗口身份，但用透明度减少对内容的干扰。
    style.Colors[ImGuiCol_TitleBgCollapsed] =
        ImVec4(0.13725491f, 0.13725491f, 0.13725491f, 0.67f);
    // 菜单栏底色连接窗口标题与内容区，避免形成突兀的横向色带。
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.21568628f, 0.21568628f, 0.21568628f, 1.0f);
    // 滚动轨道应融入容器背景，使抓手成为滚动位置的主要提示。
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.019607844f, 0.019607844f, 0.019607844f, 0.53f);
    // 滚动抓手常态需持续可见，又不能盖过正文和主要操作。
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.30980393f, 0.30980393f, 0.30980393f, 1.0f);
    // 滚动抓手悬浮态提高对比，提示鼠标已经进入可拖动区域。
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.40784314f, 0.40784314f, 0.40784314f, 1.0f);
    // 滚动抓手激活态确认持续拖动，颜色变化需强于悬浮态。
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.50980395f, 0.50980395f, 0.50980395f, 1.0f);
    // 勾选标记承载布尔结果，应从框体背景中清晰跳出。
    style.Colors[ImGuiCol_CheckMark] = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
    // 滑块抓手常态标记当前值位置，并与轨道保持明度区分。
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
    // 滑块活动抓手强调正在调整的数值，释放后回落至常态。
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(1.0f, 0.38039216f, 0.38039216f, 1.0f);
    // 按钮常态建立操作面的基础层级，为悬浮和按下反馈预留变化空间。
    style.Colors[ImGuiCol_Button] = ImVec4(0.0f, 0.0f, 0.0f, 0.5411765f);
    // 按钮悬浮态只表达指向，不应与已按下状态混淆。
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.1764706f, 0.1764706f, 0.1764706f, 0.4f);
    // 按钮激活态提供按下确认，需要成为三态中最明确的反馈。
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.21568628f, 0.21568628f, 0.21568628f, 0.67058825f);
    // Header 常态同时服务树节点、列表选项和折叠区标题。
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.21568628f, 0.21568628f, 0.21568628f, 1.0f);
    // Header 悬浮态覆盖较大行区域，应控制亮度避免产生闪烁感。
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.27058825f, 0.27058825f, 0.27058825f, 1.0f);
    // Header 激活态标识当前选择或按压结果，与按钮反馈保持一致。
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.3529412f, 0.3529412f, 0.3529412f, 1.0f);
    // 分隔线以低对比划分区域，不与可拖动边缘争夺注意力。
    style.Colors[ImGuiCol_Separator] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // 可交互分隔线在悬浮时增强，提示其支持调整布局。
    style.Colors[ImGuiCol_SeparatorHovered] = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
    // 分隔线拖动期间使用最强状态，持续反馈布局调整操作。
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(1.0f, 0.32941177f, 0.32941177f, 1.0f);
    // 缩放抓手常态保持含蓄，避免遮挡窗口角落内容。
    style.Colors[ImGuiCol_ResizeGrip] = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
    // 缩放抓手悬浮态显露可调整方向，强化边角命中提示。
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(1.0f, 0.4862745f, 0.4862745f, 1.0f);
    // 缩放抓手激活态维持拖动反馈，直到尺寸调整结束。
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(1.0f, 0.4862745f, 0.4862745f, 1.0f);
    // 普通标签以中等层级承载页面入口，不抢占当前标签的焦点。
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.21960784f, 0.21960784f, 0.21960784f, 1.0f);
    // 标签悬浮色提前提示可切换目标，并需兼容已选中标签的叠加状态。
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.2901961f, 0.2901961f, 0.2901961f, 1.0f);
    // 活动标签与内容面板建立连续关系，明确当前可见页面。
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.1764706f, 0.1764706f, 0.1764706f, 1.0f);
    // 失焦停靠节点的普通标签进一步降级，突出当前活动窗口组。
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.14901961f, 0.06666667f, 0.06666667f, 0.97f);
    // 失焦组仍需标识其已选页面，因此保留受抑制的活动态。
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.40392157f, 0.15294118f, 0.15294118f, 1.0f);
    // 折线图常态颜色优先保证细线在背景上的连续可读性。
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.60784316f, 0.60784316f, 0.60784316f, 1.0f);
    // 折线悬浮态采用明显强调色，便于定位密集曲线中的目标。
    style.Colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
    // 柱状图使用独立数据色，避免与按钮等交互强调色混淆。
    style.Colors[ImGuiCol_PlotHistogram] = ImVec4(0.8980392f, 0.0f, 0.0f, 1.0f);
    // 柱状图悬浮色强化当前样本，但仍需保留原数据系列归属。
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(0.3647059f, 0.0f, 0.0f, 1.0f);
    // 表头底色固定列语义区域，与数据行建立稳定的纵向起点。
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.3019608f, 0.3019608f, 0.3019608f, 1.0f);
    // 强表格边框用于外沿和表头分界，承担主要结构线。
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.13725491f, 0.13725491f, 0.13725491f, 1.0f);
    // 弱表格边框只分隔普通单元格，降低密集网格的视觉噪声。
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(0.13725491f, 0.13725491f, 0.13725491f, 1.0f);
    // 基础行背景允许透明继承，使表格自然嵌入所在面板。
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 交替行仅施加轻微差异，帮助横向追踪而不破坏整体底色。
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    // 文本选区背景必须兼顾选中辨识度与前景文字可读性。
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(0.2627451f, 0.63529414f, 0.8784314f, 0.43776822f);
    // 拖放目标使用高辨识强调色，明确释放操作将作用的位置。
    style.Colors[ImGuiCol_DragDropTarget] =
        ImVec4(0.46666667f, 0.18431373f, 0.18431373f, 0.9656652f);
    // 键盘和手柄导航高亮独立于鼠标悬浮态，避免输入来源含混。
    style.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.40784314f, 0.40784314f, 0.40784314f, 1.0f);
    // 窗口导航高亮标识待切换窗口，并覆盖多种面板底色。
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    // 窗口切换期间的遮罩压低非目标内容，但仍保留场景上下文。
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    // 模态遮罩明确阻断下层交互，透明度同时维持来源窗口可见。
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.35f);
}

static void applyMoonlightStyle(ImGuiStyle& style)
{
    // Moonlight 样式，来源为 ImThemes 的 Madam-Herta 配色。

    // 全局透明度会乘到整套颜色上，主题在此保持最终合成基准。
    style.Alpha = 1.0f;
    // 禁用态通过统一衰减与可交互控件拉开层级，无需逐项改写颜色。
    style.DisabledAlpha = 1.0f;
    // 窗口内边距同时决定内容密度和停靠面板的呼吸空间。
    style.WindowPadding            = ImVec2(12.0f, 12.0f);
    style.WindowRounding           = 11.5f;
    style.WindowBorderSize         = 0.0f;
    style.WindowMinSize            = ImVec2(20.0f, 20.0f);
    style.WindowTitleAlign         = ImVec2(0.5f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_Right;
    style.ChildRounding            = 0.0f;
    style.ChildBorderSize          = 1.0f;
    style.PopupRounding            = 0.0f;
    style.PopupBorderSize          = 1.0f;
    // 框体内边距决定输入框和按钮的文字基线及垂直触达面积。
    style.FramePadding    = ImVec2(20.0f, 3.4f);
    style.FrameRounding   = 11.9f;
    style.FrameBorderSize = 0.0f;
    // 项目间距约束同级控件节奏，也是多数自动布局的基础步长。
    style.ItemSpacing      = ImVec2(4.3f, 5.5f);
    style.ItemInnerSpacing = ImVec2(7.1f, 1.8f);
    // 单元格留白保证表格密集展示时文字和分隔线仍有安全距离。
    style.CellPadding       = ImVec2(12.1f, 9.2f);
    style.IndentSpacing     = 0.0f;
    style.ColumnsMinSpacing = 4.9f;
    // 滚动条宽度在可点击性与内容占用之间维持主题取舍。
    style.ScrollbarSize     = 11.6f;
    style.ScrollbarRounding = 15.9f;
    style.GrabMinSize       = 3.7f;
    style.GrabRounding      = 20.0f;
    style.TabRounding       = 0.0f;
    style.TabBorderSize     = 0.0f;
    // 选中标签的关闭按钮阈值控制窄页签中操作入口的保留策略。
    style.TabCloseButtonMinWidthSelected = 0.0f;
    // 未选中标签单独设阈值，避免拥挤时关闭图标喧宾夺主。
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    // 正文色承担主要信息层，必须与窗口和框体背景保持足够对比。
    style.Colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // 弱化文字用于不可用或次要信息，但仍需在主背景上可辨识。
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4(0.27450982f, 0.31764707f, 0.4509804f, 1.0f);
    // 窗口底色奠定整套主题明度，后续容器和控件颜色均以此为参照。
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    // 子窗口背景可选择透明继承，避免嵌套区域产生多余色块。
    style.Colors[ImGuiCol_ChildBg] =
        ImVec4(0.09411765f, 0.101960786f, 0.11764706f, 1.0f);
    // 弹窗背景需要遮住下层内容，同时以透明度保留空间关系。
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    // 边框色用于低成本描绘控件和容器边界，透明度决定强调强度。
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.15686275f, 0.16862746f, 0.19215687f, 1.0f);
    // 边框阴影独立配置，以便主题明确选择平面或浮起效果。
    style.Colors[ImGuiCol_BorderShadow] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    // 框体常态是输入和选择控件的静止基面，也是交互渐变的起点。
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.11372549f, 0.1254902f, 0.15294118f, 1.0f);
    // 框体悬浮色只提示可交互性，应比激活态更克制。
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.15686275f, 0.16862746f, 0.19215687f, 1.0f);
    // 框体激活色确认当前操作焦点，并与悬浮态形成连续反馈。
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.15686275f, 0.16862746f, 0.19215687f, 1.0f);
    // 非活动标题栏降低存在感，让当前编辑窗口更容易被识别。
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    // 活动标题栏提高层级，承担当前窗口焦点的主要视觉提示。
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    // 折叠标题仍保留窗口身份，但用透明度减少对内容的干扰。
    style.Colors[ImGuiCol_TitleBgCollapsed] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    // 菜单栏底色连接窗口标题与内容区，避免形成突兀的横向色带。
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.09803922f, 0.105882354f, 0.12156863f, 1.0f);
    // 滚动轨道应融入容器背景，使抓手成为滚动位置的主要提示。
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    // 滚动抓手常态需持续可见，又不能盖过正文和主要操作。
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    // 滚动抓手悬浮态提高对比，提示鼠标已经进入可拖动区域。
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.15686275f, 0.16862746f, 0.19215687f, 1.0f);
    // 滚动抓手激活态确认持续拖动，颜色变化需强于悬浮态。
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    // 勾选标记承载布尔结果，应从框体背景中清晰跳出。
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.972549f, 1.0f, 0.49803922f, 1.0f);
    // 滑块抓手常态标记当前值位置，并与轨道保持明度区分。
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.972549f, 1.0f, 0.49803922f, 1.0f);
    // 滑块活动抓手强调正在调整的数值，释放后回落至常态。
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(1.0f, 0.79607844f, 0.49803922f, 1.0f);
    // 按钮常态建立操作面的基础层级，为悬浮和按下反馈预留变化空间。
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    // 按钮悬浮态只表达指向，不应与已按下状态混淆。
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.18039216f, 0.1882353f, 0.19607843f, 1.0f);
    // 按钮激活态提供按下确认，需要成为三态中最明确的反馈。
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.15294118f, 0.15294118f, 0.15294118f, 1.0f);
    // Header 常态同时服务树节点、列表选项和折叠区标题。
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.14117648f, 0.16470589f, 0.20784314f, 1.0f);
    // Header 悬浮态覆盖较大行区域，应控制亮度避免产生闪烁感。
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.105882354f, 0.105882354f, 0.105882354f, 1.0f);
    // Header 激活态标识当前选择或按压结果，与按钮反馈保持一致。
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    // 分隔线以低对比划分区域，不与可拖动边缘争夺注意力。
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.12941177f, 0.14901961f, 0.19215687f, 1.0f);
    // 可交互分隔线在悬浮时增强，提示其支持调整布局。
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.15686275f, 0.18431373f, 0.2509804f, 1.0f);
    // 分隔线拖动期间使用最强状态，持续反馈布局调整操作。
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.15686275f, 0.18431373f, 0.2509804f, 1.0f);
    // 缩放抓手常态保持含蓄，避免遮挡窗口角落内容。
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.14509805f, 0.14509805f, 0.14509805f, 1.0f);
    // 缩放抓手悬浮态显露可调整方向，强化边角命中提示。
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.972549f, 1.0f, 0.49803922f, 1.0f);
    // 缩放抓手激活态维持拖动反馈，直到尺寸调整结束。
    style.Colors[ImGuiCol_ResizeGripActive] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // 普通标签以中等层级承载页面入口，不抢占当前标签的焦点。
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    // 标签悬浮色提前提示可切换目标，并需兼容已选中标签的叠加状态。
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    // 活动标签与内容面板建立连续关系，明确当前可见页面。
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    // 失焦停靠节点的普通标签进一步降级，突出当前活动窗口组。
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    // 失焦组仍需标识其已选页面，因此保留受抑制的活动态。
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.1254902f, 0.27450982f, 0.57254905f, 1.0f);
    // 折线图常态颜色优先保证细线在背景上的连续可读性。
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.52156866f, 0.6f, 0.7019608f, 1.0f);
    // 折线悬浮态采用明显强调色，便于定位密集曲线中的目标。
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(0.039215688f, 0.98039216f, 0.98039216f, 1.0f);
    // 柱状图使用独立数据色，避免与按钮等交互强调色混淆。
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.88235295f, 0.79607844f, 0.56078434f, 1.0f);
    // 柱状图悬浮色强化当前样本，但仍需保留原数据系列归属。
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(0.95686275f, 0.95686275f, 0.95686275f, 1.0f);
    // 表头底色固定列语义区域，与数据行建立稳定的纵向起点。
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    // 强表格边框用于外沿和表头分界，承担主要结构线。
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    // 弱表格边框只分隔普通单元格，降低密集网格的视觉噪声。
    style.Colors[ImGuiCol_TableBorderLight] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    // 基础行背景允许透明继承，使表格自然嵌入所在面板。
    style.Colors[ImGuiCol_TableRowBg] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    // 交替行仅施加轻微差异，帮助横向追踪而不破坏整体底色。
    style.Colors[ImGuiCol_TableRowBgAlt] =
        ImVec4(0.09803922f, 0.105882354f, 0.12156863f, 1.0f);
    // 文本选区背景必须兼顾选中辨识度与前景文字可读性。
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(0.9372549f, 0.9372549f, 0.9372549f, 1.0f);
    // 拖放目标使用高辨识强调色，明确释放操作将作用的位置。
    style.Colors[ImGuiCol_DragDropTarget] =
        ImVec4(0.49803922f, 0.5137255f, 1.0f, 1.0f);
    // 键盘和手柄导航高亮独立于鼠标悬浮态，避免输入来源含混。
    style.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.26666668f, 0.2901961f, 1.0f, 1.0f);
    // 窗口导航高亮标识待切换窗口，并覆盖多种面板底色。
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(0.49803922f, 0.5137255f, 1.0f, 1.0f);
    // 窗口切换期间的遮罩压低非目标内容，但仍保留场景上下文。
    style.Colors[ImGuiCol_NavWindowingDimBg] =
        ImVec4(0.19607843f, 0.1764706f, 0.54509807f, 0.5019608f);
    // 模态遮罩明确阻断下层交互，透明度同时维持来源窗口可见。
    style.Colors[ImGuiCol_ModalWindowDimBg] =
        ImVec4(0.19607843f, 0.1764706f, 0.54509807f, 0.5019608f);
}

/**
 * @brief 设置 Cecilia 塞西莉娅配色派生样式。
 */
static void applyCeciliaStyle(ImGuiStyle& style)
{
    applyMoonlightStyle(style);


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

    // 正文色承担主要信息层，必须与窗口和框体背景保持足够对比。
    style.Colors[ImGuiCol_Text] = detailTextColor;
    // 弱化文字用于不可用或次要信息，但仍需在主背景上可辨识。
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(
        detailTextColor.x, detailTextColor.y, detailTextColor.z, 0.6200f);
    // 窗口底色奠定整套主题明度，后续容器和控件颜色均以此为参照。
    style.Colors[ImGuiCol_WindowBg] = baseBgColor;
    // 子窗口背景可选择透明继承，避免嵌套区域产生多余色块。
    style.Colors[ImGuiCol_ChildBg] = viewBgColor;
    // 弹窗背景需要遮住下层内容，同时以透明度保留空间关系。
    style.Colors[ImGuiCol_PopupBg] = popupBgColor;
    // 边框色用于低成本描绘控件和容器边界，透明度决定强调强度。
    style.Colors[ImGuiCol_Border] = windowBorderColor;
    // 边框阴影独立配置，以便主题明确选择平面或浮起效果。
    style.Colors[ImGuiCol_BorderShadow] =
        ImVec4(cloakColor.x, cloakColor.y, cloakColor.z, 0.1600f);
    // 框体常态是输入和选择控件的静止基面，也是交互渐变的起点。
    style.Colors[ImGuiCol_FrameBg] = progressBgColor;
    // 框体悬浮色只提示可交互性，应比激活态更克制。
    style.Colors[ImGuiCol_FrameBgHovered] = panelHoverColor;
    // 框体激活色确认当前操作焦点，并与悬浮态形成连续反馈。
    style.Colors[ImGuiCol_FrameBgActive] = borderColor;
    // 非活动标题栏降低存在感，让当前编辑窗口更容易被识别。
    style.Colors[ImGuiCol_TitleBg] = titleBgColor;
    // 活动标题栏提高层级，承担当前窗口焦点的主要视觉提示。
    style.Colors[ImGuiCol_TitleBgActive] = titleBgActiveColor;
    // 折叠标题仍保留窗口身份，但用透明度减少对内容的干扰。
    style.Colors[ImGuiCol_TitleBgCollapsed] = hairColor;
    // 菜单栏底色连接窗口标题与内容区，避免形成突兀的横向色带。
    style.Colors[ImGuiCol_MenuBarBg] = deepHairColor;
    // 滚动轨道应融入容器背景，使抓手成为滚动位置的主要提示。
    style.Colors[ImGuiCol_ScrollbarBg] = viewBgColor;
    // 滚动抓手常态需持续可见，又不能盖过正文和主要操作。
    style.Colors[ImGuiCol_ScrollbarGrab] = borderColor;
    // 滚动抓手悬浮态提高对比，提示鼠标已经进入可拖动区域。
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = shadowWarmColor;
    // 滚动抓手激活态确认持续拖动，颜色变化需强于悬浮态。
    style.Colors[ImGuiCol_ScrollbarGrabActive] = progressColor;
    // 勾选标记承载布尔结果，应从框体背景中清晰跳出。
    style.Colors[ImGuiCol_CheckMark] = typeTextColor;
    // 滑块抓手常态标记当前值位置，并与轨道保持明度区分。
    style.Colors[ImGuiCol_SliderGrab] = progressColor;
    // 滑块活动抓手强调正在调整的数值，释放后回落至常态。
    style.Colors[ImGuiCol_SliderGrabActive] = fullProgressColor;
    // 按钮常态建立操作面的基础层级，为悬浮和按下反馈预留变化空间。
    style.Colors[ImGuiCol_Button] = viewBgColor;
    // 按钮悬浮态只表达指向，不应与已按下状态混淆。
    style.Colors[ImGuiCol_ButtonHovered] = panelHoverColor;
    // 按钮激活态提供按下确认，需要成为三态中最明确的反馈。
    style.Colors[ImGuiCol_ButtonActive] = borderColor;
    // Header 常态同时服务树节点、列表选项和折叠区标题。
    style.Colors[ImGuiCol_Header] = borderColor;
    // Header 悬浮态覆盖较大行区域，应控制亮度避免产生闪烁感。
    style.Colors[ImGuiCol_HeaderHovered] = hairColor;
    // Header 激活态标识当前选择或按压结果，与按钮反馈保持一致。
    style.Colors[ImGuiCol_HeaderActive] = deepHairColor;
    // 分隔线以低对比划分区域，不与可拖动边缘争夺注意力。
    style.Colors[ImGuiCol_Separator] = borderColor;
    // 可交互分隔线在悬浮时增强，提示其支持调整布局。
    style.Colors[ImGuiCol_SeparatorHovered] = hairColor;
    // 分隔线拖动期间使用最强状态，持续反馈布局调整操作。
    style.Colors[ImGuiCol_SeparatorActive] = deepHairColor;
    // 缩放抓手常态保持含蓄，避免遮挡窗口角落内容。
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(borderColor.x, borderColor.y, borderColor.z, 0.5000f);
    // 缩放抓手悬浮态显露可调整方向，强化边角命中提示。
    style.Colors[ImGuiCol_ResizeGripHovered] = hairColor;
    // 缩放抓手激活态维持拖动反馈，直到尺寸调整结束。
    style.Colors[ImGuiCol_ResizeGripActive] = deepHairColor;
    // 普通标签以中等层级承载页面入口，不抢占当前标签的焦点。
    style.Colors[ImGuiCol_Tab] = panelHoverColor;
    // 标签悬浮色提前提示可切换目标，并需兼容已选中标签的叠加状态。
    style.Colors[ImGuiCol_TabHovered] = hairColor;
    // 活动标签与内容面板建立连续关系，明确当前可见页面。
    style.Colors[ImGuiCol_TabActive]           = progressBgColor;
    style.Colors[ImGuiCol_TabSelectedOverline] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 失焦停靠节点的普通标签进一步降级，突出当前活动窗口组。
    style.Colors[ImGuiCol_TabUnfocused] = baseBgColor;
    // 失焦组仍需标识其已选页面，因此保留受抑制的活动态。
    style.Colors[ImGuiCol_TabUnfocusedActive] = panelHoverColor;
    style.Colors[ImGuiCol_TabDimmedSelectedOverline] =
        ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 折线图常态颜色优先保证细线在背景上的连续可读性。
    style.Colors[ImGuiCol_PlotLines] = borderColor;
    // 折线悬浮态采用明显强调色，便于定位密集曲线中的目标。
    style.Colors[ImGuiCol_PlotLinesHovered] = typeTextColor;
    // 柱状图使用独立数据色，避免与按钮等交互强调色混淆。
    style.Colors[ImGuiCol_PlotHistogram] = titleTextColor;
    // 柱状图悬浮色强化当前样本，但仍需保留原数据系列归属。
    style.Colors[ImGuiCol_PlotHistogramHovered] = nodeColor;
    // 表头底色固定列语义区域，与数据行建立稳定的纵向起点。
    style.Colors[ImGuiCol_TableHeaderBg] = panelHoverColor;
    // 强表格边框用于外沿和表头分界，承担主要结构线。
    style.Colors[ImGuiCol_TableBorderStrong] = borderColor;
    // 弱表格边框只分隔普通单元格，降低密集网格的视觉噪声。
    style.Colors[ImGuiCol_TableBorderLight] = panelHoverColor;
    // 基础行背景允许透明继承，使表格自然嵌入所在面板。
    style.Colors[ImGuiCol_TableRowBg] = viewBgColor;
    // 交替行仅施加轻微差异，帮助横向追踪而不破坏整体底色。
    style.Colors[ImGuiCol_TableRowBgAlt] = panelHoverColor;
    // 文本选区背景必须兼顾选中辨识度与前景文字可读性。
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(itemGroupColor.x, itemGroupColor.y, itemGroupColor.z, 0.3000f);
    // 拖放目标使用高辨识强调色，明确释放操作将作用的位置。
    style.Colors[ImGuiCol_DragDropTarget] = fullProgressColor;
    // 键盘和手柄导航高亮独立于鼠标悬浮态，避免输入来源含混。
    style.Colors[ImGuiCol_NavHighlight] = titleTextColor;
    // 窗口导航高亮标识待切换窗口，并覆盖多种面板底色。
    style.Colors[ImGuiCol_NavWindowingHighlight] = titleTextColor;
    // 窗口切换期间的遮罩压低非目标内容，但仍保留场景上下文。
    style.Colors[ImGuiCol_NavWindowingDimBg] =
        ImVec4(cloakColor.x, cloakColor.y, cloakColor.z, 0.2500f);
    // 模态遮罩明确阻断下层交互，透明度同时维持来源窗口可见。
    style.Colors[ImGuiCol_ModalWindowDimBg] =
        ImVec4(cloakColor.x, cloakColor.y, cloakColor.z, 0.3000f);
    // 停靠预览色指示潜在落点，透明度允许用户同时判断下层内容。
    style.Colors[ImGuiCol_DockingPreview] =
        ImVec4(typeTextColor.x, typeTextColor.y, typeTextColor.z, 0.3200f);
    // 空停靠节点使用独立底色，提示这里尚未承载实际窗口。
    style.Colors[ImGuiCol_DockingEmptyBg] = baseBgColor;
    style.Colors[ImGuiCol_TextLink]       = titleTextColor;

    style.WindowBorderSize = 1.0f;
}

static void applyComfortableLightStyle(ImGuiStyle& style)
{
    // Comfortable Light Orange 样式，来源为 ImThemes 的 SouthCraftX 配色。

    // 全局透明度会乘到整套颜色上，主题在此保持最终合成基准。
    style.Alpha = 1.0f;
    // 禁用态通过统一衰减与可交互控件拉开层级，无需逐项改写颜色。
    style.DisabledAlpha = 1.0f;
    // 窗口内边距同时决定内容密度和停靠面板的呼吸空间。
    style.WindowPadding            = ImVec2(20.0f, 20.0f);
    style.WindowRounding           = 11.5f;
    style.WindowBorderSize         = 0.0f;
    style.WindowMinSize            = ImVec2(20.0f, 20.0f);
    style.WindowTitleAlign         = ImVec2(0.5f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.ChildRounding            = 20.0f;
    style.ChildBorderSize          = 1.0f;
    style.PopupRounding            = 17.4f;
    style.PopupBorderSize          = 1.0f;
    // 框体内边距决定输入框和按钮的文字基线及垂直触达面积。
    style.FramePadding    = ImVec2(20.0f, 3.4f);
    style.FrameRounding   = 11.9f;
    style.FrameBorderSize = 0.0f;
    // 项目间距约束同级控件节奏，也是多数自动布局的基础步长。
    style.ItemSpacing      = ImVec2(8.9f, 13.4f);
    style.ItemInnerSpacing = ImVec2(7.1f, 1.8f);
    // 单元格留白保证表格密集展示时文字和分隔线仍有安全距离。
    style.CellPadding       = ImVec2(12.1f, 9.2f);
    style.IndentSpacing     = 0.0f;
    style.ColumnsMinSpacing = 8.7f;
    // 滚动条宽度在可点击性与内容占用之间维持主题取舍。
    style.ScrollbarSize     = 11.6f;
    style.ScrollbarRounding = 15.9f;
    style.GrabMinSize       = 3.7f;
    style.GrabRounding      = 20.0f;
    style.TabRounding       = 9.8f;
    style.TabBorderSize     = 0.0f;
    // 选中标签的关闭按钮阈值控制窄页签中操作入口的保留策略。
    style.TabCloseButtonMinWidthSelected = 0.0f;
    // 未选中标签单独设阈值，避免拥挤时关闭图标喧宾夺主。
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    // 正文色承担主要信息层，必须与窗口和框体背景保持足够对比。
    style.Colors[ImGuiCol_Text] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    // 弱化文字用于不可用或次要信息，但仍需在主背景上可辨识。
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4(0.7254902f, 0.68235296f, 0.54901963f, 1.0f);
    // 窗口底色奠定整套主题明度，后续容器和控件颜色均以此为参照。
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.92156863f, 0.9137255f, 0.8980392f, 1.0f);
    // 子窗口背景可选择透明继承，避免嵌套区域产生多余色块。
    style.Colors[ImGuiCol_ChildBg] =
        ImVec4(0.90588236f, 0.8980392f, 0.88235295f, 1.0f);
    // 弹窗背景需要遮住下层内容，同时以透明度保留空间关系。
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.92156863f, 0.9137255f, 0.8980392f, 1.0f);
    // 边框色用于低成本描绘控件和容器边界，透明度决定强调强度。
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.84313726f, 0.83137256f, 0.80784315f, 1.0f);
    // 边框阴影独立配置，以便主题明确选择平面或浮起效果。
    style.Colors[ImGuiCol_BorderShadow] =
        ImVec4(0.92156863f, 0.9137255f, 0.8980392f, 1.0f);
    // 框体常态是输入和选择控件的静止基面，也是交互渐变的起点。
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.8862745f, 0.8745098f, 0.84705883f, 1.0f);
    // 框体悬浮色只提示可交互性，应比激活态更克制。
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.84313726f, 0.83137256f, 0.80784315f, 1.0f);
    // 框体激活色确认当前操作焦点，并与悬浮态形成连续反馈。
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.84313726f, 0.83137256f, 0.80784315f, 1.0f);
    // 非活动标题栏降低存在感，让当前编辑窗口更容易被识别。
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.9529412f, 0.94509804f, 0.92941177f, 1.0f);
    // 活动标题栏提高层级，承担当前窗口焦点的主要视觉提示。
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.9529412f, 0.94509804f, 0.92941177f, 1.0f);
    // 折叠标题仍保留窗口身份，但用透明度减少对内容的干扰。
    style.Colors[ImGuiCol_TitleBgCollapsed] =
        ImVec4(0.92156863f, 0.9137255f, 0.8980392f, 1.0f);
    // 菜单栏底色连接窗口标题与内容区，避免形成突兀的横向色带。
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.9019608f, 0.89411765f, 0.8784314f, 1.0f);
    // 滚动轨道应融入容器背景，使抓手成为滚动位置的主要提示。
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.9529412f, 0.94509804f, 0.92941177f, 1.0f);
    // 滚动抓手常态需持续可见，又不能盖过正文和主要操作。
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.88235295f, 0.8666667f, 0.8509804f, 1.0f);
    // 滚动抓手悬浮态提高对比，提示鼠标已经进入可拖动区域。
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.84313726f, 0.83137256f, 0.80784315f, 1.0f);
    // 滚动抓手激活态确认持续拖动，颜色变化需强于悬浮态。
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.88235295f, 0.8666667f, 0.8509804f, 1.0f);
    // 勾选标记承载布尔结果，应从框体背景中清晰跳出。
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.96862745f, 0.050980393f, 0.15686275f, 1.0f);
    // 滑块抓手常态标记当前值位置，并与轨道保持明度区分。
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.9647059f, 0.8f, 0.02745098f, 1.0f);
    // 滑块活动抓手强调正在调整的数值，释放后回落至常态。
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.96862745f, 0.5882353f, 0.03529412f, 1.0f);
    // 按钮常态建立操作面的基础层级，为悬浮和按下反馈预留变化空间。
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.88235295f, 0.8666667f, 0.8509804f, 1.0f);
    // 按钮悬浮态只表达指向，不应与已按下状态混淆。
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.81960785f, 0.8117647f, 0.8039216f, 1.0f);
    // 按钮激活态提供按下确认，需要成为三态中最明确的反馈。
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.84705883f, 0.84705883f, 0.84705883f, 1.0f);
    // Header 常态同时服务树节点、列表选项和折叠区标题。
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.85882354f, 0.8352941f, 0.7921569f, 1.0f);
    // Header 悬浮态覆盖较大行区域，应控制亮度避免产生闪烁感。
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.89411765f, 0.89411765f, 0.89411765f, 1.0f);
    // Header 激活态标识当前选择或按压结果，与按钮反馈保持一致。
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.92156863f, 0.9137255f, 0.8980392f, 1.0f);
    // 分隔线以低对比划分区域，不与可拖动边缘争夺注意力。
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.87058824f, 0.8509804f, 0.80784315f, 1.0f);
    // 可交互分隔线在悬浮时增强，提示其支持调整布局。
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.84313726f, 0.8156863f, 0.7490196f, 1.0f);
    // 分隔线拖动期间使用最强状态，持续反馈布局调整操作。
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.84313726f, 0.8156863f, 0.7490196f, 1.0f);
    // 缩放抓手常态保持含蓄，避免遮挡窗口角落内容。
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.85490197f, 0.85490197f, 0.85490197f, 1.0f);
    // 缩放抓手悬浮态显露可调整方向，强化边角命中提示。
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.96862745f, 0.050980393f, 0.15686275f, 1.0f);
    // 缩放抓手激活态维持拖动反馈，直到尺寸调整结束。
    style.Colors[ImGuiCol_ResizeGripActive] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    // 普通标签以中等层级承载页面入口，不抢占当前标签的焦点。
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.92156863f, 0.9137255f, 0.8980392f, 1.0f);
    // 标签悬浮色提前提示可切换目标，并需兼容已选中标签的叠加状态。
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.88235295f, 0.8666667f, 0.8509804f, 1.0f);
    // 活动标签与内容面板建立连续关系，明确当前可见页面。
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.88235295f, 0.8666667f, 0.8509804f, 1.0f);
    // 失焦停靠节点的普通标签进一步降级，突出当前活动窗口组。
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.92156863f, 0.9137255f, 0.8980392f, 1.0f);
    // 失焦组仍需标识其已选页面，因此保留受抑制的活动态。
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.8745098f, 0.7254902f, 0.42745098f, 1.0f);
    // 折线图常态颜色优先保证细线在背景上的连续可读性。
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.47843137f, 0.4f, 0.29803923f, 1.0f);
    // 折线悬浮态采用明显强调色，便于定位密集曲线中的目标。
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(0.9607843f, 0.019607844f, 0.11764706f, 1.0f);
    // 柱状图使用独立数据色，避免与按钮等交互强调色混淆。
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.90588236f, 0.6627451f, 0.30980393f, 1.0f);
    // 柱状图悬浮色强化当前样本，但仍需保留原数据系列归属。
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(0.6392157f, 0.39607844f, 0.043137256f, 1.0f);
    // 表头底色固定列语义区域，与数据行建立稳定的纵向起点。
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.9529412f, 0.94509804f, 0.92941177f, 1.0f);
    // 强表格边框用于外沿和表头分界，承担主要结构线。
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.9529412f, 0.94509804f, 0.92941177f, 1.0f);
    // 弱表格边框只分隔普通单元格，降低密集网格的视觉噪声。
    style.Colors[ImGuiCol_TableBorderLight] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // 基础行背景允许透明继承，使表格自然嵌入所在面板。
    style.Colors[ImGuiCol_TableRowBg] =
        ImVec4(0.88235295f, 0.8666667f, 0.8509804f, 1.0f);
    // 交替行仅施加轻微差异，帮助横向追踪而不破坏整体底色。
    style.Colors[ImGuiCol_TableRowBgAlt] =
        ImVec4(0.9019608f, 0.89411765f, 0.8784314f, 1.0f);
    // 文本选区背景必须兼顾选中辨识度与前景文字可读性。
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(0.0627451f, 0.0627451f, 0.0627451f, 1.0f);
    // 拖放目标使用高辨识强调色，明确释放操作将作用的位置。
    style.Colors[ImGuiCol_DragDropTarget] =
        ImVec4(0.5019608f, 0.4862745f, 0.0f, 1.0f);
    // 键盘和手柄导航高亮独立于鼠标悬浮态，避免输入来源含混。
    style.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.73333335f, 0.70980394f, 0.0f, 1.0f);
    // 窗口导航高亮标识待切换窗口，并覆盖多种面板底色。
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(0.5019608f, 0.4862745f, 0.0f, 1.0f);
    // 窗口切换期间的遮罩压低非目标内容，但仍保留场景上下文。
    style.Colors[ImGuiCol_NavWindowingDimBg] =
        ImVec4(0.8039216f, 0.8235294f, 0.45490196f, 0.502f);
    // 模态遮罩明确阻断下层交互，透明度同时维持来源窗口可见。
    style.Colors[ImGuiCol_ModalWindowDimBg] =
        ImVec4(0.8039216f, 0.8235294f, 0.45490196f, 0.502f);
}

static void applyHazyDarkStyle(ImGuiStyle& style)
{
    // Hazy Dark 样式，来源为 ImThemes 的 kaitabuchi314 配色。

    // 全局透明度会乘到整套颜色上，主题在此保持最终合成基准。
    style.Alpha = 1.0f;
    // 禁用态通过统一衰减与可交互控件拉开层级，无需逐项改写颜色。
    style.DisabledAlpha = 0.6f;
    // 窗口内边距同时决定内容密度和停靠面板的呼吸空间。
    style.WindowPadding            = ImVec2(5.5f, 8.3f);
    style.WindowRounding           = 4.5f;
    style.WindowBorderSize         = 1.0f;
    style.WindowMinSize            = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign         = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_Left;
    style.ChildRounding            = 3.2f;
    style.ChildBorderSize          = 1.0f;
    style.PopupRounding            = 2.7f;
    style.PopupBorderSize          = 1.0f;
    // 框体内边距决定输入框和按钮的文字基线及垂直触达面积。
    style.FramePadding    = ImVec2(4.0f, 3.0f);
    style.FrameRounding   = 2.4f;
    style.FrameBorderSize = 0.0f;
    // 项目间距约束同级控件节奏，也是多数自动布局的基础步长。
    style.ItemSpacing      = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
    // 单元格留白保证表格密集展示时文字和分隔线仍有安全距离。
    style.CellPadding       = ImVec2(4.0f, 2.0f);
    style.IndentSpacing     = 21.0f;
    style.ColumnsMinSpacing = 6.0f;
    // 滚动条宽度在可点击性与内容占用之间维持主题取舍。
    style.ScrollbarSize     = 14.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabMinSize       = 10.0f;
    style.GrabRounding      = 3.2f;
    style.TabRounding       = 3.5f;
    style.TabBorderSize     = 1.0f;
    // 选中标签的关闭按钮阈值控制窄页签中操作入口的保留策略。
    style.TabCloseButtonMinWidthSelected = 0.0f;
    // 未选中标签单独设阈值，避免拥挤时关闭图标喧宾夺主。
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    // 正文色承担主要信息层，必须与窗口和框体背景保持足够对比。
    style.Colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // 弱化文字用于不可用或次要信息，但仍需在主背景上可辨识。
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4(0.49803922f, 0.49803922f, 0.49803922f, 1.0f);
    // 窗口底色奠定整套主题明度，后续容器和控件颜色均以此为参照。
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.05882353f, 0.05882353f, 0.05882353f, 0.94f);
    // 子窗口背景可选择透明继承，避免嵌套区域产生多余色块。
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 弹窗背景需要遮住下层内容，同时以透明度保留空间关系。
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.078431375f, 0.078431375f, 0.078431375f, 0.94f);
    // 边框色用于低成本描绘控件和容器边界，透明度决定强调强度。
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.42745098f, 0.42745098f, 0.49803922f, 0.5f);
    // 边框阴影独立配置，以便主题明确选择平面或浮起效果。
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 框体常态是输入和选择控件的静止基面，也是交互渐变的起点。
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.13725491f, 0.17254902f, 0.22745098f, 0.54f);
    // 框体悬浮色只提示可交互性，应比激活态更克制。
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.21176471f, 0.25490198f, 0.3019608f, 0.4f);
    // 框体激活色确认当前操作焦点，并与悬浮态形成连续反馈。
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.043137256f, 0.047058824f, 0.047058824f, 0.67f);
    // 非活动标题栏降低存在感，让当前编辑窗口更容易被识别。
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.039215688f, 0.039215688f, 0.039215688f, 1.0f);
    // 活动标题栏提高层级，承担当前窗口焦点的主要视觉提示。
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.078431375f, 0.08235294f, 0.09019608f, 1.0f);
    // 折叠标题仍保留窗口身份，但用透明度减少对内容的干扰。
    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.0f, 0.0f, 0.0f, 0.51f);
    // 菜单栏底色连接窗口标题与内容区，避免形成突兀的横向色带。
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.13725491f, 0.13725491f, 0.13725491f, 1.0f);
    // 滚动轨道应融入容器背景，使抓手成为滚动位置的主要提示。
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.019607844f, 0.019607844f, 0.019607844f, 0.53f);
    // 滚动抓手常态需持续可见，又不能盖过正文和主要操作。
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.30980393f, 0.30980393f, 0.30980393f, 1.0f);
    // 滚动抓手悬浮态提高对比，提示鼠标已经进入可拖动区域。
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.40784314f, 0.40784314f, 0.40784314f, 1.0f);
    // 滚动抓手激活态确认持续拖动，颜色变化需强于悬浮态。
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.50980395f, 0.50980395f, 0.50980395f, 1.0f);
    // 勾选标记承载布尔结果，应从框体背景中清晰跳出。
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.7176471f, 0.78431374f, 0.84313726f, 1.0f);
    // 滑块抓手常态标记当前值位置，并与轨道保持明度区分。
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.47843137f, 0.5254902f, 0.57254905f, 1.0f);
    // 滑块活动抓手强调正在调整的数值，释放后回落至常态。
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.2901961f, 0.31764707f, 0.3529412f, 1.0f);
    // 按钮常态建立操作面的基础层级，为悬浮和按下反馈预留变化空间。
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.14901961f, 0.16078432f, 0.1764706f, 0.4f);
    // 按钮悬浮态只表达指向，不应与已按下状态混淆。
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.13725491f, 0.14509805f, 0.15686275f, 1.0f);
    // 按钮激活态提供按下确认，需要成为三态中最明确的反馈。
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.078431375f, 0.08627451f, 0.09019608f, 1.0f);
    // Header 常态同时服务树节点、列表选项和折叠区标题。
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.19607843f, 0.21568628f, 0.23921569f, 0.31f);
    // Header 悬浮态覆盖较大行区域，应控制亮度避免产生闪烁感。
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.16470589f, 0.1764706f, 0.19215687f, 0.8f);
    // Header 激活态标识当前选择或按压结果，与按钮反馈保持一致。
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.07450981f, 0.08235294f, 0.09019608f, 1.0f);
    // 分隔线以低对比划分区域，不与可拖动边缘争夺注意力。
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.42745098f, 0.42745098f, 0.49803922f, 0.5f);
    // 可交互分隔线在悬浮时增强，提示其支持调整布局。
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.23921569f, 0.3254902f, 0.42352942f, 0.78f);
    // 分隔线拖动期间使用最强状态，持续反馈布局调整操作。
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.27450982f, 0.38039216f, 0.49803922f, 1.0f);
    // 缩放抓手常态保持含蓄，避免遮挡窗口角落内容。
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.2901961f, 0.32941177f, 0.3764706f, 0.2f);
    // 缩放抓手悬浮态显露可调整方向，强化边角命中提示。
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.23921569f, 0.29803923f, 0.36862746f, 0.67f);
    // 缩放抓手激活态维持拖动反馈，直到尺寸调整结束。
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.16470589f, 0.1764706f, 0.1882353f, 0.95f);
    // 普通标签以中等层级承载页面入口，不抢占当前标签的焦点。
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.11764706f, 0.1254902f, 0.13333334f, 0.862f);
    // 标签悬浮色提前提示可切换目标，并需兼容已选中标签的叠加状态。
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.32941177f, 0.40784314f, 0.5019608f, 0.8f);
    // 活动标签与内容面板建立连续关系，明确当前可见页面。
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.24313726f, 0.24705882f, 0.25490198f, 1.0f);
    // 失焦停靠节点的普通标签进一步降级，突出当前活动窗口组。
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.06666667f, 0.101960786f, 0.14509805f, 0.9724f);
    // 失焦组仍需标识其已选页面，因此保留受抑制的活动态。
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.13333334f, 0.25882354f, 0.42352942f, 1.0f);
    // 折线图常态颜色优先保证细线在背景上的连续可读性。
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.60784316f, 0.60784316f, 0.60784316f, 1.0f);
    // 折线悬浮态采用明显强调色，便于定位密集曲线中的目标。
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(1.0f, 0.42745098f, 0.34901962f, 1.0f);
    // 柱状图使用独立数据色，避免与按钮等交互强调色混淆。
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.8980392f, 0.69803923f, 0.0f, 1.0f);
    // 柱状图悬浮色强化当前样本，但仍需保留原数据系列归属。
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(1.0f, 0.6f, 0.0f, 1.0f);
    // 表头底色固定列语义区域，与数据行建立稳定的纵向起点。
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.1882353f, 0.1882353f, 0.2f, 1.0f);
    // 强表格边框用于外沿和表头分界，承担主要结构线。
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.30980393f, 0.30980393f, 0.34901962f, 1.0f);
    // 弱表格边框只分隔普通单元格，降低密集网格的视觉噪声。
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(0.22745098f, 0.22745098f, 0.24705882f, 1.0f);
    // 基础行背景允许透明继承，使表格自然嵌入所在面板。
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 交替行仅施加轻微差异，帮助横向追踪而不破坏整体底色。
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    // 文本选区背景必须兼顾选中辨识度与前景文字可读性。
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 0.35f);
    // 拖放目标使用高辨识强调色，明确释放操作将作用的位置。
    style.Colors[ImGuiCol_DragDropTarget] = ImVec4(1.0f, 1.0f, 0.0f, 0.9f);
    // 键盘和手柄导航高亮独立于鼠标悬浮态，避免输入来源含混。
    style.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    // 窗口导航高亮标识待切换窗口，并覆盖多种面板底色。
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    // 窗口切换期间的遮罩压低非目标内容，但仍保留场景上下文。
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    // 模态遮罩明确阻断下层交互，透明度同时维持来源窗口可见。
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.35f);
}

static void applyEverforestStyle(ImGuiStyle& style)
{
    // Everforest 样式，来源为 ImThemes 的 DestroyerDarkNess 配色。

    // 全局透明度会乘到整套颜色上，主题在此保持最终合成基准。
    style.Alpha = 1.0f;
    // 禁用态通过统一衰减与可交互控件拉开层级，无需逐项改写颜色。
    style.DisabledAlpha = 0.6f;
    // 窗口内边距同时决定内容密度和停靠面板的呼吸空间。
    style.WindowPadding            = ImVec2(6.0f, 3.0f);
    style.WindowRounding           = 6.0f;
    style.WindowBorderSize         = 1.0f;
    style.WindowMinSize            = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign         = ImVec2(0.5f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_Left;
    style.ChildRounding            = 0.0f;
    style.ChildBorderSize          = 1.0f;
    style.PopupRounding            = 0.0f;
    style.PopupBorderSize          = 1.0f;
    // 框体内边距决定输入框和按钮的文字基线及垂直触达面积。
    style.FramePadding    = ImVec2(5.0f, 1.0f);
    style.FrameRounding   = 3.0f;
    style.FrameBorderSize = 1.0f;
    // 项目间距约束同级控件节奏，也是多数自动布局的基础步长。
    style.ItemSpacing      = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
    // 单元格留白保证表格密集展示时文字和分隔线仍有安全距离。
    style.CellPadding       = ImVec2(4.0f, 2.0f);
    style.IndentSpacing     = 21.0f;
    style.ColumnsMinSpacing = 6.0f;
    // 滚动条宽度在可点击性与内容占用之间维持主题取舍。
    style.ScrollbarSize     = 13.0f;
    style.ScrollbarRounding = 16.0f;
    style.GrabMinSize       = 20.0f;
    style.GrabRounding      = 2.0f;
    style.TabRounding       = 4.0f;
    style.TabBorderSize     = 1.0f;
    // 选中标签的关闭按钮阈值控制窄页签中操作入口的保留策略。
    style.TabCloseButtonMinWidthSelected = 0.0f;
    // 未选中标签单独设阈值，避免拥挤时关闭图标喧宾夺主。
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    // 正文色承担主要信息层，必须与窗口和框体背景保持足够对比。
    style.Colors[ImGuiCol_Text] =
        ImVec4(0.8745098f, 0.87058824f, 0.8392157f, 1.0f);
    // 弱化文字用于不可用或次要信息，但仍需在主背景上可辨识。
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4(0.58431375f, 0.57254905f, 0.52156866f, 1.0f);
    // 窗口底色奠定整套主题明度，后续容器和控件颜色均以此为参照。
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.23529412f, 0.21960784f, 0.21176471f, 1.0f);
    // 子窗口背景可选择透明继承，避免嵌套区域产生多余色块。
    style.Colors[ImGuiCol_ChildBg] =
        ImVec4(0.23529412f, 0.21960784f, 0.21176471f, 1.0f);
    // 弹窗背景需要遮住下层内容，同时以透明度保留空间关系。
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.23529412f, 0.21960784f, 0.21176471f, 1.0f);
    // 边框色用于低成本描绘控件和容器边界，透明度决定强调强度。
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.3137255f, 0.28627452f, 0.27058825f, 1.0f);
    // 边框阴影独立配置，以便主题明确选择平面或浮起效果。
    style.Colors[ImGuiCol_BorderShadow] =
        ImVec4(0.23529412f, 0.21960784f, 0.21176471f, 0.0f);
    // 框体常态是输入和选择控件的静止基面，也是交互渐变的起点。
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.3137255f, 0.28627452f, 0.27058825f, 1.0f);
    // 框体悬浮色只提示可交互性，应比激活态更克制。
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.4f, 0.36078432f, 0.32941177f, 1.0f);
    // 框体激活色确认当前操作焦点，并与悬浮态形成连续反馈。
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.4862745f, 0.43529412f, 0.39215687f, 1.0f);
    // 非活动标题栏降低存在感，让当前编辑窗口更容易被识别。
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.23529412f, 0.21960784f, 0.21176471f, 1.0f);
    // 活动标题栏提高层级，承担当前窗口焦点的主要视觉提示。
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.3137255f, 0.28627452f, 0.27058825f, 1.0f);
    // 折叠标题仍保留窗口身份，但用透明度减少对内容的干扰。
    style.Colors[ImGuiCol_TitleBgCollapsed] =
        ImVec4(0.23529412f, 0.21960784f, 0.21176471f, 1.0f);
    // 菜单栏底色连接窗口标题与内容区，避免形成突兀的横向色带。
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.3137255f, 0.28627452f, 0.27058825f, 1.0f);
    // 滚动轨道应融入容器背景，使抓手成为滚动位置的主要提示。
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.23529412f, 0.21960784f, 0.21176471f, 1.0f);
    // 滚动抓手常态需持续可见，又不能盖过正文和主要操作。
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.4862745f, 0.43529412f, 0.39215687f, 1.0f);
    // 滚动抓手悬浮态提高对比，提示鼠标已经进入可拖动区域。
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.4f, 0.36078432f, 0.32941177f, 1.0f);
    // 滚动抓手激活态确认持续拖动，颜色变化需强于悬浮态。
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.3137255f, 0.28627452f, 0.27058825f, 1.0f);
    // 勾选标记承载布尔结果，应从框体背景中清晰跳出。
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.59607846f, 0.5921569f, 0.101960786f, 1.0f);
    // 滑块抓手常态标记当前值位置，并与轨道保持明度区分。
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.59607846f, 0.5921569f, 0.101960786f, 1.0f);
    // 滑块活动抓手强调正在调整的数值，释放后回落至常态。
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.7411765f, 0.7176471f, 0.41960785f, 1.0f);
    // 按钮常态建立操作面的基础层级，为悬浮和按下反馈预留变化空间。
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.4f, 0.36078432f, 0.32941177f, 1.0f);
    // 按钮悬浮态只表达指向，不应与已按下状态混淆。
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.4862745f, 0.43529412f, 0.39215687f, 1.0f);
    // 按钮激活态提供按下确认，需要成为三态中最明确的反馈。
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.7411765f, 0.7176471f, 0.41960785f, 1.0f);
    // Header 常态同时服务树节点、列表选项和折叠区标题。
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.4f, 0.36078432f, 0.32941177f, 1.0f);
    // Header 悬浮态覆盖较大行区域，应控制亮度避免产生闪烁感。
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.4862745f, 0.43529412f, 0.39215687f, 1.0f);
    // Header 激活态标识当前选择或按压结果，与按钮反馈保持一致。
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.7411765f, 0.7176471f, 0.41960785f, 1.0f);
    // 分隔线以低对比划分区域，不与可拖动边缘争夺注意力。
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.7411765f, 0.7176471f, 0.41960785f, 1.0f);
    // 可交互分隔线在悬浮时增强，提示其支持调整布局。
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.4862745f, 0.43529412f, 0.39215687f, 1.0f);
    // 分隔线拖动期间使用最强状态，持续反馈布局调整操作。
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.7411765f, 0.7176471f, 0.41960785f, 1.0f);
    // 缩放抓手常态保持含蓄，避免遮挡窗口角落内容。
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.4f, 0.36078432f, 0.32941177f, 1.0f);
    // 缩放抓手悬浮态显露可调整方向，强化边角命中提示。
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.4862745f, 0.43529412f, 0.39215687f, 1.0f);
    // 缩放抓手激活态维持拖动反馈，直到尺寸调整结束。
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.7411765f, 0.7176471f, 0.41960785f, 1.0f);
    // 普通标签以中等层级承载页面入口，不抢占当前标签的焦点。
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.3137255f, 0.28627452f, 0.27058825f, 1.0f);
    // 标签悬浮色提前提示可切换目标，并需兼容已选中标签的叠加状态。
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.4f, 0.36078432f, 0.32941177f, 1.0f);
    // 活动标签与内容面板建立连续关系，明确当前可见页面。
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.4862745f, 0.43529412f, 0.39215687f, 1.0f);
    // 失焦停靠节点的普通标签进一步降级，突出当前活动窗口组。
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.23529412f, 0.21960784f, 0.21176471f, 0.972549f);
    // 失焦组仍需标识其已选页面，因此保留受抑制的活动态。
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.3137255f, 0.28627452f, 0.27058825f, 1.0f);
    // 折线图常态颜色优先保证细线在背景上的连续可读性。
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.7411765f, 0.7176471f, 0.41960785f, 1.0f);
    // 折线悬浮态采用明显强调色，便于定位密集曲线中的目标。
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(0.8392157f, 0.7490196f, 0.4f, 1.0f);
    // 柱状图使用独立数据色，避免与按钮等交互强调色混淆。
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.7411765f, 0.7176471f, 0.41960785f, 1.0f);
    // 柱状图悬浮色强化当前样本，但仍需保留原数据系列归属。
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(0.8392157f, 0.7490196f, 0.4f, 1.0f);
    // 表头底色固定列语义区域，与数据行建立稳定的纵向起点。
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.8392157f, 0.7490196f, 0.4f, 0.60944206f);
    // 强表格边框用于外沿和表头分界，承担主要结构线。
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.30980393f, 0.30980393f, 0.34901962f, 1.0f);
    // 弱表格边框只分隔普通单元格，降低密集网格的视觉噪声。
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(0.22745098f, 0.22745098f, 0.24705882f, 1.0f);
    // 基础行背景允许透明继承，使表格自然嵌入所在面板。
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 交替行仅施加轻微差异，帮助横向追踪而不破坏整体底色。
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    // 文本选区背景必须兼顾选中辨识度与前景文字可读性。
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(0.8392157f, 0.7490196f, 0.4f, 0.43137255f);
    // 拖放目标使用高辨识强调色，明确释放操作将作用的位置。
    style.Colors[ImGuiCol_DragDropTarget] =
        ImVec4(0.8392157f, 0.7490196f, 0.4f, 0.9019608f);
    // 键盘和手柄导航高亮独立于鼠标悬浮态，避免输入来源含混。
    style.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.23529412f, 0.21960784f, 0.21176471f, 1.0f);
    // 窗口导航高亮标识待切换窗口，并覆盖多种面板底色。
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    // 窗口切换期间的遮罩压低非目标内容，但仍保留场景上下文。
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    // 模态遮罩明确阻断下层交互，透明度同时维持来源窗口可见。
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.35f);
}

static void applyWindarkStyle(ImGuiStyle& style)
{
    // Windark 样式，来源为 ImThemes 的 DestroyerDarkNess 配色。

    // 全局透明度会乘到整套颜色上，主题在此保持最终合成基准。
    style.Alpha = 1.0f;
    // 禁用态通过统一衰减与可交互控件拉开层级，无需逐项改写颜色。
    style.DisabledAlpha = 0.6f;
    // 窗口内边距同时决定内容密度和停靠面板的呼吸空间。
    style.WindowPadding            = ImVec2(8.0f, 8.0f);
    style.WindowRounding           = 8.4f;
    style.WindowBorderSize         = 1.0f;
    style.WindowMinSize            = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign         = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_Right;
    style.ChildRounding            = 3.0f;
    style.ChildBorderSize          = 1.0f;
    style.PopupRounding            = 3.0f;
    style.PopupBorderSize          = 1.0f;
    // 框体内边距决定输入框和按钮的文字基线及垂直触达面积。
    style.FramePadding    = ImVec2(4.0f, 3.0f);
    style.FrameRounding   = 3.0f;
    style.FrameBorderSize = 1.0f;
    // 项目间距约束同级控件节奏，也是多数自动布局的基础步长。
    style.ItemSpacing      = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
    // 单元格留白保证表格密集展示时文字和分隔线仍有安全距离。
    style.CellPadding       = ImVec2(4.0f, 2.0f);
    style.IndentSpacing     = 21.0f;
    style.ColumnsMinSpacing = 6.0f;
    // 滚动条宽度在可点击性与内容占用之间维持主题取舍。
    style.ScrollbarSize     = 5.6f;
    style.ScrollbarRounding = 18.0f;
    style.GrabMinSize       = 10.0f;
    style.GrabRounding      = 3.0f;
    style.TabRounding       = 3.0f;
    style.TabBorderSize     = 0.0f;
    // 选中标签的关闭按钮阈值控制窄页签中操作入口的保留策略。
    style.TabCloseButtonMinWidthSelected = 0.0f;
    // 未选中标签单独设阈值，避免拥挤时关闭图标喧宾夺主。
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    // 正文色承担主要信息层，必须与窗口和框体背景保持足够对比。
    style.Colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // 弱化文字用于不可用或次要信息，但仍需在主背景上可辨识。
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
    // 窗口底色奠定整套主题明度，后续容器和控件颜色均以此为参照。
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.1254902f, 0.1254902f, 0.1254902f, 1.0f);
    // 子窗口背景可选择透明继承，避免嵌套区域产生多余色块。
    style.Colors[ImGuiCol_ChildBg] =
        ImVec4(0.1254902f, 0.1254902f, 0.1254902f, 1.0f);
    // 弹窗背景需要遮住下层内容，同时以透明度保留空间关系。
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.16862746f, 0.16862746f, 0.16862746f, 1.0f);
    // 边框色用于低成本描绘控件和容器边界，透明度决定强调强度。
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.2509804f, 0.2509804f, 0.2509804f, 1.0f);
    // 边框阴影独立配置，以便主题明确选择平面或浮起效果。
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 框体常态是输入和选择控件的静止基面，也是交互渐变的起点。
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.16862746f, 0.16862746f, 0.16862746f, 1.0f);
    // 框体悬浮色只提示可交互性，应比激活态更克制。
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.21568628f, 0.21568628f, 0.21568628f, 1.0f);
    // 框体激活色确认当前操作焦点，并与悬浮态形成连续反馈。
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.2509804f, 0.2509804f, 0.2509804f, 1.0f);
    // 非活动标题栏降低存在感，让当前编辑窗口更容易被识别。
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.1254902f, 0.1254902f, 0.1254902f, 1.0f);
    // 活动标题栏提高层级，承担当前窗口焦点的主要视觉提示。
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.16862746f, 0.16862746f, 0.16862746f, 1.0f);
    // 折叠标题仍保留窗口身份，但用透明度减少对内容的干扰。
    style.Colors[ImGuiCol_TitleBgCollapsed] =
        ImVec4(0.1254902f, 0.1254902f, 0.1254902f, 1.0f);
    // 菜单栏底色连接窗口标题与内容区，避免形成突兀的横向色带。
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.16862746f, 0.16862746f, 0.16862746f, 1.0f);
    // 滚动轨道应融入容器背景，使抓手成为滚动位置的主要提示。
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.1254902f, 0.1254902f, 0.1254902f, 1.0f);
    // 滚动抓手常态需持续可见，又不能盖过正文和主要操作。
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.2509804f, 0.2509804f, 0.2509804f, 1.0f);
    // 滚动抓手悬浮态提高对比，提示鼠标已经进入可拖动区域。
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.3019608f, 0.3019608f, 0.3019608f, 1.0f);
    // 滚动抓手激活态确认持续拖动，颜色变化需强于悬浮态。
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.34901962f, 0.34901962f, 0.34901962f, 1.0f);
    // 勾选标记承载布尔结果，应从框体背景中清晰跳出。
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.0f, 0.47058824f, 0.84313726f, 1.0f);
    // 滑块抓手常态标记当前值位置，并与轨道保持明度区分。
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.0f, 0.47058824f, 0.84313726f, 1.0f);
    // 滑块活动抓手强调正在调整的数值，释放后回落至常态。
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.0f, 0.32941177f, 0.6f, 1.0f);
    // 按钮常态建立操作面的基础层级，为悬浮和按下反馈预留变化空间。
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.16862746f, 0.16862746f, 0.16862746f, 1.0f);
    // 按钮悬浮态只表达指向，不应与已按下状态混淆。
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.21568628f, 0.21568628f, 0.21568628f, 1.0f);
    // 按钮激活态提供按下确认，需要成为三态中最明确的反馈。
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.2509804f, 0.2509804f, 0.2509804f, 1.0f);
    // Header 常态同时服务树节点、列表选项和折叠区标题。
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.21568628f, 0.21568628f, 0.21568628f, 1.0f);
    // Header 悬浮态覆盖较大行区域，应控制亮度避免产生闪烁感。
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.2509804f, 0.2509804f, 0.2509804f, 1.0f);
    // Header 激活态标识当前选择或按压结果，与按钮反馈保持一致。
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.3019608f, 0.3019608f, 0.3019608f, 1.0f);
    // 分隔线以低对比划分区域，不与可拖动边缘争夺注意力。
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.21568628f, 0.21568628f, 0.21568628f, 1.0f);
    // 可交互分隔线在悬浮时增强，提示其支持调整布局。
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.2509804f, 0.2509804f, 0.2509804f, 1.0f);
    // 分隔线拖动期间使用最强状态，持续反馈布局调整操作。
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.3019608f, 0.3019608f, 0.3019608f, 1.0f);
    // 缩放抓手常态保持含蓄，避免遮挡窗口角落内容。
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.21568628f, 0.21568628f, 0.21568628f, 1.0f);
    // 缩放抓手悬浮态显露可调整方向，强化边角命中提示。
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.2509804f, 0.2509804f, 0.2509804f, 1.0f);
    // 缩放抓手激活态维持拖动反馈，直到尺寸调整结束。
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.3019608f, 0.3019608f, 0.3019608f, 1.0f);
    // 普通标签以中等层级承载页面入口，不抢占当前标签的焦点。
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.16862746f, 0.16862746f, 0.16862746f, 1.0f);
    // 标签悬浮色提前提示可切换目标，并需兼容已选中标签的叠加状态。
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.21568628f, 0.21568628f, 0.21568628f, 1.0f);
    // 活动标签与内容面板建立连续关系，明确当前可见页面。
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.2509804f, 0.2509804f, 0.2509804f, 1.0f);
    // 失焦停靠节点的普通标签进一步降级，突出当前活动窗口组。
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.16862746f, 0.16862746f, 0.16862746f, 1.0f);
    // 失焦组仍需标识其已选页面，因此保留受抑制的活动态。
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.21568628f, 0.21568628f, 0.21568628f, 1.0f);
    // 折线图常态颜色优先保证细线在背景上的连续可读性。
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.0f, 0.47058824f, 0.84313726f, 1.0f);
    // 折线悬浮态采用明显强调色，便于定位密集曲线中的目标。
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(0.0f, 0.32941177f, 0.6f, 1.0f);
    // 柱状图使用独立数据色，避免与按钮等交互强调色混淆。
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.0f, 0.47058824f, 0.84313726f, 1.0f);
    // 柱状图悬浮色强化当前样本，但仍需保留原数据系列归属。
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(0.0f, 0.32941177f, 0.6f, 1.0f);
    // 表头底色固定列语义区域，与数据行建立稳定的纵向起点。
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.1882353f, 0.1882353f, 0.2f, 1.0f);
    // 强表格边框用于外沿和表头分界，承担主要结构线。
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.30980393f, 0.30980393f, 0.34901962f, 1.0f);
    // 弱表格边框只分隔普通单元格，降低密集网格的视觉噪声。
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(0.22745098f, 0.22745098f, 0.24705882f, 1.0f);
    // 基础行背景允许透明继承，使表格自然嵌入所在面板。
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 交替行仅施加轻微差异，帮助横向追踪而不破坏整体底色。
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    // 文本选区背景必须兼顾选中辨识度与前景文字可读性。
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(0.0f, 0.47058824f, 0.84313726f, 1.0f);
    // 拖放目标使用高辨识强调色，明确释放操作将作用的位置。
    style.Colors[ImGuiCol_DragDropTarget] = ImVec4(1.0f, 1.0f, 0.0f, 0.9f);
    // 键盘和手柄导航高亮独立于鼠标悬浮态，避免输入来源含混。
    style.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    // 窗口导航高亮标识待切换窗口，并覆盖多种面板底色。
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    // 窗口切换期间的遮罩压低非目标内容，但仍保留场景上下文。
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    // 模态遮罩明确阻断下层交互，透明度同时维持来源窗口可见。
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.35f);
}

static void applyRestStyle(ImGuiStyle& style)
{
    // Rest 样式，来源为 ImThemes 的 AaronBeardless 配色。

    // 全局透明度会乘到整套颜色上，主题在此保持最终合成基准。
    style.Alpha = 1.0f;
    // 禁用态通过统一衰减与可交互控件拉开层级，无需逐项改写颜色。
    style.DisabledAlpha = 0.5f;
    // 窗口内边距同时决定内容密度和停靠面板的呼吸空间。
    style.WindowPadding            = ImVec2(13.0f, 10.0f);
    style.WindowRounding           = 0.0f;
    style.WindowBorderSize         = 1.0f;
    style.WindowMinSize            = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign         = ImVec2(0.5f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_Right;
    style.ChildRounding            = 3.0f;
    style.ChildBorderSize          = 1.0f;
    style.PopupRounding            = 5.0f;
    style.PopupBorderSize          = 1.0f;
    // 框体内边距决定输入框和按钮的文字基线及垂直触达面积。
    style.FramePadding    = ImVec2(20.0f, 8.1f);
    style.FrameRounding   = 2.0f;
    style.FrameBorderSize = 0.0f;
    // 项目间距约束同级控件节奏，也是多数自动布局的基础步长。
    style.ItemSpacing      = ImVec2(3.0f, 3.0f);
    style.ItemInnerSpacing = ImVec2(3.0f, 8.0f);
    // 单元格留白保证表格密集展示时文字和分隔线仍有安全距离。
    style.CellPadding       = ImVec2(6.0f, 14.1f);
    style.IndentSpacing     = 0.0f;
    style.ColumnsMinSpacing = 10.0f;
    // 滚动条宽度在可点击性与内容占用之间维持主题取舍。
    style.ScrollbarSize     = 10.0f;
    style.ScrollbarRounding = 2.0f;
    style.GrabMinSize       = 12.1f;
    style.GrabRounding      = 1.0f;
    style.TabRounding       = 2.0f;
    style.TabBorderSize     = 0.0f;
    // 选中标签的关闭按钮阈值控制窄页签中操作入口的保留策略。
    style.TabCloseButtonMinWidthSelected = 0.0f;
    // 未选中标签单独设阈值，避免拥挤时关闭图标喧宾夺主。
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    // 正文色承担主要信息层，必须与窗口和框体背景保持足够对比。
    style.Colors[ImGuiCol_Text] =
        ImVec4(0.98039216f, 0.98039216f, 0.98039216f, 1.0f);
    // 弱化文字用于不可用或次要信息，但仍需在主背景上可辨识。
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4(0.49803922f, 0.49803922f, 0.49803922f, 1.0f);
    // 窗口底色奠定整套主题明度，后续容器和控件颜色均以此为参照。
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.09411765f, 0.09411765f, 0.09411765f, 1.0f);
    // 子窗口背景可选择透明继承，避免嵌套区域产生多余色块。
    style.Colors[ImGuiCol_ChildBg] =
        ImVec4(0.15686275f, 0.15686275f, 0.15686275f, 1.0f);
    // 弹窗背景需要遮住下层内容，同时以透明度保留空间关系。
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.09411765f, 0.09411765f, 0.09411765f, 1.0f);
    // 边框色用于低成本描绘控件和容器边界，透明度决定强调强度。
    style.Colors[ImGuiCol_Border] = ImVec4(1.0f, 1.0f, 1.0f, 0.09803922f);
    // 边框阴影独立配置，以便主题明确选择平面或浮起效果。
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 框体常态是输入和选择控件的静止基面，也是交互渐变的起点。
    style.Colors[ImGuiCol_FrameBg] = ImVec4(1.0f, 1.0f, 1.0f, 0.09803922f);
    // 框体悬浮色只提示可交互性，应比激活态更克制。
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.15686275f);
    // 框体激活色确认当前操作焦点，并与悬浮态形成连续反馈。
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.0f, 0.0f, 0.0f, 0.047058824f);
    // 非活动标题栏降低存在感，让当前编辑窗口更容易被识别。
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.11764706f, 0.11764706f, 0.11764706f, 1.0f);
    // 活动标题栏提高层级，承担当前窗口焦点的主要视觉提示。
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.15686275f, 0.15686275f, 0.15686275f, 1.0f);
    // 折叠标题仍保留窗口身份，但用透明度减少对内容的干扰。
    style.Colors[ImGuiCol_TitleBgCollapsed] =
        ImVec4(0.11764706f, 0.11764706f, 0.11764706f, 1.0f);
    // 菜单栏底色连接窗口标题与内容区，避免形成突兀的横向色带。
    style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 滚动轨道应融入容器背景，使抓手成为滚动位置的主要提示。
    style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.10980392f);
    // 滚动抓手常态需持续可见，又不能盖过正文和主要操作。
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.39215687f);
    // 滚动抓手悬浮态提高对比，提示鼠标已经进入可拖动区域。
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.47058824f);
    // 滚动抓手激活态确认持续拖动，颜色变化需强于悬浮态。
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.0f, 0.0f, 0.0f, 0.09803922f);
    // 勾选标记承载布尔结果，应从框体背景中清晰跳出。
    style.Colors[ImGuiCol_CheckMark] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // 滑块抓手常态标记当前值位置，并与轨道保持明度区分。
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(1.0f, 1.0f, 1.0f, 0.39215687f);
    // 滑块活动抓手强调正在调整的数值，释放后回落至常态。
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.3137255f);
    // 按钮常态建立操作面的基础层级，为悬浮和按下反馈预留变化空间。
    style.Colors[ImGuiCol_Button] = ImVec4(1.0f, 1.0f, 1.0f, 0.09803922f);
    // 按钮悬浮态只表达指向，不应与已按下状态混淆。
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.15686275f);
    // 按钮激活态提供按下确认，需要成为三态中最明确的反馈。
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.0f, 0.0f, 0.0f, 0.047058824f);
    // Header 常态同时服务树节点、列表选项和折叠区标题。
    style.Colors[ImGuiCol_Header] = ImVec4(1.0f, 1.0f, 1.0f, 0.09803922f);
    // Header 悬浮态覆盖较大行区域，应控制亮度避免产生闪烁感。
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.15686275f);
    // Header 激活态标识当前选择或按压结果，与按钮反馈保持一致。
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.0f, 0.0f, 0.0f, 0.047058824f);
    // 分隔线以低对比划分区域，不与可拖动边缘争夺注意力。
    style.Colors[ImGuiCol_Separator] = ImVec4(1.0f, 1.0f, 1.0f, 0.15686275f);
    // 可交互分隔线在悬浮时增强，提示其支持调整布局。
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.23529412f);
    // 分隔线拖动期间使用最强状态，持续反馈布局调整操作。
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.23529412f);
    // 缩放抓手常态保持含蓄，避免遮挡窗口角落内容。
    style.Colors[ImGuiCol_ResizeGrip] = ImVec4(1.0f, 1.0f, 1.0f, 0.15686275f);
    // 缩放抓手悬浮态显露可调整方向，强化边角命中提示。
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.23529412f);
    // 缩放抓手激活态维持拖动反馈，直到尺寸调整结束。
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.23529412f);
    // 普通标签以中等层级承载页面入口，不抢占当前标签的焦点。
    style.Colors[ImGuiCol_Tab] = ImVec4(1.0f, 1.0f, 1.0f, 0.09803922f);
    // 标签悬浮色提前提示可切换目标，并需兼容已选中标签的叠加状态。
    style.Colors[ImGuiCol_TabHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.15686275f);
    // 活动标签与内容面板建立连续关系，明确当前可见页面。
    style.Colors[ImGuiCol_TabActive] = ImVec4(1.0f, 1.0f, 1.0f, 0.3137255f);
    // 失焦停靠节点的普通标签进一步降级，突出当前活动窗口组。
    style.Colors[ImGuiCol_TabUnfocused] = ImVec4(0.0f, 0.0f, 0.0f, 0.15686275f);
    // 失焦组仍需标识其已选页面，因此保留受抑制的活动态。
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.23529412f);
    // 折线图常态颜色优先保证细线在背景上的连续可读性。
    style.Colors[ImGuiCol_PlotLines] = ImVec4(1.0f, 1.0f, 1.0f, 0.3529412f);
    // 折线悬浮态采用明显强调色，便于定位密集曲线中的目标。
    style.Colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // 柱状图使用独立数据色，避免与按钮等交互强调色混淆。
    style.Colors[ImGuiCol_PlotHistogram] = ImVec4(1.0f, 1.0f, 1.0f, 0.3529412f);
    // 柱状图悬浮色强化当前样本，但仍需保留原数据系列归属。
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // 表头底色固定列语义区域，与数据行建立稳定的纵向起点。
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.15686275f, 0.15686275f, 0.15686275f, 1.0f);
    // 强表格边框用于外沿和表头分界，承担主要结构线。
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.3137255f);
    // 弱表格边框只分隔普通单元格，降低密集网格的视觉噪声。
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.19607843f);
    // 基础行背景允许透明继承，使表格自然嵌入所在面板。
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 交替行仅施加轻微差异，帮助横向追踪而不破坏整体底色。
    style.Colors[ImGuiCol_TableRowBgAlt] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.019607844f);
    // 文本选区背景必须兼顾选中辨识度与前景文字可读性。
    style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    // 拖放目标使用高辨识强调色，明确释放操作将作用的位置。
    style.Colors[ImGuiCol_DragDropTarget] =
        ImVec4(0.16862746f, 0.23137255f, 0.5372549f, 1.0f);
    // 键盘和手柄导航高亮独立于鼠标悬浮态，避免输入来源含混。
    style.Colors[ImGuiCol_NavHighlight] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // 窗口导航高亮标识待切换窗口，并覆盖多种面板底色。
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    // 窗口切换期间的遮罩压低非目标内容，但仍保留场景上下文。
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    // 模态遮罩明确阻断下层交互，透明度同时维持来源窗口可见。
    style.Colors[ImGuiCol_ModalWindowDimBg] =
        ImVec4(0.0f, 0.0f, 0.0f, 0.5647059f);
}

static void applyComfortableDarkCyanStyle(ImGuiStyle& style)
{
    // Comfortable Dark Cyan 样式，来源为 ImThemes 的 SouthCraftX 配色。

    // 全局透明度会乘到整套颜色上，主题在此保持最终合成基准。
    style.Alpha = 1.0f;
    // 禁用态通过统一衰减与可交互控件拉开层级，无需逐项改写颜色。
    style.DisabledAlpha = 1.0f;
    // 窗口内边距同时决定内容密度和停靠面板的呼吸空间。
    style.WindowPadding            = ImVec2(20.0f, 20.0f);
    style.WindowRounding           = 11.5f;
    style.WindowBorderSize         = 0.0f;
    style.WindowMinSize            = ImVec2(20.0f, 20.0f);
    style.WindowTitleAlign         = ImVec2(0.5f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.ChildRounding            = 20.0f;
    style.ChildBorderSize          = 1.0f;
    style.PopupRounding            = 17.4f;
    style.PopupBorderSize          = 1.0f;
    // 框体内边距决定输入框和按钮的文字基线及垂直触达面积。
    style.FramePadding    = ImVec2(20.0f, 3.4f);
    style.FrameRounding   = 11.9f;
    style.FrameBorderSize = 0.0f;
    // 项目间距约束同级控件节奏，也是多数自动布局的基础步长。
    style.ItemSpacing      = ImVec2(8.9f, 13.4f);
    style.ItemInnerSpacing = ImVec2(7.1f, 1.8f);
    // 单元格留白保证表格密集展示时文字和分隔线仍有安全距离。
    style.CellPadding       = ImVec2(12.1f, 9.2f);
    style.IndentSpacing     = 0.0f;
    style.ColumnsMinSpacing = 8.7f;
    // 滚动条宽度在可点击性与内容占用之间维持主题取舍。
    style.ScrollbarSize     = 11.6f;
    style.ScrollbarRounding = 15.9f;
    style.GrabMinSize       = 3.7f;
    style.GrabRounding      = 20.0f;
    style.TabRounding       = 9.8f;
    style.TabBorderSize     = 0.0f;
    // 选中标签的关闭按钮阈值控制窄页签中操作入口的保留策略。
    style.TabCloseButtonMinWidthSelected = 0.0f;
    // 未选中标签单独设阈值，避免拥挤时关闭图标喧宾夺主。
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    // 正文色承担主要信息层，必须与窗口和框体背景保持足够对比。
    style.Colors[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // 弱化文字用于不可用或次要信息，但仍需在主背景上可辨识。
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4(0.27450982f, 0.31764707f, 0.4509804f, 1.0f);
    // 窗口底色奠定整套主题明度，后续容器和控件颜色均以此为参照。
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    // 子窗口背景可选择透明继承，避免嵌套区域产生多余色块。
    style.Colors[ImGuiCol_ChildBg] =
        ImVec4(0.09411765f, 0.101960786f, 0.11764706f, 1.0f);
    // 弹窗背景需要遮住下层内容，同时以透明度保留空间关系。
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    // 边框色用于低成本描绘控件和容器边界，透明度决定强调强度。
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.15686275f, 0.16862746f, 0.19215687f, 1.0f);
    // 边框阴影独立配置，以便主题明确选择平面或浮起效果。
    style.Colors[ImGuiCol_BorderShadow] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    // 框体常态是输入和选择控件的静止基面，也是交互渐变的起点。
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.11372549f, 0.1254902f, 0.15294118f, 1.0f);
    // 框体悬浮色只提示可交互性，应比激活态更克制。
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.15686275f, 0.16862746f, 0.19215687f, 1.0f);
    // 框体激活色确认当前操作焦点，并与悬浮态形成连续反馈。
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.15686275f, 0.16862746f, 0.19215687f, 1.0f);
    // 非活动标题栏降低存在感，让当前编辑窗口更容易被识别。
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    // 活动标题栏提高层级，承担当前窗口焦点的主要视觉提示。
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    // 折叠标题仍保留窗口身份，但用透明度减少对内容的干扰。
    style.Colors[ImGuiCol_TitleBgCollapsed] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    // 菜单栏底色连接窗口标题与内容区，避免形成突兀的横向色带。
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.09803922f, 0.105882354f, 0.12156863f, 1.0f);
    // 滚动轨道应融入容器背景，使抓手成为滚动位置的主要提示。
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    // 滚动抓手常态需持续可见，又不能盖过正文和主要操作。
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    // 滚动抓手悬浮态提高对比，提示鼠标已经进入可拖动区域。
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.15686275f, 0.16862746f, 0.19215687f, 1.0f);
    // 滚动抓手激活态确认持续拖动，颜色变化需强于悬浮态。
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    // 勾选标记承载布尔结果，应从框体背景中清晰跳出。
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.03137255f, 0.9490196f, 0.84313726f, 1.0f);
    // 滑块抓手常态标记当前值位置，并与轨道保持明度区分。
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.03137255f, 0.9490196f, 0.84313726f, 1.0f);
    // 滑块活动抓手强调正在调整的数值，释放后回落至常态。
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.6f, 0.9647059f, 0.03137255f, 1.0f);
    // 按钮常态建立操作面的基础层级，为悬浮和按下反馈预留变化空间。
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    // 按钮悬浮态只表达指向，不应与已按下状态混淆。
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.18039216f, 0.1882353f, 0.19607843f, 1.0f);
    // 按钮激活态提供按下确认，需要成为三态中最明确的反馈。
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.15294118f, 0.15294118f, 0.15294118f, 1.0f);
    // Header 常态同时服务树节点、列表选项和折叠区标题。
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.14117648f, 0.16470589f, 0.20784314f, 1.0f);
    // Header 悬浮态覆盖较大行区域，应控制亮度避免产生闪烁感。
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.105882354f, 0.105882354f, 0.105882354f, 1.0f);
    // Header 激活态标识当前选择或按压结果，与按钮反馈保持一致。
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    // 分隔线以低对比划分区域，不与可拖动边缘争夺注意力。
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.12941177f, 0.14901961f, 0.19215687f, 1.0f);
    // 可交互分隔线在悬浮时增强，提示其支持调整布局。
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.15686275f, 0.18431373f, 0.2509804f, 1.0f);
    // 分隔线拖动期间使用最强状态，持续反馈布局调整操作。
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.15686275f, 0.18431373f, 0.2509804f, 1.0f);
    // 缩放抓手常态保持含蓄，避免遮挡窗口角落内容。
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.14509805f, 0.14509805f, 0.14509805f, 1.0f);
    // 缩放抓手悬浮态显露可调整方向，强化边角命中提示。
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.03137255f, 0.9490196f, 0.84313726f, 1.0f);
    // 缩放抓手激活态维持拖动反馈，直到尺寸调整结束。
    style.Colors[ImGuiCol_ResizeGripActive] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // 普通标签以中等层级承载页面入口，不抢占当前标签的焦点。
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    // 标签悬浮色提前提示可切换目标，并需兼容已选中标签的叠加状态。
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    // 活动标签与内容面板建立连续关系，明确当前可见页面。
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    // 失焦停靠节点的普通标签进一步降级，突出当前活动窗口组。
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.078431375f, 0.08627451f, 0.101960786f, 1.0f);
    // 失焦组仍需标识其已选页面，因此保留受抑制的活动态。
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.1254902f, 0.27450982f, 0.57254905f, 1.0f);
    // 折线图常态颜色优先保证细线在背景上的连续可读性。
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.52156866f, 0.6f, 0.7019608f, 1.0f);
    // 折线悬浮态采用明显强调色，便于定位密集曲线中的目标。
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(0.039215688f, 0.98039216f, 0.98039216f, 1.0f);
    // 柱状图使用独立数据色，避免与按钮等交互强调色混淆。
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.03137255f, 0.9490196f, 0.84313726f, 1.0f);
    // 柱状图悬浮色强化当前样本，但仍需保留原数据系列归属。
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(0.15686275f, 0.18431373f, 0.2509804f, 1.0f);
    // 表头底色固定列语义区域，与数据行建立稳定的纵向起点。
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    // 强表格边框用于外沿和表头分界，承担主要结构线。
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.047058824f, 0.05490196f, 0.07058824f, 1.0f);
    // 弱表格边框只分隔普通单元格，降低密集网格的视觉噪声。
    style.Colors[ImGuiCol_TableBorderLight] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    // 基础行背景允许透明继承，使表格自然嵌入所在面板。
    style.Colors[ImGuiCol_TableRowBg] =
        ImVec4(0.11764706f, 0.13333334f, 0.14901961f, 1.0f);
    // 交替行仅施加轻微差异，帮助横向追踪而不破坏整体底色。
    style.Colors[ImGuiCol_TableRowBgAlt] =
        ImVec4(0.09803922f, 0.105882354f, 0.12156863f, 1.0f);
    // 文本选区背景必须兼顾选中辨识度与前景文字可读性。
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(0.9372549f, 0.9372549f, 0.9372549f, 1.0f);
    // 拖放目标使用高辨识强调色，明确释放操作将作用的位置。
    style.Colors[ImGuiCol_DragDropTarget] =
        ImVec4(0.49803922f, 0.5137255f, 1.0f, 1.0f);
    // 键盘和手柄导航高亮独立于鼠标悬浮态，避免输入来源含混。
    style.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.26666668f, 0.2901961f, 1.0f, 1.0f);
    // 窗口导航高亮标识待切换窗口，并覆盖多种面板底色。
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(0.49803922f, 0.5137255f, 1.0f, 1.0f);
    // 窗口切换期间的遮罩压低非目标内容，但仍保留场景上下文。
    style.Colors[ImGuiCol_NavWindowingDimBg] =
        ImVec4(0.19607843f, 0.1764706f, 0.54509807f, 0.5019608f);
    // 模态遮罩明确阻断下层交互，透明度同时维持来源窗口可见。
    style.Colors[ImGuiCol_ModalWindowDimBg] =
        ImVec4(0.19607843f, 0.1764706f, 0.54509807f, 0.5019608f);
}

static void applyKazamCherryStyle(ImGuiStyle& style)
{
    // Kazam's Cherry 样式，来源为 ImThemes 的 coyoteclan 配色。

    // 全局透明度会乘到整套颜色上，主题在此保持最终合成基准。
    style.Alpha = 1.0f;
    // 禁用态通过统一衰减与可交互控件拉开层级，无需逐项改写颜色。
    style.DisabledAlpha = 0.6f;
    // 窗口内边距同时决定内容密度和停靠面板的呼吸空间。
    style.WindowPadding            = ImVec2(6.0f, 3.0f);
    style.WindowRounding           = 0.0f;
    style.WindowBorderSize         = 1.0f;
    style.WindowMinSize            = ImVec2(32.0f, 32.0f);
    style.WindowTitleAlign         = ImVec2(0.5f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_Left;
    style.ChildRounding            = 0.0f;
    style.ChildBorderSize          = 1.0f;
    style.PopupRounding            = 0.0f;
    style.PopupBorderSize          = 1.0f;
    // 框体内边距决定输入框和按钮的文字基线及垂直触达面积。
    style.FramePadding    = ImVec2(5.0f, 5.0f);
    style.FrameRounding   = 1.0f;
    style.FrameBorderSize = 0.0f;
    // 项目间距约束同级控件节奏，也是多数自动布局的基础步长。
    style.ItemSpacing      = ImVec2(7.0f, 1.0f);
    style.ItemInnerSpacing = ImVec2(1.0f, 1.0f);
    // 单元格留白保证表格密集展示时文字和分隔线仍有安全距离。
    style.CellPadding       = ImVec2(4.0f, 4.0f);
    style.IndentSpacing     = 6.0f;
    style.ColumnsMinSpacing = 6.0f;
    // 滚动条宽度在可点击性与内容占用之间维持主题取舍。
    style.ScrollbarSize     = 13.0f;
    style.ScrollbarRounding = 16.0f;
    style.GrabMinSize       = 20.0f;
    style.GrabRounding      = 2.0f;
    style.TabRounding       = 2.0f;
    style.TabBorderSize     = 0.0f;
    // 选中标签的关闭按钮阈值控制窄页签中操作入口的保留策略。
    style.TabCloseButtonMinWidthSelected = 0.0f;
    // 未选中标签单独设阈值，避免拥挤时关闭图标喧宾夺主。
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.ColorButtonPosition              = ImGuiDir_Right;
    style.ButtonTextAlign                  = ImVec2(0.5f, 0.5f);
    style.SelectableTextAlign              = ImVec2(0.0f, 0.0f);

    // 正文色承担主要信息层，必须与窗口和框体背景保持足够对比。
    style.Colors[ImGuiCol_Text] =
        ImVec4(0.85882354f, 0.92941177f, 0.8862745f, 0.88f);
    // 弱化文字用于不可用或次要信息，但仍需在主背景上可辨识。
    style.Colors[ImGuiCol_TextDisabled] =
        ImVec4(0.85882354f, 0.92941177f, 0.8862745f, 0.28f);
    // 窗口底色奠定整套主题明度，后续容器和控件颜色均以此为参照。
    style.Colors[ImGuiCol_WindowBg] =
        ImVec4(0.12941177f, 0.13725491f, 0.16862746f, 1.0f);
    // 子窗口背景可选择透明继承，避免嵌套区域产生多余色块。
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 弹窗背景需要遮住下层内容，同时以透明度保留空间关系。
    style.Colors[ImGuiCol_PopupBg] =
        ImVec4(0.2f, 0.21960784f, 0.26666668f, 0.9f);
    // 边框色用于低成本描绘控件和容器边界，透明度决定强调强度。
    style.Colors[ImGuiCol_Border] =
        ImVec4(0.5372549f, 0.47843137f, 0.25490198f, 0.162f);
    // 边框阴影独立配置，以便主题明确选择平面或浮起效果。
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 框体常态是输入和选择控件的静止基面，也是交互渐变的起点。
    style.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.2f, 0.21960784f, 0.26666668f, 1.0f);
    // 框体悬浮色只提示可交互性，应比激活态更克制。
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 0.78f);
    // 框体激活色确认当前操作焦点，并与悬浮态形成连续反馈。
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    // 非活动标题栏降低存在感，让当前编辑窗口更容易被识别。
    style.Colors[ImGuiCol_TitleBg] =
        ImVec4(0.23137255f, 0.2f, 0.27058825f, 1.0f);
    // 活动标题栏提高层级，承担当前窗口焦点的主要视觉提示。
    style.Colors[ImGuiCol_TitleBgActive] =
        ImVec4(0.5019608f, 0.07450981f, 0.25490198f, 1.0f);
    // 折叠标题仍保留窗口身份，但用透明度减少对内容的干扰。
    style.Colors[ImGuiCol_TitleBgCollapsed] =
        ImVec4(0.2f, 0.21960784f, 0.26666668f, 0.75f);
    // 菜单栏底色连接窗口标题与内容区，避免形成突兀的横向色带。
    style.Colors[ImGuiCol_MenuBarBg] =
        ImVec4(0.2f, 0.21960784f, 0.26666668f, 0.47f);
    // 滚动轨道应融入容器背景，使抓手成为滚动位置的主要提示。
    style.Colors[ImGuiCol_ScrollbarBg] =
        ImVec4(0.2f, 0.21960784f, 0.26666668f, 1.0f);
    // 滚动抓手常态需持续可见，又不能盖过正文和主要操作。
    style.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.08627451f, 0.14901961f, 0.15686275f, 1.0f);
    // 滚动抓手悬浮态提高对比，提示鼠标已经进入可拖动区域。
    style.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 0.78f);
    // 滚动抓手激活态确认持续拖动，颜色变化需强于悬浮态。
    style.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    // 勾选标记承载布尔结果，应从框体背景中清晰跳出。
    style.Colors[ImGuiCol_CheckMark] =
        ImVec4(0.70980394f, 0.21960784f, 0.26666668f, 1.0f);
    // 滑块抓手常态标记当前值位置，并与轨道保持明度区分。
    style.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.46666667f, 0.76862746f, 0.827451f, 0.14f);
    // 滑块活动抓手强调正在调整的数值，释放后回落至常态。
    style.Colors[ImGuiCol_SliderGrabActive] =
        ImVec4(0.70980394f, 0.21960784f, 0.26666668f, 1.0f);
    // 按钮常态建立操作面的基础层级，为悬浮和按下反馈预留变化空间。
    style.Colors[ImGuiCol_Button] =
        ImVec4(0.46666667f, 0.76862746f, 0.827451f, 0.14f);
    // 按钮悬浮态只表达指向，不应与已按下状态混淆。
    style.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 0.86f);
    // 按钮激活态提供按下确认，需要成为三态中最明确的反馈。
    style.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    // Header 常态同时服务树节点、列表选项和折叠区标题。
    style.Colors[ImGuiCol_Header] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 0.76f);
    // Header 悬浮态覆盖较大行区域，应控制亮度避免产生闪烁感。
    style.Colors[ImGuiCol_HeaderHovered] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 0.86f);
    // Header 激活态标识当前选择或按压结果，与按钮反馈保持一致。
    style.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.5019608f, 0.07450981f, 0.25490198f, 1.0f);
    // 分隔线以低对比划分区域，不与可拖动边缘争夺注意力。
    style.Colors[ImGuiCol_Separator] =
        ImVec4(0.42745098f, 0.42745098f, 0.49803922f, 0.5f);
    // 可交互分隔线在悬浮时增强，提示其支持调整布局。
    style.Colors[ImGuiCol_SeparatorHovered] =
        ImVec4(0.5176471f, 0.21960784f, 0.3372549f, 0.88412017f);
    // 分隔线拖动期间使用最强状态，持续反馈布局调整操作。
    style.Colors[ImGuiCol_SeparatorActive] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    // 缩放抓手常态保持含蓄，避免遮挡窗口角落内容。
    style.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.46666667f, 0.76862746f, 0.827451f, 0.04f);
    // 缩放抓手悬浮态显露可调整方向，强化边角命中提示。
    style.Colors[ImGuiCol_ResizeGripHovered] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 0.78f);
    // 缩放抓手激活态维持拖动反馈，直到尺寸调整结束。
    style.Colors[ImGuiCol_ResizeGripActive] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    // 普通标签以中等层级承载页面入口，不抢占当前标签的焦点。
    style.Colors[ImGuiCol_Tab] =
        ImVec4(0.70980394f, 0.21960784f, 0.26666668f, 0.82832617f);
    // 标签悬浮色提前提示可切换目标，并需兼容已选中标签的叠加状态。
    style.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.70980394f, 0.21960784f, 0.26666668f, 1.0f);
    // 活动标签与内容面板建立连续关系，明确当前可见页面。
    style.Colors[ImGuiCol_TabActive] =
        ImVec4(0.70980394f, 0.21960784f, 0.26666668f, 1.0f);
    // 失焦停靠节点的普通标签进一步降级，突出当前活动窗口组。
    style.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.06666667f, 0.101960786f, 0.14509805f, 0.9724f);
    // 失焦组仍需标识其已选页面，因此保留受抑制的活动态。
    style.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    // 折线图常态颜色优先保证细线在背景上的连续可读性。
    style.Colors[ImGuiCol_PlotLines] =
        ImVec4(0.85882354f, 0.92941177f, 0.8862745f, 0.63f);
    // 折线悬浮态采用明显强调色，便于定位密集曲线中的目标。
    style.Colors[ImGuiCol_PlotLinesHovered] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    // 柱状图使用独立数据色，避免与按钮等交互强调色混淆。
    style.Colors[ImGuiCol_PlotHistogram] =
        ImVec4(0.85882354f, 0.92941177f, 0.8862745f, 0.63f);
    // 柱状图悬浮色强化当前样本，但仍需保留原数据系列归属。
    style.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 1.0f);
    // 表头底色固定列语义区域，与数据行建立稳定的纵向起点。
    style.Colors[ImGuiCol_TableHeaderBg] =
        ImVec4(0.1882353f, 0.1882353f, 0.2f, 1.0f);
    // 强表格边框用于外沿和表头分界，承担主要结构线。
    style.Colors[ImGuiCol_TableBorderStrong] =
        ImVec4(0.30980393f, 0.30980393f, 0.34901962f, 1.0f);
    // 弱表格边框只分隔普通单元格，降低密集网格的视觉噪声。
    style.Colors[ImGuiCol_TableBorderLight] =
        ImVec4(0.22745098f, 0.22745098f, 0.24705882f, 1.0f);
    // 基础行背景允许透明继承，使表格自然嵌入所在面板。
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // 交替行仅施加轻微差异，帮助横向追踪而不破坏整体底色。
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    // 文本选区背景必须兼顾选中辨识度与前景文字可读性。
    style.Colors[ImGuiCol_TextSelectedBg] =
        ImVec4(0.45490196f, 0.19607843f, 0.29803923f, 0.43f);
    // 拖放目标使用高辨识强调色，明确释放操作将作用的位置。
    style.Colors[ImGuiCol_DragDropTarget] = ImVec4(1.0f, 1.0f, 0.0f, 0.9f);
    // 键盘和手柄导航高亮独立于鼠标悬浮态，避免输入来源含混。
    style.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.25882354f, 0.5882353f, 0.9764706f, 1.0f);
    // 窗口导航高亮标识待切换窗口，并覆盖多种面板底色。
    style.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    // 窗口切换期间的遮罩压低非目标内容，但仍保留场景上下文。
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    // 模态遮罩明确阻断下层交互，透明度同时维持来源窗口可见。
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.35f);
}

}  // namespace MMM::Graphic
