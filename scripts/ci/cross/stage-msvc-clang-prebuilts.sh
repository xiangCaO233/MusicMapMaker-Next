#!/usr/bin/env bash
# 将 clang-cl/MSVC 兼容构建产物复制到 Windows x86_64 预编译布局。
# 静态库和外置 PDB 分别进入 libs 与 symbols，配置和编译器标签保持一致。
# 主仓与 IonCachyEngine 使用独立目标根，由 --scope 控制写入边界。
# --packages 用于精确选择依赖，缺失被选中的归档时立即失败。
# 符号策略可要求外置 PDB，或声明 CodeView 已嵌入静态归档。
set -euo pipefail

# 输出 staging 参数契约，不读取构建树。
showUsage() {
    cat <<'EOF'
Usage: scripts/ci/cross/stage-msvc-clang-prebuilts.sh [options]

Copy clang-cl/MSVC source-build static libraries into the prebuilt layout.

Options:
  --build-dir <path>      Source build directory. Default: build_cross_msvc_sources
  --build-type <type>     Prebuilt config directory. Default: RelWithDebInfo
  --compiler-tag <tag>    Prebuilt compiler tag. Default: 2026
  --scope <all|main|ice>  Staging scope. Default: all
  --packages <list>       Comma-separated packages to stage. Default: all
  --strict-symbols        Fail when a Debug/RelWithDebInfo PDB cannot be found.
  --embedded-symbols      Remove stale PDBs because CodeView is embedded in archives.
  -h, --help              Show this help

Environment overrides:
  MSVC_PREBUILT_COMPILER_TAG  Default prebuilt compiler tag
EOF
}

