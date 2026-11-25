# 导入 EnTT 头文件包，并提供项目包装目标：3rd_entt。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# 相对本脚本定位公共头目录；安装前缀里同时存在的系统 EnTT 不参与解析。
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# EnTT 是模板库，各翻译单元必须以同一套注册表和组件定义实例化。
prebuilt_include_dir(_entt_include_dir entt)

# 保留依赖方可直接使用的上游目标；重复查找时沿用原目标的属性和使用要求。
if(NOT TARGET EnTT::EnTT)
  add_library(EnTT::EnTT INTERFACE IMPORTED GLOBAL)
  set_target_properties(EnTT::EnTT PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                              "${_entt_include_dir}")
endif()

# 业务模块只依赖包装目标，组件注册相关包含目录随上游目标传递。
if(NOT TARGET 3rd_entt)
  add_library(3rd_entt INTERFACE)
  target_link_libraries(3rd_entt INTERFACE EnTT::EnTT)
endif()

# 公共头目录已经校验，纯头文件包无需等候平台库或配置归档。
set(EnTT_FOUND TRUE)
