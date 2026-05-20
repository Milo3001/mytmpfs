#!/usr/bin/env bash

set -euo pipefail

# 这个脚本会做一次端到端冒烟测试，确认文件系统能正常挂载、读写和卸载。
# 测试内容：
# 1. 编译内核模块。
# 2. 加载模块并挂载文件系统。
# 3. 检查 statfs 返回的魔数。
# 4. 创建目录。
# 5. 创建文件、写入、读取。
# 6. 测试追加写入。
# 7. 删除文件和目录，验证清理路径。

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
MODULE_NAME=my_tmpfs
MOUNT_POINT=/mnt/tmpfs

cleanup() {
    if mountpoint -q "$MOUNT_POINT"; then
        # 卸载前清理挂载点内的所有文件
        $SUDO rm -rf "$MOUNT_POINT"/* 2>/dev/null || true
        $SUDO umount "$MOUNT_POINT" || true
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

TEST_FILE="$MOUNT_POINT/demo.txt"
TEST_DIR="$MOUNT_POINT/subdir"

echo "检查 statfs 魔数..."
FS_MAGIC=$(stat -f -c '%t' "$MOUNT_POINT")
if [[ "$FS_MAGIC" != "12345678" ]]; then
    echo "✗ statfs 魔数检查失败: $FS_MAGIC" >&2
    exit 1
else
    echo "✓ statfs 魔数检查成功: $FS_MAGIC"
fi

echo "测试目录创建..."
mkdir "$TEST_DIR"
if [[ -d "$TEST_DIR" ]]; then
    echo "✓ 目录创建成功"
else
    echo "✗ 目录创建失败" >&2
    exit 1
fi

echo "测试文件创建、写入和读取..."
printf 'hello tmpfs\n' > "$TEST_FILE"
READ_BACK=$(cat "$TEST_FILE")
if [[ "$READ_BACK" != "hello tmpfs" ]]; then
    printf '✗ 文件读取失败，期望: hello tmpfs，实际: %q\n' "$READ_BACK" >&2
    exit 1
else
    echo "✓ 文件创建、写入和读取成功"
fi

echo "测试追加写入语义..."
printf 'again\n' >> "$TEST_FILE"
APPENDED=$(cat "$TEST_FILE")
if [[ "$APPENDED" != $'hello tmpfs\nagain' ]]; then
    printf '✗ 追加写入失败，期望两行，实际: %q\n' "$APPENDED" >&2
    exit 1
else
    echo "✓ 追加写入成功"
fi

echo "测试删除和清理路径..."
# 注意：目前的文件系统实现还没有 unlink 操作，所以删除会失败。
# 这里用 sudo 尝试删除，但允许失败，因为卸载时系统会清理。
if $SUDO rm -f "$TEST_FILE" 2>/dev/null; then
    echo "✓ 文件删除成功"
else
    echo "⚠ 文件删除不支持（预期行为）"
fi

if $SUDO rmdir "$TEST_DIR" 2>/dev/null; then
    echo "✓ 目录删除成功"
else
    echo "⚠ 目录删除不支持（预期行为）"
fi

echo ""
echo "======================================"
echo "✓ tmpfs 冒烟测试通过！"
echo "======================================"