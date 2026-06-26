# 禁用构建示例
set(LUNASVG_BUILD_EXAMPLES
    OFF
    CACHE BOOL "" FORCE)

# lunasvg / plutovg 跟随项目依赖链接偏好，shared 包需要真实 DLL 以缩小 exe 本体。
if(PROJECT_LINKAGE STREQUAL "shared")
  set(BUILD_SHARED_LIBS
      ON
      CACHE BOOL "" FORCE)
else()
  set(BUILD_SHARED_LIBS
      OFF
      CACHE BOOL "" FORCE)
endif()

# 交叉编译时禁用 plutovg 对 m 的自动检测，改用 lib_proxy 中的 m.lib
if(CMAKE_CROSSCOMPILING)
  set(MATH_LIBRARY
      "m"
      CACHE FILEPATH "Forced math library" FORCE)
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/lunasvg SYSTEM)
if(PROJECT_LINKAGE STREQUAL "shared" AND WIN32)
  # lunasvg 与 plutovg 没有统一导出宏，MSVC DLL 包使用自动导出。
  if(TARGET lunasvg)
    set_target_properties(lunasvg PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
  endif()
  if(TARGET plutovg)
    set_target_properties(plutovg PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
  endif()
endif()

# 创建接口库
add_library(3rd_lunasvg INTERFACE)
target_link_libraries(3rd_lunasvg INTERFACE lunasvg::lunasvg)
