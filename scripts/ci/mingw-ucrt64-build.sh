#!/usr/bin/env bash
# 在 MSYS2 UCRT64 Runner 上使用 GCC ABI 预编译库执行完整构建与测试。
# 入口只拉取 ucrt64 静态依赖，并关闭不适用于 GCC 的 LLVM PGO 插桩。
# 独立 build_gcc 防止与 clang64 的 libc++ 产物或缓存混用。
set -euo pipefail

# 取主机核心数的四分之三，降低链接峰值对 Runner 保活的影响。
detectCiBuildJobs() {
    # 探测失败时仍允许串行构建。
    local maxThreads=1

    # 兼容 MSYS2 常见的 nproc、getconf 和 Windows 环境变量来源。
    if command -v nproc >/dev/null 2>&1; then
        maxThreads="$(nproc)"
    elif command -v getconf >/dev/null 2>&1; then
        maxThreads="$(getconf _NPROCESSORS_ONLN)"
    elif [[ -n "${NUMBER_OF_PROCESSORS:-}" ]]; then
        maxThreads="${NUMBER_OF_PROCESSORS}"
    fi

    # 输入必须是正整数才能进入 Bash 算术表达式。
    if [[ ! "${maxThreads}" =~ ^[0-9]+$ ]] || (( maxThreads < 1 )); then
        maxThreads=1
    fi

    # 向下取整后再次钳制，单核 Runner 不会得到零并发。
    local buildJobs=$(( maxThreads * 3 / 4 ))
    if (( buildJobs < 1 )); then
        buildJobs=1
    fi

    # 仅输出数字，避免污染调用方命令替换结果。
    printf "%s\n" "${buildJobs}"
}

# 构建并发与 helper 绝对目录在任何外部命令前确定。
ciBuildJobs="$(detectCiBuildJobs)"
scriptDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# helper 路径不依赖 Runner 当前工作目录。
# 初始化依赖仓后按 ucrt64 ABI 拉取必要 LFS 对象。
git submodule update --init --recursive
# LFS 切片包含测试资源，避免夹具仍是指针文本。
bash "${scriptDir}/pull-lfs-for-build.sh" \
    --platform windows \
    --arch x86_64 \
    --toolchain mingw \
    --compiler-tag ucrt64 \
    --build-type RelWithDebInfo \
    --linkage static \
    --include-tests

# 删除专用 CI 构建树，确保不复用旧工具链缓存。
rm -rf build_gcc
# 配置使用 RelWithDebInfo，并与拉取的预编译二进制目录一致。
# 源码与构建目录显式固定，避免调用位置影响生成路径。
# CI 构建不得写入 Runner 的用户配置目录。
cmake -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DSOURCES_BUILD=OFF \
    -DBUILD_TESTING=ON \
    -DMMM_SYNC_TRANSLATIONS_AND_DEFAULT_SKIN=OFF \
    -DMMM_PGO_INSTRUMENT=OFF \
    -DMMM_PGO_USE=OFF \
    -S . \
    -B build_gcc
# 完整构建成功后执行全部已注册回归测试。
cmake --build build_gcc --parallel "${ciBuildJobs}"
# --output-on-failure 保留失败测试诊断，同时成功日志保持简洁。
ctest --test-dir build_gcc --output-on-failure
