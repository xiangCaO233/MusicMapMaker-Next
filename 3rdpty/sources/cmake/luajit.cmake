# LuaJIT 使用自己的 Makefile 或批处理入口，不能直接当作 CMake 源码库。
# 本包装只维护构建命令和消费接口，不改写受版本控制的上游实现。
# Android JNI 入口需要 PIC 静态对象，不能链接宿主解释器或桌面归档。
project(LuaJIT C)

# 输出路径均跟随顶层二进制目录；Android 脚本将该目录放在仓库外。
# bin 只用于运行时文件，源码副本和宿主生成工具另放 luajit 子目录。
set(LJ_BIN_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin")
if(NOT EXISTS "${LJ_BIN_OUTPUT_DIR}")
  file(MAKE_DIRECTORY "${LJ_BIN_OUTPUT_DIR}")
endif()

# =========================================================================
# 自动检测 CPU 核心数
# =========================================================================
# LuaJIT 内层 make 根据宿主核数并行，宿主信息不用于选择目标 ABI。
# 交叉编译的 CPU 由下方目标编译器与 triple 决定。
cmake_host_system_information(RESULT HOST_CORES QUERY NUMBER_OF_LOGICAL_CORES)

# =========================================================================
# 路径定义
# =========================================================================
set(LJ_ORIGINAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/luajit")
set(LJ_BUILD_ROOT "${CMAKE_BINARY_DIR}/luajit")
set(LJ_BUILD_SRC "${LJ_BUILD_ROOT}/src")
set(LJ_BUILD_STAMP "${LJ_BUILD_ROOT}/luajit_build.stamp")

# 把上游文件和本包装都列为输入，配置或源码变化后重建成功戳。
# 排除 Git 管理信息，避免仅切换引用就把内部数据库当作编译输入。
# CONFIGURE_DEPENDS 负责发现新增文件，生成物不会写回输入目录。
file(GLOB_RECURSE LJ_SOURCE_INPUTS CONFIGURE_DEPENDS "${LJ_ORIGINAL_DIR}/*")
list(FILTER LJ_SOURCE_INPUTS EXCLUDE REGEX "/\\.git(/|$)")
list(APPEND LJ_SOURCE_INPUTS "${CMAKE_CURRENT_LIST_FILE}")
list(APPEND LJ_SOURCE_INPUTS
     "${CMAKE_CURRENT_LIST_DIR}/PatchLuaJITMsvcRuntime.cmake")
# 复制后的副本可由上游构建工具生成头文件和目标文件。
# 只同步变化文件，重复构建不因复制时间戳无条件触发所有目标。
set(LJ_SYNC_SOURCE_COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
                           "${LJ_ORIGINAL_DIR}" "${LJ_BUILD_ROOT}")

if(NOT EXISTS "${LJ_BUILD_SRC}")
  file(MAKE_DIRECTORY "${LJ_BUILD_SRC}")
endif()

# 核心构建逻辑 - 统一为静态库 统一处理 GCC/Clang 族的编译标志 (MinGW & Unix) 原生 MSVC 之后使用 bat，Android 必须使用宿主可执行的 make。
# 发现失败应停在配置阶段，不把缺少工具误报为目标编译器不可用。
find_program(MAKE_CMD NAMES gmake make REQUIRED)

# 构建配置转换成上游理解的参数，不能把 CMake 配置名称直接传给 make。
# MSVC 交叉包装器接收 Windows CRT 参数，Unix 分支接收 GCC 风格选项。
# 业务 PGO 参数不传播到 LuaJIT，保证第三方归档可独立复用。
if(NOT MSVC OR CMAKE_CROSSCOMPILING)
  if(MSVC)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
      set(LJ_G_FLAGS "TARGET_CFLAGS=/MTd /Z7 /Od -w")
    elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
      set(LJ_G_FLAGS "TARGET_CFLAGS=/MT /Z7 /O2 -w")
    else()
      set(LJ_G_FLAGS "TARGET_CFLAGS=/MT /O2 -w")
    endif()
  elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
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

# Android 分支必须先于通用 Unix 分支匹配，避免省略目标 triple。
# 同一 NDK clang 可生成不同架构，单独传编译器路径不足以确定目标。
if(ANDROID)
  # LuaJIT 的生成器在宿主运行，最终静态库由 NDK 编译；二者不能共用 CC。
  set(LJ_LIB_NAME "libluajit.a")
  set(LJ_OUTPUT_LIB "${LJ_BUILD_SRC}/${LJ_LIB_NAME}")
  # CROSS 选择配套 NDK 工具目录；STATIC_CC 和 TARGET_LD 明确携带 API 等级。
  # HOST_CC 只生成本机运行的构建辅助程序，不能替换为 Android 编译器。
  get_filename_component(_lj_ndk_bin "${CMAKE_C_COMPILER}" DIRECTORY)
  set(_lj_target_cc
      "${CMAKE_C_COMPILER} --target=${CMAKE_C_COMPILER_TARGET} --sysroot=${CMAKE_SYSROOT}"
  )
  # 成功戳只在 make 完成后写入，失败不会被下一轮误认为已构建。
  # BYPRODUCTS 将实际归档纳入 Ninja 文件依赖，保证消费者等待该步骤。
  # VERBATIM 保留带空格的编译器参数串作为单个 make 变量赋值。
  # NDK 的 clang 路径本身不携带 API 等级，编译和链接两条命令都要保留 triple。
  # 静态归档不运行手机代码，HOST_CC 生成的工具则必须可在开发机执行。
  # TARGET_AR 使用同一 NDK 的 LLVM 归档器，避免混入宿主归档索引格式。
  # Android 的 TARGET_SYS 采用上游约定的 Linux 值，不代表使用桌面 libc。
  # DYNAMIC_CC 也显式提供 PIC 参数，防止上游内部探针落回默认宿主驱动。
  # 本分支不生成独立 LuaJIT CLI，APK 只消费静态库并入原生业务共享入口。
  add_custom_command(
    OUTPUT "${LJ_BUILD_STAMP}"
    BYPRODUCTS "${LJ_OUTPUT_LIB}"
    COMMAND ${LJ_SYNC_SOURCE_COMMAND}
    COMMAND
      ${MAKE_CMD} -j${HOST_CORES} BUILDMODE=static ${LJ_G_FLAGS} "HOST_CC=cc"
      "CROSS=${_lj_ndk_bin}/" "STATIC_CC=${_lj_target_cc}"
      "DYNAMIC_CC=${_lj_target_cc} -fPIC" "TARGET_LD=${_lj_target_cc}"
      "TARGET_AR=${CMAKE_AR} rcus" "TARGET_STRIP=${_lj_ndk_bin}/llvm-strip"
      TARGET_SYS=Linux
    COMMAND ${CMAKE_COMMAND} -E touch "${LJ_BUILD_STAMP}"
    DEPENDS ${LJ_SOURCE_INPUTS}
    WORKING_DIRECTORY "${LJ_BUILD_ROOT}"
    VERBATIM)
  # 原生 Windows 使用上游批处理以保持原有 ABI 和导入库命名。
  # Linux 到 MSVC 的构建不能执行 bat，后面使用专用参数翻译包装器。
elseif(MSVC AND NOT CMAKE_CROSSCOMPILING)
  # Windows MSVC 下不传 static 参数会生成 lua51.dll 与导入库，传 static 才生成静态库。
  set(LJ_LIB_NAME "lua51.lib")
  set(LJ_OUTPUT_LIB "${LJ_BUILD_SRC}/${LJ_LIB_NAME}")
  set(LJ_OUTPUT_DLL "")
  set(LJ_OUTPUT_PDB "")
  # shared 依赖偏好构建 lua51.dll，运行库必须跟随主项目使用 /MD(d)。
  # 主程序与依赖的 CRT 选择必须一致，避免跨模块内存释放使用不同运行库。
  # 运行库偏好与是否生成 DLL 分开处理，不能只根据 Debug 字符串推断。
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
  # 编译与链接参数分别传给副本中的批处理补丁，保留上游源码树原样。
  # 发布含符号配置的 PDB 需与 DLL 一起列入构建产物。
  set(LJ_MSVC_COMPILE_FLAGS "${LJ_MSVC_RUNTIME_FLAG}")
  set(LJ_MSVC_LINK_FLAGS "")
  if(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    # LuaJIT 的 bat 构建不会继承 CMake RelWithDebInfo 调试参数，这里显式保留 PDB。
    string(APPEND LJ_MSVC_COMPILE_FLAGS " /Zi")
    set(LJ_MSVC_LINK_FLAGS "/DEBUG /OPT:REF /OPT:ICF")
  endif()

  # 构建模式参数按上游批处理约定组织，不能使用 GNU make 的 BUILDMODE。
  # Debug 参数控制上游优化和断言，static 参数控制归档输出。
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

  # Windows 目标可能来自原生 MinGW 或 Linux 交叉工具链。
  # 这些分支共享静态归档命名，但不能共享宿主工具或目标链接器假设。
elseif(MINGW OR (CMAKE_CROSSCOMPILING AND WIN32))
  # --- Windows MinGW (包含 UCRT64/Clang64) 或 Linux -> Windows 交叉编译 ---
  set(LJ_LIB_NAME "libluajit.a")
  set(LJ_OUTPUT_LIB "${LJ_BUILD_SRC}/${LJ_LIB_NAME}")
  set(LJ_MAKE_TARGET "")

  # 原生 MinGW 优先选择对应 make；Linux 交叉分支仍执行宿主 make。
  # 工具选择只影响构建调度，不参与目标库的 ABI 判定。
  set(MAKE_CMD make)
  if(MINGW)
    find_program(MAKE_EXE NAMES mingw32-make make)
    set(MAKE_CMD ${MAKE_EXE})
  endif()

  # 空列表保留原生 Windows 工具默认行为，只有交叉构建追加主目标分离参数。
  # TARGET_SYS 描述产物平台，不能按构建宿主的 UNIX 条件推导。
  set(CROSS_COMPILE_ARGS "")
  if(CMAKE_CROSSCOMPILING)
    set(CROSS_COMPILE_ARGS "HOST_CC=gcc" "TARGET_SYS=Windows")
    if(MSVC)
      # LuaJIT 的 Makefile 负责构建可在 Linux 运行的 host 工具；目标对象必须通过 clang-cl 生成，并由 llvm-lib 归档为 MSVC ABI 的 COFF 静态库。
      # 两个包装器必须配套使用，编译参数和归档参数的语法同时转换。
      # 包装器位置固定在项目维护目录，不写入第三方源码。
      set(LJ_CLANG_CL_WRAPPER
          "${CMAKE_SOURCE_DIR}/scripts/ci/cross/clang-cl-gcc-compatible.sh")
      set(LJ_LLVM_LIB_WRAPPER
          "${CMAKE_SOURCE_DIR}/scripts/ci/cross/llvm-lib-ar-compatible.sh")
      set(LJ_TARGET_FLAGS "${CMAKE_C_FLAGS}")
      # MSVC 预编译阶段只需要静态库；目标可执行文件不能在 Linux 宿主运行， 也不应把 GCC 风格链接参数直接传给 lld-link。
      set(LJ_MAKE_TARGET "libluajit.a")
      list(
        APPEND
        CROSS_COMPILE_ARGS
        "STATIC_CC=${LJ_CLANG_CL_WRAPPER}"
        "DYNAMIC_CC=${LJ_CLANG_CL_WRAPPER}"
        "TARGET_LD=${CMAKE_LINKER}"
        "TARGET_AR=${LJ_LLVM_LIB_WRAPPER}"
        "TARGET_STRIP=:"
        "TARGET_FLAGS=${LJ_TARGET_FLAGS}")
    elseif(MINGW AND CMAKE_C_COMPILER_ID MATCHES "Clang")
      # clang64 预编译包必须使用当前 CMake 工具链的 clang 目标参数，不能退回 x86_64-w64-mingw32-gcc 前缀。
      set(LJ_TARGET_FLAGS "")
      if(CMAKE_C_COMPILER_TARGET)
        string(APPEND LJ_TARGET_FLAGS " --target=${CMAKE_C_COMPILER_TARGET}")
      endif()
      if(CMAKE_SYSROOT)
        string(APPEND LJ_TARGET_FLAGS " --sysroot=${CMAKE_SYSROOT}")
      endif()
      string(STRIP "${LJ_TARGET_FLAGS}" LJ_TARGET_FLAGS)
      # 链接探针同样必须使用目标 sysroot；编译成功不代表宿主链接器可用。
      # 保留原有链接器选项，避免丢失 Windows 工具链所需的库搜索目录。
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
      # 工具链显式前缀优先于 triple，最后才按指针宽度选择历史默认前缀。
      # 这里仅处理 Windows GCC 族，不把该默认值用于 Android。
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
            ${CROSS_COMPILE_ARGS} ${LJ_MAKE_TARGET}
    COMMAND ${CMAKE_COMMAND} -E touch "${LJ_BUILD_STAMP}"
    DEPENDS ${LJ_SOURCE_INPUTS}
    WORKING_DIRECTORY "${LJ_BUILD_SRC}"
    COMMENT "Building LuaJIT for Windows (Cross/MinGW)..."
    VERBATIM)

else()
  # --- Linux / macOS ---
  set(LJ_LIB_NAME "libluajit.a")
  set(LJ_OUTPUT_LIB "${LJ_BUILD_SRC}/${LJ_LIB_NAME}")

  # 原生 Unix 分支的部署下限沿用既有 macOS 策略，本次 Android 不修改它。
  # 环境仅传给本次构建子进程，不改变用户终端的 SDK 或部署变量。
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

# 导入 target 自身不执行编译，必须显式连接到成功戳的构建目标。
# 依赖边同时保证复制后的公共头在消费目标编译前已经生成。
# 成功戳是命令级增量边界，不替代 Ninja 对实际归档 BYPRODUCTS 的依赖。
# 源码、副本和最终库在同一构建树对应固定位置，不跨配置复用目标文件。
# 清理该构建树会同时移除宿主工具和目标归档，下次构建按相同参数重建。
# 本包装不生成上游补丁文件，也不将源码构建结果写入预编译发布目录。
# 安装树的发布和 LFS 提交属于独立工作流，本脚本不执行提交或上传。
# Debug 保留对象符号，RelWithDebInfo 同样显式传递调试信息参数。
# 运行时依赖在末尾按目标平台传播，不允许靠链接顺序碰巧解析宿主符号。
# Android 与原生 Unix 分支共享接口别名，不共享最终二进制。
# 消费者不能绕过 luajit_build 直接打开未准备完成的副本头文件。
# target 的全局可见性只解决 CMake 作用域，不改变静态库生命周期。
# 更新工具链后应使用对应的独立构建树，成功戳不负责识别编译器文件内容变化。
add_custom_target(luajit_build DEPENDS "${LJ_BUILD_STAMP}")

# 定义为静态导入库 默认导入静态归档；只有原生 MSVC shared 模式提供真实运行时 DLL。
# 交叉 Windows 分支仍是静态包，不能凭 PROJECT_LINKAGE 把 .a 当作 DLL。
set(LUAJIT_IMPORTED_TYPE STATIC)
if(MSVC
   AND PROJECT_LINKAGE STREQUAL "shared"
   AND NOT CMAKE_CROSSCOMPILING)
  set(LUAJIT_IMPORTED_TYPE SHARED)
endif()
add_library(luajit ${LUAJIT_IMPORTED_TYPE} IMPORTED GLOBAL)
add_library(luajit::luajit ALIAS luajit)

add_dependencies(luajit luajit_build)

# IMPORTED_LOCATION 指向实际产物，不通过 find_library 搜索系统安装。
# 全局别名使业务和 sol2 的依赖目标不需要了解源码构建细节。
set_target_properties(luajit PROPERTIES IMPORTED_LOCATION "${LJ_OUTPUT_LIB}")
if(MSVC
   AND PROJECT_LINKAGE STREQUAL "shared"
   AND NOT CMAKE_CROSSCOMPILING)
  set_target_properties(luajit PROPERTIES IMPORTED_IMPLIB "${LJ_OUTPUT_LIB}"
                                          IMPORTED_LOCATION "${LJ_OUTPUT_DLL}")
  # DLL 必须与主程序放在同一运行目录，复制仅发生在构建成功之后。
  # copy_if_different 避免重复构建造成无内容变化的时间戳更新。
  add_custom_command(
    TARGET luajit_build
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LJ_OUTPUT_DLL}"
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/lua51.dll"
    COMMENT "Copying LuaJIT DLL to runtime output directory"
    VERBATIM)
endif()

# 导出头文件路径 消费的是与本次归档一起准备的头，不能混用系统 Lua 或其他 LuaJIT 版本。
# 头目录属于构建树，清理该树不会影响版本控制中的第三方输入。
target_include_directories(luajit INTERFACE "${LJ_BUILD_SRC}")

# 静态链接 LuaJIT 需要链接系统数学库和底层库 静态包需要最终消费者补全系统符号，线程库名称必须按目标平台区分。
# Android 的 Unix 条件也为真，因此必须在此显式排除独立 pthread 库名。
if(UNIX)
  target_link_libraries(luajit INTERFACE m dl)
  # Android 的 libc 已导出线程接口，不存在独立 libpthread。
  if(NOT APPLE AND NOT ANDROID)
    target_link_libraries(luajit INTERFACE pthread)
  endif()
endif()

# Windows 下静态链接 LuaJIT 有时需要 winmm (用于高精度时钟) Windows 的时钟依赖保留原有接口传播，不能套用 Android 的数学库集合。
# 该链接要求只附着于 LuaJIT 消费目标，不改变其他第三方库。
if(WIN32)
  target_link_libraries(luajit INTERFACE winmm)
endif()
