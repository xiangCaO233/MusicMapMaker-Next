#!/usr/bin/env bash
# 将 Linux 源码依赖构建结果复制到主仓与 IonCachyEngine 的静态预编译布局。
# 目标路径编码包名、x86_64、工具链、编译器标签和配置，供 Find 模块精确选择。
# 每个库可提供多个候选源路径，兼容依赖安装目标与直接构建目标的布局差异。
# --scope 和 --packages 支持按仓库或包增量更新，但请求项缺失时必须失败。
# 脚本不触发构建、不删除源产物，也不复制动态运行时文件。
set -euo pipefail

# 输出参数契约和环境覆盖入口，不访问文件系统。
showUsage() {
    cat <<'EOF'
Usage: scripts/ci/stage-linux-prebuilts.sh [options]

Copy native Linux source-build static libraries into the prebuilt layout.

Options:
  --build-dir <path>      Source build directory. Default: build_linux_sources
  --build-type <type>     Prebuilt config directory. Default: RelWithDebInfo
  --toolchain <name>      Prebuilt toolchain directory. Default: gcc
  --compiler-tag <tag>    Prebuilt compiler tag. Default: gcc14
  --scope <all|main|ice>  Staging scope. Default: all
  --packages <list>       Comma-separated packages to stage. Default: all
  -h, --help              Show this help

Environment overrides:
  LINUX_PREBUILT_TOOLCHAIN      Default prebuilt toolchain directory
  LINUX_PREBUILT_COMPILER_TAG   Default prebuilt compiler tag
EOF
}

