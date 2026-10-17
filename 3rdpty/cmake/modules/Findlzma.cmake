# * 导入 lzma 的预编译产物，必须与源码构建保持相同目标名。 查找限定在当前平台、架构、工具链、配置目录，缺包直接报错。
# * 布局 helper 只定位文件，不会触发源码编译或联网下载。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# * 头文件与库必须由同一固定子模块版本生成。
prebuilt_include_dir(_lzma_include lzma)
if(NOT TARGET 3rd_lzma)
  # * UNKNOWN 兼容静态归档与 Windows 导入库，类型由布局选择保证。
  add_library(3rd_lzma UNKNOWN IMPORTED GLOBAL)
  # * 包目录只包含自己的公开头，不依赖其他包目录的偶然包含。
  target_include_directories(3rd_lzma INTERFACE "${_lzma_include}")
  # * 每种配置单独定位，禁止把 Debug 归档混用为发布产物。
  prebuilt_target_configs(_lzma_configs)
  foreach(_config IN LISTS _lzma_configs)
    # * CMake 的配置属性键使用大写，但磁盘配置名保持原样。
    string(TOUPPER "${_config}" _upper)
    prebuilt_find_library(_library lzma "${_config}" lzma liblzma)
    # * helper 同时登记 Windows shared 运行时依赖供应用部署。
    # * 配置列表显式登记，确保多配置生成器不会使用未声明的映射。
    set_property(
      TARGET 3rd_lzma
      APPEND
      PROPERTY IMPORTED_CONFIGURATIONS "${_upper}")
    # * 无配置消费方使用同一已校验路径，不额外寻找系统版本。
    set_target_properties(
      3rd_lzma PROPERTIES "IMPORTED_LOCATION_${_upper}" "${_library}"
                          IMPORTED_LOCATION "${_library}")
  endforeach()
endif()
set(lzma_FOUND TRUE)
