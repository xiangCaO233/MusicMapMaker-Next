# 在 MSYS2 CLANG64 环境中安装本项目源码构建与图形运行所需工具。
# 系统升级和依赖安装使用同一命令链，升级失败时不会继续混合安装。
# --noconfirm 面向一次性开发环境或 CI 镜像，调用前应确认镜像可更新。
# autotools、CMake、Meson、Ninja 与 pkgconf 覆盖依赖使用的构建系统。
# Vulkan 包来自同一 CLANG64 仓库，保持 loader、头和辅助库 ABI 一致。
# fftw、libsamplerate、speexdsp 与 sleef 提供音频处理和向量数学依赖。
# base-devel 与 GCC 包补齐 MSYS2 基础工具及 CLANG64 运行时组件。
# 脚本仅安装环境，不配置或构建 MusicMapMaker-Next。
# 包列表使用反斜杠保持为一次 pacman 事务，禁止在续行中插入命令。
pacman -Syu && pacman -S --noconfirm base-devel \
          mingw-w64-clang-x86_64-autotools \
          mingw-w64-clang-x86_64-cmake \
          mingw-w64-clang-x86_64-gcc \
          mingw-w64-clang-x86_64-make \
          mingw-w64-clang-x86_64-meson \
          mingw-w64-clang-x86_64-nasm \
          mingw-w64-clang-x86_64-ninja \
          mingw-w64-clang-x86_64-pkgconf \
          mingw-w64-clang-x86_64-shaderc \
          mingw-w64-clang-x86_64-vulkan-headers \
          mingw-w64-clang-x86_64-vulkan-loader \
          mingw-w64-clang-x86_64-vulkan-memory-allocator \
          mingw-w64-clang-x86_64-vulkan-utility-libraries \
          mingw-w64-clang-x86_64-vulkan-validation-layers \
          mingw-w64-clang-x86_64-fftw \
          mingw-w64-clang-x86_64-libsamplerate \
          mingw-w64-clang-x86_64-speexdsp \
          mingw-w64-clang-x86_64-sleef
