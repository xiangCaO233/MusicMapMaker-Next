# Modules/Game/Graphic/CMakeLists.txt

# 找到系统中的 Vulkan 开发环境
find_package(Vulkan REQUIRED)

add_subdirectory("Font")

add_library(
        Graphic
        STATIC
        src/vk/VKContextGLFW.cpp
        src/vk/VKContext.cpp
        src/vk/VKShader.cpp
        src/vk/VKSwapchain.cpp
        src/vk/VKRenderPass.cpp
        src/vk/VKRenderPipeline.cpp
        src/vk/VKRenderer.cpp
        src/vk/mem/VKMemBuffer.cpp
        src/vk/VKRendererRess.cpp
)

target_include_directories(
        Graphic
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(Graphic PUBLIC
        Log
        Font
        3rd_glfw
        Vulkan::Vulkan
)
