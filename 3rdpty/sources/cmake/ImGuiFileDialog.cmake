# 纯源码入口仍使用父项目提供的 ImGui 目标，避免在选择器之外维护第二份 UI 库。
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/ImGuiFileDialog SYSTEM)
# 该库没有稳定导出宏，MSVC shared 预编译包使用自动导出生成 DLL 导入库。 是否提供 Windows
# 导入库由全局链接模式决定，Linux/macOS 不需要此 MSVC 属性。
if(PROJECT_LINKAGE STREQUAL "shared" AND WIN32)
  set_target_properties(ImGuiFileDialog PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS
                                                   ON)
  # 静态构建没有 DLL 边界，不需要扩大目标的导出符号表。
endif()
# 选择器公开 API 暴露 ImGui 类型，因此消费者也需要同一套包含目录。
target_link_libraries(ImGuiFileDialog PUBLIC 3rd_imgui)

# 国际化翻译 (由于该库使用宏定义 UI 文本，我们通过编译定义进行注入)
target_compile_definitions(
  ImGuiFileDialog
  PRIVATE "okButtonString=\"确定\""
          # 取消路径明确区分当前目录浏览与正式选择，无需改动上游业务逻辑。
          "cancelButtonString=\"取消\""
          # 这些文本由上游宏参与编译，直接修改运行时翻译表不会改变静态标签。
          "dirNameString=\"目录路径:\""
          "fileNameString=\"文件名:\""
          "searchString=\"搜索\""
          "resetButtonString=\"重置\""
          "buttonResetSearchString=\"重置搜索\""
          # 工具区短文案保持稳定，避免产生未翻译的驱动器和目录操作按钮。
          "buttonDriveString=\"驱动器\""
          "buttonEditPathString=\"编辑路径\""
          "buttonCreateDirString=\"创建目录\""
          # 列标题与按钮配置一起注入，保证依赖构建时语言一致。
          "tableHeaderFileNameString=\"文件名\""
          "tableHeaderFileTypeString=\"类型\""
          "tableHeaderFileSizeString=\"大小\""
          "tableHeaderFileDateTimeString=\"修改日期\"")

# 导出与预编译 Find 脚本相同的接口，调用模块不需要检查 SOURCES_BUILD。
add_library(3rd_ImGuiFileDialog INTERFACE)
# 仅底层库使用这些翻译宏，包装消费者不继承文本编译定义。
target_link_libraries(3rd_ImGuiFileDialog INTERFACE ImGuiFileDialog)
