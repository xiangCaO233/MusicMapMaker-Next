# 导入 nlohmann_json 头文件包，并提供项目包装目标：3rd_nlohmann_json。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
prebuilt_include_dir(_nlohmann_json_include_dir nlohmann_json)

if(NOT TARGET nlohmann_json::nlohmann_json)
  add_library(nlohmann_json::nlohmann_json INTERFACE IMPORTED GLOBAL)
  set_target_properties(
    nlohmann_json::nlohmann_json PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                            "${_nlohmann_json_include_dir}")
endif()

if(NOT TARGET 3rd_nlohmann_json)
  add_library(3rd_nlohmann_json INTERFACE)
  target_link_libraries(3rd_nlohmann_json
                        INTERFACE nlohmann_json::nlohmann_json)
endif()

set(nlohmann_json_FOUND TRUE)
