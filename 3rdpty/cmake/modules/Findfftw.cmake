# 导入 FFTW 预编译库，并提供项目包装目标：3rd_fftw3。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# 以本模块的位置推导预编译根；显式覆盖根目录时仍沿用同一包层级约定。
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# 分析入口使用通常的双精度 FFTW 接口，归档候选不混入单精度 fftw3f。
prebuilt_include_dir(_fftw_include_dir fftw)

# 计划创建和执行使用同一版本 FFTW；源构建和固定依赖构建均传播同一接口。
if(NOT TARGET fftw3)
  add_library(fftw3 UNKNOWN IMPORTED GLOBAL)
  # 头文件和双精度归档必须成对使用；fftw3f 的符号前缀与此接口不同。
  set_target_properties(fftw3 PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                         "${_fftw_include_dir}")

  # 多配置生成器一次生成多个配置；在配置阶段解析每个实际关心的目录。
  prebuilt_target_configs(_fftw_configs)
  # 仅登记在本次生成图中已解析的配置，避免预登记未经检查的库路径。
  set(_fftw_imported_configs "")
  set(_fftw_default_library "")
  # 针对生成器的各配置独立选库，确保 Debug 和发布配置不会共用错误产物。
  foreach(_fftw_config IN LISTS _fftw_configs)
    # CMake 的目标属性后缀用大写配置名，分发目录仍保留布局自己的拼写。
    string(TOUPPER "${_fftw_config}" _fftw_config_upper)
    # 搜索固定在上层选定的单个配置目录，缺少候选时由布局模块报告异常。
    prebuilt_find_library(_fftw_library fftw "${_fftw_config}" fftw3 libfftw3)
    # 归档解析成功之后才登记配置，保持导入清单和 LOCATION 条目一致。
    list(APPEND _fftw_imported_configs "${_fftw_config_upper}")
    # 绑定到本次配置对应的变换实现。
    set_target_properties(
      fftw3 PROPERTIES "IMPORTED_LOCATION_${_fftw_config_upper}"
                       "${_fftw_library}")
    # 第一份成功产物提供缺省位置；已明确配置的消费者用各自位置覆盖它。
    if(_fftw_default_library STREQUAL "")
      set(_fftw_default_library "${_fftw_library}")
    endif()
  endforeach()
  # 无显式构建类型的单配置生成器仍需可链接位置，由同次解析得到的缺省产物承接。
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set_target_properties(fftw3 PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                           "${_fftw_default_library}")
  endif()
  # 用同一轮验证结果构造属性表，不重新查找系统 FFTW。
  set_target_properties(
    fftw3 PROPERTIES IMPORTED_CONFIGURATIONS "${_fftw_imported_configs}"
                     IMPORTED_LOCATION "${_fftw_default_library}")
endif()

# 接口已由其他依赖拉入时保留它，避免重复查找改变公开链接要求。
if(NOT TARGET 3rd_fftw3)
  # 包装接口不生成对象，向消费端转发底层库与补充的系统依赖。
  add_library(3rd_fftw3 INTERFACE)
  # 包装目标保留 FFT 计划的底层链接依赖，业务模块不必识别依赖来源。
  target_link_libraries(3rd_fftw3 INTERFACE fftw3)
endif()

# 库及头的解析已经就绪，向使用 find_package 的入口报告可用。
set(fftw_FOUND TRUE)
