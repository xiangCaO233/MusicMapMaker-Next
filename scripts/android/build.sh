#!/usr/bin/env bash
# @brief Linux 开发机上的 Android 实验构建入口，所有生成物集中到仓库外。
set -euo pipefail
# 脚本位置确定源码根，不依赖调用终端当前目录。
android_repo="$(git -C "$(dirname "$(readlink -f "$0")")" rev-parse --show-toplevel)"
cd "$android_repo"
# SDK 复用已有安装；工作目录与源码目录必须分离。
export ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
android_work="${MMM_ANDROID_WORK_DIR:-$ANDROID_HOME/../mmm-smoke/main}"
# 先规范化路径再检查边界，防止通过 ../ 意外把生成物放回仓库。
# 用户可用环境变量迁移整棵工作树，源码中只保存可重建的配置。
android_work="$(realpath -m "$android_work")"
if [[ "$android_work" == "$android_repo" || "$android_work" == "$android_repo/"* ]]; then
    echo 'Android work directory must be outside the repository.' >&2
    exit 1
fi
# 固定 NDK 与已验证 JDK，系统 Java 默认版本保持不变。
# JDK 固定仅限本构建进程，避免影响 IDE、桌面应用或其他仓库的 Java 选择。
android_ndk="$ANDROID_HOME/ndk/30.0.16248370"
export JAVA_HOME="${MMM_ANDROID_JAVA_HOME:-/usr/lib/jvm/openjdk-21}"
# 两个 Android 验证工程共用下载缓存，不能回到用户的全局 Gradle 缓存。
# 下载缓存可与前一个最小探针共用，项目缓存按 main 单独隔离。
# Gradle 临时文件和签名也归入该工作树，不能写入新的源码侧隐藏目录。
export GRADLE_USER_HOME="${MMM_ANDROID_GRADLE_CACHE:-$android_work/../cache/gradle}"
export TMPDIR="$android_work/cache/tmp"
mkdir -p "$android_work"/{cache,evidence,dependencies,build/assets,build/jni,apk} "$TMPDIR"
if [[ ! -x "$JAVA_HOME/bin/java" || ! -f "$android_ndk/build/cmake/android.toolchain.cmake" ]]; then
    echo 'Missing pinned NDK or compatible JDK; see Android setup instructions.' >&2
    exit 1
fi
# NDK 只提供 Vulkan C 头，Vulkan-Hpp 使用固定版本完整官方头文件集合。
# 头文件版本只决定编译接口，运行时支持仍由设备 Vulkan 能力决定。
# 标签固定，重新构建不会自动跟随上游主分支变化。
android_vulkan_tag=vulkan-sdk-1.4.363.0
android_headers="$android_work/dependencies/Vulkan-Headers"
if [[ ! -f "$android_headers/include/vulkan/vulkan.hpp" ]]; then
    android_archive="$android_work/cache/Vulkan-Headers-$android_vulkan_tag.tar.gz"
    curl -fL --retry 2 -o "$android_archive" \
        "https://codeload.github.com/KhronosGroup/Vulkan-Headers/tar.gz/refs/tags/$android_vulkan_tag"
    # 解包只写依赖目录，不能创建主仓 3rdpty 的下游源码补丁。
    mkdir -p "$android_headers"
    tar -xzf "$android_archive" -C "$android_headers" --strip-components=1
