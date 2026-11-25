# 导入 LAME 预编译库，导出源码构建同名目标：3rd_lame。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# 以本模块的位置推导预编译根；显式覆盖根目录时仍沿用同一包层级约定。
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# 编码参数和缓冲所有权借用配套的 LAME 头，避免从其他 mp3 实现借用声明。
prebuilt_include_dir(_lame_include_dir lame)

# 兼顾发行库常见的 mp3lame 和 libmp3lame-static 命名，只在同一配置内探测。
if(NOT TARGET 3rd_lame)
  add_library(3rd_lame UNKNOWN IMPORTED GLOBAL)
  # 直接公开 LAME 编码器的配套头，无需在业务模块添加全局包含目录。
  set_target_properties(3rd_lame PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                            "${_lame_include_dir}")

  # 多配置生成器一次生成多个配置；在配置阶段解析每个实际关心的目录。
  prebuilt_target_configs(_lame_configs)
  # 仅登记在本次生成图中已解析的配置，避免预登记未经检查的库路径。
  set(_lame_imported_configs "")
  set(_lame_default_library "")
  # 针对生成器的各配置独立选库，确保 Debug 和发布配置不会共用错误产物。
  foreach(_lame_config IN LISTS _lame_configs)
    # CMake 的目标属性后缀用大写配置名，分发目录仍保留布局自己的拼写。
    string(TOUPPER "${_lame_config}" _lame_config_upper)
    # 搜索固定在上层选定的单个配置目录，缺少候选时由布局模块报告异常。
    prebuilt_find_library(_lame_library lame "${_lame_config}" mp3lame
                          libmp3lame libmp3lame-static)
    # 归档解析成功之后才登记配置，保持导入清单和 LOCATION 条目一致。
    list(APPEND _lame_imported_configs "${_lame_config_upper}")
    # 配置选择同时约束编码器和其运行库，避免 Debug 回退到任意系统 MP3 编码器。
    set_target_properties(
      3rd_lame PROPERTIES "IMPORTED_LOCATION_${_lame_config_upper}"
                          "${_lame_library}")
    # 第一份成功产物提供缺省位置；已明确配置的消费者用各自位置覆盖它。
    if(_lame_default_library STREQUAL "")
      set(_lame_default_library "${_lame_library}")
    endif()
  endforeach()
  # 无显式构建类型的单配置生成器仍需可链接位置，由同次解析得到的缺省产物承接。
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set_target_properties(3rd_lame PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                              "${_lame_default_library}")
  endif()
  # 配置表与缺省位置共同覆盖各生成器选择；所有路径均由上方同一流程校验。
  set_target_properties(
    3rd_lame PROPERTIES IMPORTED_CONFIGURATIONS "${_lame_imported_configs}"
                        IMPORTED_LOCATION "${_lame_default_library}")
endif()

# 库及头的解析已经就绪，向使用 find_package 的入口报告可用。
set(lame_FOUND TRUE)
