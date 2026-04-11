#!/bin/bash
# create_lowercase_proxy.sh
TARGET_DIR="$1"
shift

mkdir -p "$TARGET_DIR"

for src in "$@"; do
    if [ ! -d "$src" ]; then
        echo "Warning: Source directory $src not found, skipping."
        continue
    fi
    echo "Processing $src..."
    # 使用 -print0 处理空格
    find "$src" -maxdepth 20 -type f -print0 | while IFS= read -r -d '' file; do
        rel_path="${file#$src/}"
        # 核心修改：将整个相对路径转为小写
        lower_path=$(echo "$rel_path" | tr '[:upper:]' '[:lower:]')
        target_file="$TARGET_DIR/$lower_path"
        
        # 确保目标目录存在（也是小写的）
        mkdir -p "$(dirname "$target_file")"
        
        if [ ! -e "$target_file" ]; then
            ln -s "$file" "$target_file"
        fi
    done
done
