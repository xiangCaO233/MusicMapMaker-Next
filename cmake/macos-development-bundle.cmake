# 普通 build 产出的 .app 必须可由 Finder 直接启动；本文件只能在主应用目标所在目录引入。 与发布 CPack 流程不同，本脚本挂接
# POST_BUILD，服务本地开发产物。 staging 顺序固定为创建目录、复制 MoltenVK、写入 ICD、复制更新器，最后签名。
# 不在此处复制由链接器直接引用的 dylib，它们由正常构建 rpath 负责解析。
if(NOT APPLE)
  # 防止该脚本被公共 CMake 路径意外包含时污染其他平台目标。
  return()
endif()

# 两个可执行目标缺一不可，否则应用束中的更新协议无法完成自替换。
if(NOT TARGET MusicMapMaker-Next OR NOT TARGET MusicMapMaker-Updater)
  message(FATAL_ERROR "macOS 开发应用束 staging 要求主程序与更新器目标均已定义。")
endif()

# MoltenVK 不属于主 Mach-O 的直接依赖，必须连同 ICD 清单显式放入应用束。
find_library(MMM_DEVELOPMENT_MOLTENVK_LIBRARY NAMES MoltenVK)
if(NOT MMM_DEVELOPMENT_MOLTENVK_LIBRARY)
  # 开发应用也必须可脱离终端环境启动，因此不允许依赖未打包的 ICD。
  message(FATAL_ERROR "macOS 开发应用束需要 MoltenVK 动态库。")
endif()
# 解析符号链接后复制真实 dylib，避免应用束内留下指向 SDK 的外部链接。
file(REAL_PATH "${MMM_DEVELOPMENT_MOLTENVK_LIBRARY}"
     MMM_DEVELOPMENT_MOLTENVK_LIBRARY_REAL)
if(NOT MMM_DEVELOPMENT_MOLTENVK_LIBRARY_REAL MATCHES "\\.dylib$")
  # 静态 MoltenVK 无法由 Vulkan loader 按 ICD 清单在运行时装载。
  message(
    FATAL_ERROR
      "macOS 开发应用束需要 MoltenVK 动态库，当前找到：${MMM_DEVELOPMENT_MOLTENVK_LIBRARY_REAL}"
  )
endif()

# generated 目录同时承载配置后的 ICD，保持源码模板只读。
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
# ICD 模板输出与其他生成头共享目录。 清单声明 loader 可协商的 API 上限，不绑定 SDK 的 patch 版本。
set(MMM_MOLTENVK_API_VERSION "1.4.0")
configure_file(
  "${CMAKE_SOURCE_DIR}/cmake/MoltenVK_icd.json.in"
  # 输出留在构建树，源码清单模板保持只读。
  "${CMAKE_BINARY_DIR}/generated/MoltenVK_icd.json" @ONLY)

set(MMM_MACOS_CODESIGN_IDENTITY
    "-"
    CACHE STRING "macOS 应用签名身份；'-' 表示 ad-hoc，空值表示不签名。")
# 身份作为缓存项供本地开发者切换，默认 ad-hoc 可满足 Finder 完整性检查。

# 更新器必须先生成，随后与 MoltenVK 运行时一起复制到主应用束的标准目录。 显式依赖保证生成器不会并行复制尚未链接完成的更新器。
# 主应用每次重新链接都重新检查三个运行时文件，保持增量构建产物完整。
add_dependencies(MusicMapMaker-Next MusicMapMaker-Updater)
# POST_BUILD 每次主程序重链后刷新运行时文件，copy_if_different 避免无谓时间戳变化。
add_custom_command(
  TARGET MusicMapMaker-Next
  POST_BUILD
  # 所有复制步骤依附同一次主目标链接完成事件。
  COMMAND
    # Frameworks 存放 dylib，Resources/vulkan/icd.d 存放 loader 清单。
    "${CMAKE_COMMAND}" -E make_directory
    "$<TARGET_BUNDLE_CONTENT_DIR:MusicMapMaker-Next>/Frameworks"
    "$<TARGET_BUNDLE_CONTENT_DIR:MusicMapMaker-Next>/Resources/vulkan/icd.d"
  COMMAND
    # 固定目标文件名使 ICD 模板可以使用相对应用束的稳定路径。
    "${CMAKE_COMMAND}" -E copy_if_different
    "${MMM_DEVELOPMENT_MOLTENVK_LIBRARY_REAL}"
    "$<TARGET_BUNDLE_CONTENT_DIR:MusicMapMaker-Next>/Frameworks/libMoltenVK.dylib"
  COMMAND
    # 配置后的清单随应用分发，Finder 启动不再依赖 VULKAN_SDK 环境变量。
    "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_BINARY_DIR}/generated/MoltenVK_icd.json"
    "$<TARGET_BUNDLE_CONTENT_DIR:MusicMapMaker-Next>/Resources/vulkan/icd.d/MoltenVK_icd.json"
  COMMAND
    # 更新器与主 Mach-O 同目录，匹配运行时查找和替换约定。
    "${CMAKE_COMMAND}" -E copy_if_different
    "$<TARGET_FILE:MusicMapMaker-Updater>"
    "$<TARGET_BUNDLE_CONTENT_DIR:MusicMapMaker-Next>/MacOS/MusicMapMaker-Updater"
  COMMENT "Staging macOS development app runtime"
  # VERBATIM 保留应用束路径中的空格及生成器表达式参数边界。
  VERBATIM)

# Apple Silicon 链接器只签 Mach-O 本体；资源与运行时复制完成后必须重签整个应用束。
if(NOT MMM_MACOS_CODESIGN_IDENTITY STREQUAL "")
  # 签名步骤仅在身份非空时注册。 空身份显式关闭签名；非空身份要求宿主必须提供 codesign。
  find_program(MMM_MACOS_CODESIGN_EXECUTABLE codesign REQUIRED)
  # deep 覆盖刚复制的嵌套 dylib 与更新器，force 允许重复开发构建重签。
  set(_mmm_development_codesign_arguments --force --deep --sign
                                          "${MMM_MACOS_CODESIGN_IDENTITY}")
  if(NOT MMM_MACOS_CODESIGN_IDENTITY STREQUAL "-")
    # Developer ID 启用 hardened runtime 与时间戳，ad-hoc 签名不支持这些选项。
    list(APPEND _mmm_development_codesign_arguments --options runtime
         --timestamp)
  endif()
  add_custom_command(
    TARGET MusicMapMaker-Next
    POST_BUILD
    COMMAND
      # staging 完成后对整个应用束签名，不能只签主可执行文件。
      "${MMM_MACOS_CODESIGN_EXECUTABLE}" ${_mmm_development_codesign_arguments}
      "$<TARGET_BUNDLE_DIR:MusicMapMaker-Next>"
    # 严格验证立即阻止把不完整的开发应用交给 Finder 启动。
    COMMAND "${MMM_MACOS_CODESIGN_EXECUTABLE}" --verify --deep --strict
            --verbose=2 "$<TARGET_BUNDLE_DIR:MusicMapMaker-Next>"
    COMMENT "Signing macOS development app bundle"
    # 签名和验证视为同一目标的连续后处理步骤。
    VERBATIM)
endif()
