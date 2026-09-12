# Modules/Log/CMakeLists.txt

add_library(Log STATIC src/colorful-log.cpp)

# 对外传播 spdlog 与日志头路径，供所有业务模块使用统一日志宏。
target_link_libraries(Log PUBLIC spdlog::spdlog)
target_include_directories(Log PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# UTF-8 路径转换仅由实现使用，不扩散 Config 的头文件依赖。
target_include_directories(Log
                           PRIVATE ${PROJECT_SOURCE_DIR}/Modules/Config/include)

if(NOT MSVC)
  # Log 模块会构造 spdlog::formatter 和 sink 等第三方多态对象，需要与预编译 spdlog 保持 RTTI ABI 一致，避免
  # MinGW clang 交叉构建在退出析构日志格式器时崩溃。
  target_compile_options(Log PRIVATE -frtti)
endif()
