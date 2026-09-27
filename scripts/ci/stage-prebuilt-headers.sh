#!/usr/bin/env bash
# 将第三方开发头复制到主仓与 IonCachyEngine 的预编译 headers 布局。
# 每个包的 include 目录先经过受限路径校验与重建，避免遗留已删除头文件。
# 目录复制排除 .git，单文件和 glob 复制统一使用只读普通文件权限。
# --scope 控制主仓或引擎仓写入边界，源构建树仅提供生成安装头。
# 脚本会删除目标包旧 include，因此所有删除路径必须落在两个允许根内。
set -euo pipefail

# 显示参数契约，不修改任何预编译目录。
showUsage() {
    cat <<'EOF'
Usage: scripts/ci/stage-prebuilt-headers.sh [options]

Copy development headers into the prebuilt header layout.

Options:
  --build-dir <path>      Source build directory that contains generated installs.
                          Default: build_linux_sources
  --scope <all|main|ice>  Staging scope. Default: all
  -h, --help              Show this help
EOF
}

# 将相对构建路径锚定到项目根。
projectPath() {
    local inputPath="$1"

    if [[ "${inputPath}" = /* ]]; then
        # 绝对路径保持原样，由后续 requireDir 验证。
        printf "%s\n" "${inputPath}"
    else
        # 相对路径不依赖调用者当前工作目录。
        printf "%s/%s\n" "${projectRoot}" "${inputPath}"
    fi
}

# 要求目录存在；缺失属于 staging 前置条件错误。
requireDir() {
    local directoryPath="$1"

    if [[ ! -d "${directoryPath}" ]]; then
        # 失败消息保留完整路径，便于定位未生成的安装步骤。
        printf "error: directory not found: %s\n" "${directoryPath}" >&2
        exit 1
    fi
}

# 要求单个源头文件存在。
requireFile() {
    local filePath="$1"

    if [[ ! -f "${filePath}" ]]; then
        # 不允许跳过清单内文件，否则预编译头集合将不完整。
        printf "error: file not found: %s\n" "${filePath}" >&2
        exit 1
    fi
}

# 清空一个包的目标 include 目录，并严格限制可删除路径。
resetIncludeDir() {
    local prebuiltRoot="$1"
    local packageName="$2"
    local includeDir="${prebuiltRoot}/headers/${packageName}/include"

    # 字符串白名单覆盖主仓与自维护引擎仓两个预编译根。
    case "${includeDir}" in
        "${projectRoot}/3rdpty/prebuilts/headers/"* | "${projectRoot}/3rdpty/sources/IonCachyEngine/3rdpty/prebuilts/headers/"*)
            # 只有 headers/<package>/include 后代允许递归清理。
            ;;
        *)
            # 路径拼接异常时在 rm 前立即停止。
            printf "error: refusing to remove unexpected include directory: %s\n" "${includeDir}" >&2
            exit 1
            ;;
    esac

    # 删除旧目录可移除上游已经废弃的头，避免增量复制残留。
    rm -rf -- "${includeDir}"
    # 空目标目录随后由具体复制 helper 填充。
    mkdir -p "${includeDir}"
}

# 递归复制一个 include 树的内容，不额外保留源根目录名。
copyDirContents() {
    local sourceDir="$1"
    local destinationDir="$2"

    # 先验证源，再创建目标，避免缺失源留下误导性空包。
    requireDir "${sourceDir}"
    mkdir -p "${destinationDir}"
    # 尾部斜杠表达复制目录内容；排除版本控制元数据。
    rsync -a --exclude='.git' "${sourceDir}/" "${destinationDir}/"
}

# 将完整源目录复制为目标父目录下的指定子目录。
copyDirAsChild() {
    local sourceDir="$1"
    local destinationParent="$2"
    local childName="$3"

    # 用于需要保留 glm/entt 等顶级 include 命名空间的包。
    copyDirContents "${sourceDir}" "${destinationParent}/${childName}"
}

# 复制一个必需头文件到目标 include 目录。
copyFile() {
    local sourceFile="$1"
    local destinationDir="$2"

    # 单文件复制保留 basename，不接受调用方重命名。
    requireFile "${sourceFile}"
    mkdir -p "${destinationDir}"
    # install 统一权限，生成头不携带源构建目录的执行位。
    install -m 0644 "${sourceFile}" "${destinationDir}/$(basename "${sourceFile}")"
}

# 复制源目录顶层匹配 glob 的全部头，并要求至少命中一个文件。
copyGlob() {
    local sourceDir="$1"
    local destinationDir="$2"
    local pattern="$3"
    # copied 标志区分空匹配与成功复制，避免 Bash glob 字面值问题。
    local copied=0
    local sourceFile

    requireDir "${sourceDir}"
    mkdir -p "${destinationDir}"
    # NUL 分隔完整支持文件名空格和特殊字符。
    while IFS= read -r -d '' sourceFile; do
        install -m 0644 "${sourceFile}" "${destinationDir}/$(basename "${sourceFile}")"
        copied=1
    done < <(find "${sourceDir}" -maxdepth 1 -type f -name "${pattern}" -print0)

    if (( ! copied )); then
        # 空匹配意味着上游布局变化，必须更新清单而非生成空包。
        printf "error: no files matched %s in %s\n" "${pattern}" "${sourceDir}" >&2
        exit 1
    fi
}

# 重建并复制一个以标准 include 树发布的包。
stageDirectoryPackage() {
    local prebuiltRoot="$1"
    local packageName="$2"
    local sourceDir="$3"

    # reset 与 copy 成对调用，保证目标精确镜像当前源头。
    resetIncludeDir "${prebuiltRoot}" "${packageName}"
    copyDirContents "${sourceDir}" "${prebuiltRoot}/headers/${packageName}/include"
    printf "staged %s\n" "${prebuiltRoot#${projectRoot}/}/headers/${packageName}/include"
}

# 收集主仓直接依赖的源码头与生成头。
stageMainSourceHeaders() {
    local prebuiltRoot="$1"
    local sourceRoot="${projectRoot}/3rdpty/sources"
    local includeDir

    # 标准 include 树可直接镜像：curl。
    stageDirectoryPackage "${prebuiltRoot}" "curl" "${sourceRoot}/curl/include"
    # EnTT 源树本身以 entt 为 include 根，需要保留子目录名。
    resetIncludeDir "${prebuiltRoot}" "entt"
    copyDirAsChild "${sourceRoot}/entt/src/entt" "${prebuiltRoot}/headers/entt/include" "entt"
    printf "staged %s\n" "${prebuiltRoot#${projectRoot}/}/headers/entt/include"
    # FreeType、GLFW、NFD、JSON 与 sol2 均提供标准公开 include 目录。
    stageDirectoryPackage "${prebuiltRoot}" "freetype" "${sourceRoot}/freetype/include"
    stageDirectoryPackage "${prebuiltRoot}" "glfw" "${sourceRoot}/glfw/include"
    stageDirectoryPackage "${prebuiltRoot}" "nativefiledialog-extended" "${sourceRoot}/nativefiledialog-extended/src/include"
    stageDirectoryPackage "${prebuiltRoot}" "nlohmann_json" "${sourceRoot}/nlohmann_json/include"
    stageDirectoryPackage "${prebuiltRoot}" "sol2" "${sourceRoot}/sol2/include"
    # 自维护引擎公开头作为主仓依赖包发布。
    stageDirectoryPackage "${prebuiltRoot}" "IonCachyEngine" "${sourceRoot}/IonCachyEngine/include"
    # mbedTLS 额外附带项目固定的用户配置头。
    stageDirectoryPackage "${prebuiltRoot}" "mbedtls" "${sourceRoot}/mbedtls/include"
    copyFile "${sourceRoot}/cmake/mbedtls-user-config.h" "${prebuiltRoot}/headers/mbedtls/include"
    # libdatachannel 与 libjuice 分别复制各自公开 API。
    stageDirectoryPackage "${prebuiltRoot}" "libdatachannel" "${sourceRoot}/libdatachannel/include"
    stageDirectoryPackage "${prebuiltRoot}" "libjuice" "${sourceRoot}/libdatachannel/deps/libjuice/include"

    # usrsctp 对外只需要单一 usrsctp.h，不复制内部源码头。
    resetIncludeDir "${prebuiltRoot}" "usrsctp"
    copyFile "${sourceRoot}/libdatachannel/deps/usrsctp/usrsctplib/usrsctp.h" "${prebuiltRoot}/headers/usrsctp/include"
    printf "staged %s\n" "${prebuiltRoot#${projectRoot}/}/headers/usrsctp/include"

    # ImGuiFileDialog 公开头通过前缀 glob 收集。
    resetIncludeDir "${prebuiltRoot}" "ImGuiFileDialog"
    includeDir="${prebuiltRoot}/headers/ImGuiFileDialog/include"
    copyGlob "${sourceRoot}/ImGuiFileDialog" "${includeDir}" "ImGuiFileDialog*.h"
    printf "staged %s\n" "${prebuiltRoot#${projectRoot}/}/headers/ImGuiFileDialog/include"

    # Clay 是单头库，目标根直接放置 clay.h。
    resetIncludeDir "${prebuiltRoot}" "clay"
    copyFile "${sourceRoot}/clay/clay.h" "${prebuiltRoot}/headers/clay/include"
    printf "staged %s\n" "${prebuiltRoot#${projectRoot}/}/headers/clay/include"

    # concurrentqueue 的三个协作头必须作为同一包发布。
    resetIncludeDir "${prebuiltRoot}" "concurrentqueue"
    includeDir="${prebuiltRoot}/headers/concurrentqueue/include"
    copyFile "${sourceRoot}/concurrentqueue/concurrentqueue.h" "${includeDir}"
    copyFile "${sourceRoot}/concurrentqueue/blockingconcurrentqueue.h" "${includeDir}"
    copyFile "${sourceRoot}/concurrentqueue/lightweightsemaphore.h" "${includeDir}"
    printf "staged %s\n" "${prebuiltRoot#${projectRoot}/}/headers/concurrentqueue/include"

    # GLM 保留 glm 顶层目录以匹配 <glm/...> include 形式。
    resetIncludeDir "${prebuiltRoot}" "glm"
    copyDirAsChild "${sourceRoot}/glm/glm" "${prebuiltRoot}/headers/glm/include" "glm"
    printf "staged %s\n" "${prebuiltRoot#${projectRoot}/}/headers/glm/include"

    # ImGui 顶层头与 backends 公开头共同组成项目使用的 API。
    resetIncludeDir "${prebuiltRoot}" "imgui"
    includeDir="${prebuiltRoot}/headers/imgui/include"
    copyGlob "${sourceRoot}/imgui" "${includeDir}" "*.h"
    copyDirContents "${sourceRoot}/imgui/backends" "${includeDir}/backends"
    # backends 源目录含实现文件，预编译 headers 包仅保留 .h。
    find "${includeDir}/backends" -type f ! -name '*.h' -delete
    printf "staged %s\n" "${prebuiltRoot#${projectRoot}/}/headers/imgui/include"

    # ImPlot 只收集顶层公开头。
    resetIncludeDir "${prebuiltRoot}" "implot"
    copyGlob "${sourceRoot}/implot" "${prebuiltRoot}/headers/implot/include" "*.h"
    printf "staged %s\n" "${prebuiltRoot#${projectRoot}/}/headers/implot/include"

    # LuaJIT 公开 C API 位于 src 顶层，以 *.h 收集。
    resetIncludeDir "${prebuiltRoot}" "luajit"
    copyGlob "${sourceRoot}/luajit/src" "${prebuiltRoot}/headers/luajit/include" "*.h"
    printf "staged %s\n" "${prebuiltRoot#${projectRoot}/}/headers/luajit/include"

    # lunasvg 与其 plutovg 依赖共享一个对外 include 包。
    resetIncludeDir "${prebuiltRoot}" "lunasvg"
    includeDir="${prebuiltRoot}/headers/lunasvg/include"
    copyDirContents "${sourceRoot}/lunasvg/include" "${includeDir}"
    copyDirContents "${sourceRoot}/lunasvg/plutovg/include" "${includeDir}"
    printf "staged %s\n" "${prebuiltRoot#${projectRoot}/}/headers/lunasvg/include"

    # miniz 公开源码头加上构建生成的导出宏头。
    resetIncludeDir "${prebuiltRoot}" "miniz"
    includeDir="${prebuiltRoot}/headers/miniz/include"
    copyGlob "${sourceRoot}/miniz" "${includeDir}" "miniz*.h"
    copyFile "${buildDir}/3rdpty/sources/miniz_export.h" "${includeDir}"
    printf "staged %s\n" "${prebuiltRoot#${projectRoot}/}/headers/miniz/include"

    # stb 是头文件集合，完整复制顶层 *.h。
    resetIncludeDir "${prebuiltRoot}" "stb"
    copyGlob "${sourceRoot}/stb" "${prebuiltRoot}/headers/stb/include" "*.h"
    printf "staged %s\n" "${prebuiltRoot#${projectRoot}/}/headers/stb/include"
}

# 收集 IonCachyEngine 内部第三方依赖头，可写入主仓或引擎仓预编译根。
stageIceDependencyHeaders() {
    local prebuiltRoot="$1"
    local sourceRoot="${projectRoot}/3rdpty/sources/IonCachyEngine/3rdpty/sources"
    local includeDir

    # FFmpeg、FFTW、LAME、Rubber Band 与 zlib 使用构建安装前缀中的生成头。
    stageDirectoryPackage "${prebuiltRoot}" "ffmpeg" "${buildDir}/3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/include"
    stageDirectoryPackage "${prebuiltRoot}" "fftw" "${buildDir}/3rdpty/fftw_inst/include"
    # fmt、OpenAL、SDL 与 spdlog 可直接复制源码公开 include 树。
    stageDirectoryPackage "${prebuiltRoot}" "fmt" "${sourceRoot}/fmt/include"
    stageDirectoryPackage "${prebuiltRoot}" "lame" "${buildDir}/3rdpty/lame_inst/include"
    stageDirectoryPackage "${prebuiltRoot}" "openal" "${sourceRoot}/openal/include"
    stageDirectoryPackage "${prebuiltRoot}" "rubberband" "${buildDir}/rb_inst/include"
    stageDirectoryPackage "${prebuiltRoot}" "sdl" "${sourceRoot}/sdl/include"
    stageDirectoryPackage "${prebuiltRoot}" "spdlog" "${sourceRoot}/spdlog/include"
    stageDirectoryPackage "${prebuiltRoot}" "zlib" "${buildDir}/3rdpty/zlib_inst/include"

    # libsamplerate 只发布 samplerate.h，内部实现头不进入预编译包。
    resetIncludeDir "${prebuiltRoot}" "libsamplerate"
    includeDir="${prebuiltRoot}/headers/libsamplerate/include"
    copyFile "${sourceRoot}/libsamplerate/src/samplerate.h" "${includeDir}"
    printf "staged %s\n" "${prebuiltRoot#${projectRoot}/}/headers/libsamplerate/include"
}

# 绝对脚本目录用于稳定解析项目根。
scriptDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
projectRoot="$(cd "${scriptDir}/../.." && pwd)"

# 默认源构建树与 Linux headers staging 任务一致。
buildDir="build_linux_sources"
# 默认同时更新主仓与引擎仓头集合。
stageScope="all"

# 参数校验发生在任何 resetIncludeDir 删除之前。
while (( $# > 0 )); do
    case "$1" in
        --build-dir)
            # 允许指定包含生成安装头的其他构建树。
            if (( $# < 2 )); then
                printf "error: --build-dir requires a value\n" >&2
                exit 1
            fi
            buildDir="$2"
            shift 2
            ;;
        --scope)
            # scope 仅控制两个允许预编译根。
            if (( $# < 2 )); then
                printf "error: --scope requires a value\n" >&2
                exit 1
            fi
            stageScope="$2"
            shift 2
            ;;
        -h | --help)
            # 帮助路径不检查构建目录。
            showUsage
            exit 0
            ;;
        *)
            # 未知参数在任何文件操作前失败。
            printf "error: unknown option: %s\n" "$1" >&2
            showUsage >&2
            exit 1
            ;;
    esac
done

case "${stageScope}" in
    all | main | ice)
        # 合法 scope 在下方两个分支中解释。
        ;;
    *)
        printf "error: --scope must be one of all, main, ice: %s\n" "${stageScope}" >&2
        exit 1
        ;;
esac

# 最终构建路径必须存在，防止生成头来源缺失。
buildDir="$(projectPath "${buildDir}")"
requireDir "${buildDir}"

if [[ "${stageScope}" = "all" || "${stageScope}" = "main" ]]; then
    # 主仓需要自身依赖头，也需要引擎依赖头供公共引擎 API 编译。
    stageMainSourceHeaders "${projectRoot}/3rdpty/prebuilts"
    stageIceDependencyHeaders "${projectRoot}/3rdpty/prebuilts"
fi

if [[ "${stageScope}" = "all" || "${stageScope}" = "ice" ]]; then
    # 引擎仓只接收其内部第三方依赖，不复制主应用专属头。
    stageIceDependencyHeaders "${projectRoot}/3rdpty/sources/IonCachyEngine/3rdpty/prebuilts"
fi

# 维护约束：每个包的 headers/<package>/include 必须独立可消费。
# 维护约束：代理头和自动生成的转发目录不得混入上游公开头包。
# 维护约束：目标 include 目录重建前必须通过允许根路径白名单。
# 维护约束：任何新增 rm 操作都必须保持同等或更严格的路径校验。
# 维护约束：目录复制必须排除 .git 与其他版本控制元数据。
# 维护约束：公开头文件统一使用 0644 权限，不保留源执行位。
# 维护约束：单头库只复制公开入口，不扩散内部实现辅助文件。
# 维护约束：头文件 glob 空匹配必须失败，不能生成空依赖包。
# 维护约束：生成头必须来自与二进制归档相同的 buildDir。
# 维护约束：第三方安装前缀中的配置头需要与源码头一起复制。
# 维护约束：保留 glm、entt 等库要求的顶层 include 命名空间。
# 维护约束：ImGui backends 包只保留头，不复制后端实现源码。
# 维护约束：复合包必须包含其公开 API 直接引用的协作头。
# 维护约束：主仓预编译根需要包含 IonCachyEngine 的公开头。
# 维护约束：引擎内部根只接收引擎直接依赖的第三方头。
# 维护约束：scope=main 不得修改引擎仓预编译目录。
# 维护约束：scope=ice 不得修改主仓预编译目录。
# 维护约束：scope=all 使用同一源码快照更新两个预编译根。
# 维护约束：脚本只读第三方源码和构建安装目录。
# 维护约束：脚本不自动运行 Git LFS、提交或推送操作。
# 维护约束：staging 后必须验证目标头均由 Git LFS 规则覆盖。
# 维护约束：新增包时需同步独立 Find 模块及源码 target 名称。
# 维护约束：删除上游头后重跑本脚本应同步移除旧目标文件。
# 维护约束：更新二进制时头文件应在依赖顺序上先行提交。
# 维护约束：修改公开 API 后需执行主仓与引擎仓消费构建。
# 维护约束：Windows 与 macOS 可复用头包，但生成头必须兼容目标。
# 维护约束：资源、文档和示例不属于预编译开发头复制范围。
# 维护约束：成功到达文件末尾表示两个 scope 分支均已完成。
# 维护约束：错误退出后可能已有部分包更新，提交前必须复跑完整范围。
