# 依赖配置由父项目记录，第三方重复输出版本会淹没项目本身的诊断。
set(GLM_QUIET
    ON
    CACHE BOOL "" FORCE)
# 业务依赖矩阵和向量模板接口，保持纯头依赖以匹配分发的预编译布局。
set(GLM_BUILD_LIBRARY
    OFF
    CACHE BOOL "" FORCE)
# 项目只验证自己的 GLM 使用，不把上游测试带入常规构建和 CTest。
set(GLM_BUILD_TESTS
    OFF
    CACHE BOOL "" FORCE)
# 头文件分发由项目预编译打包脚本处理，禁用上游安装目标。
set(GLM_BUILD_INSTALL
    OFF
    CACHE BOOL "" FORCE)

# 相对包装脚本定位依赖版本，避免系统包或父工程工作目录影响解析。
set(GLM_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../glm")

# 独立二进制目录隔离上游 CMake 缓存产物，头告警不套用项目的严格策略。
add_subdirectory(${GLM_SOURCE_DIR} "${CMAKE_CURRENT_BINARY_DIR}/glm_build"
                 SYSTEM)

# 保持消费者面对预编译与源码模式具有同名目标并传递其公共使用要求。
add_library(3rd_glm INTERFACE)
# 由上游目标决定头搜索及公开选项，包装层只转发接口。
target_link_libraries(3rd_glm INTERFACE glm::glm)
