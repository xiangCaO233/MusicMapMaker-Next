# * 内置归档依赖的源码入口；预编译模式不得包含本文件。 外部项目隔离通用上游缓存，并确保解码库安装完毕后才运行 libarchive 探针。
# * 外部构建的依赖关系由 Ninja 管理，不在主项目配置期同步执行编译。
# * 源码均来自 gitlink 固定版本，更新动作不交给 ExternalProject。
include(ExternalProject)
# * 以当前脚本定位仓库，允许独立预编译工程与主工程复用。
get_filename_component(_archive_root "${CMAKE_CURRENT_LIST_DIR}/../../.."
                       ABSOLUTE)
# * 安装树属于当前二进制目录，不能覆盖系统或另一个配置的依赖。
set(_archive_prefix "${CMAKE_BINARY_DIR}/3rdpty/archive_inst")
# * 配置期仅建立接口包含目录，真实头文件等待安装步骤产生。
# * 首次配置时安装树尚不存在，预建 include 目录避免导出接口引用空路径。
file(MAKE_DIRECTORY "${_archive_prefix}/include")
# * 由主项目链接偏好选择真实库类型，不借用 BUILD_SHARED_LIBS 的偶然缓存。
# * POSIX 使用系统字符转换接口，Windows 使用库内的 Win32 转换后端。
set(_archive_iconv ON)
# * Windows 使用系统宽字符转换，不能探测宿主 Linux 的 iconv 头。
if(WIN32)
  set(_archive_iconv OFF)
endif()
set(_archive_shared OFF)
# * 归档库类型与 CRT 配置相互独立，二者都必须服从 PROJECT_LINKAGE。
set(_archive_static_crt ON)
if(PROJECT_LINKAGE STREQUAL "shared")
  # * 共享模式同时关闭上游强制静态 CRT 的选项。
  set(_archive_shared ON)
  set(_archive_static_crt OFF)
endif()
# * 传递完整工具链与部署目标，交叉编译禁止拾取宿主编译器。 外部依赖不继承业务目标的 PGO 选项；这里仅传递平台公共参数。
# * 固定 lib 目录避免不同发行版默认 lib64 导致暂存路径漂移。
# * Debug 与发布配置必须使用不同的顶层构建目录。
set(_archive_args
    "-DCMAKE_INSTALL_PREFIX=${_archive_prefix}"
    -DCMAKE_INSTALL_LIBDIR=lib
    "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
    "-DBUILD_SHARED_LIBS=${_archive_shared}"
    -DBUILD_TESTING=OFF
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON)
# MinGW 的 sysroot 已具备完整链接环境，归档库的函数探针必须检查实际符号。 只生成静态归档会把 nl_langinfo、fstatat 等不可用
# POSIX 接口误判为存在。 此覆盖仅传入归档依赖的外部工程，不改变其他依赖的默认交叉自检策略。
if(MINGW)
  list(APPEND _archive_args -DCMAKE_TRY_COMPILE_TARGET_TYPE=EXECUTABLE)
