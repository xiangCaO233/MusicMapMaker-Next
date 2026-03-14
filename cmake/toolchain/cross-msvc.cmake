# cross-msvc.cmake

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# 基础路径定义 (基于你的挂载点)
set(MSVC_BASE "/mnt/windows_c/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.50.35717")
set(WINSDK_BASE "/mnt/windows_c/Program Files (x86)/Windows Kits/10")
set(WINSDK_VER "10.0.26100.0") # <--- 请根据你 Lib/Include 下的实际文件夹名修改此处!!

# 指定编译器 (确保你 Linux 系统安装了 clang 和 lld)
set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_LINKER lld-link)

# 告诉 clang-cl 目标平台
set(MSVC_TARGET_TRIPLE x86_64-pc-windows-msvc)
set(FLAGS "--target=${MSVC_TARGET_TRIPLE} -Xclang -fms-compatibility-version=19")

# --- 核心：配置头文件搜索路径 (-imsvc 模拟 MSVC 的包含逻辑) ---
set(MSVC_INCLUDE
    "-imsvc \"${MSVC_BASE}/include\""
    "-imsvc \"${MSVC_BASE}/atlmfc/include\""
    "-imsvc \"${WINSDK_BASE}/Include/${WINSDK_VER}/ucrt\""
    "-imsvc \"${WINSDK_BASE}/Include/${WINSDK_VER}/shared\""
    "-imsvc \"${WINSDK_BASE}/Include/${WINSDK_VER}/um\""
    "-imsvc \"${WINSDK_BASE}/Include/${WINSDK_VER}/winrt\""
)

# --- 核心：配置库文件搜索路径 (/libpath) ---
set(MSVC_LIB_PATHS
    "/libpath:\"${MSVC_BASE}/lib/x64\""
    "/libpath:\"${MSVC_BASE}/atlmfc/lib/x64\""
    "/libpath:\"${WINSDK_BASE}/Lib/${WINSDK_VER}/ucrt/x64\""
    "/libpath:\"${WINSDK_BASE}/Lib/${WINSDK_VER}/um/x64\""
)

string(REPLACE ";" " " MSVC_INCLUDE_STR "${MSVC_INCLUDE}")
string(REPLACE ";" " " MSVC_LIB_STR "${MSVC_LIB_PATHS}")

# 将这些参数传给编译器和链接器
set(CMAKE_C_FLAGS "${FLAGS} ${MSVC_INCLUDE_STR}" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS "${FLAGS} ${MSVC_INCLUDE_STR}" CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS "${MSVC_LIB_STR}" CACHE STRING "" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS "${MSVC_LIB_STR}" CACHE STRING "" FORCE)

# 修复 clang-cl 找不到本地链接器的问题
set(CMAKE_C_LINK_EXECUTABLE "<CMAKE_LINKER> <FLAGS> <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
set(CMAKE_CXX_LINK_EXECUTABLE "<CMAKE_LINKER> <FLAGS> <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")