# 普通 build 产出的 .app 必须可由 Finder 直接启动；本文件只能在主应用目标所在目录引入。
if(NOT APPLE)
  return()
endif()

if(NOT TARGET MusicMapMaker-Next OR NOT TARGET MusicMapMaker-Updater)
  message(FATAL_ERROR "macOS 开发应用束 staging 要求主程序与更新器目标均已定义。")
endif()

# MoltenVK 不属于主 Mach-O 的直接依赖，必须连同 ICD 清单显式放入应用束。
find_library(MMM_DEVELOPMENT_MOLTENVK_LIBRARY NAMES MoltenVK)
if(NOT MMM_DEVELOPMENT_MOLTENVK_LIBRARY)
  message(FATAL_ERROR "macOS 开发应用束需要 MoltenVK 动态库。")
endif()
file(REAL_PATH "${MMM_DEVELOPMENT_MOLTENVK_LIBRARY}"
     MMM_DEVELOPMENT_MOLTENVK_LIBRARY_REAL)
if(NOT MMM_DEVELOPMENT_MOLTENVK_LIBRARY_REAL MATCHES "\\.dylib$")
  message(
    FATAL_ERROR
      "macOS 开发应用束需要 MoltenVK 动态库，当前找到：${MMM_DEVELOPMENT_MOLTENVK_LIBRARY_REAL}"
  )
endif()

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
set(MMM_MOLTENVK_API_VERSION "1.4.0")
configure_file("${CMAKE_SOURCE_DIR}/cmake/MoltenVK_icd.json.in"
               "${CMAKE_BINARY_DIR}/generated/MoltenVK_icd.json" @ONLY)

set(MMM_MACOS_CODESIGN_IDENTITY
    "-"
    CACHE STRING "macOS 应用签名身份；'-' 表示 ad-hoc，空值表示不签名。")

# 更新器必须先生成，随后与 MoltenVK 运行时一起复制到主应用束的标准目录。
add_dependencies(MusicMapMaker-Next MusicMapMaker-Updater)
add_custom_command(
  TARGET MusicMapMaker-Next
  POST_BUILD
  COMMAND
    "${CMAKE_COMMAND}" -E make_directory
    "$<TARGET_BUNDLE_CONTENT_DIR:MusicMapMaker-Next>/Frameworks"
    "$<TARGET_BUNDLE_CONTENT_DIR:MusicMapMaker-Next>/Resources/vulkan/icd.d"
  COMMAND
    "${CMAKE_COMMAND}" -E copy_if_different
    "${MMM_DEVELOPMENT_MOLTENVK_LIBRARY_REAL}"
    "$<TARGET_BUNDLE_CONTENT_DIR:MusicMapMaker-Next>/Frameworks/libMoltenVK.dylib"
  COMMAND
    "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_BINARY_DIR}/generated/MoltenVK_icd.json"
    "$<TARGET_BUNDLE_CONTENT_DIR:MusicMapMaker-Next>/Resources/vulkan/icd.d/MoltenVK_icd.json"
  COMMAND
    "${CMAKE_COMMAND}" -E copy_if_different
    "$<TARGET_FILE:MusicMapMaker-Updater>"
    "$<TARGET_BUNDLE_CONTENT_DIR:MusicMapMaker-Next>/MacOS/MusicMapMaker-Updater"
  COMMENT "Staging macOS development app runtime"
  VERBATIM)

# Apple Silicon 链接器只签 Mach-O 本体；资源与运行时复制完成后必须重签整个应用束。
if(NOT MMM_MACOS_CODESIGN_IDENTITY STREQUAL "")
  find_program(MMM_MACOS_CODESIGN_EXECUTABLE codesign REQUIRED)
  set(_mmm_development_codesign_arguments --force --deep --sign
                                          "${MMM_MACOS_CODESIGN_IDENTITY}")
  if(NOT MMM_MACOS_CODESIGN_IDENTITY STREQUAL "-")
    list(APPEND _mmm_development_codesign_arguments --options runtime
         --timestamp)
  endif()
  add_custom_command(
    TARGET MusicMapMaker-Next
    POST_BUILD
    COMMAND
      "${MMM_MACOS_CODESIGN_EXECUTABLE}" ${_mmm_development_codesign_arguments}
      "$<TARGET_BUNDLE_DIR:MusicMapMaker-Next>"
    COMMAND "${MMM_MACOS_CODESIGN_EXECUTABLE}" --verify --deep --strict
            --verbose=2 "$<TARGET_BUNDLE_DIR:MusicMapMaker-Next>"
    COMMENT "Signing macOS development app bundle"
    VERBATIM)
endif()
