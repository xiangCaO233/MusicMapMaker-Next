cmake_minimum_required(VERSION 3.31)

# 测试入口必须由 CTest 提供生产同步脚本和隔离输出根。 生产脚本路径由工程传入，测试不得维护第二份复制实现。
if(NOT DEFINED MMM_TEST_SYNC_SCRIPT OR MMM_TEST_SYNC_SCRIPT STREQUAL "")
  message(FATAL_ERROR "MMM_TEST_SYNC_SCRIPT is required.")
endif()
# 输出根必须位于构建树，避免测试碰触源码资源或真实用户配置。
if(NOT DEFINED MMM_TEST_OUTPUT_ROOT OR MMM_TEST_OUTPUT_ROOT STREQUAL "")
  message(FATAL_ERROR "MMM_TEST_OUTPUT_ROOT is required.")
endif()

# 校验同步后的文件存在且内容精确，避免仅检查目录存在造成假通过。
function(_mmm_assert_file_content FILE_PATH EXPECTED_CONTENT LABEL)
  # 缺失文件应给出带场景标签的直接诊断。
  if(NOT EXISTS "${FILE_PATH}")
    message(FATAL_ERROR "${LABEL}: file is missing: ${FILE_PATH}")
  endif()
  # 文件内容采用原始读取，Lua 和二进制夹具都不会被重新编码。
  file(READ "${FILE_PATH}" _MMM_ACTUAL_CONTENT)
  # 内容比较同时覆盖旧文件被正确替换的语义。
  if(NOT _MMM_ACTUAL_CONTENT STREQUAL EXPECTED_CONTENT)
    message(FATAL_ERROR "${LABEL}: unexpected content in ${FILE_PATH}")
  endif()
endfunction()

# 通过独立 cmake -P 进程运行生产脚本，确保参数边界与真实构建目标一致。
function(_mmm_run_sync SOURCE_ROOT DESTINATION_ROOT)
  # 子进程隔离生产脚本变量，避免测试函数作用域掩盖参数缺陷。
  execute_process(
    COMMAND
      "${CMAKE_COMMAND}" "-DMMM_SYNC_SOURCE_ASSETS_ROOT=${SOURCE_ROOT}"
      "-DMMM_SYNC_DESTINATION_CONFIG_ROOT=${DESTINATION_ROOT}" -P
      "${MMM_TEST_SYNC_SCRIPT}"
    RESULT_VARIABLE _MMM_SYNC_RESULT
    OUTPUT_VARIABLE _MMM_SYNC_OUTPUT
    ERROR_VARIABLE _MMM_SYNC_ERROR)
  # 同时回显标准输出与错误输出，便于定位跨平台 CMake 失败。
  if(NOT _MMM_SYNC_RESULT EQUAL 0)
    message(
      FATAL_ERROR
        "Sync failed (${_MMM_SYNC_RESULT}): ${_MMM_SYNC_OUTPUT}${_MMM_SYNC_ERROR}"
    )
  endif()
endfunction()

# 每个测试场景使用固定隔离根，便于重复运行前整体清理。
set(_MMM_TEST_ROOT "${MMM_TEST_OUTPUT_ROOT}/default_config_asset_sync")
# 源资源夹具模拟仓库 assets 目录结构。
set(_MMM_SOURCE_ROOT "${_MMM_TEST_ROOT}/source-assets")
# 目标夹具模拟 AppPaths::configRootPath 返回目录。
set(_MMM_CONFIG_ROOT "${_MMM_TEST_ROOT}/config-root")

# 每次测试从空目录开始，禁止读取开发者真实配置或前一次运行结果。
file(REMOVE_RECURSE "${_MMM_TEST_ROOT}")
# 先建立默认翻译源目录。
file(MAKE_DIRECTORY "${_MMM_SOURCE_ROOT}/translations")
# 嵌套图像目录验证默认皮肤会递归同步。
file(MAKE_DIRECTORY "${_MMM_SOURCE_ROOT}/skins/mmm-default/resources/image")
# IVM 皮肤使用独立嵌套资源验证完整目录树同步。
file(MAKE_DIRECTORY "${_MMM_SOURCE_ROOT}/skins/ivm/resources/image")
# 预建翻译目标以覆盖已有用户目录升级场景。
file(MAKE_DIRECTORY "${_MMM_CONFIG_ROOT}/assets/translations")
# 自定义皮肤目录用于验证默认同步不会执行破坏性清理。
file(MAKE_DIRECTORY "${_MMM_CONFIG_ROOT}/assets/skins/custom-skin")

# 源目录模拟仓库内默认翻译与包含嵌套资源的默认皮肤。 两种默认语言都必须从同一源根被发现。
file(WRITE "${_MMM_SOURCE_ROOT}/translations/en_us.lua" "en-v1")
file(WRITE "${_MMM_SOURCE_ROOT}/translations/zh_cn.lua" "zh-v1")
# 皮肤入口和嵌套资源共同代表完整默认皮肤。
file(WRITE "${_MMM_SOURCE_ROOT}/skins/mmm-default/skin.lua" "skin-v1")
file(WRITE "${_MMM_SOURCE_ROOT}/skins/mmm-default/resources/image/marker.txt"
     "image-v1")
