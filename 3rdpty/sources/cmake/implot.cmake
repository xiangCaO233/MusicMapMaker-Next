set(IMPLOT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/implot")

# ImPlot 跟随依赖链接偏好，shared 预编译包产出 DLL。
set(IMPLOT_LIBRARY_TYPE STATIC)
if(PROJECT_LINKAGE STREQUAL "shared")
  set(IMPLOT_LIBRARY_TYPE SHARED)
endif()

add_library(3rd_implot ${IMPLOT_LIBRARY_TYPE} "${IMPLOT_SOURCE_DIR}/implot.cpp"
                       "${IMPLOT_SOURCE_DIR}/implot_items.cpp")
if(PROJECT_LINKAGE STREQUAL "shared" AND WIN32)
  set_target_properties(3rd_implot PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
  # MSVC 跨 DLL 使用 ImPlot 时需要显式导出/导入 API 符号。
  target_compile_definitions(
    3rd_implot PRIVATE "IMPLOT_API=__declspec(dllexport)"
                       INTERFACE "IMPLOT_API=__declspec(dllimport)")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  # ImPlot 当前源码依赖 ImGui 内部 enum 的跨类型按位组合；Clang 19 在 C++26 下会直接报错。
  # 该目标仅作为第三方静态库参与预编译，限定到 GNU++20 可保持源码兼容，不影响业务模块的 C++ 标准。
  set_target_properties(3rd_implot PROPERTIES CXX_STANDARD 20
                                              CXX_STANDARD_REQUIRED ON)
endif()

target_link_libraries(3rd_implot PUBLIC 3rd_imgui)

target_include_directories(3rd_implot PUBLIC ${IMPLOT_SOURCE_DIR})
