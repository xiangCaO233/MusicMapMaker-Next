add_library(3rd_stb INTERFACE)

# 接口仅传播声明；实现宏由负责对应图像格式的翻译单元私有定义。
target_include_directories(3rd_stb INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/stb")
