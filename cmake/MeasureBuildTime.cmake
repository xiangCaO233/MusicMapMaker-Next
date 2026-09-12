cmake_minimum_required(VERSION 3.31)

# 该脚本以 cmake -P 方式包装一次真实构建，并使用 cmake -E time 输出耗时。 MMM_BUILD_DIR
# 可指定已有构建树；MMM_CLEAN 控制是否先清理目标。 MMM_TARGET 与 MMM_PARALLEL 原样映射到 cmake --build
# 的可选参数。 脚本不重新配置工程，确保测量使用待测构建树的真实缓存和生成器。

# 以脚本目录的父级作为仓库根，避免调用者工作目录影响默认路径。
get_filename_component(_MMM_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# 默认测量标准 build 目录，同时允许 CI 传入其他配置构建树。
if(NOT DEFINED MMM_BUILD_DIR OR MMM_BUILD_DIR STREQUAL "")
  set(MMM_BUILD_DIR "${_MMM_REPO_ROOT}/build")
endif()

if(NOT IS_ABSOLUTE "${MMM_BUILD_DIR}")
  # 相对路径始终相对仓库根解析，而不是相对 shell 当前目录。
  get_filename_component(MMM_BUILD_DIR "${MMM_BUILD_DIR}" ABSOLUTE BASE_DIR
                         "${_MMM_REPO_ROOT}")
endif()

# 统一路径分隔符，保证后续命令文本在各平台可读且可复现。
file(TO_CMAKE_PATH "${MMM_BUILD_DIR}" MMM_BUILD_DIR)

# CMakeCache 是已配置构建树的最小哨兵，缺失时拒绝隐式创建新目录。
if(NOT EXISTS "${MMM_BUILD_DIR}/CMakeCache.txt")
  message(
    FATAL_ERROR
      "MMM_BUILD_DIR does not contain CMakeCache.txt: ${MMM_BUILD_DIR}")
endif()

# 清理默认关闭，以便测量常见的增量构建成本。
if(NOT DEFINED MMM_CLEAN)
  set(MMM_CLEAN OFF)
endif()

# 空并行值让底层生成器使用其默认并发策略。
if(NOT DEFINED MMM_PARALLEL)
  set(MMM_PARALLEL "")
endif()

# 空目标表示执行生成器默认目标，即完整常规构建。
if(NOT DEFINED MMM_TARGET)
  set(MMM_TARGET "")
endif()

if(MMM_CLEAN)
  # 清理不计入后续 timed build，但失败会使测量失去意义。
  message(STATUS "Cleaning build directory: ${MMM_BUILD_DIR}")
  execute_process(COMMAND "${CMAKE_COMMAND}" --build "${MMM_BUILD_DIR}" --target
                          clean RESULT_VARIABLE _MMM_CLEAN_RESULT)

  if(NOT _MMM_CLEAN_RESULT EQUAL 0)
    # 保留底层构建器输出，并以脚本失败状态阻止继续计时。
    message(FATAL_ERROR "Clean failed with exit code ${_MMM_CLEAN_RESULT}.")
  endif()
endif()

# 用 CMake 列表保存参数边界，路径中的空格不会被重新分词。
set(_MMM_BUILD_COMMAND "${CMAKE_COMMAND}" "--build" "${MMM_BUILD_DIR}")

# 可选参数按 CMake 官方命令语法追加。
if(NOT MMM_TARGET STREQUAL "")
  # 目标名作为独立参数追加，支持测量单个可执行文件或辅助目标。
  list(APPEND _MMM_BUILD_COMMAND "--target" "${MMM_TARGET}")
endif()

if(NOT MMM_PARALLEL STREQUAL "")
  # 并行度仅在显式提供时传递，避免覆盖用户生成器环境设置。
  list(APPEND _MMM_BUILD_COMMAND "--parallel" "${MMM_PARALLEL}")
endif()

# 文本形式只用于日志展示，实际执行继续使用保留边界的列表。
string(REPLACE ";" " " _MMM_BUILD_COMMAND_TEXT "${_MMM_BUILD_COMMAND}")
message(STATUS "Timed build command: ${_MMM_BUILD_COMMAND_TEXT}")
# 明确回显计时器入口，避免把生成器自身统计误认为脚本计时结果。
message(STATUS "Timer command: ${CMAKE_COMMAND} -E time")

# execute_process 继承当前终端输出，使编译进度和最终耗时同时可见。
execute_process(COMMAND "${CMAKE_COMMAND}" -E time ${_MMM_BUILD_COMMAND}
                RESULT_VARIABLE _MMM_BUILD_RESULT)

# 计时包装器必须传播构建失败，不能因成功打印耗时而返回零。
if(NOT _MMM_BUILD_RESULT EQUAL 0)
  message(FATAL_ERROR "Timed build failed with exit code ${_MMM_BUILD_RESULT}.")
endif()