endif()
# * 不传播主工程业务目标的编译定义、PGO 或链接时优化属性。
foreach(
  _var
  # * 外部工程须重新应用目标工具链，不能因宿主平台恰好可编译而混入本机目标。
  CMAKE_TOOLCHAIN_FILE
  # * 系统名与处理器共同决定 Windows ABI 和平台条件。
  CMAKE_SYSTEM_NAME
  # Android 工具链缺少 ABI 时默认 ARMv7；必须与主应用同时传递 API 和 STL。
  ANDROID_ABI
  ANDROID_PLATFORM
  ANDROID_STL
  CMAKE_SYSTEM_PROCESSOR
  # * 编译器路径沿用父配置，不再次搜索 PATH 中其他 LLVM/GCC 版本。
  CMAKE_C_COMPILER
  CMAKE_CXX_COMPILER
  # * clang 的 target triple 与编译器路径是两个独立参数。
  CMAKE_C_COMPILER_TARGET
  CMAKE_CXX_COMPILER_TARGET
  # * 头和库的根目录必须跟随目标 sysroot，尤其是 MinGW 交叉构建。
  CMAKE_SYSROOT
  # * 静态归档器及索引器必须能处理目标对象格式。
  CMAKE_AR
  CMAKE_RANLIB
  # * Windows 资源编译器不能回退到宿主默认工具。
  CMAKE_RC_COMPILER
  CMAKE_LINKER
  # * macOS 架构与最低系统版本都需要传入子构建，不能只指定 SDK。
  CMAKE_OSX_ARCHITECTURES
  CMAKE_OSX_DEPLOYMENT_TARGET
  CMAKE_OSX_SYSROOT
  # * CRT 选择传给所有编码器，避免最终链接时混用 MT/MD。
  CMAKE_MSVC_RUNTIME_LIBRARY
  CMAKE_C_FLAGS
  CMAKE_CXX_FLAGS
  # * 保留调试符号参数，不使用无符号的 Release 参数替代发布含符号配置。
  CMAKE_C_FLAGS_DEBUG
  CMAKE_C_FLAGS_RELWITHDEBINFO
  CMAKE_CXX_FLAGS_DEBUG
  CMAKE_CXX_FLAGS_RELWITHDEBINFO)
  # * 空变量不覆盖上游默认探测结果，已设变量则保持完整字符串含空格。
  if(DEFINED ${_var} AND NOT "${${_var}}" STREQUAL "")
    # * 策略在外部 project 调用前设置，MSVC 调试格式与运行库选择才会生效。
    list(APPEND _archive_args "-D${_var}=${${_var}}")
  endif()
endforeach()
# * MSVC 使用内嵌 CodeView，保证静态库调试信息随库进入预编译包。 依赖无需独立编译 PDB，最终共享库仍由链接器生成 PDB。
list(APPEND _archive_args -DCMAKE_POLICY_DEFAULT_CMP0091=NEW
     -DCMAKE_POLICY_DEFAULT_CMP0141=NEW
     -DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded)
# * 所有消费路径固定到安装树；先声明 byproducts，Ninja 才能建立正确依赖。
set(_archive_lib_prefix "${CMAKE_STATIC_LIBRARY_PREFIX}")
set(_archive_lib_suffix "${CMAKE_STATIC_LIBRARY_SUFFIX}")
# * 库后缀在配置期确定，不能等构建完成后随意 glob 首个归档。
if(_archive_shared)
  if(WIN32)
    # * Windows 链接使用导入库路径，运行时 DLL 另由统一部署入口处理。
    set(_archive_lib_prefix "${CMAKE_IMPORT_LIBRARY_PREFIX}")
    set(_archive_lib_suffix "${CMAKE_IMPORT_LIBRARY_SUFFIX}")
  else()
    # * POSIX 的链接库本身即共享对象，不能使用静态后缀。
    set(_archive_lib_prefix "${CMAKE_SHARED_LIBRARY_PREFIX}")
    set(_archive_lib_suffix "${CMAKE_SHARED_LIBRARY_SUFFIX}")
  endif()
endif()
# * 安装输出是 Ninja 的真正输入，不只是外部构建目录中的中间产物。
# * CMake 层面的依赖和文件层面的 byproducts 共同保证并行构建顺序。
set(_archive_codec_outputs "")
foreach(_codec archive_zlib lzma bzip2 zstd)
  # * 包名用于依赖隔离，文件名采用上游惯用名称。
  set(_codec_name "${_codec}")
  # * zlib 的标准链接名为 z，包目录仍独立以隔离引擎依赖。
  if(_codec STREQUAL "archive_zlib")
    set(_codec_name z)
    # * bzip2 的库名为 bz2，不使用命令行程序名称作为库名。
  elseif(_codec STREQUAL "bzip2")
    set(_codec_name bz2)
  endif()
  set(_archive_${_codec}_lib
      "${_archive_prefix}/lib/${_archive_lib_prefix}${_codec_name}${_archive_lib_suffix}"
  )
  # * 显式登记所有四项，避免高并行构建时先链接尚未安装的库。
  list(APPEND _archive_codec_outputs "${_archive_${_codec}_lib}")
