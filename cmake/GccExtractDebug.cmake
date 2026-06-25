# GccExtractDebug.cmake 在 GCC (MinGW) 编译后，将 DWARF 调试信息从 exe 中剥离到 .dbg 文件 仅在
# Debug 和 RelWithDebInfo 配置下执行
#
# 调用方式: cmake -D TARGET_FILE="<path>" -D OBJCOPY="<objcopy>" -D
# STRIP_EXE="<strip>" -D CONFIG="<config>" -P GccExtractDebug.cmake

if(NOT CONFIG MATCHES "^(Debug|RelWithDebInfo)$")
  return()
endif()

if(NOT TARGET_FILE)
  message(FATAL_ERROR "GccExtractDebug: TARGET_FILE not set")
endif()

set(DEBUG_FILE "${TARGET_FILE}.dbg")
get_filename_component(TARGET_DIR "${TARGET_FILE}" DIRECTORY)
get_filename_component(TARGET_NAME "${TARGET_FILE}" NAME)
get_filename_component(DEBUG_LINK_NAME "${DEBUG_FILE}" NAME)
set(DEBUG_LINK_ARG "--add-gnu-debuglink=${DEBUG_LINK_NAME}")

message(STATUS "GCC debug extraction: ${DEBUG_FILE}")

# 1. 将调试信息提取到独立 .dbg 文件
execute_process(COMMAND "${OBJCOPY}" --only-keep-debug "${TARGET_FILE}"
                        "${DEBUG_FILE}" RESULT_VARIABLE _ret)
if(NOT _ret EQUAL 0)
  message(
    WARNING
      "GccExtractDebug: objcopy --only-keep-debug failed for ${TARGET_FILE}")
  return()
endif()

# 1. 从 exe 中移除调试节（保留符号表，用于崩溃地址解析）
execute_process(COMMAND "${STRIP_EXE}" --strip-debug "${TARGET_FILE}"
                RESULT_VARIABLE _ret)
if(NOT _ret EQUAL 0)
  message(
    WARNING "GccExtractDebug: strip --strip-debug failed for ${TARGET_FILE}")
endif()

# 1. 在 exe 中添加对 .dbg 文件的引用。debuglink 参数先拼成普通变量，避免 CMake 把引号并入 --add-gnu-debuglink
#   参数。
execute_process(
  COMMAND "${OBJCOPY}" "${DEBUG_LINK_ARG}" "${TARGET_NAME}"
  WORKING_DIRECTORY "${TARGET_DIR}"
  RESULT_VARIABLE _ret)
if(NOT _ret EQUAL 0)
  message(
    WARNING
      "GccExtractDebug: objcopy --add-gnu-debuglink failed for ${TARGET_FILE}")
endif()

message(STATUS "GCC debug extraction done: ${DEBUG_FILE}")