# 把调用方提供的相对路径稳定解析到项目根。
projectPath() {
    local inputPath="$1"

    if [[ "${inputPath}" = /* ]]; then
        # 绝对路径保持原值，稍后由存在性检查负责验证。
        printf "%s\n" "${inputPath}"
    else
        # 相对路径不依赖 shell 当前目录。
        printf "%s/%s\n" "${projectRoot}" "${inputPath}"
    fi
}

# 从候选列表选择首个现有静态库并复制到目标预编译根。
# 前三个参数分别是目标根、包名和规范输出名，其余参数为候选相对路径。
copyLib() {
    local prebuiltRoot="$1"
    local packageName="$2"
    local outputName="$3"
    shift 3

    if ! shouldStagePackage "${packageName}"; then
        # 包过滤器未选中时正常跳过，不检查任何候选路径。
        return 0
    fi

    # 空值表示尚未命中候选，循环结束后统一报告全部搜索路径。
    local sourcePath=""
    local candidatePath
    for candidatePath in "$@"; do
        # 候选路径全部相对同一源构建树解析。
        candidatePath="${buildDir}/${candidatePath}"
        if [[ -f "${candidatePath}" ]]; then
            # 列表顺序表达优先级，首个命中后停止搜索。
            sourcePath="${candidatePath}"
            break
        fi
    done

    if [[ -z "${sourcePath}" ]]; then
        # 缺失信息包含包和目标文件名，便于定位清单项。
        printf "error: source library not found for %s/%s\n" "${packageName}" "${outputName}" >&2
        printf "searched:\n" >&2
        local searchedPath
        # 回显原始候选而非循环中修改后的局部值。
        for searchedPath in "$@"; do
            printf "  %s/%s\n" "${buildDir}" "${searchedPath}" >&2
        done
        exit 1
    fi

    # 输出目录严格遵循 Linux 静态预编译约定。
    local outputPath="${prebuiltRoot}/binaries/linux/${packageName}/libs/x86_64/${prebuiltToolchain}/${compilerTag}/${buildType}/${outputName}"
    # install 同时创建父目录并统一只读归档权限。
    install -D -m 0644 "${sourcePath}" "${outputPath}"
    printf "staged %s\n" "${outputPath#${projectRoot}/}"
}

# 主仓 wrapper 在 ice-only 模式跳过，其余参数原样传给 copyLib。
copyMainLib() {
    if [[ "${stageScope}" = "ice" ]]; then
        # 引擎独立发布不应修改主仓 LFS 内容。
        return 0
    fi

    copyLib "${projectRoot}/3rdpty/prebuilts" "$@"
}

# 引擎 wrapper 在 main-only 模式跳过。
copyIceLib() {
    if [[ "${stageScope}" = "main" ]]; then
        # 主仓增量发布保留引擎仓预编译目录不变。
        return 0
    fi

    copyLib "${projectRoot}/3rdpty/sources/IonCachyEngine/3rdpty/prebuilts" "$@"
}

# 绝对根路径保证脚本可以从 CI 任意工作目录调用。
scriptDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
projectRoot="$(cd "${scriptDir}/../.." && pwd)"

# 逗号包围匹配避免 fmt 等短包名误命中其他名称。
shouldStagePackage() {
    local packageName="$1"

    [[ -z "${packageFilter}" || ",${packageFilter}," == *",${packageName},"* ]]
}

# 默认构建目录对应 Linux 源码依赖任务。
buildDir="build_linux_sources"
# 配置目录名直接进入输出路径。
buildType="RelWithDebInfo"
# 工具链和 compiler tag 可由 CI 环境选择 gcc 或 clang 产物。
prebuiltToolchain="${LINUX_PREBUILT_TOOLCHAIN:-gcc}"
compilerTag="${LINUX_PREBUILT_COMPILER_TAG:-gcc14}"
stageScope="all"
# 空过滤器表示复制完整清单。
packageFilter=""

# 在任何写入前解析并验证所有选项。
while (( $# > 0 )); do
    case "$1" in
        --build-dir)
            # 源构建树可以使用绝对路径或项目根相对路径。
            if (( $# < 2 )); then
                printf "error: --build-dir requires a value\n" >&2
                exit 1
            fi
            buildDir="$2"
            shift 2
            ;;
        --build-type)
            # Debug 会进一步选择带 d 后缀的部分归档。
            if (( $# < 2 )); then
                printf "error: --build-type requires a value\n" >&2
                exit 1
            fi
            buildType="$2"
            shift 2
            ;;
        --toolchain)
            # 目录标签必须与生成归档的 ABI 工具链一致。
            if (( $# < 2 )); then
                printf "error: --toolchain requires a value\n" >&2
                exit 1
            fi
            prebuiltToolchain="$2"
            shift 2
            ;;
        --compiler-tag)
            # compiler tag 区分 gcc14、clang19 等具体产物集。
            if (( $# < 2 )); then
                printf "error: --compiler-tag requires a value\n" >&2
                exit 1
            fi
            compilerTag="$2"
            shift 2
            ;;
        --scope)
            # scope 决定两个预编译根中的写入边界。
            if (( $# < 2 )); then
                printf "error: --scope requires a value\n" >&2
                exit 1
            fi
            stageScope="$2"
            shift 2
            ;;
        --packages)
            # 逗号列表不执行模糊匹配或自动依赖展开。
            if (( $# < 2 )); then
                printf "error: --packages requires a value\n" >&2
                exit 1
            fi
            packageFilter="$2"
            shift 2
            ;;
        -h | --help)
            # 帮助成功退出且不验证构建树。
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

if [[ -z "${prebuiltToolchain}" || -z "${compilerTag}" ]]; then
    # 空目录层会破坏预编译 ABI 隔离，必须拒绝。
    printf "error: prebuilt toolchain and compiler tag must not be empty\n" >&2
    exit 1
fi

case "${stageScope}" in
    all | main | ice)
        # 合法值由两个 wrapper 分别解释。
        ;;
    *)
        printf "error: --scope must be one of all, main, ice: %s\n" "${stageScope}" >&2
        exit 1
        ;;
esac

buildDir="$(projectPath "${buildDir}")"

if [[ ! -d "${buildDir}" ]]; then
    # staging 只消费既有构建结果，不隐式创建或配置源构建树。
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
# avformat 提供容器封装与探测。
copyMainLib "ffmpeg" "libavformat.a" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libavformat.a"
# avutil 是其余 FFmpeg 组件的共享基础。
copyMainLib "ffmpeg" "libavutil.a" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libavutil.a"
# swresample 处理音频格式转换。
copyMainLib "ffmpeg" "libswresample.a" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libswresample.a"
# swscale 处理视频帧转换。
copyMainLib "ffmpeg" "libswscale.a" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libswscale.a"
# 外部 Vorbis 和 Opus 实现属于 FFmpeg 静态链接闭包，必须与 avcodec 同配置发布。
copyMainLib "xiph" "libvorbisenc.a" "3rdpty/xiph_inst/lib/libvorbisenc.a"
# Vorbis 编码归档需要基础 Vorbis 算法归档共同解析。
copyMainLib "xiph" "libvorbis.a" "3rdpty/xiph_inst/lib/libvorbis.a"
# Ogg 和 Opus 与编码库来自同一配置，不能跨工具链复用。
copyMainLib "xiph" "libogg.a" "3rdpty/xiph_inst/lib/libogg.a"
copyMainLib "xiph" "libopus.a" "3rdpty/xiph_inst/lib/libopus.a"
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
# LuaJIT 与 SVG 渲染仅由主应用直接消费。
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
copyMainLib "openal" "libopenal.a" "lib/libopenal.a" "lib/libOpenAL.a" "lib/libOpenAL32.a"
copyMainLib "rubberband" "librubberband.a" "rb_inst/lib/librubberband.a"
# SDL、spdlog 与 zlib 使用构建树或安装前缀中的规范归档。
copyMainLib "sdl" "libSDL3.a" "lib/libSDL3.a"
copyMainLib "spdlog" "${spdlogOutputName}" "lib/${spdlogOutputName}" "lib/libspdlog.a" "lib/libspdlogd.a"
copyMainLib "zlib" "libz.a" "3rdpty/zlib_inst/lib/libz.a"

# 引擎预编译根只包含其直接依赖，不复制主应用 UI 与网络栈。
# FFmpeg 组件与主仓复用相同源产物，输出名保持一致。
copyIceLib "ffmpeg" "libavcodec.a" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libavcodec.a"
# avformat 与 avutil 必须来自同一构建配置。
copyIceLib "ffmpeg" "libavformat.a" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libavformat.a"
copyIceLib "ffmpeg" "libavutil.a" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libavutil.a"
# 两个转换组件分别覆盖音频和视频数据路径。
copyIceLib "ffmpeg" "libswresample.a" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libswresample.a"
copyIceLib "ffmpeg" "libswscale.a" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libswscale.a"
# 引擎独立消费预编译 FFmpeg 时也需要同版 Xiph 静态归档。
copyIceLib "xiph" "libvorbisenc.a" "3rdpty/xiph_inst/lib/libvorbisenc.a"
copyIceLib "xiph" "libvorbis.a" "3rdpty/xiph_inst/lib/libvorbis.a"
copyIceLib "xiph" "libogg.a" "3rdpty/xiph_inst/lib/libogg.a"
copyIceLib "xiph" "libopus.a" "3rdpty/xiph_inst/lib/libopus.a"
# FFTW、fmt、LAME 与 libsamplerate 构成 DSP 和编码依赖。
copyIceLib "fftw" "libfftw3.a" "3rdpty/fftw_inst/lib/libfftw3.a"
copyIceLib "fmt" "${fmtOutputName}" "lib/${fmtOutputName}" "lib/libfmt.a" "lib/libfmtd.a"
copyIceLib "lame" "libmp3lame.a" "3rdpty/lame_inst/lib/libmp3lame.a"
copyIceLib "libsamplerate" "libsamplerate.a" "3rdpty/libsamplerate/libsamplerate.a"
# OpenAL 接受多个历史静态库名称并规范输出为 libopenal.a。
copyIceLib "openal" "libopenal.a" "lib/libopenal.a" "lib/libOpenAL.a" "lib/libOpenAL32.a"
# 最后复制时间伸缩、平台、日志与压缩依赖。
copyIceLib "rubberband" "librubberband.a" "rb_inst/lib/librubberband.a"
copyIceLib "sdl" "libSDL3.a" "lib/libSDL3.a"
copyIceLib "spdlog" "${spdlogOutputName}" "lib/${spdlogOutputName}" "lib/libspdlog.a" "lib/libspdlogd.a"
copyIceLib "zlib" "libz.a" "3rdpty/zlib_inst/lib/libz.a"

# 维护约束：新增归档时必须先确认对应 Find 模块导出的 target 名称。
# 维护约束：输出文件名必须与源码构建 target 的实际产物一致。
# 维护约束：主仓专属库不得写入引擎内部预编译根。
# 维护约束：引擎公共依赖需要同时评估两个预编译根。
# 维护约束：所有目标目录必须保留 platform/package/libs 层级。
# 维护约束：Linux 架构层当前固定 x86_64，不从宿主自动推断。
# 维护约束：toolchain 层只描述 gcc 或 clang 等 ABI 家族。
# 维护约束：compiler tag 必须编码实际编译器版本标签。
# 维护约束：Debug 与 RelWithDebInfo 归档不得写入同一配置目录。
# 维护约束：静态归档必须保留调试段，staging 不执行 strip。
# 维护约束：候选源路径按从具体安装产物到兼容名称排列。
# 维护约束：候选命中首项后停止，避免混合多个构建树结果。
# 维护约束：包过滤器只控制复制，不自动补齐传递依赖。
# 维护约束：scope=all 应使用同一次源构建更新两个仓库。
# 维护约束：缺少清单项必须失败，不能发布部分依赖集合。
# 维护约束：install 权限固定 0644，归档不应带可执行位。
# 维护约束：源构建树保持只读，不移动或重命名其中产物。
# 维护约束：脚本不负责 Git LFS add、commit 或 push。
# 维护约束：staging 后应检查所有新二进制均由 LFS 跟踪。
# 维护约束：更新头文件时应与 stage-prebuilt-headers 同批完成。
# 维护约束：跨编译器发布前需执行对应预编译消费构建。
# 维护约束：主仓和引擎仓同名依赖必须保持 API 与 ABI 同步。
# 维护约束：新增配置名需要同步 Find 模块的配置映射规则。
# 维护约束：动态库布局不由本脚本处理，禁止混入 shared 子目录。
# 维护约束：成功到达文件末尾表示所有选中归档均已复制。