endforeach()
ExternalProject_Add(
  # * 压缩器独立工程先安装，再允许 libarchive 的函数链接探针运行。
  archive_codecs_project
  SOURCE_DIR "${_archive_root}/cmake/archive-codecs"
  BINARY_DIR "${CMAKE_BINARY_DIR}/3rdpty/archive_codecs_bld"
  # * 版本由父仓 gitlink 控制，构建不隐式拉取远端新提交。
  UPDATE_COMMAND ""
  CMAKE_ARGS ${_archive_args}
  BUILD_BYPRODUCTS ${_archive_codec_outputs})
# * 本地包装脚本变化必须触发重新配置，否则增量 CI 会保留旧库文件名。
# * 依赖文件的时间戳驱动此步骤，不要求每次运行都重建第三方库。
ExternalProject_Add_Step(
  archive_codecs_project codec_layout
  COMMAND "${CMAKE_COMMAND}" -E true
  DEPENDEES patch
  # * 编码器安装先完成，libarchive 探针才能解析更名后的库。
  DEPENDERS configure
  DEPENDS "${_archive_root}/cmake/archive-codecs/CMakeLists.txt")
# * libarchive 不探测系统可选依赖，所有启用的压缩器都来自上述固定版本。 关闭命令行、测试、ACL 和扩展属性；解包不恢复宿主权限或系统元数据。
# * libarchive 的静态和共享库在不同构建树中使用同一基础名称。
# * 不使用系统 find_library，库名与外部工程选定的链接类型完全一致。
set(_archive_library
    "${_archive_prefix}/lib/${_archive_lib_prefix}archive${_archive_lib_suffix}"
)
ExternalProject_Add(
  libarchive_project
  SOURCE_DIR "${_archive_root}/3rdpty/sources/libarchive"
  BINARY_DIR "${CMAKE_BINARY_DIR}/3rdpty/archive_bld"
  # * 这条依赖覆盖配置阶段，只有安装步骤结束后上游 Find 才能看到头与库。
  DEPENDS archive_codecs_project
  UPDATE_COMMAND ""
  CMAKE_ARGS ${_archive_args}
             # * 不通过宿主 pkg-config 选择可选压缩库，下面均传入确定路径。
             -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON
             # * 只需要库，不发布 bsdtar 等外部进程，也不使用进程解压回退。
             -DENABLE_TAR=OFF
             -DENABLE_CPIO=OFF
             -DENABLE_CAT=OFF
             -DENABLE_UNZIP=OFF
             # * 第三方测试不进入应用 CTest；项目自身的容器回归负责验证业务入口。
             -DENABLE_TEST=OFF
             -DENABLE_WERROR=OFF
             -DENABLE_INSTALL=ON
             # * 密码归档暂不提供交互解密，避免附带不必要的第三方加密后端。
             -DENABLE_OPENSSL=OFF
             -DENABLE_MBEDTLS=OFF
             -DENABLE_NETTLE=OFF
             # * RAR5 哈希使用上游内置实现，不从系统搜寻独立 libb2。
             -DENABLE_LIBB2=OFF
             # * 尚未打包的过滤器显式关闭，用户侧也不注册对应后缀。
             -DENABLE_LZ4=OFF
             -DENABLE_LZO=OFF
             -DENABLE_MD=OFF
             # * 未发布 XAR 能力，因此不引入 XML 解析器及其额外依赖。
             -DENABLE_LIBXML2=OFF
             -DENABLE_BSDXML=OFF
             -DENABLE_EXPAT=OFF
             -DENABLE_WIN32_XMLLITE=OFF
             # * 路径安全检查由业务代码完成，不需要可选正则表达式后端。
             -DENABLE_PCREPOSIX=OFF
             -DENABLE_PCRE2POSIX=OFF
             "-DENABLE_ICONV=${_archive_iconv}"
             # * 关闭 AUTO 可避免 Windows 交叉构建误拾取宿主 regex.h。
             -DPOSIX_REGEX_LIB=NONE
             # * 临时项目不恢复 ACL 和扩展属性，跨平台资源只保留目录与数据。
             -DENABLE_ACL=OFF
             -DENABLE_XATTR=OFF
             # * 这四个编码器属于已承诺能力，不能让每个构建机自行决定是否启用。
             -DENABLE_ZLIB=ON
             -DENABLE_LZMA=ON
             -DENABLE_BZip2=ON
             -DENABLE_ZSTD=ON
             # * 上游保留了自己的 CRT 开关，需要与 CMake 抽象设置同步。
             "-DMSVC_USE_STATIC_CRT=${_archive_static_crt}"
             # * 所有显式 Find 路径都指向同一个已完成的安装前缀。
             "-DZLIB_INCLUDE_DIR=${_archive_prefix}/include"
             "-DZLIB_LIBRARY=${_archive_archive_zlib_lib}"
             "-DLIBLZMA_INCLUDE_DIR=${_archive_prefix}/include"
             "-DLIBLZMA_LIBRARY=${_archive_lzma_lib}"
             "-DBZIP2_INCLUDE_DIR=${_archive_prefix}/include"
             # * 上游区分 Debug/Release 变量；当前构建树只有一种配置，两者指向同一产物。
             "-DBZIP2_LIBRARY_RELEASE=${_archive_bzip2_lib}"
             "-DBZIP2_LIBRARY_DEBUG=${_archive_bzip2_lib}"
             "-DZSTD_INCLUDE_DIR=${_archive_prefix}/include"
             "-DZSTD_LIBRARY=${_archive_zstd_lib}"
  # * 对消费方的文件依赖保证首次构建也正确，而非依赖曾经成功构建的缓存。
  BUILD_BYPRODUCTS "${_archive_library}")
