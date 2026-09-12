#!/usr/bin/env bash
# 将原生 macOS 源码依赖产物复制到主仓和 IonCachyEngine 的静态预编译布局。
# 输出路径编码架构、工具链、编译器标签和配置，防止 ABI 不兼容产物混用。
# 每项归档可声明多个候选源路径，按清单顺序选择首个现有文件。
# --scope 与 --packages 仅缩小写入范围，不会自动构建或补齐传递依赖。
# 复制前使用 lipo 验证目标架构，避免目录名与二进制内容不一致。
set -euo pipefail

# 输出参数契约和环境变量入口，不访问构建树。
showUsage() {
    cat <<'EOF'
Usage: scripts/ci/stage-macos-prebuilts.sh [options]

Copy native macOS source-build static libraries into the prebuilt layout.

Options:
  --build-dir <path>      Source build directory. Default: build_macos_sources_<arch>
  --build-type <type>     Prebuilt config directory. Default: RelWithDebInfo
  --arch <arm64|x86_64>   Prebuilt architecture directory. Default: native architecture
  --toolchain <name>      Prebuilt toolchain directory. Default: clang
  --compiler-tag <tag>    Prebuilt compiler tag. Default: detected clang major
  --scope <all|main|ice>  Staging scope. Default: all
  --packages <list>       Comma-separated packages to stage. Default: all
  -h, --help              Show this help

Environment overrides:
  MACOS_PREBUILT_ARCH          Default prebuilt architecture directory
  MACOS_PREBUILT_TOOLCHAIN     Default prebuilt toolchain directory
  MACOS_PREBUILT_COMPILER_TAG  Default prebuilt compiler tag
EOF
}

# 将常用架构别名规范为预编译目录采用的名称。
normalizeArchitecture() {
    case "$1" in
        arm64 | aarch64)
            # Apple Silicon 目录统一使用 arm64。
            printf "arm64\n"
            ;;
        x86_64 | amd64)
            # Intel 64 位目录统一使用 x86_64。
            printf "x86_64\n"
            ;;
        *)
            # 未知架构在任何文件复制前失败。
            printf "error: unsupported macOS architecture: %s\n" "$1" >&2
            exit 1
            ;;
    esac
}

