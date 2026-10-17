# * 导入 libarchive 的预编译产物，必须与源码构建保持相同目标名。 查找限定在当前平台、架构、工具链、配置目录，缺包直接报错。
# * 布局 helper 只定位文件，不会触发源码编译或联网下载。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# * 头文件与库必须由同一固定子模块版本生成。
prebuilt_include_dir(_libarchive_include libarchive)
if(NOT TARGET 3rd_libarchive)
  # * UNKNOWN 兼容静态归档与 Windows 导入库，类型由布局选择保证。
  add_library(3rd_libarchive UNKNOWN IMPORTED GLOBAL)
  # * 包目录只包含自己的公开头，不依赖其他包目录的偶然包含。
  target_include_directories(3rd_libarchive INTERFACE "${_libarchive_include}")
  # * 每种配置单独定位，禁止把 Debug 归档混用为发布产物。
  prebuilt_target_configs(_libarchive_configs)
  foreach(_config IN LISTS _libarchive_configs)
    # * CMake 的配置属性键使用大写，但磁盘配置名保持原样。
    string(TOUPPER "${_config}" _upper)
    prebuilt_find_library(_library libarchive "${_config}" archive libarchive)
    # * helper 同时登记 Windows shared 运行时依赖供应用部署。
    # * 配置列表显式登记，确保多配置生成器不会使用未声明的映射。
    set_property(
      TARGET 3rd_libarchive
      APPEND
      PROPERTY IMPORTED_CONFIGURATIONS "${_upper}")
    # * 无配置消费方使用同一已校验路径，不额外寻找系统版本。
    set_target_properties(
      3rd_libarchive PROPERTIES "IMPORTED_LOCATION_${_upper}" "${_library}"
                                IMPORTED_LOCATION "${_library}")
  endforeach()
endif()
# * 所需配置均成功解析之后才报告包可用。
set(libarchive_FOUND TRUE)
# * 静态链接显式携带编解码器；共享模式仍解析依赖以登记运行时 DLL。
# * Deflate 与应用原有 miniz 是不同符号集合，归档库使用自己构建的 zlib。
find_package(archive_zlib REQUIRED)
# * 7z 的 LZMA2 为必要能力，不能静默忽略此依赖。
find_package(lzma REQUIRED)
# * BZip2 同时用于压缩 TAR 和部分 ZIP/7z 编码。
find_package(bzip2 REQUIRED)
# * 即使主库为 DLL，也需解析编码器包以便部署间接依赖。
find_package(zstd REQUIRED)
target_link_libraries(3rd_libarchive INTERFACE 3rd_archive_zlib 3rd_lzma
                                               3rd_bzip2 3rd_zstd)
if(PROJECT_LINKAGE STREQUAL "static")
  target_compile_definitions(3rd_libarchive INTERFACE LIBARCHIVE_STATIC)
  # * POSIX 编码器包含线程实现，静态最终链接需保留系统线程目标。
  find_package(Threads REQUIRED)
  target_link_libraries(
    3rd_libarchive INTERFACE 3rd_archive_zlib 3rd_lzma 3rd_bzip2 3rd_zstd
                             Threads::Threads)
  if(WIN32)
    target_link_libraries(3rd_libarchive INTERFACE bcrypt)
  elseif(APPLE)
    target_link_libraries(3rd_libarchive INTERFACE iconv)
  endif()
endif()
