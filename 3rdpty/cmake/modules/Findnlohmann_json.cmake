# 导入 nlohmann_json 头文件包，并提供项目包装目标：3rd_nlohmann_json。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# 序列化点和 ADL 重载分散在各模块，解析必须共享同一版模板定义。
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# 预编译分发的 JSON 包只取公共头，不从系统路径补齐另一版定义。
prebuilt_include_dir(_nlohmann_json_include_dir nlohmann_json)

# 查找只在目标尚未定义时发布属性，保留其他依赖已经绑定的定义。
if(NOT TARGET nlohmann_json::nlohmann_json)
  # 以全局上游目标承接包含路径，让不同 CMake 子目录解析同一接口。
  add_library(nlohmann_json::nlohmann_json INTERFACE IMPORTED GLOBAL)
  # 只声明接口使用要求，不需要选择导入库的架构或二进制位置。
  set_target_properties(
    nlohmann_json::nlohmann_json PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                            "${_nlohmann_json_include_dir}")
endif()

# 业务模块链接稳定名称，切换源码来源时无需调整公开头的依赖声明。
if(NOT TARGET 3rd_nlohmann_json)
  add_library(3rd_nlohmann_json INTERFACE)
  target_link_libraries(3rd_nlohmann_json
                        INTERFACE nlohmann_json::nlohmann_json)
endif()

# 头目录验证成功便足以判定可用，没有按配置区分的 JSON 库。
set(nlohmann_json_FOUND TRUE)
