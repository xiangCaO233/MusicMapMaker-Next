# macOS 发布包使用 .app + DMG 布局；该文件只在顶层完成所有目标定义后引入。 安装阶段负责组装可迁移应用束，CPack 再从同一安装树生成
# DMG 与 ZIP。 运行时依赖修复和签名由配置后的 MacOSBundleFixup 脚本统一收尾。
if(NOT APPLE)
  # 公共配置意外包含时不应在非 Apple 平台注册安装与 CPack 规则。
  return()
endif()

# 打包必须同时包含主程序与同目录更新器，缺失目标属于配置错误。
if(NOT TARGET MusicMapMaker-Next OR NOT TARGET MusicMapMaker-Updater)
  message(FATAL_ERROR "macOS 打包要求主程序与更新器目标均已定义。")
endif()

# MoltenVK 由 Vulkan loader 通过 ICD JSON 动态发现，不会出现在 otool 依赖中，必须显式装入应用束。
find_library(MMM_MOLTENVK_LIBRARY NAMES MoltenVK)
if(NOT MMM_MOLTENVK_LIBRARY)
  # MoltenVK 不会被普通链接依赖扫描发现，必须显式要求动态库存在。
  message(FATAL_ERROR "macOS 打包需要 MoltenVK 动态库；请安装 Vulkan SDK 或 molten-vk。")
endif()
# 复制真实文件而非 SDK 内部符号链接，保证包离开构建机后仍有效。
file(REAL_PATH "${MMM_MOLTENVK_LIBRARY}" MMM_MOLTENVK_LIBRARY_REAL)
if(NOT MMM_MOLTENVK_LIBRARY_REAL MATCHES "\\.dylib$")
  # ICD 运行时装载要求 dylib，静态归档不能放入 Frameworks 替代。
  message(FATAL_ERROR "macOS 打包需要 MoltenVK 动态库，当前找到："
                      "${MMM_MOLTENVK_LIBRARY_REAL}")
endif()

get_filename_component(_mmm_vulkan_library_dir "${Vulkan_LIBRARY}" DIRECTORY)
# BundleUtilities 在这些目录中解析 Vulkan loader 与 MoltenVK 的非系统依赖。
get_filename_component(_mmm_moltenvk_library_dir "${MMM_MOLTENVK_LIBRARY_REAL}"
                       DIRECTORY)
set(MMM_MACOS_DEPENDENCY_DIRS
    "${_mmm_vulkan_library_dir};${_mmm_moltenvk_library_dir}")

# MoltenVK 当前公开 Vulkan 1.4 入口；ICD patch 版本由 loader 协商，不在包内写死 SDK patch 号。
set(MMM_MOLTENVK_API_VERSION "1.4.0")
configure_file("${CMAKE_SOURCE_DIR}/cmake/MoltenVK_icd.json.in"
               "${CMAKE_BINARY_DIR}/generated/MoltenVK_icd.json" @ONLY)

set(MMM_MACOS_CODESIGN_IDENTITY
    "-"
    CACHE STRING "macOS CPack 应用签名身份；'-' 表示 ad-hoc，空值表示不签名。")
# 身份被写入 fixup 脚本，签名发生在所有 Mach-O 路径修复之后。
configure_file("${CMAKE_SOURCE_DIR}/cmake/MacOSBundleFixup.cmake.in"
               "${CMAKE_BINARY_DIR}/generated/MacOSBundleFixup.cmake" @ONLY)

# 主程序使用 BUNDLE 安装；独立更新器必须与主 Mach-O 相邻，沿用现有更新查找约定。 DESTINATION "." 保留 CMake
# 目标已经声明的标准 .app 目录结构。
install(TARGETS MusicMapMaker-Next BUNDLE DESTINATION ".")
# 更新器不创建独立应用束，而是作为主应用内的辅助可执行文件安装。
install(TARGETS MusicMapMaker-Updater
        RUNTIME DESTINATION "MusicMapMaker-Next.app/Contents/MacOS")
install(
  # MoltenVK 使用固定名称，必须与生成的 ICD JSON 库路径一致。
  FILES "${MMM_MOLTENVK_LIBRARY_REAL}"
  DESTINATION "MusicMapMaker-Next.app/Contents/Frameworks"
  RENAME "libMoltenVK.dylib")
install(FILES "${CMAKE_BINARY_DIR}/generated/MoltenVK_icd.json"
        DESTINATION "MusicMapMaker-Next.app/Contents/Resources/vulkan/icd.d")

# BundleUtilities 会复制 Vulkan loader 等直接依赖、改写 rpath 并验证应用束可独立运行。
install(SCRIPT "${CMAKE_BINARY_DIR}/generated/MacOSBundleFixup.cmake")

if(CMAKE_OSX_ARCHITECTURES)
  # universal 架构列表转换为文件名安全的连字符形式。
  string(REPLACE ";" "-" _mmm_package_arch "${CMAKE_OSX_ARCHITECTURES}")
else()
  # 未显式指定架构时沿用预编译依赖选择出的目标架构标签。
  set(_mmm_package_arch "${PROJECT_PREBUILT_ARCH}")
endif()

# CPack 同时生成供人工安装的 DMG，以及供自动更新器解压的完整 App ZIP。
set(CPACK_GENERATOR "DragNDrop;ZIP")
# 两种产物来自同一完整安装树。 单体安装避免 CPack 为组件拆出不完整的应用束。
set(CPACK_MONOLITHIC_INSTALL ON)
set(CPACK_PACKAGE_NAME "MusicMapMaker-Next")
# vendor 字段用于安装包元数据，不参与应用显示名。
set(CPACK_PACKAGE_VENDOR "MusicMapMaker")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME
    "MusicMapMaker-Next-${PROJECT_VERSION}-macos-${_mmm_package_arch}")
set(CPACK_DMG_VOLUME_NAME "MusicMapMaker-Next ${PROJECT_VERSION}")
# UDZO 提供只读压缩镜像，适合直接发布下载。
set(CPACK_DMG_FORMAT "UDZO")
set(CPACK_DMG_DISABLE_APPLICATIONS_SYMLINK OFF)
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/packages")
# 所有生成器共享同一 packages 输出根，便于 CI 统一收集发布产物。

# 所有 CPACK 变量确定后再包含模块，使生成目标捕获最终值。
include(CPack)
