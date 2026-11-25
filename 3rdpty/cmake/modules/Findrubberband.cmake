# 导入 Rubber Band 预编译库，导出源码构建同名目标：3rd_rubberband。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# 以本模块的位置推导预编译根；显式覆盖根目录时仍沿用同一包层级约定。
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# 时伸频域处理链接双精度 FFT，必须在导入 Rubber Band 前解析 FFTW。
find_package(fftw REQUIRED)
# 不同速率之间的采样转换由 libsamplerate 提供，静态包仍需传递该依赖。
find_package(libsamplerate REQUIRED)
# 时伸接口头与库配套，调用者不能用系统版本替换固定的音高处理实现。
prebuilt_include_dir(_rubberband_include_dir rubberband)

# 解析平台归档后再补线程和数学库依赖，业务端只需链接处理接口。
if(NOT TARGET 3rd_rubberband)
  add_library(3rd_rubberband UNKNOWN IMPORTED GLOBAL)
  # 时伸 C++ 接口的头和库共同分发，不和发行版的另一版头文件混用。
  set_target_properties(3rd_rubberband PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                                  "${_rubberband_include_dir}")

  # 多配置生成器一次生成多个配置；在配置阶段解析每个实际关心的目录。
  prebuilt_target_configs(_rubberband_configs)
  # 仅登记在本次生成图中已解析的配置，避免预登记未经检查的库路径。
  set(_rubberband_imported_configs "")
  set(_rubberband_default_library "")
  # 针对生成器的各配置独立选库，确保 Debug 和发布配置不会共用错误产物。
  foreach(_rubberband_config IN LISTS _rubberband_configs)
    # CMake 的目标属性后缀用大写配置名，分发目录仍保留布局自己的拼写。
    string(TOUPPER "${_rubberband_config}" _rubberband_config_upper)
    # 搜索固定在上层选定的单个配置目录，缺少候选时由布局模块报告异常。
    prebuilt_find_library(
      _rubberband_library
      rubberband
      "${_rubberband_config}"
      rubberband
      rubberband-static
      librubberband
      librubberband-static)
    # 归档解析成功之后才登记配置，保持导入清单和 LOCATION 条目一致。
    list(APPEND _rubberband_imported_configs "${_rubberband_config_upper}")
    # 保持每个生成器配置选用对应的时伸实现。
    set_target_properties(
      3rd_rubberband PROPERTIES "IMPORTED_LOCATION_${_rubberband_config_upper}"
                                "${_rubberband_library}")
    # 第一份成功产物提供缺省位置；已明确配置的消费者用各自位置覆盖它。
    if(_rubberband_default_library STREQUAL "")
      set(_rubberband_default_library "${_rubberband_library}")
    endif()
  endforeach()
  # 无显式构建类型的单配置生成器仍需可链接位置，由同次解析得到的缺省产物承接。
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set_target_properties(
      3rd_rubberband PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                "${_rubberband_default_library}")
  endif()
  # 补充通用位置前先登记所有已验证配置。
  set_target_properties(
    3rd_rubberband
    PROPERTIES IMPORTED_CONFIGURATIONS "${_rubberband_imported_configs}"
               IMPORTED_LOCATION "${_rubberband_default_library}")
  # 附带 FFTW 和重采样包装目标，确保由静态归档延后的符号可在最终链接闭合。
  target_link_libraries(3rd_rubberband INTERFACE 3rd_fftw3 3rd_libsamplerate)
  # Windows 使用独立的导入与系统库契约，其他平台不沿用这组链接参数。
  if(WIN32)
    if(NOT MSVC)
      # MinGW 使用独立 pthread 接口，MSVC 分支不链接该库。
      target_link_libraries(3rd_rubberband INTERFACE pthread)
    endif()
    # 跨平台线程依赖分别判断，Apple 的系统线程符号无需这里额外列 pthread。
  else()
    # POSIX 分支显式补充时伸处理所需数学库。
    target_link_libraries(3rd_rubberband INTERFACE m)
    if(NOT APPLE)
      # 非 Apple POSIX 目标仍需独立链接 pthread。
      target_link_libraries(3rd_rubberband INTERFACE pthread)
    endif()
  endif()
endif()

# 库及头的解析已经就绪，向使用 find_package 的入口报告可用。
set(rubberband_FOUND TRUE)
