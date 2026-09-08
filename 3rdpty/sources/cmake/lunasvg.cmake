# * 这里只配置依赖目标，示例程序不参与编辑器构建或预编译打包。
# * 缓存使用 FORCE，以便已有构建树与当前项目链接偏好保持一致。
set(LUNASVG_BUILD_EXAMPLES
    OFF
    CACHE BOOL "" FORCE)

# * lunasvg / plutovg 跟随项目依赖链接偏好，shared 包需要真实 DLL。
# * 设置必须早于 add_subdirectory，使内部 plutovg 与外层 lunasvg 类型一致。
# * 此设置不替代 MSVC CRT 配置，后者仍由项目统一管理。
if(PROJECT_LINKAGE STREQUAL "shared")
  set(BUILD_SHARED_LIBS
      ON
      CACHE BOOL "" FORCE)
else()
  set(BUILD_SHARED_LIBS
      OFF
      CACHE BOOL "" FORCE)
endif()

# * MSVC 的数学函数由 CRT 提供，不存在独立的 Unix libm。
# * 固定为空也会覆盖旧缓存中的 m，避免生成不存在的 m.lib 链接输入。
# * 使用 MSVC 而非 WIN32 判断，MinGW 仍可依赖独立数学库。
# * 必须在创建上游目标前设置，才能阻止错误库名进入传递链接接口。
if(MSVC)
  set(MATH_LIBRARY
      ""
      CACHE FILEPATH "Math functions provided by the MSVC CRT" FORCE)
elseif(CMAKE_CROSSCOMPILING)
  # 其他交叉工具链保留独立数学库的既有约定。
  set(MATH_LIBRARY
      "m"
      CACHE FILEPATH "Forced math library" FORCE)
endif()

# 只在集成层提供配置，不修改上游 lunasvg 或其内部 plutovg 源码。
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/lunasvg SYSTEM)
if(PROJECT_LINKAGE STREQUAL "shared" AND WIN32)
  # * lunasvg 与 plutovg 没有统一导出宏，MSVC DLL 包使用自动导出。
  # * 仅修改真实存在的上游目标，稳定接口库自身不生成 DLL。
  if(TARGET lunasvg)
    set_target_properties(lunasvg PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
  endif()
  if(TARGET plutovg)
    set_target_properties(plutovg PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
  endif()
endif()

# 业务模块通过稳定接口消费库，不感知源码和预编译依赖来源。 底层数学函数依赖由上游目标传递，这里不额外补入平台库。
add_library(3rd_lunasvg INTERFACE)
target_link_libraries(3rd_lunasvg INTERFACE lunasvg::lunasvg)
