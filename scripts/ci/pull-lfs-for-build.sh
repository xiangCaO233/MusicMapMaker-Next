#!/usr/bin/env bash
# 按单一预编译 ABI 坐标精确拉取构建所需 Git LFS 对象。
# include 列表始终包含公共头、运行资源和主程序图标，可选包含测试资源。
# 静态与动态链接分别选择不同 libs/bin 目录结构。
# 发布型配置优先使用 RelWithDebInfo，仓库缺失时才回退 Release。
# --dry-run 只输出最终 include 规则，不访问 LFS 远端。
set -euo pipefail

# 输出所有必需选择器和可选行为开关。
showUsage() {
    cat <<'EOF'
Usage: scripts/ci/pull-lfs-for-build.sh [options]

Pull only the Git LFS objects required by one prebuilt toolchain.

Required options:
  --platform <windows|linux|macos>
  --arch <arch>
  --toolchain <name>
  --compiler-tag <tag>
  --build-type <type>
  --linkage <static|shared>

Optional:
  --include-tests    Include tests/data objects.
  --dry-run          Print the exact include list without pulling.
  -h, --help         Show this help.
EOF
}

# 检查带值选项是否还有下一个参数。
requireValue() {
    if (( $# < 2 )); then
        # 在更新内部状态前报告缺值选项。
        printf "error: %s requires a value\n" "$1" >&2
        exit 1
    fi
}

# 限制选择器字符集，防止构造越界 glob 或路径层级。
validateSelector() {
    local selectorName="$1"
    local selectorValue="$2"

    if [[ "${selectorValue}" == "." || "${selectorValue}" == ".." || \
        ! "${selectorValue}" =~ ^[A-Za-z0-9._+-]+$ ]]; then
        # 单点和双点即使符合字符集也不得作为目录选择器。
        printf "error: invalid %s selector: %s\n" "${selectorName}" "${selectorValue}" >&2
        exit 1
    fi
}

# 规范化预编译布局支持的常见架构别名。
normalizeArchitecture() {
    case "$1" in
        x86_64 | amd64)
            # AMD64 别名统一进入 x86_64 目录。
            printf "x86_64\n"
            ;;
        arm64 | aarch64)
            # AArch64 别名统一进入 arm64 目录。
            printf "arm64\n"
            ;;
        *)
            # 其他架构保留原值，交给 tracked 对象存在性验证。
            printf "%s\n" "$1"
            ;;
    esac
}

# 空值确保所有必需选择器都必须由参数明确提供。
platform=""
architecture=""
toolchain=""
compilerTag=""
buildType=""
linkage=""
includeTests=0
dryRun=0

# 在查询 Git 前完整解析所有参数。
while (( $# > 0 )); do
    case "$1" in
        --platform)
            # 平台随后限制为 windows、linux 或 macos。
            requireValue "$@"
            platform="$2"
            shift 2
            ;;
        --arch)
            # 架构在解析完成后统一规范化。
            requireValue "$@"
            architecture="$2"
            shift 2
            ;;
        --toolchain)
            # toolchain 对应预编译目录中的 ABI 家族层。
            requireValue "$@"
            toolchain="$2"
            shift 2
            ;;
        --compiler-tag)
            # compiler tag 对应具体编译器产物集合。
            requireValue "$@"
            compilerTag="$2"
            shift 2
            ;;
        --build-type)
            # 请求配置将映射为实际可用预编译配置。
            requireValue "$@"
            buildType="$2"
            shift 2
            ;;
        --linkage)
            # 链接方式决定是否进入 shared 子目录。
            requireValue "$@"
            linkage="$2"
            shift 2
            ;;
        --include-tests)
            # 测试数据可能体积较大，仅按调用方需要拉取。
            includeTests=1
            shift
            ;;
        --dry-run)
            # 诊断模式不要求安装 Git LFS。
            dryRun=1
            shift
            ;;
        -h | --help)
            # 帮助成功退出且无需仓库对象。
            showUsage
            exit 0
            ;;
        *)
            # 未知参数作为调用错误处理。
            printf "error: unknown option: %s\n" "$1" >&2
            showUsage >&2
            exit 1
            ;;
    esac
done

# 通过间接展开统一检查全部必需选择器。
for requiredValue in platform architecture toolchain compilerTag buildType linkage; do
    if [[ -z "${!requiredValue}" ]]; then
        printf "error: --%s is required\n" "${requiredValue}" >&2
        exit 1
    fi
