#!/usr/bin/env bash
# 显式生成新增归档依赖，供随后 SOURCES_BUILD=OFF 的应用构建消费。
# 这是 CI 的独立准备阶段，Find 模块仍对缺包直接报错，不自动源码回退。
set -euo pipefail
# 参数沿用已有平台构建脚本，避免复制编译器、sysroot 和 ABI 标签推导。
# 调用形式：prepare-archive-prebuilts.sh <平台构建脚本> [该脚本的平台选项]
if [[ $# -lt 1 ]]; then
    echo "需要指定平台构建脚本" >&2
    exit 2
fi
script="$1"
shift
# 入口固定在仓库根执行；不依赖 Runner 当前目录或用户 shell 环境。
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${root}"
# 脚本名称构成独立缓存名，Linux 不同编译器通过配置参数摘要隔离。
# 保留失败构建树供诊断，禁止自动清理其他任务的构建缓存。
key="$(printf '%s\n' "${script}" "$@" | cksum | awk '{print $1}')"
# Debug 与发布库分别生成，满足多配置消费者的查找约定。
for config in Debug RelWithDebInfo; do
    buildDir="${root}/build_archive_ci_${key}_${config}"
    # 平台脚本仅配置源码工程，不构建应用或其余预编译依赖。
    # 明确关闭 PGO，第三方压缩库不能带业务插桩。
    # 当前发布 CI 使用静态链接，末尾参数固定该类型，与下方暂存坐标一致。
    bash "${script}" "$@" --sources-build --configure-only \
        --no-pgo-instrument --linkage static --build-type "${config}" --build-dir "${buildDir}"
    # 仅构建归档依赖闭包，避免重新编译音频、渲染等无关第三方库。
    cmake --build "${buildDir}" --target libarchive_project --parallel
    # 坐标读取实际配置缓存，不把宿主 OS 当成交叉编译目标平台。
    cache="${buildDir}/CMakeCache.txt"
    # 每个键只读取精确匹配的一行；缺失时由暂存入口拒绝空坐标。
    cacheValue() {
        sed -n "s/^${1}:[^=]*=//p" "${cache}" | head -n 1
    }
    # ABI 标签由既有平台脚本负责生成，包含 AppleClang 的实际主版本。
    platform="$(cacheValue PROJECT_PREBUILT_PLATFORM)"
    arch="$(cacheValue PROJECT_PREBUILT_ARCH)"
    # 现有 Windows 脚本仅支持 x86_64；原生 Linux 默认跟随宿主架构。
    if [[ -z "${arch}" ]]; then
        if [[ "${platform}" == windows ]]; then arch=x86_64; else arch="$(uname -m)"; fi
    fi
    toolchain="$(cacheValue PROJECT_PREBUILT_TOOLCHAIN)"
    tag="$(cacheValue PROJECT_PREBUILT_COMPILER_TAG)"
    # 本阶段只写入五个新增包，现有预编译资产保持原有来源。
    cmake -DBUILD_DIR="${buildDir}" -DPLATFORM="${platform}" -DARCH="${arch}" \
        -DTOOLCHAIN="${toolchain}" -DCOMPILER_TAG="${tag}" -DCONFIG="${config}" \
        -DLINKAGE=static -P "${root}/cmake/StageArchivePrebuilts.cmake"
done
