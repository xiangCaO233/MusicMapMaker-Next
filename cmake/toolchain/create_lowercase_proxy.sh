#!/bin/bash
# create_lowercase_proxy.sh
# 为大小写敏感的 Linux 文件系统生成 Windows SDK 小写路径代理树。
# 第一个参数是代理根，其余参数是按优先级处理的 SDK 源目录。
# 脚本只创建缺失符号链接，不覆盖既有代理，允许多次增量执行。
TARGET_DIR="$1"
shift

# 目标根预先存在后，后续每个文件只需创建对应的父目录。
mkdir -p "$TARGET_DIR"

# 多个 SDK 目录按参数顺序合并，较早来源在名称冲突时优先。
for src in "$@"; do
    # 可选组件目录缺失不会中断其余 SDK 根的代理生成。
    if [ ! -d "$src" ]; then
        echo "Warning: Source directory $src not found, skipping."
        continue
    fi
    echo "Processing $src..."
    # 使用 NUL 分隔路径，完整保留 Windows SDK 文件名中的空格。
    find "$src" -maxdepth 20 -type f -print0 | while IFS= read -r -d '' file; do
        # 相对路径用于在代理根中复刻源目录层次。
        rel_path="${file#$src/}"
        # 整段路径转为小写，兼容第三方源码使用的非规范 include 拼写。
        lower_path=$(echo "$rel_path" | tr '[:upper:]' '[:lower:]')
        target_file="$TARGET_DIR/$lower_path"
        
        # 父目录沿用转换后的层级，避免链接目标与代理路径混淆。
        mkdir -p "$(dirname "$target_file")"
        
        # 已有路径视为更高优先级来源，不覆盖文件或既有符号链接。
        if [ ! -e "$target_file" ]; then
            ln -s "$file" "$target_file"
        fi
    done
done
