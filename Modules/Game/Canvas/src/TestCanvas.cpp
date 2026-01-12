#include "TestCanvas.h"
#include "CanvasDefs.h"
#include "colorful-log.h"
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>

#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

namespace MMM
{
namespace Canvas
{
// 定义内部实现类，持有 GLFW上下文和GLFWwindow
struct TestCanvas::ContextImpl {
    class WindowContext
    {
    public:
        WindowContext()
        {
            if ( !glfwInit() ) {
                XERROR("GLFW Init Failed");
                return;
            }
            glfwSetErrorCallback([](int error, const char* description) {
                XERROR("GLFW Error %d: %s", error, description);
            });

            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        }
        ~WindowContext() { glfwTerminate(); }
    };
    WindowContext m_context;
    GLFWwindow*   m_windowHandle = nullptr;

    // 渲染循环的核心函数
    void frameRender(ImDrawData* draw_data);
    void framePresent();
    void recreateSwapchain();

    // --- Vulkan 初始化辅助函数 ---
    void initVulkan(GLFWwindow* window) {}
    void createInstance() {}
    void setupDebugMessenger() {}
    void createSurface(GLFWwindow* window) {}
    void pickPhysicalDevice() {}
    void createLogicalDevice() {}
    void createDescriptorPool() {}

    // --- 交换链及其相关资源的创建 ---
    void createSwapchain() {}
    void createImageViews() {}
    void createRenderPass() {}
    void createFramebuffers() {}
    void createSyncObjects() {}  // Fences 和 Semaphores

    void cleanupSwapchain() {}

    // --- 成员变量 ---
    vk::Instance                 m_instance;
    vk::DebugUtilsMessengerEXT   m_debugMessenger;
    vk::SurfaceKHR               m_surface;
    vk::PhysicalDevice           m_physicalDevice;
    vk::Device                   m_device;
    vk::Queue                    m_graphicsQueue;
    uint32_t                     m_queueFamily;
    vk::SwapchainKHR             m_swapchain;
    std::vector<vk::Image>       m_swapchainImages;
    std::vector<vk::ImageView>   m_swapchainImageViews;
    std::vector<vk::Framebuffer> m_swapchainFramebuffers;
    vk::Format                   m_swapchainImageFormat;
    vk::Extent2D                 m_swapchainExtent;
    vk::RenderPass               m_renderPass;
    vk::DescriptorPool           m_descriptorPool;
    vk::CommandPool              m_commandPool;

    // 仿照官方示例，为每个 Frame-in-flight 创建独立的资源
    struct FrameData {
        vk::CommandPool   commandPool;
        vk::CommandBuffer commandBuffer;
    };
    std::vector<FrameData> m_frames;

    struct FrameSemaphores {
        vk::Semaphore imageAcquiredSemaphore;
        vk::Semaphore renderCompleteSemaphore;
        vk::Fence     fence;
    };
    std::vector<FrameSemaphores> m_semaphores;
    uint32_t                     m_frameIndex     = 0;
    uint32_t                     m_semaphoreIndex = 0;

    // 状态标志
    bool     m_swapchainRebuild = false;
    uint32_t m_minImageCount    = 2;
    ContextImpl(int w, int h)
    {
        m_windowHandle =
            glfwCreateWindow(w, h, "MusicMapMaker", nullptr, nullptr);
        if ( !m_windowHandle ) {
            glfwTerminate();
            throw std::runtime_error("Failed to create GLFW window");
        }

        if ( !glfwVulkanSupported() ) {
            glfwDestroyWindow(m_windowHandle);
            glfwTerminate();
            throw std::runtime_error("GLFW: Vulkan Not Supported");
        }

        m_swapchainExtent = vk::Extent2D(w, h);

        initVulkan(m_windowHandle);
        createSwapchain();
        createImageViews();
        createRenderPass();
        createFramebuffers();
        createDescriptorPool();
        createSyncObjects();
    }
    ~ContextImpl()
    {
        cleanupSwapchain();

        for ( size_t i = 0; i < m_semaphores.size(); i++ ) {
            m_device.destroySemaphore(m_semaphores[i].imageAcquiredSemaphore);
            m_device.destroySemaphore(m_semaphores[i].renderCompleteSemaphore);
            m_device.destroyFence(m_semaphores[i].fence);
        }

        m_device.destroyDescriptorPool(m_descriptorPool);
        m_device.destroyRenderPass(m_renderPass);
        m_device.destroy();

        if ( m_debugMessenger ) {
            m_instance.destroyDebugUtilsMessengerEXT(m_debugMessenger);
        }
        m_instance.destroySurfaceKHR(m_surface);
        m_instance.destroy();
    }
};

TestCanvas::TestCanvas(int w, int h)
    : m_CtxImplPtr(std::make_unique<ContextImpl>(w, h))
{


    // 初始化 ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    if ( io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable ) {
        style.WindowRounding              = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    // 初始化 ImGui 后端
    ImGui_ImplGlfw_InitForVulkan(m_CtxImplPtr->m_windowHandle, true);
    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance                  = m_CtxImplPtr->m_instance;
    init_info.PhysicalDevice            = m_CtxImplPtr->m_physicalDevice;
    init_info.Device                    = m_CtxImplPtr->m_device;
    init_info.QueueFamily               = m_CtxImplPtr->m_queueFamily;
    init_info.Queue                     = m_CtxImplPtr->m_graphicsQueue;
    init_info.DescriptorPool            = m_CtxImplPtr->m_descriptorPool;
    init_info.MinImageCount             = m_CtxImplPtr->m_minImageCount;
    init_info.ImageCount =
        static_cast<uint32_t>(m_CtxImplPtr->m_swapchainImages.size());
    // init_info.PipelineInfoForViewports     = VK_SAMPLE_COUNT_1_BIT;
    init_info.CheckVkResultFn = [](VkResult err) {
        if ( err != 0 ) throw std::runtime_error("ImGui Vulkan backend error!");
    };

    ImGui_ImplVulkan_Init(&init_info);
}

TestCanvas::~TestCanvas() {}

bool TestCanvas::shouldClose()
{
    return m_CtxImplPtr->m_windowHandle &&
           glfwWindowShouldClose(m_CtxImplPtr->m_windowHandle);
}

// 具体的 GLFW 操作封装在这
void TestCanvas::update()
{
    if ( m_CtxImplPtr->m_windowHandle ) {
        ImGui::NewFrame();
        ImGui::Begin("Settings");
        ImGui::Button("Hello ImGui");
        static float value{ 1.f };
        ImGui::DragFloat("Value Name", &value);
        ImGui::End();

        glfwPollEvents();
        glfwSwapBuffers(m_CtxImplPtr->m_windowHandle);
    }
}
}  // namespace Canvas
}  // namespace MMM
