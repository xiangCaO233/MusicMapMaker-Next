# 导入 nativefiledialog-extended 预编译库，并提供项目包装目标：3rd_nfd。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# 原生文件选择按仓库发布布局定位；独立验收可覆盖根，但不更改包名约定。
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# 原生文件选择器的平台后端由分发配方选择，公共声明保持稳定。
prebuilt_include_dir(_nfd_include_dir nativefiledialog-extended)

# 各平台的 GTK 或系统框架由包装目标传递，使业务代码无需了解选择器的后端。
if(NOT TARGET nfd::nfd)
  # 将原生文件选择作为全局导入发布，兄弟目录使用同一选择。
  add_library(nfd::nfd UNKNOWN IMPORTED GLOBAL)
  # 消费者通过公开路径读取选择器 ABI 和路径类型，不必手写额外包含目录。
  set_target_properties(nfd::nfd PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                            "${_nfd_include_dir}")

  # 根据生成器启用的配置验证打包的选择器归档，避免切换配置时才发现分发遗漏。
  prebuilt_target_configs(_nfd_configs)
  # 记录实际完成解析的原生文件选择配置，以便给缺失产物报错。
  set(_nfd_imported_configs "")
  # 文件路径选择入口可能来自无显式构建类型的生成器；这里延迟选定默认产物。
  set(_nfd_default_library "")
  # 原生文件选择每个消费配置单独选库，调试产物保留自己的目录边界。
  foreach(_nfd_config IN LISTS _nfd_configs)
    # nfd::nfd 的 CMake 属性名按大写消费配置指定；目录搜索仍按原配置执行。
    string(TOUPPER "${_nfd_config}" _nfd_config_upper)
    # 只在已选工具链和配置目录尝试打包的选择器归档，不会命中系统安装。
    prebuilt_find_library(_nfd_library nativefiledialog-extended
                          "${_nfd_config}" nfd nativefiledialog-extended)
    # 处理成功才登记 nfd::nfd 当前配置，未知产物不产生空导入条目。
    list(APPEND _nfd_imported_configs "${_nfd_config_upper}")
    # 记录 nfd::nfd 对应本轮配置的库路径，生成器可在构建时按配置取用。
    set_target_properties(
      nfd::nfd PROPERTIES "IMPORTED_LOCATION_${_nfd_config_upper}"
                          "${_nfd_library}")
    # nfd::nfd 只在第一份产物就绪时保留缺省位置，后续配置仍有各自条目。
    if(_nfd_default_library STREQUAL "")
      set(_nfd_default_library "${_nfd_library}")
    endif()
  endforeach()
  # 如果文件路径选择入口走无配置的单配置生成器，以缺省库承接其链接请求。
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    # 显式无配置位置仅承接 nfd::nfd 缺省链接，已指定配置不受它影响。
    set_target_properties(nfd::nfd PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                              "${_nfd_default_library}")
  endif()
  # 将 nfd::nfd 的配置集合与通用位置同时暴露，完整保留各明确配置的解析。
  set_target_properties(
    nfd::nfd PROPERTIES IMPORTED_CONFIGURATIONS "${_nfd_imported_configs}"
                        IMPORTED_LOCATION "${_nfd_default_library}")
endif()

# 调用方只需稳定的原生选择器目标，不感知 Cocoa 与 GTK 的实现差别。
if(NOT TARGET 3rd_nfd)
  # 包装接口供文件路径选择入口获得桌面文件对话框后端，无需在该处重新声明系统库。
  add_library(3rd_nfd INTERFACE)
  # Apple 使用系统框架，其他 Unix 依赖 GTK 导入目标，Windows 不复用这两组依赖。
  target_link_libraries(3rd_nfd INTERFACE nfd::nfd)
  if(APPLE)
    # macOS 静态 NFD 依赖这些系统框架，导入目标必须与源码目标保持一致。
    find_library(_nfd_core_services_framework CoreServices REQUIRED)
    find_library(_nfd_appkit_framework AppKit REQUIRED)
    find_library(_nfd_uniform_type_identifiers_framework UniformTypeIdentifiers
                 REQUIRED)
    target_link_libraries(
      3rd_nfd
      INTERFACE "${_nfd_core_services_framework}" "${_nfd_appkit_framework}"
                "${_nfd_uniform_type_identifiers_framework}")
    # 原生文件选择在此配置使用另一组发布文件名或系统依赖，仍限定本库的作用域。
  elseif(UNIX)
    # Linux 版 NFD 静态库使用 GTK/GDK 后端；源码构建时这些依赖来自 nfd::nfd，预编译导入目标需要显式恢复。

    # 先完成 PkgConfig 的依赖解析，使原生文件选择的缺失在配置阶段可见。
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(
      NFD_GTK
      REQUIRED
      IMPORTED_TARGET
      gtk+-3.0
      gdk-x11-3.0
      gdk-wayland-3.0
      wayland-client)
    target_link_libraries(3rd_nfd INTERFACE PkgConfig::NFD_GTK)
  endif()
endif()

# 文件路径选择入口使用的头与所需链接位置已就绪，缺失时由之前的解析步骤报告。
set(nfd_FOUND TRUE)
