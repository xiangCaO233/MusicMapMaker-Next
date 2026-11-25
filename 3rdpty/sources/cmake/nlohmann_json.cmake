# 序列化实现依赖上游目标的公开包含路径，避免绕过其源码构建选项。
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/nlohmann_json SYSTEM)

# 维持预编译模式相同的使用要求，项目序列化模块无需按来源分支。
add_library(3rd_nlohmann_json INTERFACE)
target_link_libraries(3rd_nlohmann_json INTERFACE nlohmann_json::nlohmann_json)
