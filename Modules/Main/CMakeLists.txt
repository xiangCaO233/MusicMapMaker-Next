# Modules/Main/CMakeLists.txt 主程序目标按平台选择可执行文件外壳，业务源码与依赖集合保持一致。

# Android 复用 Main 启动与退出流程，只改变操作系统要求的原生入口类型。 SDLActivity 通过共享库符号寻找
# SDL_main，不能打包桌面 ELF 可执行文件。 Java 外壳不参与业务逻辑和帧循环。
if(ANDROID)
  # SDLActivity 加载原生共享入口，桌面仍使用下面原有 executable 分支。
  add_library(MusicMapMaker-Next SHARED src/main.cpp src/AndroidBootstrap.cpp)
  target_link_libraries(MusicMapMaker-Next PRIVATE 3rd_sdl3)
  set_target_properties(MusicMapMaker-Next PROPERTIES OUTPUT_NAME main)
  # JNI 入口来自主目标对象，静态依赖无需导出符号供其他共享模块替换。 隐藏归档导出也保证 AArch64 汇编对内部表的直接寻址可在共享库内绑定。
  target_link_options(MusicMapMaker-Next PRIVATE "-Wl,--exclude-libs,ALL")
elseif(APPLE)
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
  # Wayland 不接受客户端窗口图标，桌面文件名必须匹配 GLFW_WAYLAND_APP_ID。 安装入口与 hicolor 图标同名，X11
  # 同时使用 StartupWMClass 匹配现有窗口类。
  include(GNUInstallDirs)
  # 应用入口进入标准 bin；不在配置或构建期间写入宿主桌面目录。
  install(TARGETS MusicMapMaker-Next
          RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
  # SVG 是桌面环境的可缩放主图标，PNG 是不读取 SVG 的环境的兼容回退。 两种格式都由 logo.svg 母版生成，不根据系统主题烘焙不同背景。
  # 组件支持先暂存桌面资源验证，不要求安装主二进制或修改当前桌面设置。 使用相对 GNUInstallDirs 路径，保证用户级 prefix
  # 与打包暂存目录均可重定位。
  install(
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/src/logo.svg"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
    RENAME MusicMapMaker-Next.svg
    COMPONENT DesktopIntegration)
  # PNG 回退保留透明通道，避免桌面面板切换深浅主题后出现固定底色方块。
  install(
    FILES
      "${CMAKE_SOURCE_DIR}/assets/skins/mmm-default/resources/image/logo.png"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps"
    RENAME MusicMapMaker-Next.png
    COMPONENT DesktopIntegration)
  # 桌面注册只发生在显式 install，供发行包或用户级安装使用。
  install(
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/src/MusicMapMaker-Next.desktop"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
    COMPONENT DesktopIntegration)
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
if(NOT ANDROID)
  # Android 的原始符号保留在原生构建树，APK 裁剪交给 Android 打包工具。
  add_strip_command_for_release(MusicMapMaker-Next)
endif()

# 仅显式开启编译耗时诊断时分析 trace，关闭时连旧 trace 的扫描排序也不执行。
if(MMM_ENABLE_BUILD_TIMING)
  # Python 不可用时跳过报告，不应阻止主程序配置与构建。
  find_package(Python3 QUIET)
  if(Python3_EXECUTABLE)
    # POST_BUILD 只读取构建目录中的 trace，不参与可执行文件内容生成。
    add_custom_command(
      TARGET MusicMapMaker-Next
      POST_BUILD
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/scripts/analyze_build_times.py"
        "${CMAKE_BINARY_DIR}"
      COMMENT "Analyzing compile times (Top 10 slowest TUs)...")
  endif()
endif()

# 主程序和独立预览使用相同的窗口初始化、启动界面与 Shader。 保持源码私有，正式目标沿用各平台现有的编译和链接参数。
set(MMM_STARTUP_VIEW_SOURCES src/StartupProgressDialog.cpp
                             src/StartupLogoShader.cpp src/StartupWindow.cpp)
target_sources(
  MusicMapMaker-Next PRIVATE src/PGOProfiler.cpp src/PGOProfileCache.cpp
                             ${MMM_STARTUP_VIEW_SOURCES})
# 启动 Logo 与桌面图标使用同一份 SVG。构建期嵌入母版，以便首次同步前离线播放动画。

# 跟踪 SVG 源文件，增量构建也会更新启动遮罩。
set_property(
  DIRECTORY
  APPEND
  PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/logo.svg")
# 先读入原始文本，不能让 configure_file 对 SVG 中的字符执行变量替换。
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/logo.svg" MMM_STARTUP_LOGO_SVG)

# Android 源码构建与各桌面构建一样，必须先取得启动所需的 LFS 对象。 跳过 LFS 拉取时工作树只有指针；构建立即指出需要的对象，避免安装后没有
# Logo。
if(MMM_STARTUP_LOGO_SVG MATCHES "^version https://git-lfs")
  message(
    FATAL_ERROR
      "Missing startup logo SVG. Run git lfs pull --include=Modules/Main/src/logo.svg --exclude=''"
  )
endif()
# 模板用原始字符串保留曲线和透明度，无需专用的字节数组生成程序。

# 生成件放在构建树，不影响其他平台或工具链共用的源码目录。
configure_file(src/StartupLogoSvg.h.in
               "${CMAKE_CURRENT_BINARY_DIR}/generated/StartupLogoSvg.h" @ONLY)
# 私有头和生成的 SVG 文本只服务启动层，不构成安装接口。
target_include_directories(
  MusicMapMaker-Next PRIVATE include "${CMAKE_CURRENT_BINARY_DIR}/generated")

# 人工启动的桌面界面预览使用测试目录隔离配置，不登记为自动用例或安装目标。 Android 需由 SDLActivity
# 提供窗口，不能直接从独立桌面入口启动。
if(NOT ANDROID)
  # 目标始终可显式构建，包括关闭 BUILD_TESTING 的开发构建树。

  # 共用生成的 SVG 头以保持两个入口的启动画面一致。

  mmm_add_test_executable(
    Main StartupPagePreview STANDALONE tests/StartupPagePreview.cpp
    ${MMM_STARTUP_VIEW_SOURCES})
  target_include_directories(
    StartupPagePreview PRIVATE include "${CMAKE_CURRENT_BINARY_DIR}/generated")
  # 直接链接真实的渲染器和按钮反馈入口；模拟进度无需进入 GameLoop。
  target_link_libraries(StartupPagePreview PRIVATE Graphic UI Network
                                                   3rd_lunasvg)
  if(COMMAND prebuilt_copy_runtime_files)
    # 与主程序相同，使用 shared 预编译库时把运行库复制到预览程序目录。
    prebuilt_copy_runtime_files(StartupPagePreview)
  endif()
endif()

# 缓存回归只依赖文件系统，复用 Common 基础测试套件，不新增独立程序。 独立目录和小容量夹具验证边界，禁止触及用户真实临时缓存。
mmm_add_test_executable(Common PGOProfileCacheTest
                        tests/PGOProfileCacheTest.cpp src/PGOProfileCache.cpp)
target_include_directories(PGOProfileCacheTest PRIVATE include)
# 只链接日志以输出断言；容量算法不依赖图形、上传库或真实应用单例。
target_link_libraries(PGOProfileCacheTest PRIVATE Log)
# 输出固定在构建树，测试入口另建唯一子目录以隔离重复运行。
mmm_register_test_case(NAME PGOProfileCacheTest COMMAND PGOProfileCacheTest
                       "${CMAKE_BINARY_DIR}/test_output/PGOProfileCacheTest")

# 主程序是依赖汇聚点；各业务模块之间仍按自身公开 target 传播依赖。 Runtime 必须保留在链接集合中，为启动同步和退出 PGO
# 上传提供共享线程池。 显式链接 LunaSVG，启动层在默认皮肤与业务 UI 加载之前就能解析 SVG。
target_link_libraries(
  MusicMapMaker-Next
  PRIVATE Log
          Config
          Audio
          Game
          Network
          NetworkCollaboration
          Runtime
          3rd_lunasvg)

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

if(ANDROID)
  # 记录真实目标路径，打包脚本不根据目录或后缀猜测 SDL3 产物。
  file(
    GENERATE
    OUTPUT "${CMAKE_BINARY_DIR}/android-runtime-libraries.txt"
    CONTENT "$<TARGET_FILE:MusicMapMaker-Next>\n$<TARGET_FILE:SDL3::SDL3>\n")
endif()
