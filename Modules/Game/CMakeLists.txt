# Modules/Game/CMakeLists.txt Game 聚合公共类型、底层图形、编辑逻辑、UI 和画布实现。 子模块顺序体现依赖层级：UI
# 可以消费 Graphic，但 Graphic 不得反向引用 UI。

# Common 提供跨子模块共享的轻量类型和服务契约。
add_subdirectory("Common")
# Graphic 封装窗口和 Vulkan 底层资源。
add_subdirectory("Graphic")
# Logic 管理会话、ECS 与编辑命令，不依赖高层 UI 绘制结构。
add_subdirectory("Logic")
# UI 通过抽象渲染回调消费 Graphic 与 Logic 服务。
add_subdirectory("UI")
# Canvas 实现具体编辑视口并复用前述基础模块。
add_subdirectory("Canvas")

# 聚合库只编译跨子模块应用服务和主循环，不重复收编子模块源码。
add_library(
  Game STATIC src/CanvasWorkspaceService.cpp src/EditorApplicationService.cpp
              src/GameLoop.cpp src/GlobDefs.cpp)

target_link_libraries(
  Game
  # PUBLIC 依赖构成 Game 对上层 Main 暴露的完整链接闭包。
  PUBLIC Log
         Runtime
         Common
         UI
         Canvas
         Graphic
         Logic
         Audio
         3rd_entt
         3rd_glm
         MMM
         Network
         NetworkCollaboration
  # 启动实现复用 Config 路径服务，公开 Game 头不依赖此服务。
  PRIVATE Config)

# 平台专属系统库保持 PRIVATE 行为；此处 PUBLIC 仅延续现有静态链接闭包需求。
if(WIN32)
  # 宽字符参数由 Shell32 解析。仅编译该平台的适配实现，通用启动流程不包含系统头。 Windows CRT 单独选择 UTF-8
  # 字符代码页，避免把平台 locale 名称暴露给通用代码。
  target_sources(Game PRIVATE src/platform/StartupCommandLinePathWin32.cpp
                              src/platform/StartupLocaleWin32.cpp)
  target_link_libraries(Game PRIVATE shell32)
  # Windows 崩溃诊断路径使用 DbgHelp 符号解析 API。
  target_link_libraries(Game PUBLIC dbghelp)
else()
  # POSIX 参数已经使用原生字节，无需链接额外平台库。 POSIX 按运行库可用数据选择字符 locale，路径仍使用原生字节。
  target_sources(Game PRIVATE src/platform/StartupCommandLinePathPosix.cpp
                              src/platform/StartupLocalePosix.cpp)
endif()

# 公开 include 根仅暴露 Game 门面与应用服务头。
target_include_directories(Game PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