done

case "${platform}" in
    windows | linux | macos)
        # 平台值直接映射预编译 binaries 第一层目录。
        ;;
    *)
        printf "error: unsupported platform: %s\n" "${platform}" >&2
        exit 1
        ;;
esac

case "${linkage}" in
    static | shared)
        # 两种模式使用不同目录模板。
        ;;
    *)
        printf "error: unsupported linkage: %s\n" "${linkage}" >&2
        exit 1
        ;;
esac

case "${buildType}" in
    Debug | Release | RelWithDebInfo | MinSizeRel)
        # 接受标准单配置 CMake 构建类型。
        ;;
    *)
        printf "error: unsupported prebuilt build type: %s\n" "${buildType}" >&2
        exit 1
        ;;
esac

architecture="$(normalizeArchitecture "${architecture}")"
# 对所有进入 glob 的自由文本执行同一安全验证。
validateSelector platform "${platform}"
validateSelector architecture "${architecture}"
validateSelector toolchain "${toolchain}"
validateSelector compiler-tag "${compilerTag}"

scriptDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# 项目根由脚本位置解析，不依赖调用者工作目录。
projectRoot="$(cd "${scriptDir}/../.." && pwd)"

if ! git -C "${projectRoot}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    # LFS 路径选择必须针对实际 Git 工作树执行。
    printf "error: project root is not a Git working tree: %s\n" "${projectRoot}" >&2
    exit 1
fi

# 构造单一配置下静态或动态导入库的 tracked glob。
prebuiltLibraryPattern() {
    local configName="$1"

    if [[ "${linkage}" == "shared" ]]; then
        # 动态导入库在 compiler tag 后包含 shared 层。
        printf "3rdpty/prebuilts/binaries/%s/*/libs/%s/%s/%s/shared/%s/**\n" \
            "${platform}" "${architecture}" "${toolchain}" "${compilerTag}" "${configName}"
    else
        # 静态归档直接进入配置目录。
        printf "3rdpty/prebuilts/binaries/%s/*/libs/%s/%s/%s/%s/**\n" \
            "${platform}" "${architecture}" "${toolchain}" "${compilerTag}" "${configName}"
    fi
}

# 查询仓库索引是否至少跟踪一个匹配当前 ABI 的归档。
hasTrackedPrebuiltConfig() {
    local configName="$1"
    local libraryPattern
    local firstMatch
    libraryPattern="$(prebuiltLibraryPattern "${configName}")"
    # pathspec glob 在 Git 索引层判断，无需先下载 LFS 对象。
    firstMatch="$(git -C "${projectRoot}" ls-files -- ":(glob)${libraryPattern}" | sed -n '1p')"
    [[ -n "${firstMatch}" ]]
}

# 发布型配置优先使用 RelWithDebInfo，仅在仓库确实缺失时回退到 Release。
if [[ "${buildType}" == "Debug" ]]; then
    # Debug 请求不允许回退发布配置。
    prebuiltConfig="Debug"
elif hasTrackedPrebuiltConfig RelWithDebInfo; then
    # 所有发布型请求优先消费带调试信息产物。
    prebuiltConfig="RelWithDebInfo"
else
    # 只有仓库缺失匹配 RelWithDebInfo 对象时才回退 Release。
    prebuiltConfig="Release"
fi

if ! hasTrackedPrebuiltConfig "${prebuiltConfig}"; then
    # 没有任何匹配归档时禁止执行无效 LFS 拉取。
    printf "error: no tracked prebuilt objects for %s/%s/%s/%s/%s/%s\n" \
        "${platform}" "${architecture}" "${toolchain}" "${compilerTag}" \
        "${linkage}" "${prebuiltConfig}" >&2
    exit 1
fi

# 公共头、运行资源和应用图标是所有构建共享对象。
includes=(
    "3rdpty/prebuilts/headers/**"
    "assets/**"
    "Modules/Main/src/logo.svg"
)

if (( includeTests )); then
    # 测试数据只由启用测试的构建显式请求。
    includes+=("tests/data/**")
fi

if [[ "${linkage}" == "shared" ]]; then
    # 动态模式同时拉取导入库和运行时文件。
    includes+=(
        "3rdpty/prebuilts/binaries/${platform}/*/libs/${architecture}/${toolchain}/${compilerTag}/shared/${prebuiltConfig}/**"
        "3rdpty/prebuilts/binaries/${platform}/*/bin/${architecture}/${toolchain}/${compilerTag}/shared/${prebuiltConfig}/**"
    )
