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
# shared 依赖偏好下保留既有 target 名称，但产物改为 DLL。
set(IMGUI_LIBRARY_TYPE STATIC)
if(PROJECT_LINKAGE STREQUAL "shared")
  set(IMGUI_LIBRARY_TYPE SHARED)
endif()
add_library(imgui-static ${IMGUI_LIBRARY_TYPE} ${IMGUI_SOURCES})
if(PROJECT_LINKAGE STREQUAL "shared" AND WIN32)
  set_target_properties(imgui-static PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON
                                                OUTPUT_NAME imgui)
  # MSVC 跨 DLL 访问 ImGui 数据符号必须显式区分导出和导入。
  target_compile_definitions(
    imgui-static
    PRIVATE "IMGUI_API=__declspec(dllexport)"
            "IMGUI_IMPL_API=__declspec(dllexport)"
    INTERFACE "IMGUI_API=__declspec(dllimport)"
              "IMGUI_IMPL_API=__declspec(dllimport)")
endif()

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
