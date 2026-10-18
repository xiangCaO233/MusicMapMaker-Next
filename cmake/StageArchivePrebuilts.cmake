# * 将归档解码依赖安装树按现有预编译布局分包。 本脚本只复制已构建产物，不编译、不降级到系统库，也不 strip 调试信息。
cmake_minimum_required(VERSION 3.31)
# * 平台坐标必须由构建调用方提供，不能根据宿主猜测交叉编译目标。
# * 缺少坐标直接失败，避免误写另一工具链的预编译目录。
foreach(_required BUILD_DIR PLATFORM ARCH TOOLCHAIN COMPILER_TAG CONFIG)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "缺少暂存参数 ${_required}")
  endif()
endforeach()
# * 仓库位置来自脚本自身，允许构建目录位于源码目录以外。
get_filename_component(_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
# * 只读取 wrapper 的安装树，不搜寻系统库或任意中间对象。
set(_install "${BUILD_DIR}/3rdpty/archive_inst")
# * 调用方可按包增量暂存，其他包缺失不影响本批请求。
string(REPLACE "," ";" _filter "${PACKAGES}")
# * 与项目默认静态偏好一致；共享构建调用方必须明确传入 shared。
if(NOT DEFINED LINKAGE)
  set(LINKAGE static)
endif()
# * shared 文件和静态文件分目录，防止同名导入库覆盖静态归档。
set(_suffix "${ARCH}/${TOOLCHAIN}/${COMPILER_TAG}")
if(LINKAGE STREQUAL "shared")
  string(APPEND _suffix "/shared")
elseif(NOT LINKAGE STREQUAL "static")
  message(FATAL_ERROR "LINKAGE 必须是 static 或 shared")
endif()
# * Debug 和发布配置保留独立文件，不能用文件名后缀代替目录隔离。
string(APPEND _suffix "/${CONFIG}")
# * 包集合与 libarchive wrapper、具体 Find 模块一一对应。
foreach(_package libarchive archive_zlib lzma bzip2 zstd)
  # * 增量请求不删除未选包，支持只补齐新增依赖。
  if(_filter AND NOT _package IN_LIST _filter)
    continue()
  endif()
  # * 独立包只携带自己的头，避免库之间通过无关传递包含偶然编译成功。
  if(_package STREQUAL "libarchive")
    # * 主库保留上游文件名，编解码器保留各自标准库名。
    set(_stem archive)
    set(_headers archive.h archive_entry.h)
  elseif(_package STREQUAL "archive_zlib")
    set(_stem z)
    # * zconf.h 含当前构建生成配置，必须与公开 zlib.h 一起安装。
    set(_headers zlib.h zconf.h)
  elseif(_package STREQUAL "lzma")
    set(_stem lzma)
    # * lzma.h 依赖整个 lzma 子目录，不能只复制顶层入口头。
    set(_headers lzma.h lzma)
  elseif(_package STREQUAL "bzip2")
    set(_stem bz2)
    set(_headers bzlib.h)
  else()
    set(_stem zstd)
    # * 同时保留错误码和字典接口，公开头包与上游安装集合保持一致。
    set(_headers zstd.h zstd_errors.h zdict.h)
  endif()
  # * 先验证输入，不能把残缺包复制成表面成功的预编译目录。
  foreach(_header IN LISTS _headers)
    if(NOT EXISTS "${_install}/include/${_header}")
      message(FATAL_ERROR "归档依赖缺少头文件：${_header}")
    endif()
    # * 共享头包按包隔离，不将安装前缀中的无关头文件混入。
    file(COPY "${_install}/include/${_header}"
         DESTINATION "${_root}/3rdpty/prebuilts/headers/${_package}/include")
  endforeach()
  # * 每个库独立分包，具体 Find 模块负责按相同坐标定位。
  set(_destination "${_root}/3rdpty/prebuilts/binaries/${PLATFORM}/${_package}")
  # * Windows shared 区分导入库与 DLL；POSIX 保留 soname 链接及版本实体。
  if(PLATFORM STREQUAL "windows" AND TOOLCHAIN STREQUAL "msvc")
    # * MSVC 静态库与 DLL 导入库均为 .lib，由 linkage 子目录区分。
    set(_pattern "${_stem}.lib")
  elseif(PLATFORM STREQUAL "windows" AND LINKAGE STREQUAL "shared")
    # * MinGW 共享链接消费导入归档，不把 DLL 当作链接输入。
    set(_pattern "lib${_stem}.dll.a")
    # * GNU 风格静态归档使用统一前缀，Windows MSVC 已由前一分支处理。
  elseif(LINKAGE STREQUAL "static")
    set(_pattern "lib${_stem}.a")
  elseif(PLATFORM STREQUAL "macos")
    # * macOS 同时保留未版本化链接名及真实版本库。
    set(_pattern "lib${_stem}*.dylib")
  else()
    # * Linux 的 soname 链与版本实体必须一并存在。
    set(_pattern "lib${_stem}.so*")
  endif()
  # * glob 仅限已知库名前缀；零匹配属于不完整构建，不能跳过。
  file(GLOB _libraries "${_install}/lib/${_pattern}")
  if(NOT _libraries)
    message(FATAL_ERROR "归档依赖缺少库：${_install}/lib/${_pattern}")
  endif()
  # * CMake COPY 保留符号链接，不对库执行裁剪或重新链接。
  file(COPY ${_libraries} DESTINATION "${_destination}/libs/${_suffix}")
  if(PLATFORM STREQUAL "windows" AND LINKAGE STREQUAL "shared")
    # * Windows 应用部署从 bin 读取运行时，只有导入库不能运行。
    file(GLOB _runtime "${_install}/bin/*${_stem}*.dll")
    if(NOT _runtime)
      message(FATAL_ERROR "归档依赖缺少 DLL：${_stem}")
    endif()
    # * DLL 进入相同 ABI 和配置坐标，不能借用其他配置的运行时。
    file(COPY ${_runtime} DESTINATION "${_destination}/bin/${_suffix}")
  endif()
  # macOS 共享库链接后只保留调试映射，需要在构建对象仍存在时生成可搬移的 dSYM。 静态归档的 DWARF 已位于对象中，不额外复制整棵构建目录。
  if(PLATFORM STREQUAL "macos" AND LINKAGE STREQUAL "shared")
    # 使用原生 Mac Runner 的工具解析链接调试映射，不以开发机 LLVM 版本替代。
    # 缺少工具直接失败，不能发布依赖本机对象路径才能调试的共享库。
    find_program(_dsymutil NAMES dsymutil REQUIRED)
    # 符号包采用与 dylib 相同的平台、架构、编译器、链接类型和配置坐标。 bundle 内含 UUID 与 DWARF，整体保留以供离线符号化。
    file(MAKE_DIRECTORY "${_destination}/symbols/${_suffix}")
    foreach(_library IN LISTS _libraries)
      # 版本化符号链接共享同一实体，只为真实 dylib 生成一次符号包。
      if(IS_SYMLINK "${_library}")
        continue()
      endif()
      get_filename_component(_library_name "${_library}" NAME)
      # 必须读取安装树中的真实库，不能针对链接名猜测版本或相邻构建目录。 不执行 strip，生成符号包不会改变原 dylib 的 UUID
      # 和导出符号。
      execute_process(
        COMMAND "${_dsymutil}" "${_library}" -o
                "${_destination}/symbols/${_suffix}/${_library_name}.dSYM"
        RESULT_VARIABLE _symbol_result)
      # 任一真实库符号提取失败都阻止本次暂存成功，避免漏包却报告完成。
      if(NOT _symbol_result EQUAL 0)
        message(FATAL_ERROR "归档依赖 dSYM 生成失败：${_library_name}")
      endif()
    endforeach()
  endif()
  # * /Z7 静态库的符号在归档内；共享库的链接 PDB 仍须收集。
  # * 只搜索这两个专用构建树，避免收集应用或其他库的 PDB。
  file(GLOB_RECURSE _symbols "${BUILD_DIR}/3rdpty/archive_bld/*${_stem}*.pdb"
       "${BUILD_DIR}/3rdpty/archive_codecs_bld/*${_stem}*.pdb")
  # * 无旁路 PDB 的静态归档可合法通过；内嵌调试段随库原样保留。
  if(_symbols)
    file(COPY ${_symbols} DESTINATION "${_destination}/symbols/${_suffix}")
  endif()
  # * 成功日志列出实际写入坐标，便于检查交叉构建没有混用宿主标签。
  message(STATUS "已暂存 ${_package}: ${PLATFORM}/${_suffix}")
endforeach()