fi
# 每个 ABI 都有独立依赖和构建树，模拟器不能复用宿主 Linux 或手机归档。
# 两个 ABI 的目标三元组均由 NDK 提供，宿主 Linux 的 x86_64 库不能用于模拟器。
# 各 ABI 的 CMake 缓存分离，重复构建只执行必要的增量编译和资源更新。
android_abis="${MMM_ANDROID_ABIS:-arm64-v8a x86_64}"
for android_abi in $android_abis; do
    case "$android_abi" in
        arm64-v8a) android_triple=aarch64-linux-android ;;
        x86_64) android_triple=x86_64-linux-android ;;
        *) echo "Unsupported Android ABI: $android_abi" >&2; exit 1 ;;
    esac
    android_native="$android_work/build/$android_abi"
    cmake -S "$android_repo" -B "$android_native" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$android_ndk/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$android_abi" -DANDROID_PLATFORM=android-28 -DANDROID_STL=c++_shared \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSOURCES_BUILD=ON -DBUILD_TESTING=OFF \
        -DMMM_SYNC_TRANSLATIONS_AND_DEFAULT_SKIN=OFF -DMMM_DISABLE_CLANG_LTO=ON \
        -DMMM_PGO_INSTRUMENT=OFF -DMMM_PGO_USE=OFF \
        -DVulkan_INCLUDE_DIR="$android_headers/include"
    # 原生依赖使用相同 NDK，任何一步失败都不继续生成包含陈旧库的 APK。
    cmake --build "$android_native" --target MusicMapMaker-Next --parallel "${MMM_ANDROID_JOBS:-24}"
    android_jni="$android_work/build/jni/$android_abi"
    mkdir -p "$android_jni"
    while IFS= read -r android_library; do
        [[ -f "$android_library" ]] || { echo "Missing runtime: $android_library" >&2; exit 1; }
        cp "$android_library" "$android_jni/"
    done < "$android_native/android-runtime-libraries.txt"
    # 同一 APK 的各 ABI 各带一份匹配架构的标准库，不混用 NDK 版本。
    cp "$android_ndk/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/$android_triple/libc++_shared.so" "$android_jni/"
done
# 资源 ZIP 和内容标记只生成到集中构建目录，LFS 内容缺失时立即失败。
python3 "$android_repo/scripts/android/package_assets.py" --repo "$android_repo" --output "$android_work/build/assets"
# 独立调试签名只属于实验包，不写入默认 ~/.android/debug.keystore。
if [[ ! -f "$android_work/cache/debug.keystore" ]]; then
    "$JAVA_HOME/bin/keytool" -genkeypair -keystore "$android_work/cache/debug.keystore" \
        -storepass android -keypass android -alias androiddebugkey -keyalg RSA \
        -keysize 2048 -validity 10000 -dname 'CN=MusicMapMaker Android Dev,O=Local,C=CN'
fi
# Java 外壳必须与本次 SDL3 原生源码配套，禁止复用另一版本的 AAR。
# SDL 的 Java 外壳与当前 libSDL3.so 必须成对，避免升级后 JNI 方法签名不一致。
# Gradle 不自行下载或替换 SDL 版本，源码身份由仓库子模块固定。
android_sdl_java="$android_repo/3rdpty/sources/IonCachyEngine/3rdpty/sources/sdl/android-project/app/src/main/java"
# --project-cache-dir 显式覆盖源码旁默认 .gradle，buildDirectory 由 Gradle 外壳覆盖。
# SDK 目录由 ANDROID_HOME 定位，不需要创建包含本机绝对路径的 local.properties。
"$android_repo/packaging/android/gradlew" -p "$android_repo/packaging/android" \
    --project-cache-dir "$android_work/cache/gradle-project" --no-daemon \
    -PmmmAndroidWorkDir="$android_work" -PmmmSdlJavaDir="$android_sdl_java" \
    -PmmmAndroidAbis="${android_abis// /,}" :app:assembleDebug
# 交付 APK 与调试符号分开存放；用户可以整体清理 build，而不碰源码和 SDK。
# 仅复制当前成功构建的包，编译失败时不会把历史文件冒充本轮结果。
# 外置工作树可整体迁移，但应用私有工程工作副本属于用户数据，不能当缓存清理。
cp "$android_work/build/gradle/app/outputs/apk/debug/app-debug.apk" "$android_work/apk/MusicMapMaker-Next-android-debug.apk"
"$ANDROID_HOME/build-tools/36.1.0/apksigner" verify "$android_work/apk/MusicMapMaker-Next-android-debug.apk"
echo "APK: $android_work/apk/MusicMapMaker-Next-android-debug.apk"
