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
set(LJ_BUILD_STAMP "${LJ_BUILD_ROOT}/luajit_build.stamp")

file(GLOB_RECURSE LJ_SOURCE_INPUTS CONFIGURE_DEPENDS "${LJ_ORIGINAL_DIR}/*")
list(FILTER LJ_SOURCE_INPUTS EXCLUDE REGEX "/\\.git(/|$)")
list(APPEND LJ_SOURCE_INPUTS "${CMAKE_CURRENT_LIST_FILE}")
list(APPEND LJ_SOURCE_INPUTS
     "${CMAKE_CURRENT_LIST_DIR}/PatchLuaJITMsvcRuntime.cmake")
set(LJ_SYNC_SOURCE_COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
                           "${LJ_ORIGINAL_DIR}" "${LJ_BUILD_ROOT}")

if(NOT EXISTS "${LJ_BUILD_SRC}")
  file(MAKE_DIRECTORY "${LJ_BUILD_SRC}")
endif()

# 核心构建逻辑 - 统一为静态库 统一处理 GCC/Clang 族的编译标志 (MinGW & Unix)
if(NOT MSVC OR CMAKE_CROSSCOMPILING)
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    # 注意：这里直接写字符串，不要在里面加 \"
    set(LJ_G_FLAGS "XCFLAGS=-O0 -fPIC" "CCDEBUG=-g")
  elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    # LuaJIT 的 Makefile 不理解 CMake 配置名，RelWithDebInfo 需要显式传入 -g。
    set(LJ_G_FLAGS "XCFLAGS=-O2 -g -fPIC" "CCDEBUG=-g")
  else()
    # 注意：这里直接写字符串，不要在里面加 \"
    set(LJ_G_FLAGS "XCFLAGS=-O3 -fPIC")
  endif()
endif()

