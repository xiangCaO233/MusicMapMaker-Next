#!/usr/bin/env bash
# 在 MSYS2 MINGW64 环境中准备 GCC、构建器和 Vulkan 开发依赖。
# 脚本会更新整个 pacman 环境，适合新建镜像而非冻结的软件环境。
set -euo pipefail

# 数组保持每个包独立，便于审查和追加同一 ABI 前缀的依赖。
packages=(
    # 基础编译工具与 autotools 覆盖传统第三方构建系统。
    base-devel
    mingw-w64-x86_64-autotools
    mingw-w64-x86_64-cmake
    # GCC、Make、Meson 与 Ninja 提供项目及依赖的编译后端。
    mingw-w64-x86_64-gcc
    mingw-w64-x86_64-make
    mingw-w64-x86_64-meson
    mingw-w64-x86_64-nasm
    # pkgconf 负责向 CMake/Meson 暴露已安装库元数据。
    mingw-w64-x86_64-ninja
    mingw-w64-x86_64-pkgconf
    mingw-w64-x86_64-shaderc
    # Vulkan 组件必须来自同一 MINGW64 仓库，避免 UCRT/MSVCRT 混用。
    mingw-w64-x86_64-vulkan-headers
    mingw-w64-x86_64-vulkan-loader
    mingw-w64-x86_64-vulkan-memory-allocator
    mingw-w64-x86_64-vulkan-utility-libraries
    mingw-w64-x86_64-vulkan-validation-layers
)

# 先升级数据库与基础系统，防止新包依赖旧核心库。
pacman -Syu
# 数组按原始参数边界展开，--noconfirm 支持无人值守镜像构建。
pacman -S --noconfirm "${packages[@]}"