# IVM 入口和独立纹理都必须随内置资源同步。
file(WRITE "${_MMM_SOURCE_ROOT}/skins/ivm/skin.lua" "ivm-v1")
file(WRITE "${_MMM_SOURCE_ROOT}/skins/ivm/resources/image/note.txt"
     "ivm-image-v1")

# 目标目录预置旧默认文件和用户扩展，覆盖与保留语义必须同时验证。 同名旧默认文件必须被源码版本覆盖。
file(WRITE "${_MMM_CONFIG_ROOT}/assets/translations/en_us.lua" "stale")
# 非默认语言代表用户自行安装的扩展文件。
file(WRITE "${_MMM_CONFIG_ROOT}/assets/translations/custom.lua" "custom")
# 非默认皮肤必须跨多次同步持续保留。
file(WRITE "${_MMM_CONFIG_ROOT}/assets/skins/custom-skin/skin.lua"
     "custom-skin")
# 用户配置和 ImGui 布局位于配置根，任何资源同步都不得覆盖。
file(WRITE "${_MMM_CONFIG_ROOT}/user_config.json" "user-config")
file(WRITE "${_MMM_CONFIG_ROOT}/imgui.ini" "imgui-layout")

# 首次同步应覆盖旧默认翻译并复制完整的默认皮肤目录树。
_mmm_run_sync("${_MMM_SOURCE_ROOT}" "${_MMM_CONFIG_ROOT}")
_mmm_assert_file_content("${_MMM_CONFIG_ROOT}/assets/translations/en_us.lua"
                         "en-v1" "首次英文翻译同步")
_mmm_assert_file_content("${_MMM_CONFIG_ROOT}/assets/translations/zh_cn.lua"
                         "zh-v1" "首次中文翻译同步")
_mmm_assert_file_content("${_MMM_CONFIG_ROOT}/assets/skins/mmm-default/skin.lua"
                         "skin-v1" "默认皮肤入口同步")
_mmm_assert_file_content(
  "${_MMM_CONFIG_ROOT}/assets/skins/mmm-default/resources/image/marker.txt"
  "image-v1" "默认皮肤嵌套资源同步")
_mmm_assert_file_content("${_MMM_CONFIG_ROOT}/assets/skins/ivm/skin.lua"
                         "ivm-v1" "IVM 皮肤入口同步")
_mmm_assert_file_content(
  "${_MMM_CONFIG_ROOT}/assets/skins/ivm/resources/image/note.txt"
  "ivm-image-v1" "IVM 皮肤嵌套资源同步")

# 用户额外语言与自定义皮肤不属于受管默认文件，必须原样保留。
_mmm_assert_file_content("${_MMM_CONFIG_ROOT}/assets/translations/custom.lua"
                         "custom" "保留用户额外语言")
_mmm_assert_file_content("${_MMM_CONFIG_ROOT}/assets/skins/custom-skin/skin.lua"
                         "custom-skin" "保留用户自定义皮肤")
_mmm_assert_file_content("${_MMM_CONFIG_ROOT}/user_config.json" "user-config"
                         "保留用户配置")
_mmm_assert_file_content("${_MMM_CONFIG_ROOT}/imgui.ini" "imgui-layout"
                         "保留 ImGui 布局")

# 第二次同步使用同路径更新内容，覆盖同时间戳旧文件的增量行为不能退化。 改变长度确保第二次内容更新不会依赖文件时间戳差异。
file(WRITE "${_MMM_SOURCE_ROOT}/translations/en_us.lua" "en-v2-longer")
file(WRITE "${_MMM_SOURCE_ROOT}/skins/mmm-default/skin.lua" "skin-v2-longer")
# IVM 内置入口也应随仓库版本正常增量更新。
file(WRITE "${_MMM_SOURCE_ROOT}/skins/ivm/skin.lua" "ivm-v2-longer")
# 重复运行同一生产脚本验证增量同步幂等边界。
_mmm_run_sync("${_MMM_SOURCE_ROOT}" "${_MMM_CONFIG_ROOT}")
_mmm_assert_file_content("${_MMM_CONFIG_ROOT}/assets/translations/en_us.lua"
                         "en-v2-longer" "增量英文翻译同步")
_mmm_assert_file_content("${_MMM_CONFIG_ROOT}/assets/skins/mmm-default/skin.lua"
                         "skin-v2-longer" "增量默认皮肤同步")
_mmm_assert_file_content("${_MMM_CONFIG_ROOT}/assets/skins/ivm/skin.lua"
                         "ivm-v2-longer" "增量 IVM 皮肤同步")
# 第二次同步仍必须证明根目录用户状态没有被资源更新波及。
_mmm_assert_file_content("${_MMM_CONFIG_ROOT}/user_config.json" "user-config"
                         "增量同步保留用户配置")
_mmm_assert_file_content("${_MMM_CONFIG_ROOT}/imgui.ini" "imgui-layout"
                         "增量同步保留 ImGui 布局")
