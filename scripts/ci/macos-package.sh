#!/usr/bin/env bash
# 构建原生 macOS 应用，并生成 DMG、ZIP 与独立更新器发布产物。
# 脚本只编排已有构建和 CPack 配置，不改变项目源码或预编译依赖。
# 所有相对路径均以项目根解析，确保本地终端与 CI 的行为一致。
# --package-only 用于复用已验证构建树，--fresh 仅作用于实际构建流程。
# 签名身份默认采用 ad-hoc，可通过参数或环境变量显式覆盖。
set -euo pipefail

# 输出命令行契约与环境变量入口，不执行构建或文件写入。
showUsage() {
    cat <<'EOF'
Usage: scripts/ci/macos-package.sh [options]

Build the native macOS app bundle and package DMG and ZIP release artifacts.

Options:
  --arch <arm64|x86_64>      Target architecture. Default: host architecture
  --build-dir <path>         Build directory. Default: build_macos_package_<arch>
  --package-dir <path>       Package output directory. Default: <build-dir>/packages
  --build-type <type>        CMake build type. Default: RelWithDebInfo
  --deployment-target <ver>  Minimum macOS deployment target. Default: 11.0
  --jobs <count>             Parallel build jobs passed to macos-build.sh
  --codesign-identity <name> Code-signing identity. Default: - (ad-hoc)
  --no-codesign              Do not sign the generated app bundle
  --package-only             Skip configure/build and package an existing build
  --fresh                    Remove the build directory before configuring
  -h, --help                 Show this help

Environment overrides:
  MACOS_PREBUILT_ARCH       Default target architecture
  MACOSX_DEPLOYMENT_TARGET  Default minimum macOS deployment target
  MACOS_CODESIGN_IDENTITY   Default code-signing identity
EOF
}

