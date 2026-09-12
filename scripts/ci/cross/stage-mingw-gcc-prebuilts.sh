#!/usr/bin/env bash
# 把 MinGW 源码构建树中的静态库复制到主仓与 IonCachyEngine 预编译布局。
# 目标路径编码平台、包、架构、工具链、compiler tag 和构建配置。
# --scope 控制主仓或引擎仓，--packages 支持只更新指定包的增量发布。
# 脚本只复制已存在归档；任一请求库缺失都会立即失败，禁止不完整 staging。
# Debug 包名差异在复制清单前统一解析，其他配置沿用发布归档名。
set -euo pipefail

# 输出用法与所有默认值，不访问构建树。
showUsage() {
    cat <<'EOF'
Usage: scripts/ci/cross/stage-mingw-gcc-prebuilts.sh [options]

Copy MinGW source-build static libraries into the prebuilt layout.

Options:
  --build-dir <path>      Source build directory. Default: build_cross_mingw_gcc_sources
  --build-type <type>     Prebuilt config directory. Default: RelWithDebInfo
  --compiler-tag <tag>    Prebuilt compiler tag. Default: ucrt64
  --scope <all|main|ice>  Staging scope. Default: all
  --packages <list>       Comma-separated packages to stage. Default: all
  -h, --help              Show this help
EOF
}

