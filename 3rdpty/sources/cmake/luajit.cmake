project(LuaJIT C)

set(LJ_BIN_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin")
if(NOT EXISTS "${LJ_BIN_OUTPUT_DIR}")
    file(MAKE_DIRECTORY "${LJ_BIN_OUTPUT_DIR}")
endif()

# =========================================================================
# 自动检测 CPU 核心数
# =========================================================================
cmake_host_system_information(RESULT HOST_CORES QUERY NUMBER_OF_LOGICAL_CORES)

# =========================================================================
# 路径定义
# =========================================================================
set(LJ_ORIGINAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/luajit")
set(LJ_BUILD_ROOT "${CMAKE_BINARY_DIR}/luajit")
set(LJ_BUILD_SRC "${LJ_BUILD_ROOT}/src")

if(NOT EXISTS "${LJ_BUILD_SRC}")
    file(MAKE_DIRECTORY "${LJ_BUILD_SRC}")
endif()

# 核心构建逻辑 - 统一为静态库
# 统一处理 GCC/Clang 族的编译标志 (MinGW & Unix)
if(NOT MSVC OR CMAKE_CROSSCOMPILING)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        # 注意：这里直接写字符串，不要在里面加 \"
        set(LJ_G_FLAGS "XCFLAGS=-O0 -fPIC" "CCDEBUG=-g")
    else()
        # 注意：这里直接写字符串，不要在里面加 \"
        set(LJ_G_FLAGS "XCFLAGS=-O3 -fPIC")
    endif()
endif()

if(MSVC AND NOT CMAKE_CROSSCOMPILING)
    # Windows MSVC 静态构建
    # msvcbuild.bat static 会生成 lua51.lib (静态库)
    set(LJ_LIB_NAME "lua51.lib")
    set(LJ_OUTPUT_LIB "${LJ_BUILD_SRC}/${LJ_LIB_NAME}")

    set(BUILD_CMD "msvcbuild.bat")
    if(CMAKE_BUILD_TYPE MATCHES Debug)
        list(APPEND BUILD_CMD debug)
    endif()
    list(APPEND BUILD_CMD static) # 强制静态

    add_custom_command(
  OUTPUT "${LJ_OUTPUT_LIB}"
  COMMAND
   ${CMAKE_COMMAND}
   -E
   copy_directory
   "${LJ_ORIGINAL_DIR}"
   "${LJ_BUILD_ROOT}"
   # MSVC 下需要在 src 目录运行 bat
  COMMAND ${BUILD_CMD}
  WORKING_DIRECTORY "${LJ_BUILD_SRC}"
  COMMENT "Building LuaJIT Statically (MSVC)..."
  VERBATIM
 )

elseif(MINGW OR (CMAKE_CROSSCOMPILING AND WIN32))
    # --- Windows MinGW (包含 UCRT64/Clang64) 或 Linux -> Windows 交叉编译 ---
    set(LJ_LIB_NAME "libluajit.a")
    if(MSVC)
        set(LJ_LIB_NAME "lua51.lib") # clang-cl 目标期望这个名字
    endif()
    set(LJ_OUTPUT_LIB "${LJ_BUILD_SRC}/${LJ_LIB_NAME}")

    set(MAKE_CMD make)
    if(MINGW)
        find_program(MAKE_EXE NAMES mingw32-make make)
        set(MAKE_CMD ${MAKE_EXE})
    endif()

    set(CROSS_COMPILE_ARGS "")
    if(CMAKE_CROSSCOMPILING)
        set(CROSS_COMPILE_ARGS "HOST_CC=gcc" "TARGET_SYS=Windows")
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            list(APPEND CROSS_COMPILE_ARGS "CROSS=x86_64-w64-mingw32-")
        else()
            list(APPEND CROSS_COMPILE_ARGS "CROSS=i686-w64-mingw32-")
        endif()
    endif()

    add_custom_command(
  OUTPUT "${LJ_OUTPUT_LIB}"
  COMMAND
   ${CMAKE_COMMAND}
   -E
   copy_directory
   "${LJ_ORIGINAL_DIR}"
   "${LJ_BUILD_ROOT}"
  COMMAND ${MAKE_CMD} -j${HOST_CORES} BUILDMODE=static ${LJ_G_FLAGS} ${CROSS_COMPILE_ARGS}
  WORKING_DIRECTORY "${LJ_BUILD_SRC}"
  COMMENT "Building LuaJIT for Windows (Cross/MinGW)..."
  VERBATIM
 )

else()
    # --- Linux / macOS ---
    set(LJ_LIB_NAME "libluajit.a")
    set(LJ_OUTPUT_LIB "${LJ_BUILD_SRC}/${LJ_LIB_NAME}")

    set(MAKE_ENV "")
    if(APPLE)
        # 强制写死为 11.0，这是一个安全且广泛兼容的版本
        set(MAKE_ENV "MACOSX_DEPLOYMENT_TARGET=11.0")
        message(STATUS "Forcing LuaJIT macOS Target to 11.0")
    endif()

    add_custom_command(
  OUTPUT "${LJ_OUTPUT_LIB}"
  COMMAND
   ${CMAKE_COMMAND}
   -E
   copy_directory
   "${LJ_ORIGINAL_DIR}"
   "${LJ_BUILD_ROOT}"
   # 加上了 ${LJ_G_FLAGS} 包含 -O 选项
  COMMAND ${CMAKE_COMMAND} -E env ${MAKE_ENV} make -j${HOST_CORES} amalg
   BUILDMODE=static ${LJ_G_FLAGS}
  WORKING_DIRECTORY "${LJ_BUILD_ROOT}"
  COMMENT "Building LuaJIT (${CMAKE_BUILD_TYPE}) for Unix..."
  VERBATIM
 )
endif()

# =========================================================================
# 绑定 Target
# =========================================================================

add_custom_target(luajit_build DEPENDS "${LJ_OUTPUT_LIB}")

# 定义为静态导入库
add_library(luajit STATIC IMPORTED GLOBAL)
add_library(luajit::luajit ALIAS luajit)

add_dependencies(luajit luajit_build)

set_target_properties(luajit
 PROPERTIES IMPORTED_LOCATION "${LJ_OUTPUT_LIB}"
)

# 导出头文件路径
target_include_directories(luajit INTERFACE "${LJ_BUILD_SRC}")

# 静态链接 LuaJIT 需要链接系统数学库和底层库
if(UNIX)
    target_link_libraries(luajit INTERFACE m dl)
    if(NOT APPLE)
        target_link_libraries(luajit INTERFACE pthread)
    endif()
endif()

# Windows 下静态链接 LuaJIT 有时需要 winmm (用于高精度时钟)
if(WIN32)
    target_link_libraries(luajit INTERFACE winmm)
endif()
