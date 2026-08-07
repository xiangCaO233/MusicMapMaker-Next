# Modules/Main/CMakeLists.txt

if(APPLE)
  # macOS 使用标准 .app 应用束，供 CPack DragNDrop 生成可直接拖入 Applications 的 DMG。
  add_executable(MusicMapMaker-Next MACOSX_BUNDLE src/main.cpp)
  set_target_properties(
    MusicMapMaker-Next
    PROPERTIES MACOSX_BUNDLE_BUNDLE_NAME "MusicMapMaker-Next"
               MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
               MACOSX_BUNDLE_GUI_IDENTIFIER "top.xiang233.musicmapmaker-next"
               MACOSX_BUNDLE_INFO_PLIST
               "${CMAKE_SOURCE_DIR}/cmake/MacOSBundleInfo.plist.in"
               MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
elseif(WIN32)
  # Windows 可执行文件编译配置 编译MusicMapMaker-Next.exe
  set(ICON_RESOURCE src/icon.rc)
  if(CMAKE_BUILD_TYPE STREQUAL Debug)
    add_executable(MusicMapMaker-Next src/main.cpp ${ICON_RESOURCE})
  else()
    # Windows 非Debug模式下添加WIN32标识不显示控制台
    add_executable(MusicMapMaker-Next WIN32 src/main.cpp ${ICON_RESOURCE})
  endif()
  if(MSVC)
    set_target_properties(MusicMapMaker-Next PROPERTIES LINK_FLAGS
                                                        "/ENTRY:mainCRTStartup")
  endif()
else()
  # Linux 可执行文件编译配置 编译MusicMapMaker-Next可执行文件
  add_executable(MusicMapMaker-Next src/main.cpp)
endif()
add_gcc_debug_extract(MusicMapMaker-Next)
add_mingw_clang_pdb(MusicMapMaker-Next)
add_strip_command_for_release(MusicMapMaker-Next)

# 构建完成后分析 Clang -ftime-trace 并打印编译耗时 Top 10
find_package(Python3 QUIET)
if(Python3_EXECUTABLE)
  add_custom_command(
    TARGET MusicMapMaker-Next
    POST_BUILD
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/scripts/analyze_build_times.py" "${CMAKE_BINARY_DIR}"
    COMMENT "Analyzing compile times (Top 10 slowest TUs)...")
endif()

target_sources(MusicMapMaker-Next PRIVATE src/PGOProfiler.cpp
                                          src/StartupProgressDialog.cpp)
target_include_directories(MusicMapMaker-Next PRIVATE include)

target_link_libraries(MusicMapMaker-Next PRIVATE Log Config Audio Game Network
                                                 NetworkCollaboration)

# shared 预编译依赖复制到主程序输出目录，便于直接运行和 ldd/dumpbin 诊断完整动态链路。
if(COMMAND prebuilt_copy_runtime_files)
  prebuilt_copy_runtime_files(MusicMapMaker-Next)
endif()

if(APPLE)
  # 普通构建的 .app 需要与发布包一致的 Vulkan 运行时布局和完整签名。
  include("${CMAKE_SOURCE_DIR}/cmake/macos-development-bundle.cmake")
endif()
