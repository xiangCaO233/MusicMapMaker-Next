# Modules/Game/Graphic/CMakeLists.txt

# 找到系统中的 Vulkan 开发环境
find_package(Vulkan REQUIRED)

add_subdirectory("Font")

add_library(
  Graphic STATIC
  src/glfw/GLFWCallback.cpp
  src/glfw/NativeWindow.cpp
  src/glfw/adapters/Win32WindowAdapter.cpp
  src/glfw/adapters/X11WindowAdapter.cpp
  src/imguivk/context/VKContext.cpp
  src/imguivk/context/VKContextDiagnostics.cpp
  src/imguivk/context/VKContextGLFW.cpp
  src/imguivk/context/VKContextDevice.cpp
  src/imguivk/context/VKContextDebugFeats.cpp
  src/imguivk/context/VKContextInfos.cpp
  src/imguivk/context/VKContextImguiImpl.cpp
  src/imguivk/mem/VKMemBuffer.cpp
  src/imguivk/VKOffScreenRenderer.cpp
  src/imguivk/VKOffScreenRendererRess.cpp
  src/imguivk/VKRenderPass.cpp
  src/imguivk/VKRenderPipeline.cpp
  src/imguivk/VKRenderer.cpp
  src/imguivk/VKRendererRender.cpp
  src/imguivk/VKRendererRess.cpp
  src/imguivk/VKShader.cpp
  src/imguivk/VKSwapchain.cpp
  src/imguivk/VKTexture.cpp
  src/imguivk/VKTextureAtlas.cpp
  src/theme/ImGuiTheme.cpp
  src/theme/ImGuiThemeRegistry.cpp
  src/system/SystemTheme.cpp
  src/CursorManager.cpp)

target_include_directories(Graphic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(
  Graphic
  PUBLIC Log
         Font
         Event
         Config
         Common
         Runtime
         3rd_glfw
         3rd_glm
         3rd_imgui
         3rd_implot
         3rd_clay
         3rd_stb
         3rd_lunasvg
         3rd_sol2
         luajit::luajit
         Vulkan::Vulkan)

if(WIN32)
  target_link_libraries(Graphic PRIVATE dwmapi comctl32 advapi32)
endif()

# 主题插件测试覆盖 Lua 解析、内置主题继承以及重载时清除旧实例。
mmm_add_test_executable(Graphic ThemePluginLoaderTest
                        tests/ThemePluginLoaderTest.cpp)
target_link_libraries(ThemePluginLoaderTest PRIVATE Graphic)
add_test(
  NAME ThemePluginLoaderTest
  COMMAND ThemePluginLoaderTest "${CMAKE_BINARY_DIR}/test_output/theme_plugins"
          "${CMAKE_SOURCE_DIR}/docs/plugin/examples/theme-example.lua")

if(APPLE)
  target_sources(
    Graphic
    PRIVATE src/glfw/adapters/MacOSWindowAdapter.mm
            src/glfw/adapters/MacOSWindowUtils.mm
            src/system/SystemThemeMacOS.mm)
  target_link_libraries(Graphic PRIVATE "-framework AppKit")
endif()

if(UNIX AND NOT APPLE)
  # XDG Settings Portal 通过 GIO 提供跨 GNOME、KDE 与 Wayland/X11 的统一亮暗主题通知。
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
  target_compile_definitions(Graphic PRIVATE MMM_ENABLE_XDG_PORTAL_THEME)
  target_link_libraries(Graphic PRIVATE PkgConfig::GIO)

  find_package(X11)
  if(X11_FOUND)
    target_compile_definitions(Graphic PRIVATE MMM_ENABLE_X11_FRAME_INTERACTION)
    target_include_directories(Graphic PRIVATE ${X11_INCLUDE_DIR})
    target_link_libraries(Graphic PRIVATE ${X11_LIBRARIES})
    if(X11_Xext_FOUND)
      target_compile_definitions(Graphic PRIVATE MMM_ENABLE_X11_FRAME_SHAPE)
      target_link_libraries(Graphic PRIVATE ${X11_Xext_LIB})
    endif()
  endif()
endif()
