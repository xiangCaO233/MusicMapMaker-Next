# Modules/Main/CMakeLists.txt 主程序目标按平台选择可执行文件外壳，业务源码与依赖集合保持一致。

if(APPLE)
  # macOS 使用标准 .app 应用束，供 CPack DragNDrop 生成可直接拖入 Applications 的 DMG。 图标作为
  # Resources 内容加入 Bundle，不能仅作为普通源文件链接。
  set(MMM_APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/src/logo.icns")
  # MACOSX_PACKAGE_LOCATION 必须在图标加入目标前设置，生成器才能复制到资源目录。
  set_source_files_properties("${MMM_APP_ICON}"
                              PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
  add_executable(MusicMapMaker-Next MACOSX_BUNDLE src/main.cpp
                                                  "${MMM_APP_ICON}")
  # 版本、标识和 Info.plist 模板集中写入目标属性，供开发构建与 CPack 共用。
  set_target_properties(
    MusicMapMaker-Next
    PROPERTIES MACOSX_BUNDLE_BUNDLE_NAME "MusicMapMaker-Next"
               MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
               MACOSX_BUNDLE_ICON_FILE "logo.icns"
               MACOSX_BUNDLE_GUI_IDENTIFIER "top.xiang233.musicmapmaker-next"
               MACOSX_BUNDLE_INFO_PLIST
               "${CMAKE_SOURCE_DIR}/cmake/MacOSBundleInfo.plist.in"
               MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
elseif(WIN32)
  # Windows 资源脚本为可执行文件嵌入应用图标。
  set(ICON_RESOURCE src/icon.rc)
  if(CMAKE_BUILD_TYPE STREQUAL Debug)
    # Debug 保留控制台窗口，便于直接观察启动阶段诊断日志。
    add_executable(MusicMapMaker-Next src/main.cpp ${ICON_RESOURCE})
  else()
    # 发布配置使用 WIN32 子系统，启动时不显示额外控制台窗口。
    add_executable(MusicMapMaker-Next WIN32 src/main.cpp ${ICON_RESOURCE})
  endif()
  if(MSVC)
    # 业务仍实现标准 main，显式入口让 GUI 子系统沿用同一跨平台启动函数。
    set_target_properties(MusicMapMaker-Next PROPERTIES LINK_FLAGS
                                                        "/ENTRY:mainCRTStartup")
  endif()
  # 所有平台分支必须最终创建同名目标，后续公共配置不再判断平台。
else()
  # Linux 使用普通可执行目标，图标由桌面集成包单独安装。
  add_executable(MusicMapMaker-Next src/main.cpp)
endif()

# 本机显式构建应用目标时也先同步默认资源；关闭开关后目标不存在且不产生写入。

# 使用目标存在性判断可让所有关闭该选项的 CI 与交叉构建保持原依赖图。
if(TARGET SyncDefaultConfigAssets)
  # 依赖边只附着在显式应用构建上，不影响单独构建库和测试目标。
  add_dependencies(MusicMapMaker-Next SyncDefaultConfigAssets)
endif()

# GCC 调试信息提取 helper 会自行检查编译器和构建配置。
add_gcc_debug_extract(MusicMapMaker-Next)
# MinGW 与 MSVC 风格调试信息布局由公共 helper 按实际工具链选择。
add_mingw_clang_pdb(MusicMapMaker-Next)
# 仅发布型配置剥离主二进制，外置符号仍由前述 helper 保存。
add_strip_command_for_release(MusicMapMaker-Next)

# 构建完成后分析 Clang -ftime-trace 并打印编译耗时 Top 10。 Python 不可用时跳过报告，不应阻止主程序配置与构建。
find_package(Python3 QUIET)
if(Python3_EXECUTABLE)
  # POST_BUILD 只读取构建目录中的 trace，不参与可执行文件内容生成。
  add_custom_command(
    TARGET MusicMapMaker-Next
    POST_BUILD
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/scripts/analyze_build_times.py" "${CMAKE_BINARY_DIR}"
    COMMENT "Analyzing compile times (Top 10 slowest TUs)...")
endif()

# 启动辅助实现归入主目标，不额外暴露静态库或跨模块链接接口。
target_sources(MusicMapMaker-Next PRIVATE src/PGOProfiler.cpp
                                          src/StartupProgressDialog.cpp)
# include 目录只服务 Main 内部实现，其他模块不得反向依赖启动层。 两个辅助头通过该私有目录供 main.cpp 使用，不构成安装接口。
target_include_directories(MusicMapMaker-Next PRIVATE include)

# 主程序是依赖汇聚点；各业务模块之间仍按自身公开 target 传播依赖。 Runtime 必须保留在链接集合中，为启动同步和退出 PGO
# 上传提供共享线程池。
target_link_libraries(
  MusicMapMaker-Next
  PRIVATE Log
          Config
          Audio
          Game
          Network
          NetworkCollaboration
          Runtime)

# 平台运行库复制和 Bundle 修整均在链接依赖完整后执行。

# shared 预编译依赖复制到主程序输出目录，便于直接运行和 ldd/dumpbin 诊断完整动态链路。
if(COMMAND prebuilt_copy_runtime_files)
  # helper 仅在预编译依赖入口定义时存在，源码依赖构建无需复制运行库。
  prebuilt_copy_runtime_files(MusicMapMaker-Next)
endif()

if(APPLE)
  # 普通构建的 .app 需要与发布包一致的 Vulkan 运行时布局和完整签名。 平台脚本放在目标全部定义之后，便于收集最终依赖和 Bundle 属性。
  include("${CMAKE_SOURCE_DIR}/cmake/macos-development-bundle.cmake")
endif()
