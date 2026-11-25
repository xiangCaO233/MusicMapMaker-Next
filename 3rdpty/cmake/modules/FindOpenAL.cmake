# 导入 OpenAL 预编译库，导出源码构建同名目标：OpenAL::OpenAL。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# 以本模块的位置推导预编译根；显式覆盖根目录时仍沿用同一包层级约定。
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# 公开头与软实现配套，设备枚举及扩展声明必须对应归档内实现。
prebuilt_include_dir(_openal_include_dir openal)
set(_openal_include_dirs "${_openal_include_dir}" "${_openal_include_dir}/AL")

# 按配置选择同一软件实现的产物，由平台链接分支再补设备和线程调度依赖。
if(NOT TARGET OpenAL::OpenAL)
  add_library(OpenAL::OpenAL UNKNOWN IMPORTED GLOBAL)
  # 同时公开包包含根和 AL 子目录，适配两种既有头文件引用方式。
  set_target_properties(OpenAL::OpenAL PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                                  "${_openal_include_dirs}")

  # 多配置生成器一次生成多个配置；在配置阶段解析每个实际关心的目录。
  prebuilt_target_configs(_openal_configs)
  # 仅登记在本次生成图中已解析的配置，避免预登记未经检查的库路径。
  set(_openal_imported_configs "")
  set(_openal_default_library "")
  # 针对生成器的各配置独立选库，确保 Debug 和发布配置不会共用错误产物。
  foreach(_openal_config IN LISTS _openal_configs)
    # CMake 的目标属性后缀用大写配置名，分发目录仍保留布局自己的拼写。
    string(TOUPPER "${_openal_config}" _openal_config_upper)
    # 搜索固定在上层选定的单个配置目录，缺少候选时由布局模块报告异常。
    prebuilt_find_library(
      _openal_library
      openal
      "${_openal_config}"
      OpenAL
      OpenAL32
      openal
      soft_oal)
    # 归档解析成功之后才登记配置，保持导入清单和 LOCATION 条目一致。
    list(APPEND _openal_imported_configs "${_openal_config_upper}")
    # 各配置直接使用已解析的 OpenAL 文件，不能意外混用同名系统库。
    set_target_properties(
      OpenAL::OpenAL PROPERTIES "IMPORTED_LOCATION_${_openal_config_upper}"
                                "${_openal_library}")
    # 第一份成功产物提供缺省位置；已明确配置的消费者用各自位置覆盖它。
    if(_openal_default_library STREQUAL "")
      set(_openal_default_library "${_openal_library}")
    endif()
  endforeach()
  # 无显式构建类型的单配置生成器仍需可链接位置，由同次解析得到的缺省产物承接。
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    # 无配置时只使用已经确认存在的目标文件。
    set_target_properties(
      OpenAL::OpenAL PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                "${_openal_default_library}")
  endif()
  # 生成器用此配置表匹配 LOCATION 属性，通用位置用于未映射配置。
  set_target_properties(
    OpenAL::OpenAL
    PROPERTIES IMPORTED_CONFIGURATIONS "${_openal_imported_configs}"
               IMPORTED_LOCATION "${_openal_default_library}")

  if(PROJECT_LINKAGE STREQUAL "static")
    # OpenAL 头文件默认按 DLL 导入声明符号；静态预编译库必须显式关闭 dllimport，否则 MinGW 会查找 __imp_al*
    # 导入符号。
    target_compile_definitions(
      OpenAL::OpenAL INTERFACE AL_LIBTYPE_STATIC ICE_OPENALSOFT_STATIC_LINKAGE)
  endif()

  # Windows 使用独立的导入与系统库契约，其他平台不沿用这组链接参数。
  if(WIN32)
    # winmm、OLE 和 AVRT 分别为 Windows 设备、COM 和实时音频调度提供入口。
    target_link_libraries(OpenAL::OpenAL INTERFACE winmm ole32 avrt)
    # macOS 分支以原生音频框架提供设备交互，勿沿用 Windows 链接闭包。
  elseif(APPLE)
    # Apple 音频后端依赖系统框架；作为使用要求传给静态链接的消费目标。
    target_link_libraries(
      OpenAL::OpenAL INTERFACE "-framework CoreAudio" "-framework AudioToolbox"
                               "-framework AudioUnit")
    # 动态后端加载属于其他平台分支。
  else()
    # 非 Windows/macOS 平台保留动态加载接口，供后端探测实际设备库。
    target_link_libraries(OpenAL::OpenAL INTERFACE dl)
  endif()
endif()

# 库及头的解析已经就绪，向使用 find_package 的入口报告可用。
set(OpenAL_FOUND TRUE)