# 把相对路径稳定解析到项目根目录。
projectPath() {
    local inputPath="$1"

    if [[ "${inputPath}" = /* ]]; then
        # 绝对路径保持原值，允许消费外部 CI 构建树。
        printf "%s\n" "${inputPath}"
    else
        # 相对路径不依赖调用者当前目录。
        printf "%s/%s\n" "${projectRoot}" "${inputPath}"
    fi
}

# 从当前 Xcode toolchain 探测 Clang 主版本目录标签。
detectClangCompilerTag() {
# xcrun 保证选择当前开发者目录中的 clang++。
    local cxxCompiler
    cxxCompiler="$(xcrun --find clang++)"

    local macroOutput
    # 读取预定义宏避免解析易变的版本说明文本。
    if ! macroOutput="$(printf "\n" | "${cxxCompiler}" -dM -E -x c++ - 2>/dev/null)"; then
        printf "error: failed to query Clang predefined macros: %s\n" "${cxxCompiler}" >&2
        exit 1
    fi

    local clangMajor
    # 只接受纯数字主版本，防止生成不可预测的路径层级。
    clangMajor="$(awk '$2 == "__clang_major__" { print $3; exit }' <<<"${macroOutput}")"
    if [[ ! "${clangMajor}" =~ ^[0-9]+$ ]]; then
        printf "error: failed to detect Clang major version from: %s\n" "${cxxCompiler}" >&2
        exit 1
    fi

    printf "clang%s\n" "${clangMajor}"
}

# 验证候选归档确实包含请求的目标架构 slice。
verifyArchitecture() {
    local sourcePath="$1"
    local architectures

    if ! architectures="$(xcrun lipo -archs "${sourcePath}" 2>/dev/null)"; then
        # 无法读取 Mach-O 架构视为损坏或错误格式。
        printf "error: failed to inspect library architecture: %s\n" "${sourcePath}" >&2
        exit 1
    fi

    if [[ " ${architectures} " != *" ${targetArch} "* ]]; then
        # 使用空格包围匹配，避免架构名发生子串误判。
        printf "error: library does not contain %s architecture: %s (%s)\n" \
            "${targetArch}" "${sourcePath}" "${architectures}" >&2
        exit 1
    fi
}

# 选择、验证并复制一项静态库到指定预编译根。
# 前三个参数为目标根、包名和规范输出名，其余参数为构建树内候选路径。
copyLib() {
    local prebuiltRoot="$1"
    local packageName="$2"
    local outputName="$3"
    shift 3

    if ! shouldStagePackage "${packageName}"; then
        # 未选中的包正常跳过，且不验证其源产物。
        return 0
    fi

    local sourcePath=""
    # 空值表示尚未命中候选，失败时将统一报告搜索列表。
    local candidatePath
    for candidatePath in "$@"; do
        # 所有候选都相对同一源构建树解析。
        candidatePath="${buildDir}/${candidatePath}"
        if [[ -f "${candidatePath}" ]]; then
            # 候选顺序表达优先级，首个命中后立即停止。
            sourcePath="${candidatePath}"
            break
        fi
    done

    if [[ -z "${sourcePath}" ]]; then
        # 缺失任何请求归档都会中止，禁止发布部分依赖集合。
        printf "error: source library not found for %s/%s\n" "${packageName}" "${outputName}" >&2
        printf "searched:\n" >&2
        local searchedPath
        # 回显原始相对候选，方便从构建日志定位布局变化。
        for searchedPath in "$@"; do
            printf "  %s/%s\n" "${buildDir}" "${searchedPath}" >&2
        done
        exit 1
    fi

    verifyArchitecture "${sourcePath}"

    # 输出目录严格遵循 macOS 静态预编译布局。
    local outputPath="${prebuiltRoot}/binaries/macos/${packageName}/libs/${targetArch}/${prebuiltToolchain}/${compilerTag}/${buildType}/${outputName}"
    # install 前显式创建父目录，macOS install 不提供 GNU -D 行为。
    mkdir -p "$(dirname "${outputPath}")"
    install -m 0644 "${sourcePath}" "${outputPath}"
    printf "staged %s\n" "${outputPath#${projectRoot}/}"
}

# 主仓包装器在 ice-only 模式保持主仓预编译目录不变。
copyMainLib() {
    if [[ "${stageScope}" = "ice" ]]; then
        # 引擎独立发布不应写入主仓 LFS 内容。
        return 0
    fi

    copyLib "${projectRoot}/3rdpty/prebuilts" "$@"
}

# 引擎包装器在 main-only 模式跳过内部依赖根。
copyIceLib() {
    if [[ "${stageScope}" = "main" ]]; then
        # 主仓增量发布不应修改引擎仓内容。
        return 0
    fi

    copyLib "${projectRoot}/3rdpty/sources/IonCachyEngine/3rdpty/prebuilts" "$@"
}

# 通过脚本位置定位仓库，支持从 CI 任意目录调用。
scriptDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
projectRoot="$(cd "${scriptDir}/../.." && pwd)"

# 逗号包围匹配避免短包名误命中其他名称。
shouldStagePackage() {
    local packageName="$1"

    [[ -z "${packageFilter}" || ",${packageFilter}," == *",${packageName},"* ]]
}

# staging 只能在具备原生 Mach-O 工具链的 macOS 上执行。
if [[ "$(uname -s)" != "Darwin" ]]; then
    printf "error: scripts/ci/stage-macos-prebuilts.sh must run on macOS\n" >&2
    exit 1
fi

# lipo 与 clang 探测都通过 xcrun 进入当前 Xcode toolchain。
if ! command -v xcrun >/dev/null 2>&1; then
    printf "error: required command not found: xcrun\n" >&2
    exit 1
fi

# 空构建目录会在架构规范化后派生默认名称。
buildDir=""
# 配置名直接进入最终输出路径。
buildType="RelWithDebInfo"
# 架构默认跟随环境覆盖或当前宿主。
targetArch="${MACOS_PREBUILT_ARCH:-$(uname -m)}"
# toolchain 与 compiler tag 共同标识 ABI 产物集。
prebuiltToolchain="${MACOS_PREBUILT_TOOLCHAIN:-clang}"
compilerTag="${MACOS_PREBUILT_COMPILER_TAG:-}"
stageScope="all"
# 空过滤器表示复制完整清单。
packageFilter=""

# 在写入文件前完整解析所有选项。
while (( $# > 0 )); do
    case "$1" in
        --build-dir)
            # 构建树可使用项目相对路径或绝对路径。
            if (( $# < 2 )); then
                printf "error: --build-dir requires a value\n" >&2
                exit 1
            fi
            buildDir="$2"
            shift 2
            ;;
        --build-type)
            # Debug 配置还会选择带 d 后缀的部分归档。
            if (( $# < 2 )); then
                printf "error: --build-type requires a value\n" >&2
                exit 1
            fi
            buildType="$2"
            shift 2
            ;;
        --arch)
            # 输入别名在解析完成后统一规范化。
            if (( $# < 2 )); then
                printf "error: --arch requires a value\n" >&2
                exit 1
            fi
            targetArch="$2"
            shift 2
            ;;
        --toolchain)
            # 目录标签必须与实际编译产物的 ABI 家族一致。
            if (( $# < 2 )); then
                printf "error: --toolchain requires a value\n" >&2
                exit 1
            fi
            prebuiltToolchain="$2"
            shift 2
            ;;
        --compiler-tag)
            # 显式标签可覆盖本机 Clang 自动探测结果。
            if (( $# < 2 )); then
                printf "error: --compiler-tag requires a value\n" >&2
                exit 1
            fi
            compilerTag="$2"
            shift 2
            ;;
        --scope)
            # scope 决定两个预编译根的写入边界。
            if (( $# < 2 )); then
                printf "error: --scope requires a value\n" >&2
                exit 1
            fi
            stageScope="$2"
            shift 2
            ;;
        --packages)
            # 包列表执行精确匹配，不自动展开依赖。
            if (( $# < 2 )); then
                printf "error: --packages requires a value\n" >&2
                exit 1
            fi
            packageFilter="$2"
            shift 2
            ;;
        -h | --help)
            # 帮助成功退出且无需现有构建树。
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

# 默认目录名和架构验证均使用规范值。
targetArch="$(normalizeArchitecture "${targetArch}")"

if [[ -z "${buildDir}" ]]; then
    # 架构进入默认目录名，避免不同产物相互覆盖。
    buildDir="build_macos_sources_${targetArch}"
fi
if [[ -z "${compilerTag}" ]]; then
    # 仅在调用方没有固定标签时查询本机编译器。
    compilerTag="$(detectClangCompilerTag)"
fi

if [[ -z "${prebuiltToolchain}" || -z "${compilerTag}" ]]; then
    # 空路径层会破坏预编译 ABI 隔离。
    printf "error: prebuilt toolchain and compiler tag must not be empty\n" >&2
    exit 1
fi

case "${stageScope}" in
    all | main | ice)
        # 合法值由两个包装器分别解释。
        ;;
    *)
        # 其他值可能造成意外写入范围，必须拒绝。
        printf "error: --scope must be one of all, main, ice: %s\n" "${stageScope}" >&2
        exit 1
        ;;
esac

buildDir="$(projectPath "${buildDir}")"

if [[ ! -d "${buildDir}" ]]; then
    # staging 只消费既有构建结果，不隐式配置源构建树。
    printf "error: build directory not found: %s\n" "${buildDir}" >&2
    exit 1
fi

# 发布配置使用无调试后缀的规范名称。
fmtOutputName="libfmt.a"
freetypeOutputName="libfreetype.a"
spdlogOutputName="libspdlog.a"

case "${buildType}" in
    Debug | debug)
        # 当前仅三个库通过文件名区分 Debug 归档。
        fmtOutputName="libfmtd.a"
        freetypeOutputName="libfreetyped.a"
        spdlogOutputName="libspdlogd.a"
        ;;
esac

# 主仓专属应用、UI 与网络依赖从此处开始。
copyMainLib "ImGuiFileDialog" "libImGuiFileDialog.a" "lib/libImGuiFileDialog.a"
# 音频引擎本体只发布到主仓依赖根。
copyMainLib "IonCachyEngine" "libIonCachyEngine-static.a" "lib/libIonCachyEngine-static.a"
# curl 接受常规或显式 static 输出名。
copyMainLib "curl" "libcurl.a" "lib/libcurl.a" "lib/libcurl_static.a"
# FFmpeg 五个归档来自引擎依赖安装前缀。
copyMainLib "ffmpeg" "libavcodec.a" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libavcodec.a"
# avformat 和 avutil 必须来自同一次依赖构建。
copyMainLib "ffmpeg" "libavformat.a" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libavformat.a"
copyMainLib "ffmpeg" "libavutil.a" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libavutil.a"
# 两个转换组件分别覆盖音频和视频数据路径。
copyMainLib "ffmpeg" "libswresample.a" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libswresample.a"
copyMainLib "ffmpeg" "libswscale.a" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libswscale.a"
# FFTW 使用独立安装前缀。
copyMainLib "fftw" "libfftw3.a" "3rdpty/fftw_inst/lib/libfftw3.a"
# fmt 与 FreeType 按配置优先寻找规范输出名。
copyMainLib "fmt" "${fmtOutputName}" "lib/${fmtOutputName}" "lib/libfmt.a" "lib/libfmtd.a"
copyMainLib "freetype" "${freetypeOutputName}" "lib/${freetypeOutputName}" "lib/libfreetype.a" "lib/libfreetyped.a"
# GLFW 兼容两个常见静态归档名。
copyMainLib "glfw" "libglfw3.a" "lib/libglfw3.a" "lib/libglfw.a"
# ImGui 与 ImPlot 使用项目 target 的输出名称。
copyMainLib "imgui" "libimgui-static.a" "lib/libimgui-static.a"
copyMainLib "implot" "lib3rd_implot.a" "lib/lib3rd_implot.a"
# 音频编码和重采样库来自各自依赖子目录。
copyMainLib "lame" "libmp3lame.a" "3rdpty/lame_inst/lib/libmp3lame.a"
copyMainLib "libsamplerate" "libsamplerate.a" "3rdpty/libsamplerate/libsamplerate.a"
# LuaJIT 与 SVG 渲染只由主应用直接消费。
copyMainLib "luajit" "libluajit.a" "luajit/src/libluajit.a"
copyMainLib "lunasvg" "liblunasvg.a" "lib/liblunasvg.a"
copyMainLib "lunasvg" "libplutovg.a" "lib/libplutovg.a"
# miniz 与 mbedTLS 组成压缩和 TLS 支持。
copyMainLib "miniz" "lib3rd_miniz.a" "lib/lib3rd_miniz.a"
copyMainLib "mbedtls" "libmbedcrypto.a" "lib/libmbedcrypto.a"
copyMainLib "mbedtls" "libmbedx509.a" "lib/libmbedx509.a"
copyMainLib "mbedtls" "libmbedtls.a" "lib/libmbedtls.a"
copyMainLib "mbedtls" "libeverest.a" "lib/libeverest.a"
copyMainLib "mbedtls" "libp256m.a" "lib/libp256m.a"
# 数据通道栈按 ICE、SCTP 和高级封装复制。
copyMainLib "libjuice" "libjuice-static.a" "lib/libjuice-static.a"
copyMainLib "usrsctp" "libusrsctp.a" "lib/libusrsctp.a"
copyMainLib "libdatachannel" "libdatachannel-static.a" "lib/libdatachannel-static.a"
# 文件对话框与音频运行库完成主程序平台依赖。
copyMainLib "nativefiledialog-extended" "libnfd.a" "lib/libnfd.a"
copyMainLib "openal" "libopenal.a" "lib/libopenal.a" "lib/libOpenAL.a"
copyMainLib "rubberband" "librubberband.a" "rb_inst/lib/librubberband.a"
# SDL、spdlog 与 zlib 使用构建树或安装前缀中的规范归档。
copyMainLib "sdl" "libSDL3.a" "lib/libSDL3.a"
copyMainLib "spdlog" "${spdlogOutputName}" "lib/${spdlogOutputName}" "lib/libspdlog.a" "lib/libspdlogd.a"
copyMainLib "zlib" "libz.a" "3rdpty/zlib_inst/lib/libz.a"

# 引擎预编译根只包含其直接依赖，不复制主应用 UI 与网络栈。
# FFmpeg 组件与主仓复用相同源产物和规范输出名。
copyIceLib "ffmpeg" "libavcodec.a" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libavcodec.a"
copyIceLib "ffmpeg" "libavformat.a" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libavformat.a"
copyIceLib "ffmpeg" "libavutil.a" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libavutil.a"
# 两个转换组件分别覆盖音频和视频数据路径。
copyIceLib "ffmpeg" "libswresample.a" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libswresample.a"
copyIceLib "ffmpeg" "libswscale.a" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libswscale.a"
# FFTW、fmt、LAME 与 libsamplerate 构成 DSP 和编码依赖。
copyIceLib "fftw" "libfftw3.a" "3rdpty/fftw_inst/lib/libfftw3.a"
copyIceLib "fmt" "${fmtOutputName}" "lib/${fmtOutputName}" "lib/libfmt.a" "lib/libfmtd.a"
copyIceLib "lame" "libmp3lame.a" "3rdpty/lame_inst/lib/libmp3lame.a"
copyIceLib "libsamplerate" "libsamplerate.a" "3rdpty/libsamplerate/libsamplerate.a"
# OpenAL 接受历史静态库名并规范输出为 libopenal.a。
copyIceLib "openal" "libopenal.a" "lib/libopenal.a" "lib/libOpenAL.a"
# 最后复制时间伸缩、平台、日志与压缩依赖。
copyIceLib "rubberband" "librubberband.a" "rb_inst/lib/librubberband.a"
copyIceLib "sdl" "libSDL3.a" "lib/libSDL3.a"
copyIceLib "spdlog" "${spdlogOutputName}" "lib/${spdlogOutputName}" "lib/libspdlog.a" "lib/libspdlogd.a"
copyIceLib "zlib" "libz.a" "3rdpty/zlib_inst/lib/libz.a"

# 维护约束：新增归档前必须确认对应 Find 模块导出的 target 名称。
# 维护约束：输出文件名必须与源码构建 target 的实际产物一致。
# 维护约束：主仓专属库不得写入引擎内部预编译根。
# 维护约束：引擎公共依赖需要同时评估两个预编译根。
# 维护约束：所有目标目录必须保留 platform/package/libs 层级。
# 维护约束：架构目录必须与 lipo 验证结果一致。
# 维护约束：toolchain 层只描述 clang 等 ABI 家族。
# 维护约束：compiler tag 必须编码实际 Clang 主版本。
# 维护约束：Debug 与 RelWithDebInfo 归档不得写入同一目录。
# 维护约束：静态归档必须保留调试信息，staging 不执行 strip。
# 维护约束：候选路径按具体安装产物到兼容名称排列。
# 维护约束：候选命中首项后停止，避免混合不同构建结果。
# 维护约束：包过滤器只控制复制，不自动补齐传递依赖。
# 维护约束：scope=all 应使用同一次源构建更新两个仓库。
# 维护约束：缺少请求项必须失败，不能发布部分依赖集合。
# 维护约束：install 权限固定 0644，归档不应带可执行位。
# 维护约束：源构建树保持只读，不移动或重命名产物。
# 维护约束：脚本不负责 Git LFS add、commit 或 push。
# 维护约束：staging 后应确认所有新二进制均由 LFS 跟踪。
# 维护约束：更新头文件时应与 stage-prebuilt-headers 同批完成。
# 维护约束：跨架构发布前需执行对应预编译消费构建。
# 维护约束：主仓和引擎仓同名依赖必须保持 API 与 ABI 同步。
# 维护约束：新增配置名需要同步 Find 模块的配置映射规则。
# 维护约束：动态库布局不由本脚本处理，禁止混入 shared 子目录。
# 维护约束：Universal Binary 可包含额外 slice，但必须包含目标架构。
# 维护约束：xcrun 应服从 CI 预先选择的 DEVELOPER_DIR。
# 维护约束：成功到达文件末尾表示所有选中归档均已复制。
# 维护约束：不同 Xcode 版本产物必须使用不同 compiler tag 目录。
# 维护约束：compiler tag 自动探测只读取 Clang 主版本。
# 维护约束：显式 compiler tag 的正确性由发布工作流负责保证。
# 维护约束：架构验证必须发生在覆盖目标归档之前。
# 维护约束：Universal Binary 的其他 slice 不会在 staging 中裁剪。
# 维护约束：macOS 归档不得写入 Linux 或 Windows 平台目录。
# 维护约束：同包多个归档必须共享包目录和配置层级。
# 维护约束：Debug 后缀兼容项应优先使用请求配置的规范名称。
# 维护约束：源候选变化时应同步对应平台构建脚本的输出布局。
# 维护约束：包过滤列表使用英文逗号且不接受通配符。
# 维护约束：空 packages 值仍表示完整清单，不表示不复制。
# 维护约束：复制完成后调用方负责执行预编译消费验证。
# 维护约束：发布前应使用 file 或 lipo 复核最终目标目录。
