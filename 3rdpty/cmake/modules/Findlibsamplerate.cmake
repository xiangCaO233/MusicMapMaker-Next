# 导入 libsamplerate 预编译库，并提供项目包装目标：3rd_libsamplerate。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# 以本模块的位置推导预编译根；显式覆盖根目录时仍沿用同一包层级约定。
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# 重采样状态与回调定义由同版公共头提供，不能把状态跨不同来源的库使用。
prebuilt_include_dir(_samplerate_include_dir libsamplerate)

# 消费者通过包装目标获取同一重采样实现，尊重项目生成器选中的优化配置。
if(NOT TARGET samplerate)
  add_library(samplerate UNKNOWN IMPORTED GLOBAL)
  # 重采样状态和流回调的定义跟随包中的公共头。
  set_target_properties(samplerate PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                              "${_samplerate_include_dir}")

  # 多配置生成器一次生成多个配置；在配置阶段解析每个实际关心的目录。
  prebuilt_target_configs(_samplerate_configs)
  # 仅登记在本次生成图中已解析的配置，避免预登记未经检查的库路径。
  set(_samplerate_imported_configs "")
  set(_samplerate_default_library "")
  # 针对生成器的各配置独立选库，确保 Debug 和发布配置不会共用错误产物。
  foreach(_samplerate_config IN LISTS _samplerate_configs)
    # CMake 的目标属性后缀用大写配置名，分发目录仍保留布局自己的拼写。
    string(TOUPPER "${_samplerate_config}" _samplerate_config_upper)
    # 搜索固定在上层选定的单个配置目录，缺少候选时由布局模块报告异常。
    prebuilt_find_library(_samplerate_library libsamplerate
                          "${_samplerate_config}" samplerate libsamplerate)
    # 归档解析成功之后才登记配置，保持导入清单和 LOCATION 条目一致。
    list(APPEND _samplerate_imported_configs "${_samplerate_config_upper}")
    # 每一份已确认的库都注册为独立配置属性。
    set_target_properties(
      samplerate PROPERTIES "IMPORTED_LOCATION_${_samplerate_config_upper}"
                            "${_samplerate_library}")
    # 第一份成功产物提供缺省位置；已明确配置的消费者用各自位置覆盖它。
    if(_samplerate_default_library STREQUAL "")
      set(_samplerate_default_library "${_samplerate_library}")
    endif()
  endforeach()
  # 无显式构建类型的单配置生成器仍需可链接位置，由同次解析得到的缺省产物承接。
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set_target_properties(
      samplerate PROPERTIES IMPORTED_LOCATION_NOCONFIG
                            "${_samplerate_default_library}")
  endif()
  # 无构建类型使用已校验的缺省位置，指定类型优先对应配置。
  set_target_properties(
    samplerate
    PROPERTIES IMPORTED_CONFIGURATIONS "${_samplerate_imported_configs}"
               IMPORTED_LOCATION "${_samplerate_default_library}")
endif()

# 接口已由其他依赖拉入时保留它，避免重复查找改变公开链接要求。
if(NOT TARGET 3rd_libsamplerate)
  # 包装接口不生成对象，向消费端转发底层库与补充的系统依赖。
  add_library(3rd_libsamplerate INTERFACE)
  # 让依赖重采样的目标通过一个包装接口取得头和符号，不直接在源码中拼接库路径。
  target_link_libraries(3rd_libsamplerate INTERFACE samplerate)
endif()

# 库及头的解析已经就绪，向使用 find_package 的入口报告可用。
set(libsamplerate_FOUND TRUE)
