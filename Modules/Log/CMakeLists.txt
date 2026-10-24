# Modules/Log/CMakeLists.txt

add_library(Log STATIC src/colorful-log.cpp)

# 对外传播 spdlog 与日志头路径，供所有业务模块使用统一日志宏。
target_link_libraries(
  Log
  PUBLIC spdlog::spdlog
  PRIVATE Utf8Path)
target_include_directories(Log PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# UTF-8 路径目标在 Config 子目录登记，但自身不依赖 Config，避免形成静态库环。 PRIVATE
# 链接仅提供实现需要的路径符号，不向日志公共接口扩散配置头目录。

if(NOT MSVC)
  # Log 模块会构造 spdlog::formatter 和 sink 等第三方多态对象，需要与预编译 spdlog 保持 RTTI ABI 一致，避免
  # MinGW clang 交叉构建在退出析构日志格式器时崩溃。
  target_compile_options(Log PRIVATE -frtti)
endif()
