# 示例可能拉入独立窗口和渲染依赖；项目只使用头接口，所以源码构建也关闭示例。
set(CLAY_INCLUDE_ALL_EXAMPLES
    OFF
    CACHE BOOL "" FORCE)

# 业务目标保留与预编译模式相同的名称，实际实现由单独的项目翻译单元负责。
add_library(3rd_clay INTERFACE)
# 头搜索以当前第三方目录为根，避免误用系统版本。
target_include_directories(3rd_clay INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/clay)
