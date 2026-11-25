# 使用上游构建传递模板库的使用要求，SYSTEM 只隔离第三方头的诊断。
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/entt SYSTEM)

# 包装名与预编译模式一致，使用 EnTT 组件类型的业务模块无须了解来源。
add_library(3rd_entt INTERFACE)
target_link_libraries(3rd_entt INTERFACE EnTT::EnTT)
