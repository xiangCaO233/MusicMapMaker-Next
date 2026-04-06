add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/ImGuiFileDialog SYSTEM)
target_link_libraries(ImGuiFileDialog PUBLIC 3rd_imgui)

# 国际化翻译 (由于该库使用宏定义 UI 文本，我们通过编译定义进行注入)
target_compile_definitions(ImGuiFileDialog PRIVATE
    "okButtonString=\"确定\""
    "cancelButtonString=\"取消\""
    "dirNameString=\"目录路径:\""
    "fileNameString=\"文件名:\""
    "searchString=\"搜索\""
    "resetButtonString=\"重置\""
    "buttonResetSearchString=\"重置搜索\""
    "buttonDriveString=\"驱动器\""
    "buttonEditPathString=\"编辑路径\""
    "buttonCreateDirString=\"创建目录\""
    "tableHeaderFileNameString=\"文件名\""
    "tableHeaderFileTypeString=\"类型\""
    "tableHeaderFileSizeString=\"大小\""
    "tableHeaderFileDateTimeString=\"修改日期\""
)

add_library(3rd_ImGuiFileDialog INTERFACE)
target_link_libraries(3rd_ImGuiFileDialog INTERFACE ImGuiFileDialog)
