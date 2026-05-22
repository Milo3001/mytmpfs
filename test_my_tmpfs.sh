#!/usr/bin/env bash

set -euo pipefail

# 这个脚本会做一次端到端冒烟测试，确认文件系统能正常挂载、读写、截断、
# 重命名/移动、硬链接、符号链接，以及删除和卸载。

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
MODULE_NAME=my_tmpfs
MOUNT_POINT=/mnt/tmpfs

cleanup() {
    if mountpoint -q "$MOUNT_POINT"; then
        # 卸载前清理挂载点内的所有文件
        $SUDO rm -rf "$MOUNT_POINT"/* 2>/dev/null || true
        if ! $SUDO umount "$MOUNT_POINT" 2>/dev/null; then
            $SUDO umount -l "$MOUNT_POINT" 2>/dev/null || true
        fi
    fi

    if lsmod | awk '{print $1}' | grep -qx "$MODULE_NAME"; then
        $SUDO rmmod "$MODULE_NAME" || true
    fi
}

trap cleanup EXIT

if [[ ${EUID:-$(id -u)} -eq 0 ]]; then
    SUDO=""
elif command -v sudo >/dev/null 2>&1; then
    SUDO=sudo
else
    echo "Run this script as root or install sudo." >&2
    exit 1
fi

echo "构建模块..."
make -C "$SCRIPT_DIR"

echo "加载模块..."
# 如果模块已经被加载，先卸载它
if lsmod | awk '{print $1}' | grep -qx "$MODULE_NAME"; then
    echo "模块已经加载，先卸载..."
    $SUDO rmmod "$MODULE_NAME"
fi
$SUDO insmod "$SCRIPT_DIR/my_tmpfs.ko"

echo "创建挂载点 $MOUNT_POINT..."
$SUDO mkdir -p "$MOUNT_POINT" || true

echo "挂载文件系统到 $MOUNT_POINT..."
$SUDO mount -t my_tmpfs none "$MOUNT_POINT"

echo "检查 statfs 魔数..."
FS_MAGIC=$(stat -f -c '%t' "$MOUNT_POINT")
if [[ "$FS_MAGIC" != "12345678" ]]; then
    echo "✗ statfs 魔数检查失败: $FS_MAGIC" >&2
    exit 1
else
    echo "✓ statfs 魔数检查成功: $FS_MAGIC"
fi

echo "测试目录创建..."
mkdir -p "$MOUNT_POINT/src" "$MOUNT_POINT/dst" "$MOUNT_POINT/move-me"
for path in "$MOUNT_POINT/src" "$MOUNT_POINT/dst" "$MOUNT_POINT/move-me"; do
    if [[ ! -d "$path" ]]; then
        echo "✗ 目录创建失败: $path" >&2
        exit 1
    fi
done
echo "✓ 目录创建成功"

echo "测试文件创建、截断和写入..."
printf 'hello tmpfs\n' > "$MOUNT_POINT/src/file.txt"
truncate -s 5 "$MOUNT_POINT/src/file.txt"
TRUNCATED_PREFIX=$(head -c 5 "$MOUNT_POINT/src/file.txt")
if [[ "$TRUNCATED_PREFIX" != "hello" ]]; then
    printf '✗ 截断后内容不对，期望: hello，实际: %q\n' "$TRUNCATED_PREFIX" >&2
    exit 1
fi

printf ' tmpfs' >> "$MOUNT_POINT/src/file.txt"
WRITTEN=$(head -c 11 "$MOUNT_POINT/src/file.txt")
if [[ "$WRITTEN" != "hello tmpfs" ]]; then
    printf '✗ 追加写入失败，期望: hello tmpfs，实际: %q\n' "$WRITTEN" >&2
    exit 1
fi

truncate -s 16 "$MOUNT_POINT/src/file.txt"
TRUNCATE_SIZE=$(stat -c '%s' "$MOUNT_POINT/src/file.txt")
if [[ "$TRUNCATE_SIZE" != "16" ]]; then
    printf '✗ 扩展截断失败，期望大小 16，实际: %s\n' "$TRUNCATE_SIZE" >&2
    exit 1
fi
TRUNCATED_SUFFIX=$(dd if="$MOUNT_POINT/src/file.txt" bs=1 skip=11 count=5 2>/dev/null | od -An -tx1 | tr -d ' \n')
if [[ "$TRUNCATED_SUFFIX" != "0000000000" ]]; then
    printf '✗ 扩展后尾部不是零填充，实际十六进制: %s\n' "$TRUNCATED_SUFFIX" >&2
    exit 1
fi
echo "✓ 截断、扩展和写入成功"

echo "测试硬链接..."
ln "$MOUNT_POINT/src/file.txt" "$MOUNT_POINT/file-hardlink"
printf '!' >> "$MOUNT_POINT/file-hardlink"
if ! cmp -s "$MOUNT_POINT/src/file.txt" "$MOUNT_POINT/file-hardlink"; then
    echo "✗ 硬链接内容不同" >&2
    exit 1
fi
echo "✓ 硬链接成功"

echo "测试文件重命名/移动..."
mv "$MOUNT_POINT/src/file.txt" "$MOUNT_POINT/dst/renamed.txt"
if [[ -e "$MOUNT_POINT/src/file.txt" ]]; then
    echo "✗ 原路径仍然存在" >&2
    exit 1
fi
if ! cmp -s "$MOUNT_POINT/dst/renamed.txt" "$MOUNT_POINT/file-hardlink"; then
    echo "✗ 移动后内容不一致" >&2
    exit 1
fi
echo "✓ 文件重命名/移动成功"

echo "测试符号链接..."
ln -s "$MOUNT_POINT/dst/renamed.txt" "$MOUNT_POINT/file-symlink"
SYMLINK_TARGET=$(readlink "$MOUNT_POINT/file-symlink")
if [[ "$SYMLINK_TARGET" != "$MOUNT_POINT/dst/renamed.txt" ]]; then
    printf '✗ readlink 结果不对，期望: %s，实际: %s\n' "$MOUNT_POINT/dst/renamed.txt" "$SYMLINK_TARGET" >&2
    exit 1
fi
if ! cmp -s "$MOUNT_POINT/file-symlink" "$MOUNT_POINT/dst/renamed.txt"; then
    echo "✗ 符号链接读取失败" >&2
    exit 1
fi
echo "✓ 符号链接成功"

echo "测试目录移动..."
printf 'dir move test\n' > "$MOUNT_POINT/move-me/nested.txt"
mv "$MOUNT_POINT/move-me" "$MOUNT_POINT/dst/move-me"
if [[ -e "$MOUNT_POINT/move-me" ]]; then
    echo "✗ 原目录仍然存在" >&2
    exit 1
fi
MOVED_DIR_CONTENT=$(cat "$MOUNT_POINT/dst/move-me/nested.txt")
if [[ "$MOVED_DIR_CONTENT" != "dir move test" ]]; then
    printf '✗ 目录移动后读取失败，实际: %q\n' "$MOVED_DIR_CONTENT" >&2
    exit 1
fi
echo "✓ 目录移动成功"

echo "测试删除和清理路径..."
rm -f "$MOUNT_POINT/file-hardlink" "$MOUNT_POINT/file-symlink" "$MOUNT_POINT/dst/renamed.txt"
rm -f "$MOUNT_POINT/dst/move-me/nested.txt"
rmdir "$MOUNT_POINT/dst/move-me"
rmdir "$MOUNT_POINT/src"
rmdir "$MOUNT_POINT/dst"
rmdir "$MOUNT_POINT/move-me" 2>/dev/null || true

if [[ -e "$MOUNT_POINT/file-hardlink" || -e "$MOUNT_POINT/file-symlink" || -e "$MOUNT_POINT/dst/renamed.txt" ]]; then
    echo "✗ 清理失败" >&2
    exit 1
fi

echo ""
echo "======================================"
echo "✓ tmpfs 冒烟测试通过！"
echo "======================================"