else
    # 静态模式只需要配置目录下的 libs。
    includes+=(
        "3rdpty/prebuilts/binaries/${platform}/*/libs/${architecture}/${toolchain}/${compilerTag}/${prebuiltConfig}/**"
    )
fi

includeList="$(IFS=,; printf "%s" "${includes[*]}")"

if (( dryRun )); then
    # 输出可直接审查或传给 git lfs pull 的逗号列表。
    printf "%s\n" "${includeList}"
    exit 0
fi

if ! git lfs version >/dev/null 2>&1; then
    # 真实拉取路径要求 Git LFS 已安装。
    printf "error: Git LFS is required for prebuilt mode\n" >&2
    exit 1
fi

printf "Git LFS: pulling %s/%s/%s/%s (%s, %s)\n" \
    "${platform}" "${architecture}" "${toolchain}" "${compilerTag}" \
    "${linkage}" "${prebuiltConfig}"
# 空 exclude 禁止用户全局配置意外排除请求对象。
git -C "${projectRoot}" lfs pull "--include=${includeList}" "--exclude="

# 维护约束：include 规则必须保持精确，禁止退化为全仓 LFS 拉取。
# 维护约束：所有路径选择器必须先通过字符集和点目录检查。
# 维护约束：包名通配层只允许匹配当前 ABI 下的多个依赖包。
# 维护约束：静态模式不得拉取 shared/bin 运行时目录。
# 维护约束：动态模式必须同时拉取导入库和运行时文件。
# 维护约束：头文件始终全量拉取以保持依赖 API 与二进制同步。
# 维护约束：assets 始终拉取，因为主程序运行和资源嵌入都需要它们。
# 维护约束：测试资源只在调用方显式要求时加入。
# 维护约束：Debug 缺失时不得回退到发布配置。
# 维护约束：发布型回退必须先依据 Git 索引确认配置目录存在。
# 维护约束：buildType 映射语义需与 Find 模块保持一致。
# 维护约束：dry-run 不得调用 git lfs version 或访问网络。
# 维护约束：脚本不执行 git lfs install 或修改仓库 attributes。
# 维护约束：脚本不检出分支、不更新索引也不提交文件。
# 维护约束：LFS 失败直接传播退出状态，构建不得继续。
# 维护约束：新增平台需先建立预编译布局与选择器白名单。
# 维护约束：新增链接模式需同步 pattern 和 include 数组。
# 维护约束：新增公共 LFS 资源需加入所有 ABI 的共享 includes。
# 维护约束：成功退出表示请求对象已拉取，不代表构建可通过。
# 维护约束：platform 选择器不能包含路径分隔符或 glob 元字符。
# 维护约束：architecture 选择器规范化后仍必须通过安全校验。
# 维护约束：toolchain 与 compiler tag 不能使用点目录绕过布局根。
# 维护约束：buildType 使用白名单，无需拼入自由文本 pathspec。
# 维护约束：linkage 使用白名单，无需接受自定义目录层。
# 维护约束：git ls-files 只检查 tracked 指针，不要求对象已下载。
# 维护约束：配置存在性以至少一个依赖归档为最低判定。
# 维护约束：缺失单个包的诊断仍由后续 Find 模块负责。
# 维护约束：RelWithDebInfo 优先级适用于 Release 和 MinSizeRel 请求。
# 维护约束：Debug 目录必须与发布型目录保持严格隔离。
# 维护约束：shared 模式的 bin 路径用于目标运行时文件。
# 维护约束：shared 模式的 libs 路径用于链接期导入库或共享库。
# 维护约束：静态模式不会请求 symbols 目录的旁路调试文件。
# 维护约束：外置符号的拉取策略应由需要它们的 CI 阶段单独扩展。
# 维护约束：逗号连接不得在单个 include pattern 内引入逗号。
# 维护约束：Git LFS include 列表顺序保持公共资源在前、ABI 资源在后。
# 维护约束：空 exclude 参数属于覆盖全局排除配置的显式契约。
# 维护约束：主程序 logo 是构建输入，不能仅依赖 assets 通配。
# 维护约束：调用方日志应记录最终选中的 prebuiltConfig。
# 维护约束：LFS 对象下载完成后仍需由 Git smudge 状态或构建验证消费。
# 维护约束：选择规则变更时必须同步各平台 build 脚本调用参数。
