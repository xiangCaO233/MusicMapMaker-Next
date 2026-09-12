#!/usr/bin/env bash
# 在 MSYS2 CLANG64 Runner 上使用已发布静态预编译库执行完整构建与测试。
# 该入口负责拉取精确 LFS 切片、创建独立构建树并隔离用户配置写入。
# PGO 插桩在 clang64 构建启用，用于产出可分发的采样版本。
set -euo pipefail

# 计算 CI 构建并发度，保留约四分之一核心供 Runner 服务和链接峰值。
detectCiBuildJobs() {
    # 无法探测时使用单线程安全默认值。
    local maxThreads=1

    # 优先使用 GNU coreutils，其次 POSIX getconf 与 Windows 环境变量。
    if command -v nproc >/dev/null 2>&1; then
        maxThreads="$(nproc)"
    elif command -v getconf >/dev/null 2>&1; then
        maxThreads="$(getconf _NPROCESSORS_ONLN)"
    elif [[ -n "${NUMBER_OF_PROCESSORS:-}" ]]; then
        maxThreads="${NUMBER_OF_PROCESSORS}"
    fi

    # 拒绝空值、非数字和零，避免算术展开失败。
    if [[ ! "${maxThreads}" =~ ^[0-9]+$ ]] || (( maxThreads < 1 )); then
        maxThreads=1
    fi

    # 整数运算向下取整，小型 Runner 最终仍至少保留一个 job。
    local buildJobs=$(( maxThreads * 3 / 4 ))
    if (( buildJobs < 1 )); then
        buildJobs=1
    fi

    # stdout 仅输出数值，供命令替换安全读取。
    printf "%s\n" "${buildJobs}"
}

# 并发度只探测一次，配置、构建和测试共享同一 Runner 状态。
ciBuildJobs="$(detectCiBuildJobs)"
# 绝对脚本目录用于从任意工作目录定位同级 LFS helper。
scriptDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# 子模块必须先就绪，LFS helper 才能遍历主仓与自维护引擎仓。
git submodule update --init --recursive
# 只拉取 clang64、静态 RelWithDebInfo、资源和测试所需对象。
# include-tests 同时覆盖测试夹具，确保完整 CTest 不因 LFS 指针失败。
bash "${scriptDir}/pull-lfs-for-build.sh" \
    --platform windows \
    --arch x86_64 \
    --toolchain mingw \
    --compiler-tag clang64 \
    --build-type RelWithDebInfo \
    --linkage static \
    --include-tests

# CI 构建树可安全重建，避免旧缓存混入其他 MinGW ABI。
rm -rf build_clang
# 配置使用 RelWithDebInfo，并与拉取的预编译二进制目录一致。
# 源码根和构建根显式给出，避免依赖调用者目录推断。
# CI 构建不得写入 Runner 的用户配置目录。
cmake -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DSOURCES_BUILD=OFF \
    -DBUILD_TESTING=ON \
    -DMMM_SYNC_TRANSLATIONS_AND_DEFAULT_SKIN=OFF \
    -DMMM_PGO_INSTRUMENT=ON \
    -DMMM_PGO_USE=OFF \
    -S . \
    -B build_clang
# 完整构建后运行构建树中注册的全部测试。
cmake --build build_clang --parallel "${ciBuildJobs}"
ctest --test-dir build_clang --output-on-failure
