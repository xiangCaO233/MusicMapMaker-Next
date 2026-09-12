# Modules/Game/Graphic/CMakeLists.txt

# Graphic 目标封装窗口、Vulkan/ImGui 后端、主题与软件光标。上层模块只通过 include/
# 中的公开接口访问这些能力，不应直接编译本目录实现文件。

# Vulkan loader 和头文件属于图形模块的硬依赖，配置阶段缺失时立即失败，避免把 不完整目标传播给后续业务模块。
find_package(Vulkan REQUIRED)

# Font 独立维护系统字体解析与 ASCII 栅格化，并以 Font 目标提供给 Graphic。
add_subdirectory("Font")

# 主静态库按职责列出窗口后端、上下文、内存、离屏渲染和主题实现。显式源文件清单 让平台无关编译单元保持可审查；macOS Objective-C++
# 文件在下方条件分支追加。
#
# Windows 与 X11 适配实现保留在通用清单中，因为源文件会在原生 API 未启用时 编译平台后备分支。Apple 实现要求
# Objective-C++ 编译器，必须只在 APPLE 分支 通过 target_sources 追加，不能进入其他平台的源文件集合。
add_library(
  Graphic STATIC
  # GLFW 生命周期、原生窗口及 Linux/Windows 窗框适配。
  src/glfw/GLFWCallback.cpp
  src/glfw/NativeWindow.cpp
  src/glfw/adapters/Win32WindowAdapter.cpp
  src/glfw/adapters/X11WindowAdapter.cpp
  # Vulkan instance、device、诊断和 ImGui 后端初始化。
  src/imguivk/context/VKContext.cpp
  src/imguivk/context/VKContextDiagnostics.cpp
  src/imguivk/context/VKContextGLFW.cpp
  src/imguivk/context/VKContextDevice.cpp
  src/imguivk/context/VKContextDebugFeats.cpp
  src/imguivk/context/VKContextInfos.cpp
  src/imguivk/context/VKContextImguiImpl.cpp
  # GPU 缓冲、离屏目标与固定渲染对象。
  src/imguivk/mem/VKMemBuffer.cpp
  src/imguivk/VKOffScreenRenderer.cpp
  src/imguivk/VKOffScreenRendererRess.cpp
  src/imguivk/VKRenderPass.cpp
  src/imguivk/VKRenderPipeline.cpp
  # 主帧调度、交换链、着色器与纹理资源。
  src/imguivk/VKRenderer.cpp
  src/imguivk/VKRendererRender.cpp
  src/imguivk/VKRendererRess.cpp
  src/imguivk/VKShader.cpp
  src/imguivk/VKSwapchain.cpp
  src/imguivk/VKTexture.cpp
  src/imguivk/VKTextureAtlas.cpp
  # 主题注册、系统外观和软件光标。
  src/theme/ImGuiTheme.cpp
  src/theme/ImGuiThemeRegistry.cpp
  src/system/SystemTheme.cpp
  src/CursorManager.cpp)

# include/ 构成模块公共 API；使用 Graphic 的目标需要继承该搜索路径。
target_include_directories(Graphic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# 这些库均出现在公开头或构成 Graphic 对外行为，因此使用 PUBLIC 传播与源码构建 一致的链接闭包。业务目标无需感知依赖来自源码还是预编译包。
#
# 依赖名称必须与各 Find 模块或源码构建导出的 target 保持一致。新增依赖不能在 此处拼接 prebuilts 路径，SOURCES_BUILD
# 会在模块配置前选择唯一依赖来源。
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
  # Win32 窗框使用 DWM 与通用控件 API，注册表主题探测使用 Advapi32。
  target_link_libraries(Graphic PRIVATE dwmapi comctl32 advapi32)
endif()

# 主题插件测试覆盖 Lua 解析、内置主题继承以及重载时清除旧实例。 mmm_add_test_executable
# 统一测试二进制位置并注入隔离配置根，避免测试读取或保存 用户的个人配置。
#
# 命令把可写输出目录与只读示例插件分开传入；新增生成物必须继续写入 build， 不能落到 docs/plugin/examples 或其他源码资源目录。
mmm_add_test_executable(Graphic ThemePluginLoaderTest
                        tests/ThemePluginLoaderTest.cpp)
# 测试进程链接真实 Graphic 目标，以覆盖与应用相同的 Lua 和主题注册实现。
target_link_libraries(ThemePluginLoaderTest PRIVATE Graphic)
# CTest 参数顺序属于测试入口契约，调整夹具路径时必须同步测试 main 的解析顺序。
add_test(
  NAME ThemePluginLoaderTest
  COMMAND
    ThemePluginLoaderTest "${CMAKE_BINARY_DIR}/test_output/theme_plugins"
    "${CMAKE_SOURCE_DIR}/docs/plugin/examples/theme-example.lua"
    "${CMAKE_SOURCE_DIR}/docs/plugin/examples/ivm.lua")

if(APPLE)
  # Cocoa 窗框和系统主题查询需要 Objective-C++；这些源不能进入其他平台编译器。
  target_sources(
    Graphic
    PRIVATE src/glfw/adapters/MacOSWindowAdapter.mm
            src/glfw/adapters/MacOSWindowUtils.mm
            src/system/SystemThemeMacOS.mm)
  # AppKit 只服务本目标的原生窗口实现，不作为 Graphic 公共链接接口传播。
  target_link_libraries(Graphic PRIVATE "-framework AppKit")
endif()

if(UNIX AND NOT APPLE)
  # XDG Settings Portal 通过 GIO 提供跨 GNOME、KDE 与 Wayland/X11 的统一亮暗主题通知。 GIO 是
  # Linux 系统主题能力的必需后端，使用导入目标保留 pkg-config 给出的编译 与链接参数，避免手工拆分后遗漏线程或传递依赖。
  # MMM_ENABLE_XDG_PORTAL_THEME 只选择 Graphic 内部实现，不应作为使用者可见的 功能宏传播到上层业务目标。
  find_package(PkgConfig REQUIRED)
  # gio-2.0 导入目标集中携带 include、编译选项和链接库，避免目录级全局污染。
  pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
  # 平台能力宏和 GIO 链接均保持 PRIVATE，公共头不暴露任何 GObject 类型。
  target_compile_definitions(Graphic PRIVATE MMM_ENABLE_XDG_PORTAL_THEME)
  target_link_libraries(Graphic PRIVATE PkgConfig::GIO)

  # X11 仅增强无边框窗口的移动、缩放与形状裁剪；Wayland-only 环境仍可构建并 使用基础 GLFW 窗口，因此这里保持可选查找。 未找到
  # X11 时不定义适配宏，X11WindowAdapter 会编译为 GLFW 后备实现。
  find_package(X11)
  if(X11_FOUND)
    # 宏把原生交互代码限制在已确认头文件和库可用的构建中。
    target_compile_definitions(Graphic PRIVATE MMM_ENABLE_X11_FRAME_INTERACTION)
    target_include_directories(Graphic PRIVATE ${X11_INCLUDE_DIR})
    target_link_libraries(Graphic PRIVATE ${X11_LIBRARIES})
    if(X11_Xext_FOUND)
      # Shape 扩展单独控制圆角裁剪；基础 X11 窗框交互不依赖该可选库。
      target_compile_definitions(Graphic PRIVATE MMM_ENABLE_X11_FRAME_SHAPE)
      target_link_libraries(Graphic PRIVATE ${X11_Xext_LIB})
    endif()
  endif()
endif()