# 将用户路径解析为稳定的项目绝对路径。
projectPath() {
    local inputPath="$1"

    if [[ "${inputPath}" = /* ]]; then
        # 绝对路径保持原样，便于 CI 把产物放到工作区外。
        printf "%s\n" "${inputPath}"
    else
        # 相对路径不依赖调用脚本时的当前目录。
        printf "%s/%s\n" "${projectRoot}" "${inputPath}"
    fi
}

# 规范化 CMake 与预编译布局共同接受的 macOS 架构名。
normalizeArchitecture() {
    case "$1" in
        arm64 | aarch64)
            # Apple Silicon 的多个常用名称统一为 arm64。
            printf "arm64\n"
            ;;
        x86_64 | amd64)
            # Intel 64 位别名统一为 x86_64。
            printf "x86_64\n"
            ;;
        *)
            # 不支持的值在创建构建目录前立即失败。
            printf "error: unsupported macOS architecture: %s\n" "$1" >&2
            exit 1
            ;;
    esac
}

# 检查带值选项是否仍有下一个命令行参数。
requireOptionValue() {
    local optionName="$1"
    local argumentCount="$2"

    if (( argumentCount < 2 )); then
        # 统一错误格式，避免各选项分支重复实现边界判断。
        printf "error: %s requires a value\n" "${optionName}" >&2
        exit 1
    fi
}

# 确认发布阶段依赖的宿主命令可从 PATH 调用。
requireCommand() {
    local commandName="$1"

    if ! command -v "${commandName}" >/dev/null 2>&1; then
        # 命令缺失属于环境错误，不尝试隐式安装。
        printf "error: required command not found: %s\n" "${commandName}" >&2
        exit 1
    fi
}

# 通过脚本自身位置定位仓库，允许从任意目录调用。
scriptDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
projectRoot="$(cd "${scriptDir}/../.." && pwd)"

# CPack 的 macOS 生成器和 codesign 必须运行在 Darwin 宿主。
if [[ "$(uname -s)" != "Darwin" ]]; then
    printf "error: scripts/ci/macos-package.sh must run on macOS\n" >&2
    exit 1
fi

# 在解析耗时构建选项前验证核心打包工具。
requireCommand cpack

# 架构默认跟随环境覆盖或当前宿主。
targetArch="${MACOS_PREBUILT_ARCH:-$(uname -m)}"
# 空路径在参数解析后按规范名称补齐。
buildDir=""
packageDir=""
# 发布包默认保留调试信息。
buildType="RelWithDebInfo"
# 最低系统版本同时约束构建与最终产物。
deploymentTarget="${MACOSX_DEPLOYMENT_TARGET:-11.0}"
buildJobs=""
# 单个连字符表示 ad-hoc 签名。
codesignIdentity="${MACOS_CODESIGN_IDENTITY:--}"
packageOnly=0
freshBuild=0

# 先完整解析参数，再派生目录或触发构建。
while (( $# > 0 )); do
    case "$1" in
        --arch)
            # 目标架构随后会经过别名规范化。
            requireOptionValue "$1" "$#"
            targetArch="$2"
            shift 2
            ;;
        --build-dir)
            # 构建目录可为项目相对路径或绝对路径。
            requireOptionValue "$1" "$#"
            buildDir="$2"
            shift 2
            ;;
        --package-dir)
            # 包目录独立于构建目录，便于 CI 收集 artifacts。
            requireOptionValue "$1" "$#"
            packageDir="$2"
            shift 2
            ;;
        --build-type)
            # 配置名同时传给构建脚本和 CPack。
            requireOptionValue "$1" "$#"
            buildType="$2"
            shift 2
            ;;
        --deployment-target)
            # 部署目标必须与预编译依赖的兼容下限一致。
            requireOptionValue "$1" "$#"
            deploymentTarget="$2"
            shift 2
            ;;
        --jobs)
            # 并行度只传递给实际构建，不影响 package-only。
            requireOptionValue "$1" "$#"
            buildJobs="$2"
            shift 2
            ;;
        --codesign-identity)
            # 非空身份用于应用构建以及独立更新器签名。
            requireOptionValue "$1" "$#"
            codesignIdentity="$2"
            shift 2
            ;;
        --no-codesign)
            # 空身份是禁用签名的唯一内部表示。
            codesignIdentity=""
            shift
            ;;
        --package-only)
            # 复用现有 CPackConfig，不重新配置或编译。
            packageOnly=1
            shift
            ;;
        --fresh)
            # 清理职责委托给 macos-build.sh。
            freshBuild=1
            shift
            ;;
        -h | --help)
            # 帮助路径不要求现有构建树。
            showUsage
            exit 0
            ;;
        *)
            # 未知参数作为调用错误处理，并附带完整用法。
            printf "error: unknown option: %s\n" "$1" >&2
            showUsage >&2
            exit 1
            ;;
    esac
done

# 目录名称只使用规范架构值。
targetArch="$(normalizeArchitecture "${targetArch}")"
if [[ -n "${buildJobs}" ]] &&
    { [[ ! "${buildJobs}" =~ ^[0-9]+$ ]] || (( buildJobs < 1 )); }; then
    # CMake 并行度必须是正整数，空值表示使用下游默认值。
    printf "error: --jobs must be a positive integer\n" >&2
    exit 1
fi
if (( packageOnly && freshBuild )); then
    # 复用与重建语义互斥，避免已有构建被误清理。
    printf "error: --package-only and --fresh cannot be used together\n" >&2
    exit 1
fi

if [[ -z "${buildDir}" ]]; then
    # 默认目录编码架构，防止两种原生产物互相覆盖。
    buildDir="build_macos_package_${targetArch}"
fi
buildDir="$(projectPath "${buildDir}")"

if [[ -z "${packageDir}" ]]; then
    # 默认把发布物集中在对应构建树的 packages 子目录。
    packageDir="${buildDir}/packages"
else
    # 显式包目录仍按项目根解析。
    packageDir="$(projectPath "${packageDir}")"
fi

if (( !packageOnly )); then
    # 静态链接确保应用包不依赖仓库内第三方动态库。
    buildArgs=(
        --arch "${targetArch}"
        --build-dir "${buildDir}"
        --build-type "${buildType}"
        --deployment-target "${deploymentTarget}"
        --linkage static
    )
    if [[ -n "${buildJobs}" ]]; then
        # 仅在用户给出并行度时覆盖构建脚本的自动选择。
        buildArgs+=(--jobs "${buildJobs}")
    fi
    if (( freshBuild )); then
        # fresh 标志保持为无值开关原样传递。
        buildArgs+=(--fresh)
    fi

    # 用单次命令环境覆盖签名身份，避免污染调用者环境。
    MACOS_CODESIGN_IDENTITY="${codesignIdentity}" \
        "${scriptDir}/macos-build.sh" "${buildArgs[@]}"
fi

# CPackConfig 是配置阶段成功完成的最低可验证产物。
cpackConfig="${buildDir}/CPackConfig.cmake"
if [[ ! -f "${cpackConfig}" ]]; then
    # package-only 下缺失配置时给出直接恢复提示。
    printf "error: CPack configuration not found: %s\n" "${cpackConfig}" >&2
    printf "hint: omit --package-only to configure and build first\n" >&2
    exit 1
fi

# 两种生成器共享同一输出目录和构建配置。
mkdir -p "${packageDir}"
# DragNDrop 生成面向用户安装的 DMG。
cpack --config "${cpackConfig}" \
    -G DragNDrop \
    -C "${buildType}" \
    -B "${packageDir}"
# ZIP 保留便于自动化分发和解压验证的应用包。
cpack --config "${cpackConfig}" \
    -G ZIP \
    -C "${buildType}" \
    -B "${packageDir}"

# 更新器独立于应用包发布，便于安装器原地替换主程序。
updaterSource="${buildDir}/bin/MusicMapMaker-Updater"
updaterOutput="${packageDir}/MusicMapMaker-Updater"
if [[ ! -f "${updaterSource}" ]]; then
    # 缺失更新器意味着发布集合不完整，禁止静默跳过。
    printf "error: updater executable not found: %s\n" "${updaterSource}" >&2
    exit 1
fi
# 使用复制保留构建树原件，package 目录可以单独上传。
cp "${updaterSource}" "${updaterOutput}"
if [[ -n "${codesignIdentity}" ]]; then
    # 独立二进制不会由 CPack 的应用签名流程覆盖。
    requireCommand codesign
    codesign --force --sign "${codesignIdentity}" "${updaterOutput}"
fi

# 最终清单限制为本脚本负责的三类发布文件。
printf "macOS package output:\n"
find "${packageDir}" -maxdepth 1 -type f \
    \( -name '*.dmg' -o -name '*.zip' -o -name 'MusicMapMaker-Updater' \) \
    -print | sort

# 维护约束：新增生成器时必须确认产物仍能由 CI artifact 规则收集。
# 维护约束：构建配置和 CPack 配置必须始终使用同一个 buildType。
# 维护约束：架构目录不得混放 arm64 与 x86_64 应用。
# 维护约束：部署目标不得低于预编译依赖的最低兼容版本。
# 维护约束：package-only 不得隐式修改现有 CMake cache。
# 维护约束：fresh 只由构建脚本实现，打包层不直接删除目录。
# 维护约束：签名关闭时不得调用 codesign。
# 维护约束：非空签名身份必须同时覆盖应用与独立更新器。
# 维护约束：发布脚本不负责公证、上传或创建 GitHub Release。
# 维护约束：CPack 失败后不得继续复制不完整发布集合。
# 维护约束：输出文件列表保持排序，便于 CI 日志稳定比较。
# 维护约束：路径中包含空格时必须继续使用数组和双引号传参。
# 维护约束：新增参数必须同步 showUsage 和缺值检查。
# 维护约束：脚本不修改源码、依赖归档或 Git 工作树。
# 维护约束：成功结束表示 DMG、ZIP 和更新器均已生成。
# 维护约束：应用包名称由 CPack 配置决定，本脚本不硬编码重命名。
# 维护约束：独立更新器的名称属于网站发布协议的一部分。
# 维护约束：更新器复制后必须保留可执行权限。
# 维护约束：自定义 package-dir 不应反向改变构建树位置。
# 维护约束：同一目录重复打包前应由 CI 管理旧产物清理。
# 维护约束：本脚本不猜测或选择钥匙串中的签名身份。
# 维护约束：正式签名环境应预先解锁所需钥匙串。
# 维护约束：ad-hoc 签名只提供本地完整性，不表示开发者认证。
# 维护约束：公证流程应消费这里生成且已正式签名的产物。
# 维护约束：发布前需分别验证两个架构的原生启动能力。
# 维护约束：ZIP 与 DMG 应来自同一次未经修改的构建树。
# 维护约束：CPack 输出异常时以其退出状态为准，不解析文本日志。
# 维护约束：脚本成功后调用方仍需校验哈希并执行上传。
# 维护约束：任何新发布文件都应加入末尾清单过滤条件。