if(MSVC AND NOT CMAKE_CROSSCOMPILING)
  # Windows MSVC 下不传 static 参数会生成 lua51.dll 与导入库，传 static 才生成静态库。
  set(LJ_LIB_NAME "lua51.lib")
  set(LJ_OUTPUT_LIB "${LJ_BUILD_SRC}/${LJ_LIB_NAME}")
  set(LJ_OUTPUT_DLL "")
  set(LJ_OUTPUT_PDB "")
  # shared 依赖偏好构建 lua51.dll，运行库必须跟随主项目使用 /MD(d)。
  set(LJ_USE_DLL_CRT OFF)
  if(PROJECT_LINKAGE STREQUAL "shared" OR CMAKE_MSVC_RUNTIME_LIBRARY MATCHES
                                          "DLL")
    set(LJ_USE_DLL_CRT ON)
  endif()
  if(PROJECT_LINKAGE STREQUAL "shared")
    set(LJ_OUTPUT_DLL "${LJ_BUILD_SRC}/lua51.dll")
    set(LJ_OUTPUT_PDB "${LJ_BUILD_SRC}/lua51.pdb")
  endif()
  if(CMAKE_BUILD_TYPE MATCHES Debug)
    if(LJ_USE_DLL_CRT)
      set(LJ_MSVC_RUNTIME_FLAG "/MDd")
    else()
      set(LJ_MSVC_RUNTIME_FLAG "/MTd")
    endif()
  else()
    if(LJ_USE_DLL_CRT)
      set(LJ_MSVC_RUNTIME_FLAG "/MD")
    else()
      set(LJ_MSVC_RUNTIME_FLAG "/MT")
    endif()
  endif()
  set(LJ_MSVC_COMPILE_FLAGS "${LJ_MSVC_RUNTIME_FLAG}")
  set(LJ_MSVC_LINK_FLAGS "")
  if(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    # LuaJIT 的 bat 构建不会继承 CMake RelWithDebInfo 调试参数，这里显式保留 PDB。
    string(APPEND LJ_MSVC_COMPILE_FLAGS " /Zi")
    set(LJ_MSVC_LINK_FLAGS "/DEBUG /OPT:REF /OPT:ICF")
  endif()

  set(BUILD_CMD "msvcbuild.bat")
  if(CMAKE_BUILD_TYPE MATCHES Debug)
    list(APPEND BUILD_CMD debug)
  endif()
  if(NOT PROJECT_LINKAGE STREQUAL "shared")
    list(APPEND BUILD_CMD static)
  endif()

  add_custom_command(
    OUTPUT "${LJ_BUILD_STAMP}"
    BYPRODUCTS "${LJ_OUTPUT_LIB}" ${LJ_OUTPUT_DLL} ${LJ_OUTPUT_PDB}
    COMMAND ${LJ_SYNC_SOURCE_COMMAND}
    COMMAND
      ${CMAKE_COMMAND} -DLUAJIT_MSVCBUILD=${LJ_BUILD_SRC}/msvcbuild.bat
      "-DLUAJIT_MSVC_COMPILE_FLAGS=${LJ_MSVC_COMPILE_FLAGS}"
      "-DLUAJIT_MSVC_LINK_FLAGS=${LJ_MSVC_LINK_FLAGS}" -P
      ${CMAKE_CURRENT_LIST_DIR}/PatchLuaJITMsvcRuntime.cmake
      # MSVC 下需要在 src 目录运行 bat
    COMMAND ${BUILD_CMD}
    COMMAND ${CMAKE_COMMAND} -E touch "${LJ_BUILD_STAMP}"
    DEPENDS ${LJ_SOURCE_INPUTS}
    WORKING_DIRECTORY "${LJ_BUILD_SRC}"
    COMMENT "Building LuaJIT (MSVC)..."
    VERBATIM)

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
    if(MINGW AND CMAKE_C_COMPILER_ID MATCHES "Clang")
      # clang64 预编译包必须使用当前 CMake 工具链的 clang 目标参数，不能退回 x86_64-w64-mingw32-gcc 前缀。
      set(LJ_TARGET_FLAGS "")
      if(CMAKE_C_COMPILER_TARGET)
        string(APPEND LJ_TARGET_FLAGS " --target=${CMAKE_C_COMPILER_TARGET}")
      endif()
      if(CMAKE_SYSROOT)
        string(APPEND LJ_TARGET_FLAGS " --sysroot=${CMAKE_SYSROOT}")
      endif()
      string(STRIP "${LJ_TARGET_FLAGS}" LJ_TARGET_FLAGS)
      set(LJ_TARGET_LINK_FLAGS "${LJ_TARGET_FLAGS} ${CMAKE_EXE_LINKER_FLAGS}")
      string(STRIP "${LJ_TARGET_LINK_FLAGS}" LJ_TARGET_LINK_FLAGS)
      list(
        APPEND
        CROSS_COMPILE_ARGS
        "TARGET_CC=${CMAKE_C_COMPILER} ${LJ_TARGET_FLAGS}"
        "TARGET_STCC=${CMAKE_C_COMPILER} ${LJ_TARGET_FLAGS}"
        "TARGET_LD=${CMAKE_C_COMPILER} ${LJ_TARGET_LINK_FLAGS}"
        "TARGET_AR=${CMAKE_AR} rcus"
        "TARGET_STRIP=${CMAKE_STRIP}")
    else()
      if(DEFINED MINGW_TOOLCHAIN_PREFIX AND NOT MINGW_TOOLCHAIN_PREFIX STREQUAL
                                            "")
        # GCC MinGW 的 UCRT64 与 win32 线程模型使用不同前缀，LuaJIT 必须继承当前工具链。
        list(APPEND CROSS_COMPILE_ARGS "CROSS=${MINGW_TOOLCHAIN_PREFIX}-")
      elseif(CMAKE_C_COMPILER_TARGET)
        list(APPEND CROSS_COMPILE_ARGS "CROSS=${CMAKE_C_COMPILER_TARGET}-")
      elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
        list(APPEND CROSS_COMPILE_ARGS "CROSS=x86_64-w64-mingw32-")
      else()
        list(APPEND CROSS_COMPILE_ARGS "CROSS=i686-w64-mingw32-")
      endif()
    endif()
  endif()

  add_custom_command(
    OUTPUT "${LJ_BUILD_STAMP}"
    BYPRODUCTS "${LJ_OUTPUT_LIB}"
    COMMAND ${LJ_SYNC_SOURCE_COMMAND}
    COMMAND ${MAKE_CMD} -j${HOST_CORES} BUILDMODE=static ${LJ_G_FLAGS}
            ${CROSS_COMPILE_ARGS}
    COMMAND ${CMAKE_COMMAND} -E touch "${LJ_BUILD_STAMP}"
    DEPENDS ${LJ_SOURCE_INPUTS}
    WORKING_DIRECTORY "${LJ_BUILD_SRC}"
    COMMENT "Building LuaJIT for Windows (Cross/MinGW)..."
    VERBATIM)

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
    OUTPUT "${LJ_BUILD_STAMP}"
    BYPRODUCTS "${LJ_OUTPUT_LIB}"
    COMMAND ${LJ_SYNC_SOURCE_COMMAND}
            # 加上了 ${LJ_G_FLAGS} 包含 -O 选项
    COMMAND ${CMAKE_COMMAND} -E env ${MAKE_ENV} make -j${HOST_CORES} amalg
            BUILDMODE=static ${LJ_G_FLAGS}
    COMMAND ${CMAKE_COMMAND} -E touch "${LJ_BUILD_STAMP}"
    DEPENDS ${LJ_SOURCE_INPUTS}
    WORKING_DIRECTORY "${LJ_BUILD_ROOT}"
    COMMENT "Building LuaJIT (${CMAKE_BUILD_TYPE}) for Unix..."
    VERBATIM)
endif()

# =========================================================================
# 绑定 Target
# =========================================================================

add_custom_target(luajit_build DEPENDS "${LJ_BUILD_STAMP}")

# 定义为静态导入库
set(LUAJIT_IMPORTED_TYPE STATIC)
if(MSVC
   AND PROJECT_LINKAGE STREQUAL "shared"
   AND NOT CMAKE_CROSSCOMPILING)
  set(LUAJIT_IMPORTED_TYPE SHARED)
endif()
add_library(luajit ${LUAJIT_IMPORTED_TYPE} IMPORTED GLOBAL)
add_library(luajit::luajit ALIAS luajit)

add_dependencies(luajit luajit_build)

set_target_properties(luajit PROPERTIES IMPORTED_LOCATION "${LJ_OUTPUT_LIB}")
if(MSVC
   AND PROJECT_LINKAGE STREQUAL "shared"
   AND NOT CMAKE_CROSSCOMPILING)
  set_target_properties(luajit PROPERTIES IMPORTED_IMPLIB "${LJ_OUTPUT_LIB}"
                                          IMPORTED_LOCATION "${LJ_OUTPUT_DLL}")
  add_custom_command(
    TARGET luajit_build
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LJ_OUTPUT_DLL}"
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/lua51.dll"
    COMMENT "Copying LuaJIT DLL to runtime output directory"
    VERBATIM)
endif()

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
