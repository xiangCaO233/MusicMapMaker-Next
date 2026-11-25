# 导入 SDL3 预编译库，并提供项目包装目标：3rd_sdl3。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# 以本模块的位置推导预编译根；显式覆盖根目录时仍沿用同一包层级约定。
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# 项目继续使用 SDL3 的公共声明，具体交付形态由链接偏好决定。
prebuilt_include_dir(_sdl_include_dir sdl)

# 动态分发与静态归档使用各自候选名，不能以最终目标名称猜测文件名。
if(NOT TARGET SDL3-static)
  add_library(SDL3-static UNKNOWN IMPORTED GLOBAL)
  # 即使动态偏好下选中 SDL3 导入库，也沿用源码分发的同一套公共头。
  set_target_properties(SDL3-static PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                               "${_sdl_include_dir}")

  # 多配置生成器一次生成多个配置；在配置阶段解析每个实际关心的目录。
  prebuilt_target_configs(_sdl_configs)
  # 仅登记在本次生成图中已解析的配置，避免预登记未经检查的库路径。
  set(_sdl_imported_configs "")
  set(_sdl_default_library "")
  # 针对生成器的各配置独立选库，确保 Debug 和发布配置不会共用错误产物。
  foreach(_sdl_config IN LISTS _sdl_configs)
    # CMake 的目标属性后缀用大写配置名，分发目录仍保留布局自己的拼写。
    string(TOUPPER "${_sdl_config}" _sdl_config_upper)
    if(PROJECT_LINKAGE STREQUAL "shared")
      set(_sdl_library_names SDL3 libSDL3 SDL3-static)
      # 共享偏好优先解析 SDL3 动态候选；仍按该配置目录的已有产物查找。
    else()
      # 静态偏好优先定位带 static 后缀的归档，兼容未区分后缀的旧包。 文件名的回退顺序不切换工具链或配置，仍交给布局入口约束。
      set(_sdl_library_names SDL3-static SDL3 libSDL3)
    endif()
    # 搜索固定在上层选定的单个配置目录，缺少候选时由布局模块报告异常。
    prebuilt_find_library(_sdl_library sdl "${_sdl_config}"
                          ${_sdl_library_names})
    # 归档解析成功之后才登记配置，保持导入清单和 LOCATION 条目一致。
    list(APPEND _sdl_imported_configs "${_sdl_config_upper}")
    # 保留当前配置实际解析出的库名，而非由 SDL3-static 目标名反推文件。
    set_target_properties(
      SDL3-static PROPERTIES "IMPORTED_LOCATION_${_sdl_config_upper}"
                             "${_sdl_library}")
    # 第一份成功产物提供缺省位置；已明确配置的消费者用各自位置覆盖它。
    if(_sdl_default_library STREQUAL "")
      set(_sdl_default_library "${_sdl_library}")
    endif()
  endforeach()
  # 无显式构建类型的单配置生成器仍需可链接位置，由同次解析得到的缺省产物承接。
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    # 未指定配置时也将选中的 SDL3 文件公开给生成器。
    set_target_properties(SDL3-static PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                                 "${_sdl_default_library}")
  endif()
  # CMake 依此映射配置路径；共享偏好不改变业务所依赖的目标名称。
  set_target_properties(
    SDL3-static PROPERTIES IMPORTED_CONFIGURATIONS "${_sdl_imported_configs}"
                           IMPORTED_LOCATION "${_sdl_default_library}")
endif()

# 接口已由其他依赖拉入时保留它，避免重复查找改变公开链接要求。
if(NOT TARGET 3rd_sdl3)
  # 包装接口不生成对象，向消费端转发底层库与补充的系统依赖。
  add_library(3rd_sdl3 INTERFACE)
  # 业务只需链接 3rd_sdl3。导入形态和平台系统依赖在本脚本闭合。
  target_link_libraries(3rd_sdl3 INTERFACE SDL3-static)
  # Windows 使用独立的导入与系统库契约，其他平台不沿用这组链接参数。
  if(WIN32 AND NOT PROJECT_LINKAGE STREQUAL "shared")
    # SDL3 静态库不会携带 Windows 系统库信息，导入目标必须补齐源码目标的链接闭包。

    # 这些库由 SDL 窗口、设备枚举、输入与 COM 路径使用，动态分发不传播此表。
    target_link_libraries(
      3rd_sdl3
      INTERFACE kernel32
                user32
                gdi32
                winmm
                imm32
                ole32
                oleaut32
                version
                uuid
                advapi32
                setupapi
                shell32
                dinput8)
  endif()
endif()

# 库及头的解析已经就绪，向使用 find_package 的入口报告可用。
set(SDL3Static_FOUND TRUE)