# 将相对路径解析到项目根，绝对路径保持原值。
projectPath() {
    local inputPath="$1"

    if [[ "${inputPath}" = /* ]]; then
        # 外部 CI 构建树可直接传入绝对路径。
        printf "%s\n" "${inputPath}"
    else
        # 相对路径不依赖调用者当前目录。
        printf "%s/%s\n" "${projectRoot}" "${inputPath}"
    fi
}

# 从候选列表选择构建树内首个现有普通文件。
# 结果通过变量名回写，避免命令替换吞掉路径中的状态信息。
findSourceFile() {
    local outVar="$1"
    shift

    local resolvedPath=""
    # 空值保留到循环结束，供调用方统一生成错误信息。
    local candidatePath
    for candidatePath in "$@"; do
        # 候选顺序表达不同构建系统输出布局的优先级。
        if [[ -f "${buildDir}/${candidatePath}" ]]; then
            # 首次命中即停止，禁止混合不同候选目录的产物。
            resolvedPath="${buildDir}/${candidatePath}"
            break
        fi
    done

    printf -v "${outVar}" "%s" "${resolvedPath}"
}

# 复制一项 COFF 静态库到规范的 MSVC 预编译目录。
copyLib() {
    local prebuiltRoot="$1"
    local packageName="$2"
    local outputName="$3"
    shift 3

    if ! shouldStagePackage "${packageName}"; then
        # 包过滤器未选中时不检查源文件。
        return 0
    fi

    local sourcePath=""
    # 候选解析集中在 helper，保持错误报告使用原始路径列表。
    findSourceFile sourcePath "$@"

    if [[ -z "${sourcePath}" ]]; then
        # 请求项缺失意味着发布集合不完整。
        printf "error: source library not found for %s/%s\n" "${packageName}" "${outputName}" >&2
        printf "searched:\n" >&2
        local searchedPath
        # 回显全部候选，便于定位上游 target 输出名变化。
        for searchedPath in "$@"; do
            printf "  %s/%s\n" "${buildDir}" "${searchedPath}" >&2
        done
        exit 1
    fi

    local outputPath="${prebuiltRoot}/binaries/windows/${packageName}/libs/x86_64/msvc/${compilerTag}/${buildType}/${outputName}"
    # install 同时创建目录并移除源文件可能携带的执行权限。
    install -D -m 0644 "${sourcePath}" "${outputPath}"
    printf "staged %s\n" "${outputPath#${projectRoot}/}"
}

# 按 PDB 文件名在整个源构建树中寻找首个匹配项。
findPdbByName() {
    local outVar="$1"
    shift

    local foundPath=""
    # 调用方可按首选名到兼容名的顺序传入多个名称。
    local pdbName
    for pdbName in "$@"; do
        # print0 保证包含空格的 Windows 工具链路径可安全读取。
        while IFS= read -r -d '' foundPath; do
            printf -v "${outVar}" "%s" "${foundPath}"
            # 找到首个明确匹配后立即返回。
            return
        done < <(find "${buildDir}" -type f -name "${pdbName}" -print0 -quit)
    done

    printf -v "${outVar}" "%s" ""
}

# 按配置和符号策略复制或清理一项外置 PDB。
copyPdb() {
    local prebuiltRoot="$1"
    local packageName="$2"
    local outputName="$3"
    shift 3

    if ! shouldStagePackage "${packageName}"; then
        # 符号与其静态库遵循同一包过滤器。
        return 0
    fi

    case "${buildType}" in
        Debug | RelWithDebInfo)
            # 只有需要保留调试信息的配置管理外置 PDB。
            ;;
        *)
            # 其他配置不创建空 symbols 目录。
            return 0
            ;;
    esac

    local outputPath="${prebuiltRoot}/binaries/windows/${packageName}/symbols/x86_64/msvc/${compilerTag}/${buildType}/${outputName}"
    if (( embeddedSymbols )); then
        # 内嵌模式删除旧外置文件，防止发布陈旧 PDB。
        if [[ -f "${outputPath}" ]]; then
            # 删除范围仅限当前包和配置的解析后目标文件。
            rm -f -- "${outputPath}"
            printf "removed stale %s\n" "${outputPath#${projectRoot}/}"
        fi
        return 0
    fi

    local sourcePath=""
    # PDB 搜索使用清单提供的规范名称。
    findPdbByName sourcePath "$@"

    if [[ -z "${sourcePath}" ]]; then
        # 默认仅警告，严格模式把符号缺失提升为发布错误。
        local message="warning: PDB not found for ${packageName}/${outputName}"
        if (( strictSymbols )); then
            # 严格模式用于要求外置 PDB 完整的正式构建。
            printf "error: %s\n" "${message#warning: }" >&2
            exit 1
        fi
        printf "%s\n" "${message}" >&2
        return 0
    fi

    install -D -m 0644 "${sourcePath}" "${outputPath}"
    # 输出项目相对路径以保持 CI 日志可移植。
    printf "staged %s\n" "${outputPath#${projectRoot}/}"
}

# 主仓库复制包装器在 ice-only 模式跳过。
copyMainLib() {
    if [[ "${stageScope}" = "ice" ]]; then
        # 引擎独立 staging 不应修改主仓 LFS 内容。
        return 0
    fi

    copyLib "${projectRoot}/3rdpty/prebuilts" "$@"
}

# 引擎仓复制包装器在 main-only 模式跳过。
copyIceLib() {
    if [[ "${stageScope}" = "main" ]]; then
        # 主仓增量 staging 保持引擎预编译根不变。
        return 0
    fi

    copyLib "${projectRoot}/3rdpty/sources/IonCachyEngine/3rdpty/prebuilts" "$@"
}

# 主仓 PDB 包装器与静态库采用相同 scope 规则。
copyMainPdb() {
    if [[ "${stageScope}" = "ice" ]]; then
        # 不为跳过的主仓归档写入孤立符号文件。
        return 0
    fi

    copyPdb "${projectRoot}/3rdpty/prebuilts" "$@"
}

# 引擎 PDB 包装器与静态库采用相同 scope 规则。
copyIcePdb() {
    if [[ "${stageScope}" = "main" ]]; then
        # 不为跳过的引擎归档写入孤立符号文件。
        return 0
    fi

    copyPdb "${projectRoot}/3rdpty/sources/IonCachyEngine/3rdpty/prebuilts" "$@"
}

# 把主仓静态库和对应 PDB 作为一个清单项处理。
stageMainLibWithPdb() {
    local packageName="$1"
    local outputName="$2"
    local pdbName="$3"
    shift 3

    copyMainLib "${packageName}" "${outputName}" "$@"
    # PDB 搜索名由清单显式给出，不从 lib 名称猜测。
    copyMainPdb "${packageName}" "${pdbName}" "${pdbName}"
}

# 把引擎静态库和对应 PDB 作为一个清单项处理。
stageIceLibWithPdb() {
    local packageName="$1"
    local outputName="$2"
    local pdbName="$3"
    shift 3

    copyIceLib "${packageName}" "${outputName}" "$@"
    # 两个目标根共享符号选择策略。
    copyIcePdb "${packageName}" "${pdbName}" "${pdbName}"
}

# 通过脚本自身位置定位三层之外的项目根。
scriptDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
projectRoot="$(cd "${scriptDir}/../../.." && pwd)"

# 逗号包围精确匹配，避免短包名发生子串命中。
shouldStagePackage() {
    local packageName="$1"

    [[ -z "${packageFilter}" || ",${packageFilter}," == *",${packageName},"* ]]
}

# 默认值对应 Linux 上 clang-cl 的标准源码依赖任务。
buildDir="build_cross_msvc_sources"
buildType="RelWithDebInfo"
# 编译器标签进入 ABI 路径，允许 CI 环境覆盖。
compilerTag="${MSVC_PREBUILT_COMPILER_TAG:-2026}"
stageScope="all"
# 空包过滤器表示完整清单。
packageFilter=""
strictSymbols=0
embeddedSymbols=0

# 在任何复制或删除前完整解析所有选项。
while (( $# > 0 )); do
    case "$1" in
        --build-dir)
            # 支持项目相对路径和外部绝对构建树。
            if (( $# < 2 )); then
                printf "error: --build-dir requires a value\n" >&2
                exit 1
            fi
            buildDir="$2"
            shift 2
            ;;
        --build-type)
            # 配置名同时控制归档名称和符号目录。
            if (( $# < 2 )); then
                printf "error: --build-type requires a value\n" >&2
                exit 1
            fi
            buildType="$2"
            shift 2
            ;;
        --compiler-tag)
            # 标签必须对应生成这些 COFF 归档的 MSVC ABI 版本。
            if (( $# < 2 )); then
                printf "error: --compiler-tag requires a value\n" >&2
                exit 1
            fi
            compilerTag="$2"
            shift 2
            ;;
        --scope)
            # scope 控制两个仓库预编译根的写入边界。
            if (( $# < 2 )); then
                printf "error: --scope requires a value\n" >&2
                exit 1
            fi
            stageScope="$2"
            shift 2
            ;;
        --packages)
            # 列表采用英文逗号分隔的精确包名。
            if (( $# < 2 )); then
                printf "error: --packages requires a value\n" >&2
                exit 1
            fi
            packageFilter="$2"
            shift 2
            ;;
        --strict-symbols)
            # 正式外置符号发布可要求任一 PDB 缺失即失败。
            strictSymbols=1
            shift
            ;;
        --embedded-symbols)
            # 内嵌 CodeView 模式主动移除历史外置 PDB。
            embeddedSymbols=1
            shift
            ;;
        -h | --help)
            # 帮助路径不要求构建树存在。
            showUsage
            exit 0
            ;;
        *)
            # 未知参数作为调用错误并展示完整用法。
            printf "error: unknown option: %s\n" "$1" >&2
            showUsage >&2
            exit 1
            ;;
    esac
done

if [[ -z "${compilerTag}" ]]; then
    # 空标签会打破不同 MSVC ABI 产物的目录隔离。
    printf "error: --compiler-tag must not be empty\n" >&2
    exit 1
fi

if [[ -n "${packageFilter}" && ",${packageFilter}," == *",,"* ]]; then
    # 空包名通常来自尾随或重复逗号，应尽早拒绝。
    printf "error: --packages contains an empty package name: %s\n" "${packageFilter}" >&2
    exit 1
fi

case "${stageScope}" in
    all | main | ice)
        # 合法值由各 wrapper 解释。
        ;;
    *)
        # 未知范围可能写错仓库，禁止容错猜测。
        printf "error: --scope must be one of all, main, ice: %s\n" "${stageScope}" >&2
        exit 1
        ;;
esac

if (( strictSymbols && embeddedSymbols )); then
    # 要求外置 PDB 与声明仅有内嵌符号在语义上互斥。
    printf "error: --strict-symbols and --embedded-symbols cannot be used together\n" >&2
    exit 1
fi

buildDir="$(projectPath "${buildDir}")"

if [[ ! -d "${buildDir}" ]]; then
    # staging 只消费现有构建结果，不隐式触发配置或编译。
    printf "error: build directory not found: %s\n" "${buildDir}" >&2
    exit 1
fi

# 发布配置默认使用不带 d 后缀的规范名称。
fmtOutputName="fmt.lib"
fmtPdbName="fmt.pdb"
freetypeOutputName="freetype.lib"
freetypePdbName="freetype.pdb"
spdlogOutputName="spdlog.lib"
spdlogPdbName="spdlog.pdb"
zlibOutputName="libzs.lib"

case "${buildType}" in
    Debug | debug)
        # Debug 产物按各上游库的实际命名规则切换。
        fmtOutputName="fmtd.lib"
        fmtPdbName="fmtd.pdb"
        freetypeOutputName="freetyped.lib"
        freetypePdbName="freetype.pdb"
        spdlogOutputName="spdlogd.lib"
        spdlogPdbName="spdlog.pdb"
        zlibOutputName="libzsd.lib"
        ;;
esac

# 主仓清单从应用、UI 和引擎本体开始。
stageMainLibWithPdb "ImGuiFileDialog" "ImGuiFileDialog.lib" "ImGuiFileDialog.pdb" "lib/ImGuiFileDialog.lib"
stageMainLibWithPdb "IonCachyEngine" "IonCachyEngine-static.lib" "IonCachyEngine-static.pdb" "lib/IonCachyEngine-static.lib"
# 网络与 FFmpeg 归档使用 MSVC 风格输出名。
stageMainLibWithPdb "curl" "libcurl.lib" "libcurl.pdb" "lib/libcurl.lib" "lib/libcurl_static.lib"
stageMainLibWithPdb "ffmpeg" "avcodec.lib" "avcodec.pdb" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/avcodec.lib"
stageMainLibWithPdb "ffmpeg" "avformat.lib" "avformat.pdb" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/avformat.lib"
stageMainLibWithPdb "ffmpeg" "avutil.lib" "avutil.pdb" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/avutil.lib"
stageMainLibWithPdb "ffmpeg" "swresample.lib" "swresample.pdb" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/swresample.lib"
stageMainLibWithPdb "ffmpeg" "swscale.lib" "swscale.pdb" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/swscale.lib"
# Xiph 的静态归档使用上游 CMake 输出名，PDB 由各项目内部目标决定。
copyMainLib "xiph" "vorbisenc.lib" "3rdpty/xiph_inst/lib/vorbisenc.lib"
# 四个 COFF 归档来自同一 CRT 配置；此处不搜索宿主安装目录。
copyMainLib "xiph" "vorbis.lib" "3rdpty/xiph_inst/lib/vorbis.lib"
copyMainLib "xiph" "ogg.lib" "3rdpty/xiph_inst/lib/ogg.lib"
copyMainLib "xiph" "opus.lib" "3rdpty/xiph_inst/lib/opus.lib"
# 数学、格式化与字体依赖允许 Debug 名称差异。
stageMainLibWithPdb "fftw" "fftw3.lib" "fftw3.pdb" "3rdpty/fftw_inst/lib/fftw3.lib"
stageMainLibWithPdb "fmt" "${fmtOutputName}" "${fmtPdbName}" "lib/${fmtOutputName}" "lib/fmt.lib" "lib/fmtd.lib"
stageMainLibWithPdb "freetype" "${freetypeOutputName}" "${freetypePdbName}" "lib/${freetypeOutputName}" "lib/freetype.lib" "lib/freetyped.lib"
# UI 渲染库来自主 CMake 构建目录。
stageMainLibWithPdb "glfw" "glfw3.lib" "glfw.pdb" "lib/glfw3.lib" "lib/glfw.lib"
stageMainLibWithPdb "imgui" "imgui-static.lib" "imgui-static.pdb" "lib/imgui-static.lib"
stageMainLibWithPdb "implot" "3rd_implot.lib" "3rd_implot.pdb" "lib/3rd_implot.lib"
# 音频编解码和重采样依赖来自各自安装前缀。
stageMainLibWithPdb "lame" "libmp3lame-static.lib" "mp3lame.pdb" "3rdpty/lame_inst/lib/mp3lame.lib"
stageMainLibWithPdb "libsamplerate" "samplerate.lib" "samplerate.pdb" "3rdpty/libsamplerate/samplerate.lib" "lib/samplerate.lib"
# LuaJIT 与 SVG 渲染依赖只由主应用消费。
stageMainLibWithPdb "luajit" "lua51.lib" "lua51.pdb" "luajit/src/libluajit.a"
stageMainLibWithPdb "lunasvg" "lunasvg.lib" "lunasvg.pdb" "lib/lunasvg.lib"
stageMainLibWithPdb "lunasvg" "plutovg.lib" "plutovg.pdb" "lib/plutovg.lib"
# 压缩、TLS 与数据通道栈保持各上游 target 的名称。
stageMainLibWithPdb "miniz" "3rd_miniz.lib" "3rd_miniz.pdb" "lib/3rd_miniz.lib"
stageMainLibWithPdb "mbedtls" "mbedcrypto.lib" "mbedcrypto.pdb" "lib/mbedcrypto.lib"
stageMainLibWithPdb "mbedtls" "mbedx509.lib" "mbedx509.pdb" "lib/mbedx509.lib"
stageMainLibWithPdb "mbedtls" "mbedtls.lib" "mbedtls.pdb" "lib/mbedtls.lib"
stageMainLibWithPdb "mbedtls" "everest.lib" "everest.pdb" "lib/everest.lib"
stageMainLibWithPdb "mbedtls" "p256m.lib" "p256m.pdb" "lib/p256m.lib"
stageMainLibWithPdb "libjuice" "juice-static.lib" "juice-static.pdb" "lib/juice-static.lib"
stageMainLibWithPdb "usrsctp" "usrsctp.lib" "usrsctp.pdb" "lib/usrsctp.lib"
stageMainLibWithPdb "libdatachannel" "datachannel-static.lib" "datachannel-static.pdb" "lib/datachannel-static.lib"
# 平台文件、音频与基础设施依赖完成主仓清单。
stageMainLibWithPdb "nativefiledialog-extended" "nfd.lib" "nfd.pdb" "lib/nfd.lib"
stageMainLibWithPdb "openal" "OpenAL32.lib" "OpenAL.pdb" "lib/OpenAL32.lib" "lib/OpenAL.lib"
stageMainLibWithPdb "rubberband" "rubberband-static.lib" "rubberband-static.pdb" "rb_inst/lib/rubberband-static.lib" "rb_inst/lib/rubberband.lib" "rb_inst/lib/librubberband.a" "lib/rubberband-static.lib"
stageMainLibWithPdb "sdl" "SDL3-static.lib" "SDL3-static.pdb" "lib/SDL3-static.lib" "lib/SDL3.lib"
stageMainLibWithPdb "spdlog" "${spdlogOutputName}" "${spdlogPdbName}" "lib/${spdlogOutputName}" "lib/spdlog.lib" "lib/spdlogd.lib"
stageMainLibWithPdb "zlib" "${zlibOutputName}" "zlib.pdb" "3rdpty/zlib_inst/lib/libz.lib"

# 引擎预编译根只复制其直接音频和基础设施依赖。
# FFmpeg 五个组件必须来自同一次安装前缀构建。
stageIceLibWithPdb "ffmpeg" "avcodec.lib" "avcodec.pdb" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/avcodec.lib"
stageIceLibWithPdb "ffmpeg" "avformat.lib" "avformat.pdb" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/avformat.lib"
stageIceLibWithPdb "ffmpeg" "avutil.lib" "avutil.pdb" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/avutil.lib"
stageIceLibWithPdb "ffmpeg" "swresample.lib" "swresample.pdb" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/swresample.lib"
stageIceLibWithPdb "ffmpeg" "swscale.lib" "swscale.pdb" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/swscale.lib"
# 引擎独立构建时的 FFmpeg 也要解析相同 CRT 配置的 Xiph 归档。
copyIceLib "xiph" "vorbisenc.lib" "3rdpty/xiph_inst/lib/vorbisenc.lib"
copyIceLib "xiph" "vorbis.lib" "3rdpty/xiph_inst/lib/vorbis.lib"
copyIceLib "xiph" "ogg.lib" "3rdpty/xiph_inst/lib/ogg.lib"
copyIceLib "xiph" "opus.lib" "3rdpty/xiph_inst/lib/opus.lib"
# DSP、编码与重采样库复用主仓源产物。
stageIceLibWithPdb "fftw" "fftw3.lib" "fftw3.pdb" "3rdpty/fftw_inst/lib/fftw3.lib"
stageIceLibWithPdb "fmt" "${fmtOutputName}" "${fmtPdbName}" "lib/${fmtOutputName}" "lib/fmt.lib" "lib/fmtd.lib"
stageIceLibWithPdb "lame" "libmp3lame-static.lib" "mp3lame.pdb" "3rdpty/lame_inst/lib/mp3lame.lib"
stageIceLibWithPdb "libsamplerate" "samplerate.lib" "samplerate.pdb" "3rdpty/libsamplerate/samplerate.lib" "lib/samplerate.lib"
# 平台音频、时间伸缩和日志压缩依赖位于清单末尾。
stageIceLibWithPdb "openal" "OpenAL32.lib" "OpenAL.pdb" "lib/OpenAL32.lib" "lib/OpenAL.lib"
stageIceLibWithPdb "rubberband" "rubberband-static.lib" "rubberband-static.pdb" "rb_inst/lib/rubberband-static.lib" "rb_inst/lib/rubberband.lib" "rb_inst/lib/librubberband.a" "lib/rubberband-static.lib"
stageIceLibWithPdb "sdl" "SDL3-static.lib" "SDL3-static.pdb" "lib/SDL3-static.lib" "lib/SDL3.lib"
stageIceLibWithPdb "spdlog" "${spdlogOutputName}" "${spdlogPdbName}" "lib/${spdlogOutputName}" "lib/spdlog.lib" "lib/spdlogd.lib"
stageIceLibWithPdb "zlib" "${zlibOutputName}" "zlib.pdb" "3rdpty/zlib_inst/lib/libz.lib"

# 维护约束：新增库前必须核对源码 target 与 Find 模块的导出名称。
# 维护约束：libs 与 symbols 的 platform、arch、toolchain 层必须一致。
# 维护约束：compiler tag 必须对应实际采用的 MSVC 兼容 ABI。
# 维护约束：不同配置的归档和 PDB 绝不能写入同一目录。
# 维护约束：Debug 与 RelWithDebInfo 产物必须保留可调试信息。
# 维护约束：embedded-symbols 只适用于已确认归档内含 CodeView 的构建。
# 维护约束：strict-symbols 只检查被 scope 和 packages 选中的项目。
# 维护约束：PDB 名称可能与静态库名称不同，必须在清单中显式声明。
# 维护约束：PDB 搜索首次命中规则要求构建树内避免同名陈旧文件。
# 维护约束：正式 staging 前应使用全新或已清理的源构建目录。
# 维护约束：清理陈旧 PDB 时只能删除解析后的单一目标文件。
# 维护约束：静态库复制不删除旧 symbols，符号策略需显式选择。
# 维护约束：包过滤器不解析空格，调用方应传规范英文包名。
# 维护约束：主仓专属 UI 与网络库不得进入引擎预编译根。
# 维护约束：引擎共有依赖更新时应同步评估两个目标根。
# 维护约束：源候选列表应从最具体布局排列到兼容布局。
# 维护约束：找不到归档必须失败，禁止形成不完整发布集合。
# 维护约束：默认符号缺失警告需由 CI 日志明确保存。
# 维护约束：正式发布应优先使用 strict-symbols 或 embedded-symbols。
# 维护约束：install 权限固定 0644，库与 PDB 均不应带可执行位。
# 维护约束：本脚本不执行 strip、objcopy 或 PDB 内容转换。
# 维护约束：本脚本不负责构建第三方 target 或修复其输出布局。
# 维护约束：本脚本不负责 Git LFS add、commit 或 push。
# 维护约束：staging 后必须确认新增二进制和 PDB 均由 LFS 跟踪。
# 维护约束：头文件更新应与 stage-prebuilt-headers 同一发布批次完成。
# 维护约束：主仓与引擎仓同名库必须保持 API 和 ABI 同步。
# 维护约束：新增配置名需同步预编译查找脚本的配置映射。
# 维护约束：动态导入库布局含 shared 层，不由本脚本处理。
# 维护约束：MSVC 原生 PDB 与 MinGW 内嵌 CodeView 不得混淆。
# 维护约束：成功到达文件末尾表示所有选中库均已完成处理。
# 维护约束：clang-cl 产物必须使用 COFF/MSVC 归档格式。
# 维护约束：不得把 GNU ar 生成的 MinGW 归档改名为 .lib 后发布。
# 维护约束：架构层当前固定 x86_64，不从宿主架构自动推断。
# 维护约束：目标根的 windows 层表示目标平台而非执行宿主。
# 维护约束：同名 PDB 的选择依赖全新构建树这一前置条件。
# 维护约束：上游开始生成目标旁路 PDB 时应优先增加精确候选路径。
# 维护约束：递归名称搜索仅作为多个上游布局的兼容方案。
# 维护约束：复制前不修改 PDB age、GUID 或调试目录记录。
# 维护约束：归档与 PDB 配对验证应在后续消费构建中完成。
# 维护约束：同一包有多个静态库时每项都需独立清单记录。
# 维护约束：清单顺序不表达链接顺序，只表达发布枚举顺序。
# 维护约束：包名决定预编译目录，不能使用显示名称替代。
# 维护约束：输出名决定 Find 模块查找结果，修改需同步消费端。
# 维护约束：源码路径始终相对单个 buildDir，不能跨构建树取件。
# 维护约束：scope=all 应由一次一致的源码构建更新两个预编译根。
# 维护约束：scope=main 不应留下引擎 symbols 目录的半更新状态。
# 维护约束：scope=ice 不应留下主仓 symbols 目录的半更新状态。
# 维护约束：切换符号策略前需清楚区分外置与归档内调试信息。
# 维护约束：Release 等无符号配置不会主动清理其他配置目录。
# 维护约束：构建类型大小写兼容仅对 Debug 输出名生效。
# 维护约束：RelWithDebInfo 使用发布库名并搭配调试信息。
# 维护约束：错误输出应保留完整包名、目标名和候选路径。
# 维护约束：成功日志使用项目相对路径避免暴露 CI 工作区前缀。
# 维护约束：新增命令行选项必须同步 showUsage 文本。
# 维护约束：任何删除行为都必须继续受 embedded-symbols 显式开关约束。
# 维护约束：脚本不得递归删除 symbols 或其他预编译目录。
