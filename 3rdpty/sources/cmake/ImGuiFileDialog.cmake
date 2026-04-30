add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/ImGuiFileDialog SYSTEM)
target_link_libraries(ImGuiFileDialog PUBLIC 3rd_imgui)

###############################################################################
#  
#    I have a Windows (GBK) ~ 
#               \
#                \   ____
#                 \ [____]  <-- ASCII-only CMD
#    
#    I have a Clang (UTF-8) ~
#               /
#              /    __
#             /    / /
#                 / /____  <-- LLVM-chan
#                /_/____/
#    
#    (╯°□°）╯︵ ┻━┻
#    
#    UHH!!! --- [ "Differing user-defined suffixes" BOOM! ]
#    
#    哼，哼，哼，啊啊啊啊啊啊啊啊啊啊啊啊啊啊啊啊啊啊啊
#    F**K Windows
#    另存为GBK(?)
#
###############################################################################





# 强制 Clang 将字符集设为 UTF-8,修复windows环境下传递宏定义时的编码解析错误 --- 毫无卵用，NM$L
# if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
#     target_compile_options(ImGuiFileDialog PRIVATE "-finput-charset=UTF-8" "-fexec-charset=UTF-8")
# endif()

# 国际化翻译 (由于该库使用宏定义 UI 文本，我们通过编译定义进行注入)

set(SYSTEM_IS_GBK FALSE)
if(WIN32)
    # Active Code Page
    execute_process(
        COMMAND cmd /c chcp
        OUTPUT_VARIABLE CHCP_OUTPUT
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    #  check 936 (GBK/GB2312)
    if(CHCP_OUTPUT MATCHES "936")
        set(SYSTEM_IS_GBK TRUE)
        message(STATUS "Windows GBK (CP936) environment!!!")
    else()
        set(SYSTEM_IS_GBK FALSE)
        message(STATUS "Windows Code Page: ${CHCP_OUTPUT}")
    endif()
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND SYSTEM_IS_GBK)
    # target_compile_options(ImGuiFileDialog PRIVATE "-finput-charset=UTF-8" "-fexec-charset=UTF-8")
    # Windows GBK 环境下，Clang 编译器需要特殊处理字符串编码 (由于该库使用宏定义 UI 文本，我们通过编译定义进行注入)
    string(ASCII 200 183 182 168 STR_OK)                                              # 确定
    string(ASCII 200 161 207 251 STR_CANCEL)                                          # 取消
    string(ASCII 196 191 194 188 194 183 190 182 58 STR_DIR_PATH)                     # 目录路径:
    string(ASCII 194 183 190 182 STR_PATH)                                            # 路径
    string(ASCII 196 191 194 188 STR_DIR)                                             # 目录
    string(ASCII 206 196 188 254 195 251 STR_FILENAME)                                # 文件名
    string(ASCII 203 209 203 247 STR_SEARCH)                                          # 搜索
    string(ASCII 214 216 214 195 STR_RESET)                                           # 重置
    string(ASCII 199 253 182 175 198 247 STR_DRIVE)                                   # 驱动器
    string(ASCII 177 224 188 173 STR_EDIT)                                            # 编辑
    string(ASCII 180 180 189 168 STR_CREATE)                                          # 创建
    string(ASCII 192 224 208 205 STR_TYPE)                                            # 类型
    string(ASCII 180 243 208 161 STR_SIZE)                                            # 大小
    string(ASCII 208 222 184 196 STR_MODIFY)                                          # 修改
    string(ASCII 200 213 198 218 STR_DATE)                                            # 日期

    target_compile_definitions(ImGuiFileDialog PRIVATE
        "okButtonString=\"${STR_OK}\""
        "cancelButtonString=\"${STR_CANCEL}\""
        "dirNameString=\"${STR_DIR_PATH}:\""
        "fileNameString=\"${STR_FILENAME}:\""
        "searchString=\"${STR_SEARCH}\""
        "resetButtonString=\"${STR_RESET}\""
        "buttonResetSearchString=\"${STR_RESET}${STR_SEARCH}\""
        "buttonDriveString=\"${STR_DRIVE}\""
        "buttonEditPathString=\"${STR_EDIT}${STR_PATH}\""
        "buttonCreateDirString=\"${STR_CREATE}${STR_DIR}\""
        "tableHeaderFileNameString=\"${STR_FILENAME}\""
        "tableHeaderFileTypeString=\"${STR_TYPE}\""
        "tableHeaderFileSizeString=\"${STR_SIZE}\""
        "tableHeaderFileDateTimeString=\"${STR_MODIFY}${STR_DATE}\""
    )
else()
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
endif()


add_library(3rd_ImGuiFileDialog INTERFACE)
target_link_libraries(3rd_ImGuiFileDialog INTERFACE ImGuiFileDialog)
