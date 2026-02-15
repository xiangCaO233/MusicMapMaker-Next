project(LuaJIT C)

set(LJ_BIN_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin")
if(NOT EXISTS "${LJ_BIN_OUTPUT_DIR}")
    file(MAKE_DIRECTORY "${LJ_BIN_OUTPUT_DIR}")
endif()

# =========================================================================
# 自动检测 CPU 核心数 (用于多线程编译)
# =========================================================================
cmake_host_system_information(RESULT HOST_CORES QUERY NUMBER_OF_LOGICAL_CORES)
message(STATUS "Detected ${HOST_CORES} cores for building LuaJIT")

# =========================================================================
# 路径定义
# =========================================================================
set(LJ_ORIGINAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/luajit")
set(LJ_BUILD_ROOT "${CMAKE_BINARY_DIR}/luajit")
set(LJ_BUILD_SRC "${LJ_BUILD_ROOT}/src")

# 预先创建目录，防止 Configure 阶段报错
if(NOT EXISTS "${LJ_BUILD_SRC}")
    file(MAKE_DIRECTORY "${LJ_BUILD_SRC}")
endif()

# =========================================================================
# 核心构建逻辑
# =========================================================================

if(MSVC)
    # --- Windows MSVC ---
    set(LJ_LIB_NAME "lua51.lib")
    set(LJ_DLL_NAME "lua51.dll")
    set(LJ_OUTPUT_LIB "${LJ_BUILD_SRC}/${LJ_LIB_NAME}")
    set(LJ_OUTPUT_DLL "${LJ_BUILD_SRC}/${LJ_DLL_NAME}")

    set(BUILD_CMD "${LJ_BUILD_SRC}/msvcbuild.bat")
    if(CMAKE_BUILD_TYPE MATCHES Debug)
        list(APPEND BUILD_CMD debug)
    endif()
    list(APPEND BUILD_CMD static)

    # 这里的技巧：COMMAND 列表是顺序执行的
    # 把 copy 和 build 放在同一个 command 块里
    add_custom_command(
            OUTPUT "${LJ_OUTPUT_LIB}" "${LJ_OUTPUT_DLL}" # 指定输出文件，文件存在则不重编
            # 复制源码
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${LJ_ORIGINAL_DIR}" "${LJ_BUILD_ROOT}"
            # 执行构建
            COMMAND ${BUILD_CMD}
            # 编译完成后复制 DLL 到 bin 目录
            COMMAND ${CMAKE_COMMAND} -E copy "${LJ_OUTPUT_DLL}" "${LJ_BIN_OUTPUT_DIR}/"
            WORKING_DIRECTORY "${LJ_BUILD_SRC}"
            COMMENT "Building LuaJIT (MSVC)..."
            VERBATIM
    )

elseif(MINGW)
    # --- Windows MinGW ---
    find_program(MAKE_EXE NAMES mingw32-make make)

    set(LJ_LIB_NAME "libluajit-5.1.dll.a")  # 这是链接时用的导入库
    set(LJ_DLL_NAME "lua51.dll")            # 这是运行时用的动态库

    set(LJ_OUTPUT_LIB "${LJ_BUILD_SRC}/${LJ_LIB_NAME}")
    set(LJ_OUTPUT_DLL "${LJ_BUILD_SRC}/${LJ_DLL_NAME}")

    add_custom_command(
            OUTPUT "${LJ_OUTPUT_LIB}" "${LJ_OUTPUT_DLL}"
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${LJ_ORIGINAL_DIR}" "${LJ_BUILD_ROOT}"
            COMMAND ${MAKE_EXE} -j${HOST_CORES}
            # 编译完成后复制 DLL 到 bin 目录
            COMMAND ${CMAKE_COMMAND} -E copy "${LJ_OUTPUT_DLL}" "${LJ_BIN_OUTPUT_DIR}/"
            WORKING_DIRECTORY "${LJ_BUILD_SRC}"
            COMMENT "Building LuaJIT (MinGW)..."
    )

else()
    # --- Linux / macOS ---
    set(LJ_LIB_NAME "libluajit.a")
    set(LJ_OUTPUT_LIB "${LJ_BUILD_SRC}/${LJ_LIB_NAME}")

    set(MAKE_ENV "")
    if(APPLE)
        set(MAKE_ENV "MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
    endif()

    # 注意：
    # 'make amalg' 把所有代码合并成一个文件编译，优化好但无法并行。
    # 如果更在意编译速度，去掉 'amalg' 改用标准 'make -j'。
    # 这里保留 amalg 但给它加上 -j (用于并行构建 minilua 等工具)

    add_custom_command(
            OUTPUT "${LJ_OUTPUT_LIB}" # 告诉 CMake 只有这个文件不存在(或脏了)才运行
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${LJ_ORIGINAL_DIR}" "${LJ_BUILD_ROOT}"
            # 加上 -j 参数启用多核编译,XCFLAGS=-fPIC确保动态链接静态库
            COMMAND ${CMAKE_COMMAND} -E env ${MAKE_ENV} make -j${HOST_CORES} amalg XCFLAGS=-fPIC
            WORKING_DIRECTORY "${LJ_BUILD_ROOT}"
            COMMENT "Building LuaJIT (Linux/Make) with -fPIC..."
    )

endif()

# =========================================================================
# 绑定 Target
# =========================================================================

# 创建一个 Target 绑定上面的 Command
add_custom_target(luajit_build DEPENDS "${LJ_OUTPUT_LIB}")

add_library(luajit STATIC IMPORTED GLOBAL)
add_library(luajit::luajit ALIAS luajit)

# 确保 luajit 库依赖于构建 target
add_dependencies(luajit luajit_build)

set_target_properties(luajit PROPERTIES
        IMPORTED_LOCATION "${LJ_OUTPUT_LIB}"
)

target_include_directories(luajit INTERFACE
        "${LJ_BUILD_SRC}"
)

if(UNIX AND NOT APPLE)
    set_target_properties(luajit PROPERTIES
            INTERFACE_LINK_LIBRARIES "dl;m"
    )
endif()
