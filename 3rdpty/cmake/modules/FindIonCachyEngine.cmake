# 导入 IonCachyEngine 预编译库，导出源码构建同名目标：ICE::IonCachyEngine。
# cmake-format: off
# 查找器只负责预编译模式；源码模式由 IonCachyEngine 自己创建目标。
# 导入目标的名称必须与源码目标一致，业务模块才能复用链接声明。
# 公开头文件目录来自预编译包，不能混入源码树中的同名头文件。
# 库目录按平台、架构、工具链、链接方式和构建配置分别选择。
# 调试库与发布库不可互换，运行时库及标准库状态可能不同。
#
# ICE 的 AudioTrack 在业务对象中以 shared_ptr 传递，但其成员仍会内联访问。
# AudioTrack 按值保存 MediaInfo，后者继续按值保存 AlbumArt。
# 任一嵌套类型尺寸变化，都会移动后续的解码器指针。
# 库内构造函数与业务对象必须使用完全一致的公开头文件布局。
# 静态链接成功只证明符号可解析，无法证明两个对象文件采用同一布局。
# 已发生的 Windows 崩溃正是旧业务对象按 +0xb0 读取新库的 +0xb8 成员。
# +0xb0 实际是 std::string 容量，因此读出了不可解引用的 0x5f。
#
# 普通链接依赖只会触发最终可执行文件的重新链接。
# 即使头文件已更新，旧对象文件也可能因保留的修改时间而继续被复用。
# 在配置阶段计算公开头文件及所选预编译库的内容指纹。
# 指纹进入导入目标的 INTERFACE 编译定义，只用于改变消费方编译命令。
# 指纹不作为运行时逻辑使用，也不改变 ICE 的导出 API。
# 所有配置的库都参与指纹，切换构建配置后不会复用另一布局的对象。
# 文件集合排序并去重，确保相同输入生成稳定定义。
# 文件路径也参与哈希，切换工具链或配置目录时同样触发重编。
#
# CMAKE_CONFIGURE_DEPENDS 监测已有文件的常规时间戳变化。
# GLOB_RECURSE CONFIGURE_DEPENDS 监测公开头文件集合的增删。
# 构建脚本需要先执行 CMake 配置；首次启用本规则会重编旧构建树。
# 如果外部同步程序强行保留旧时间戳，必须重新配置后再编译。
# 更新预编译包时应避免保留旧时间戳，详见 prebuilt-abi-validation.md。
# 这些限制属于构建增量判定，不应靠运行时的空指针检查掩盖。
#
# 共享链接时继续通过 ICE_SHARED_LIBRARY 使用导入声明。
# 第三方间接依赖仍由导入目标传递，业务模块无需感知依赖来源。
# cmake-format: on
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
find_package(SDL3Static REQUIRED)
find_package(fmt REQUIRED)
find_package(spdlog REQUIRED)
find_package(OpenAL REQUIRED)
find_package(ffmpeg REQUIRED)
find_package(rubberband REQUIRED)
prebuilt_include_dir(_ice_include_dir IonCachyEngine)

if(NOT TARGET ICE::IonCachyEngine)
  add_library(ICE::IonCachyEngine UNKNOWN IMPORTED GLOBAL)
  set_target_properties(
    ICE::IonCachyEngine PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                   "${_ice_include_dir}")

  prebuilt_target_configs(_ice_configs)
  set(_ice_imported_configs "")
  set(_ice_default_library "")
  set(_ice_libraries "")
  foreach(_ice_config IN LISTS _ice_configs)
    string(TOUPPER "${_ice_config}" _ice_config_upper)
    if(PROJECT_LINKAGE STREQUAL "shared")
      set(_ice_library_names IonCachyEngine libIonCachyEngine
                             IonCachyEngine-static libIonCachyEngine-static)
    else()
      set(_ice_library_names IonCachyEngine-static IonCachyEngine
                             libIonCachyEngine-static libIonCachyEngine)
    endif()
    prebuilt_find_library(_ice_library IonCachyEngine "${_ice_config}"
                          ${_ice_library_names})
    list(APPEND _ice_libraries "${_ice_library}")
    list(APPEND _ice_imported_configs "${_ice_config_upper}")
    set_target_properties(
      ICE::IonCachyEngine PROPERTIES "IMPORTED_LOCATION_${_ice_config_upper}"
                                     "${_ice_library}")
    if(_ice_default_library STREQUAL "")
      set(_ice_default_library "${_ice_library}")
    endif()
  endforeach()
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set_target_properties(
      ICE::IonCachyEngine PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                     "${_ice_default_library}")
  endif()
  set_target_properties(
    ICE::IonCachyEngine
    PROPERTIES IMPORTED_CONFIGURATIONS "${_ice_imported_configs}"
               IMPORTED_LOCATION "${_ice_default_library}")

  # 用内容指纹改变消费方编译命令，清除预编译库更新前的旧布局对象。
  file(GLOB_RECURSE _ice_public_headers CONFIGURE_DEPENDS
       "${_ice_include_dir}/ice/*.h" "${_ice_include_dir}/ice/*.hpp")
  set(_ice_abi_inputs ${_ice_public_headers} ${_ice_libraries})
  list(REMOVE_DUPLICATES _ice_abi_inputs)
  list(SORT _ice_abi_inputs)
  set_property(
    DIRECTORY
    APPEND
    PROPERTY CMAKE_CONFIGURE_DEPENDS ${_ice_abi_inputs})
  set(_ice_abi_material "")
  foreach(_ice_abi_input IN LISTS _ice_abi_inputs)
    file(SHA256 "${_ice_abi_input}" _ice_input_hash)
    string(APPEND _ice_abi_material "${_ice_abi_input}:${_ice_input_hash}\n")
  endforeach()
  string(SHA256 _ice_abi_fingerprint "${_ice_abi_material}")
  target_compile_definitions(
    ICE::IonCachyEngine INTERFACE "ICE_PREBUILT_ABI_${_ice_abi_fingerprint}")

  target_link_libraries(
    ICE::IonCachyEngine INTERFACE 3rd_sdl3 fmt::fmt spdlog::spdlog
                                  OpenAL::OpenAL 3rd_ffmpeg 3rd_rubberband)
  if(PROJECT_LINKAGE STREQUAL "shared" AND WIN32)
    # shared ICE 预编译包需要让使用方按 dllimport 访问公共数据符号。
    target_compile_definitions(ICE::IonCachyEngine INTERFACE ICE_SHARED_LIBRARY)
  endif()
endif()

set(IonCachyEngine_FOUND TRUE)