# 将相对构建目录解析到项目根，绝对路径保持不变。
projectPath() {
    local inputPath="$1"

    if [[ "${inputPath}" = /* ]]; then
        # POSIX 绝对路径可直接使用，不做 realpath 以允许尚未验证的输入。
        printf "%s\n" "${inputPath}"
    else
        # CI 常从项目根调用，但函数不依赖当前 shell 工作目录。
        printf "%s/%s\n" "${projectRoot}" "${inputPath}"
    fi
}

# 复制一个静态库到给定预编译根，并应用包过滤器。
# sourceRelativePath 相对源构建树，outputName 是目标归档文件名。
copyLib() {
    local prebuiltRoot="$1"
    local packageName="$2"
    local sourceRelativePath="$3"
    local outputName="$4"

    if ! shouldStagePackage "${packageName}"; then
        # 未选中的包正常跳过，不计为缺失或错误。
        return 0
    fi

    # 源路径来自同一完整依赖构建树。
    local sourcePath="${buildDir}/${sourceRelativePath}"
    # 目标目录严格匹配 Find 模块约定的 MinGW 静态布局。
    local outputPath="${prebuiltRoot}/binaries/windows/${packageName}/libs/x86_64/mingw/${compilerTag}/${buildType}/${outputName}"

    if [[ ! -f "${sourcePath}" ]]; then
        # 显式请求的包缺失时禁止留下半更新预编译集合。
        printf "error: source library not found: %s\n" "${sourcePath}" >&2
        exit 1
    fi

    # install -D 原子化创建父目录并统一归档权限。
    install -D -m 0644 "${sourcePath}" "${outputPath}"
    printf "staged %s\n" "${outputPath#${projectRoot}/}"
}

# 主仓复制入口尊重 ice-only scope，其余参数交给统一实现。
copyMainLib() {
    if [[ "${stageScope}" = "ice" ]]; then
        # 只更新引擎内部依赖时不触碰主仓预编译目录。
        return 0
    fi

    copyLib "${projectRoot}/3rdpty/prebuilts" "$@"
}

# 引擎仓复制入口尊重 main-only scope。
copyIceLib() {
    if [[ "${stageScope}" = "main" ]]; then
        # 只更新主仓时保留 IonCachyEngine 仓当前预编译状态。
        return 0
    fi

    copyLib "${projectRoot}/3rdpty/sources/IonCachyEngine/3rdpty/prebuilts" "$@"
}

# 绝对项目根使复制路径与调用者工作目录无关。
scriptDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
projectRoot="$(cd "${scriptDir}/../../.." && pwd)"

# 判断包名是否位于逗号包围的过滤列表中，避免子串误匹配。
shouldStagePackage() {
    local packageName="$1"

    [[ -z "${packageFilter}" || ",${packageFilter}," == *",${packageName},"* ]]
}

# 默认值匹配交叉源码构建脚本的输出目录与发布配置。
buildDir="build_cross_mingw_gcc_sources"
buildType="RelWithDebInfo"
compilerTag="${MINGW_GCC_PREBUILT_COMPILER_TAG:-ucrt64}"
# all 同时更新主仓与引擎仓的重叠依赖。
stageScope="all"
# 空过滤器表示复制清单中的所有包。
packageFilter=""

# 逐项解析参数并在缺值时立即失败，避免 shift 越界。
while (( $# > 0 )); do
    case "$1" in
        --build-dir)
            # 构建目录可为相对项目根或绝对路径。
            if (( $# < 2 )); then
                printf "error: --build-dir requires a value\n" >&2
                exit 1
            fi
            buildDir="$2"
            shift 2
            ;;
        --build-type)
            # 配置名直接形成目标目录层。
            if (( $# < 2 )); then
                printf "error: --build-type requires a value\n" >&2
                exit 1
            fi
            buildType="$2"
            shift 2
            ;;
        --compiler-tag)
            # tag 区分 ucrt64、gcc14-win32 或 clang64 等 ABI。
            if (( $# < 2 )); then
                printf "error: --compiler-tag requires a value\n" >&2
                exit 1
            fi
            compilerTag="$2"
            shift 2
            ;;
        --scope)
            # scope 仅接受后续 case 校验的三个值。
            if (( $# < 2 )); then
                printf "error: --scope requires a value\n" >&2
                exit 1
            fi
            stageScope="$2"
            shift 2
            ;;
        --packages)
            # 过滤列表使用逗号分隔且不包含空格归一化。
            if (( $# < 2 )); then
                printf "error: --packages requires a value\n" >&2
                exit 1
            fi
            packageFilter="$2"
            shift 2
            ;;
        -h | --help)
            # 帮助成功退出，不验证构建目录。
            showUsage
            exit 0
            ;;
        *)
            # 未知参数打印完整用法并返回调用错误。
            printf "error: unknown option: %s\n" "$1" >&2
            showUsage >&2
            exit 1
            ;;
    esac
done

if [[ -z "${compilerTag}" ]]; then
    # 空 tag 会破坏目录 ABI 边界，禁止回退无标签路径。
    printf "error: --compiler-tag must not be empty\n" >&2
    exit 1
fi

case "${stageScope}" in
    all | main | ice)
        # 三个合法值在复制 wrapper 中分别解释。
        ;;
    *)
        # 非法 scope 在任何文件写入前失败。
        printf "error: --scope must be one of all, main, ice: %s\n" "${stageScope}" >&2
        exit 1
        ;;
esac

# 参数完成校验后再解析最终源构建路径。
buildDir="$(projectPath "${buildDir}")"

if [[ ! -d "${buildDir}" ]]; then
    # staging 不负责触发源码构建，源目录必须由前置任务生成。
    printf "error: build directory not found: %s\n" "${buildDir}" >&2
    exit 1
fi

# 发布配置使用无 d 后缀的常规归档名。
fmtLib="lib/libfmt.a"
fmtOutputName="libfmt.a"
freetypeLib="lib/libfreetype.a"
freetypeOutputName="libfreetype.a"
spdlogLib="lib/libspdlog.a"
spdlogOutputName="libspdlog.a"

case "${buildType}" in
    Debug | debug)
        # 仅 fmt、freetype 与 spdlog 在当前构建中使用显式 Debug 文件名。
        fmtLib="lib/libfmtd.a"
        fmtOutputName="libfmtd.a"
        freetypeLib="lib/libfreetyped.a"
        freetypeOutputName="libfreetyped.a"
        spdlogLib="lib/libspdlogd.a"
        spdlogOutputName="libspdlogd.a"
        ;;
esac

# 主仓专属 UI、应用与网络依赖。
copyMainLib "ImGuiFileDialog" "lib/libImGuiFileDialog.a" "libImGuiFileDialog.a"
# 自维护音频引擎本体以静态目标名进入主仓预编译目录。
copyMainLib "IonCachyEngine" "lib/libIonCachyEngine-static.a" "libIonCachyEngine-static.a"
# curl 提供更新与网络下载支持。
copyMainLib "curl" "lib/libcurl.a" "libcurl.a"
# FFmpeg 五个组件来自引擎依赖安装前缀，但主程序也直接链接。
copyMainLib "ffmpeg" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libavcodec.a" "libavcodec.a"
# avformat 处理媒体容器封装与探测。
copyMainLib "ffmpeg" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libavformat.a" "libavformat.a"
# avutil 提供 FFmpeg 共享数据结构和工具函数。
copyMainLib "ffmpeg" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libavutil.a" "libavutil.a"
# swresample 提供音频采样格式和采样率转换。
copyMainLib "ffmpeg" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libswresample.a" "libswresample.a"
# swscale 供视频像素格式与尺寸转换路径使用。
copyMainLib "ffmpeg" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libswscale.a" "libswscale.a"
# FFTW 与格式化、字体库使用各自规范化输出名。
copyMainLib "fftw" "3rdpty/fftw_inst/lib/libfftw3.a" "libfftw3.a"
# fmt 文件名随 Debug 配置选择 d 后缀。
copyMainLib "fmt" "${fmtLib}" "${fmtOutputName}"
# FreeType 同样区分 Debug 归档名称。
copyMainLib "freetype" "${freetypeLib}" "${freetypeOutputName}"
# 图形 UI 栈按 GLFW、ImGui、ImPlot 顺序列出。
copyMainLib "glfw" "lib/libglfw3.a" "libglfw3.a"
# ImGui 使用项目构建的 static 目标名称。
copyMainLib "imgui" "lib/libimgui-static.a" "libimgui-static.a"
# ImPlot 归档保留 3rd 前缀以匹配 Find 脚本。
copyMainLib "implot" "lib/lib3rd_implot.a" "lib3rd_implot.a"
# 音频编解码和重采样依赖来自各自安装或子构建目录。
copyMainLib "lame" "3rdpty/lame_inst/lib/libmp3lame.a" "libmp3lame.a"
# libsamplerate 的构建输出位于依赖专用子目录。
copyMainLib "libsamplerate" "3rdpty/libsamplerate/libsamplerate.a" "libsamplerate.a"
# LuaJIT 与 SVG 渲染库只属于主应用脚本和 UI 层。
copyMainLib "luajit" "luajit/src/libluajit.a" "libluajit.a"
# lunasvg 依赖的 plutovg 作为同包第二归档发布。
copyMainLib "lunasvg" "lib/liblunasvg.a" "liblunasvg.a"
copyMainLib "lunasvg" "lib/libplutovg.a" "libplutovg.a"
# miniz 与 mbedTLS 组件构成压缩和 TLS 基础能力。
copyMainLib "miniz" "lib/lib3rd_miniz.a" "lib3rd_miniz.a"
# mbedcrypto 提供 TLS 基础密码算法。
copyMainLib "mbedtls" "lib/libmbedcrypto.a" "libmbedcrypto.a"
# mbedx509 提供证书解析和验证。
copyMainLib "mbedtls" "lib/libmbedx509.a" "libmbedx509.a"
# mbedtls 主归档提供协议层实现。
copyMainLib "mbedtls" "lib/libmbedtls.a" "libmbedtls.a"
# everest 与 p256m 是当前 mbedTLS 配置的椭圆曲线依赖。
copyMainLib "mbedtls" "lib/libeverest.a" "libeverest.a"
copyMainLib "mbedtls" "lib/libp256m.a" "libp256m.a"
# WebRTC 数据通道依赖按 libjuice、usrsctp、libdatachannel 闭包复制。
copyMainLib "libjuice" "lib/libjuice-static.a" "libjuice-static.a"
# usrsctp 实现数据通道使用的 SCTP 传输。
copyMainLib "usrsctp" "lib/libusrsctp.a" "libusrsctp.a"
# libdatachannel 使用 static 后缀与源码 target 对齐。
copyMainLib "libdatachannel" "lib/libdatachannel-static.a" "libdatachannel-static.a"
# 文件对话框、OpenAL 和 Rubber Band 支持主程序交互与音频处理。
copyMainLib "nativefiledialog-extended" "lib/libnfd.a" "libnfd.a"
# OpenAL 归档沿用 Windows 风格库名 OpenAL32。
copyMainLib "openal" "lib/libOpenAL32.a" "libOpenAL32.a"
# Rubber Band 安装前缀由依赖构建脚本固定为 rb_inst。
copyMainLib "rubberband" "rb_inst/lib/librubberband.a" "librubberband.a"
# SDL、spdlog 与 zlib 完成平台、日志和压缩依赖集合。
copyMainLib "sdl" "lib/libSDL3.a" "libSDL3.a"
# spdlog Debug 名称通过前置变量统一选择。
copyMainLib "spdlog" "${spdlogLib}" "${spdlogOutputName}"
# zlib 来源于独立安装前缀而非顶层 lib。
copyMainLib "zlib" "3rdpty/zlib_inst/lib/libz.a" "libz.a"

# IonCachyEngine 内部预编译根只复制其实际直接依赖。
# FFmpeg 组件保持与主仓相同来源和输出文件名。
copyIceLib "ffmpeg" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libavcodec.a" "libavcodec.a"
# 引擎 avformat 与 codec 来自同一安装前缀。
copyIceLib "ffmpeg" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libavformat.a" "libavformat.a"
# avutil 版本必须匹配其他 FFmpeg 归档。
copyIceLib "ffmpeg" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libavutil.a" "libavutil.a"
# swresample 服务引擎音频转换链。
copyIceLib "ffmpeg" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libswresample.a" "libswresample.a"
# swscale 保留给视频帧转换路径。
copyIceLib "ffmpeg" "3rdpty/sources/IonCachyEngine/3rdpty/sources/ffmpeg_install/lib/libswscale.a" "libswscale.a"
# FFTW、fmt 与音频编解码库构成引擎处理链。
copyIceLib "fftw" "3rdpty/fftw_inst/lib/libfftw3.a" "libfftw3.a"
# fmt 复用当前配置选择后的归档名称。
copyIceLib "fmt" "${fmtLib}" "${fmtOutputName}"
# LAME 提供 MP3 编码能力。
copyIceLib "lame" "3rdpty/lame_inst/lib/libmp3lame.a" "libmp3lame.a"
# libsamplerate 提供独立重采样实现。
copyIceLib "libsamplerate" "3rdpty/libsamplerate/libsamplerate.a" "libsamplerate.a"
# OpenAL、Rubber Band 与 SDL 提供播放、伸缩和平台音频设施。
copyIceLib "openal" "lib/libOpenAL32.a" "libOpenAL32.a"
copyIceLib "rubberband" "rb_inst/lib/librubberband.a" "librubberband.a"
copyIceLib "sdl" "lib/libSDL3.a" "libSDL3.a"
# spdlog 与 zlib 是引擎内部日志和压缩基础依赖。
copyIceLib "spdlog" "${spdlogLib}" "${spdlogOutputName}"
# 最后一项 zlib 使用依赖安装前缀中的规范归档名。
copyIceLib "zlib" "3rdpty/zlib_inst/lib/libz.a" "libz.a"