# * 对外目标与各自 Find 模块一致，业务模块无需区分依赖来源。
foreach(_codec archive_zlib lzma bzip2 zstd)
  # * 接口目标只传播真实库及包含路径，不重复编译编码器源文件。
  add_library(3rd_${_codec} INTERFACE)
  # * 包含公开头的消费者也必须等待安装，不能只保证最终链接顺序。
  add_dependencies(3rd_${_codec} archive_codecs_project)
  target_include_directories(3rd_${_codec}
                             INTERFACE "${_archive_prefix}/include")
  target_link_libraries(3rd_${_codec} INTERFACE "${_archive_${_codec}_lib}")
endforeach()
# * Logic 只引用这一统一入口，保持来源切换透明。
# * 主库目标依赖安装完成，首次编译使用 archive.h 时也不会抢跑。
add_library(3rd_libarchive INTERFACE)
add_dependencies(3rd_libarchive libarchive_project)
target_include_directories(3rd_libarchive
                           INTERFACE "${_archive_prefix}/include")
target_link_libraries(3rd_libarchive INTERFACE "${_archive_library}")
# * 传播编解码器路径，静态模式需要符号，共享模式需要运行时搜索路径。
target_link_libraries(3rd_libarchive INTERFACE 3rd_archive_zlib 3rd_lzma
                                               3rd_bzip2 3rd_zstd)
# * DLL 部署复用主工程已有统一入口，测试套件与主程序都需要相同运行库。
if(WIN32 AND _archive_shared)
  # * 这里只复用 DLL 部署 helper，不调用预编译布局初始化或寻找预编译文件。
  include("${_archive_root}/3rdpty/cmake/modules/PrebuiltLayout.cmake")
  foreach(_name archive z lzma bz2 zstd)
    prebuilt_record_runtime_file(
      "${_archive_prefix}/bin/${CMAKE_SHARED_LIBRARY_PREFIX}${_name}.dll")
  endforeach()
endif()
# * 仅静态模式传播静态导出宏，避免 Windows DLL 消费方使用错误声明。
if(NOT _archive_shared)
  target_compile_definitions(3rd_libarchive INTERFACE LIBARCHIVE_STATIC)
  # * 静态 liblzma/zstd 的线程符号需要由最终可执行文件解析。
  find_package(Threads REQUIRED)
  target_link_libraries(
    3rd_libarchive INTERFACE 3rd_archive_zlib 3rd_lzma 3rd_bzip2 3rd_zstd
                             Threads::Threads)
  if(WIN32)
    # * Windows 校验和实现使用系统 CNG，不额外分发系统 DLL。
    target_link_libraries(3rd_libarchive INTERFACE bcrypt)
  elseif(APPLE)
    # * macOS 的 iconv 不是 libc 内置符号，需要显式传播系统库。
    target_link_libraries(3rd_libarchive INTERFACE iconv)
  endif()
endif()
