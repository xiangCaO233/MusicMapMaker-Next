cmake_minimum_required(VERSION 3.31)

# 调用方必须显式提供仓库资源根和应用配置根，禁止脚本自行猜测测试目录。 空资源根无法建立默认文件完整性边界，应在任何目录写入前终止。
if(NOT DEFINED MMM_SYNC_SOURCE_ASSETS_ROOT OR MMM_SYNC_SOURCE_ASSETS_ROOT
                                              STREQUAL "")
  message(FATAL_ERROR "MMM_SYNC_SOURCE_ASSETS_ROOT is required.")
endif()
# 空目标根可能把文件落入当前目录，因此单独进行强制校验。
if(NOT DEFINED MMM_SYNC_DESTINATION_CONFIG_ROOT
   OR MMM_SYNC_DESTINATION_CONFIG_ROOT STREQUAL "")
  message(FATAL_ERROR "MMM_SYNC_DESTINATION_CONFIG_ROOT is required.")
endif()

# 翻译源根允许后续自动纳入新增语言文件。
set(_MMM_SOURCE_TRANSLATIONS "${MMM_SYNC_SOURCE_ASSETS_ROOT}/translations")
# 默认皮肤源根包含入口及其字体、音频和图像资源。
set(_MMM_SOURCE_DEFAULT_SKIN "${MMM_SYNC_SOURCE_ASSETS_ROOT}/skins/mmm-default")
# 目标统一落在 AppPaths 使用的 assets 子目录。
set(_MMM_DESTINATION_ASSETS "${MMM_SYNC_DESTINATION_CONFIG_ROOT}/assets")

# 默认中英文和默认皮肤入口缺失时立即失败，避免生成看似成功但无法启动的本机目录。
foreach(
  _MMM_REQUIRED_FILE
  "${_MMM_SOURCE_TRANSLATIONS}/en_us.lua"
  "${_MMM_SOURCE_TRANSLATIONS}/zh_cn.lua"
  "${_MMM_SOURCE_DEFAULT_SKIN}/skin.lua")
  # 任一基础入口缺失都说明源码资源不完整，不能继续部分同步。
  if(NOT EXISTS "${_MMM_REQUIRED_FILE}")
    message(
      FATAL_ERROR "Required default asset is missing: ${_MMM_REQUIRED_FILE}")
  endif()
endforeach()

# 增量复制一个受管目录中的普通文件，不删除目标目录内的用户扩展。
function(_mmm_sync_managed_directory SOURCE_ROOT DESTINATION_ROOT)
  # 每次构建重新枚举，使新增翻译或皮肤资源无需重新配置 CMake 即可生效。
  file(
    GLOB_RECURSE _MMM_MANAGED_FILES
    LIST_DIRECTORIES false
    RELATIVE "${SOURCE_ROOT}"
    "${SOURCE_ROOT}/*")
  # 固定遍历顺序，便于不同平台复现日志和失败位置。
  list(SORT _MMM_MANAGED_FILES)
  # 每个相对路径始终保持在对应受管目标根内。
  foreach(_MMM_RELATIVE_FILE IN LISTS _MMM_MANAGED_FILES)
    get_filename_component(_MMM_RELATIVE_DIRECTORY "${_MMM_RELATIVE_FILE}"
                           DIRECTORY)
    # 逐级创建资源子目录，保留默认皮肤中的字体、音频和图像布局。
    file(MAKE_DIRECTORY "${DESTINATION_ROOT}/${_MMM_RELATIVE_DIRECTORY}")
    # configure_file(COPYONLY) 按内容覆盖变化文件，避免同时间戳的旧文件漏同步。
    configure_file("${SOURCE_ROOT}/${_MMM_RELATIVE_FILE}"
                   "${DESTINATION_ROOT}/${_MMM_RELATIVE_FILE}" COPYONLY)
  endforeach()
  # 不清理目标中的多余文件是保护用户扩展的关键约束。
endfunction()

# 两个受管根分别增量写入，用户额外语言和自定义皮肤目录不会被删除。
_mmm_sync_managed_directory("${_MMM_SOURCE_TRANSLATIONS}"
                            "${_MMM_DESTINATION_ASSETS}/translations")
# 默认皮肤独立写入固定名称，避免影响同级自定义皮肤。
_mmm_sync_managed_directory("${_MMM_SOURCE_DEFAULT_SKIN}"
                            "${_MMM_DESTINATION_ASSETS}/skins/mmm-default")

# 构建日志明确输出实际目标根，便于排查本机路径选择。
message(STATUS "已同步默认翻译与皮肤到 ${MMM_SYNC_DESTINATION_CONFIG_ROOT}")
