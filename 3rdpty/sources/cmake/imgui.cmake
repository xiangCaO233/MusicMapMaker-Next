# 3rdpty/sources/cmake/Buildimgui.cmake

# 定义源码路径
set(IMGUI_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/imgui")
message(STATUS "Configuring ImGui from: ${IMGUI_SOURCE_DIR}")

# 收集源码文件
set(IMGUI_SOURCES
    # Core ImGui files
    ${IMGUI_SOURCE_DIR}/imgui.cpp
    ${IMGUI_SOURCE_DIR}/imgui_draw.cpp
    ${IMGUI_SOURCE_DIR}/imgui_tables.cpp
    ${IMGUI_SOURCE_DIR}/imgui_widgets.cpp
    # Docking and Viewports support (essential for editors)
    ${IMGUI_SOURCE_DIR}/imgui_demo.cpp # Optional, but highly recommended for
                                       # examples and debugging
    # Backend files for Vulkan and GLFW
    ${IMGUI_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)

# 创建静态库 `imgui-static`
add_library(imgui-static STATIC ${IMGUI_SOURCES})

# 设置 Target 属性
target_include_directories(imgui-static PUBLIC ${IMGUI_SOURCE_DIR}
                                               ${IMGUI_SOURCE_DIR}/backends)

target_compile_features(imgui-static PRIVATE cxx_std_11)

# ImGui's Vulkan backend needs to know where to find the Vulkan headers. By
# linking against the `Vulkan::Vulkan` target (which `3rd_glfw` should already
# do), the include paths are handled automatically.

# Link the necessary dependencies. We link against `3rd_glfw`, which is our
# interface library for GLFW + Vulkan. This will transitively link glfw and
# Vulkan::Vulkan.
target_link_libraries(imgui-static PUBLIC 3rd_glfw Vulkan::Vulkan)

# --- 5. 创建统一的接口库 `3rd_imgui` ---
# This is the final target that the main application (`MusicMapMaker-Next`)
# should link against. It encapsulates all of ImGui's build details.
add_library(3rd_imgui INTERFACE)
target_link_libraries(3rd_imgui INTERFACE imgui-static)

message(STATUS "Created '3rd_imgui' target with Vulkan backend.")
