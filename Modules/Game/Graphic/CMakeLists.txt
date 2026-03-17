# Modules/Game/Graphic/CMakeLists.txt

# 找到系统中的 Vulkan 开发环境
find_package(Vulkan REQUIRED)

add_subdirectory("Font")

# add_library(
#         Graphic
#         STATIC
#         src/glfw/GLFWCallback.cpp
#         src/vk/VKContextGLFW.cpp
#         src/vk/VKContext.cpp
#         src/vk/VKShader.cpp
#         src/vk/VKSwapchain.cpp
#         src/vk/VKRenderPass.cpp
#         src/vk/VKRenderPipeline.cpp
#         src/vk/VKRenderer.cpp
#         src/vk/mem/VKMemBuffer.cpp
#         src/vk/VKRendererRess.cpp
# )

add_library(
        Graphic
        STATIC
        src/glfw/GLFWCallback.cpp
        src/glfw/NativeWindow.cpp
        src/imguivk/context/VKContext.cpp
        src/imguivk/context/VKContextGLFW.cpp
        src/imguivk/context/VKContextDevice.cpp
        src/imguivk/context/VKContextDebugFeats.cpp
        src/imguivk/context/VKContextInfos.cpp
        src/imguivk/context/VKContextImguiImpl.cpp
        src/imguivk/mem/VKMemBuffer.cpp
        src/imguivk/VKOffScreenRenderer.cpp
        src/imguivk/VKRenderPass.cpp
        src/imguivk/VKRenderPipeline.cpp
        src/imguivk/VKRenderer.cpp
        src/imguivk/VKRendererRender.cpp
        src/imguivk/VKRendererRess.cpp
        src/imguivk/VKShader.cpp
        src/imguivk/VKSwapchain.cpp
        src/ui/imgui/MainDockSpaceUI.cpp
        src/ui/imgui/ImguiTestWindowUI.cpp
        src/ui/imgui/CLayoutTestUI.cpp
        src/ui/UIManager.cpp
)

target_include_directories(
        Graphic
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(Graphic PUBLIC
        Log
        Font
        Event
        Config
        3rd_glfw
        3rd_glm
        3rd_imgui
        3rd_clay
        Vulkan::Vulkan
)
