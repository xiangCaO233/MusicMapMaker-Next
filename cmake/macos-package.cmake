# macOS 发布包使用 .app + DMG 布局；该文件只在顶层完成所有目标定义后引入。
if(NOT APPLE)
  return()
endif()

if(NOT TARGET MusicMapMaker-Next OR NOT TARGET MusicMapMaker-Updater)
  message(FATAL_ERROR "macOS 打包要求主程序与更新器目标均已定义。")
endif()

# MoltenVK 由 Vulkan loader 通过 ICD JSON 动态发现，不会出现在 otool 依赖中，必须显式装入应用束。
find_library(MMM_MOLTENVK_LIBRARY NAMES MoltenVK)
if(NOT MMM_MOLTENVK_LIBRARY)
  message(FATAL_ERROR "macOS 打包需要 MoltenVK 动态库；请安装 Vulkan SDK 或 molten-vk。")
endif()
file(REAL_PATH "${MMM_MOLTENVK_LIBRARY}" MMM_MOLTENVK_LIBRARY_REAL)
if(NOT MMM_MOLTENVK_LIBRARY_REAL MATCHES "\\.dylib$")
  message(FATAL_ERROR "macOS 打包需要 MoltenVK 动态库，当前找到："
                      "${MMM_MOLTENVK_LIBRARY_REAL}")
endif()

get_filename_component(_mmm_vulkan_library_dir "${Vulkan_LIBRARY}" DIRECTORY)
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
configure_file("${CMAKE_SOURCE_DIR}/cmake/MacOSBundleFixup.cmake.in"
               "${CMAKE_BINARY_DIR}/generated/MacOSBundleFixup.cmake" @ONLY)

# 主程序使用 BUNDLE 安装；独立更新器必须与主 Mach-O 相邻，沿用现有更新查找约定。
install(TARGETS MusicMapMaker-Next BUNDLE DESTINATION ".")
install(TARGETS MusicMapMaker-Updater
        RUNTIME DESTINATION "MusicMapMaker-Next.app/Contents/MacOS")
install(
  FILES "${MMM_MOLTENVK_LIBRARY_REAL}"
  DESTINATION "MusicMapMaker-Next.app/Contents/Frameworks"
  RENAME "libMoltenVK.dylib")
install(FILES "${CMAKE_BINARY_DIR}/generated/MoltenVK_icd.json"
        DESTINATION "MusicMapMaker-Next.app/Contents/Resources/vulkan/icd.d")

# BundleUtilities 会复制 Vulkan loader 等直接依赖、改写 rpath 并验证应用束可独立运行。
install(SCRIPT "${CMAKE_BINARY_DIR}/generated/MacOSBundleFixup.cmake")

if(CMAKE_OSX_ARCHITECTURES)
  string(REPLACE ";" "-" _mmm_package_arch "${CMAKE_OSX_ARCHITECTURES}")
else()
  set(_mmm_package_arch "${PROJECT_PREBUILT_ARCH}")
endif()

# CPack 同时生成供人工安装的 DMG，以及供自动更新器解压的完整 App ZIP。
set(CPACK_GENERATOR "DragNDrop;ZIP")
set(CPACK_MONOLITHIC_INSTALL ON)
set(CPACK_PACKAGE_NAME "MusicMapMaker-Next")
set(CPACK_PACKAGE_VENDOR "MusicMapMaker")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME
    "MusicMapMaker-Next-${PROJECT_VERSION}-macos-${_mmm_package_arch}")
set(CPACK_DMG_VOLUME_NAME "MusicMapMaker-Next ${PROJECT_VERSION}")
set(CPACK_DMG_FORMAT "UDZO")
set(CPACK_DMG_DISABLE_APPLICATIONS_SYMLINK OFF)
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/packages")

include(CPack)
