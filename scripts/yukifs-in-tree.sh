#!/bin/sh
# yukifs-in-tree.sh - integrate YukiFS into a Linux kernel source tree fo
# in-tree builds (fs/yukifs/), so the module is built by the kernel's own
# kbuild with no external linux-headers required.
#
# Usage:
#   scripts/yukifs-in-tree.sh <kernel-source-dir> [yukifs-repo-dir]
#
#   <kernel-source-dir>  path to an unpacked Linux source tree
#                        (e.g. linux-6.18.50, linux-7.3-rcX)
#   [yukifs-repo-dir]    defaults to the parent of this script
#
# What it does:
#   1. copies src/ko/*.c *.h + include/{internal,version,file_table}.h
#      into <kernel>/fs/yukifs/
#   2. rewrites the "../../include/..." includes to local "..." includes
#   3. drops fs/yukifs/Makefile (kbuild form) and fs/yukifs/Kconfig
#   4. hooks YUKI_FS into <kernel>/fs/Kconfig, compatible with the classic
#      testkrnl.sh approach: strip trailing blank lines + the closing
#      endmenu, then append fs.Kconfig (which carries its own endmenu).
#      Idempotent: repeated runs are no-ops.
#
# After running, inside the kernel tree:
#   make defconfig
#   ./scripts/config --module YUKI_FS   # module, or --enable for built-in
#   make olddefconfig
#   make -j"$(nproc)"
#
# The resulting module is fs/yukifs/yukifs.ko (or built-in if --enable).
set -eu

KERNEL="${1:-}"
if [ -z "$KERNEL" ]; then
    echo "usage: $0 <kernel-source-dir> [yukifs-repo-dir]" >&2
    exit 2
fi
KERNEL=$(cd "$KERNEL" && pwd)
if [ ! -f "$KERNEL/fs/Kconfig" ]; then
    echo "error: $KERNEL does not look like a kernel source tree (no fs/Kconfig)" >&2
    exit 1
fi

REPO="${2:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}"
KO="$REPO/src/ko"
INC="$REPO/include"
DST="$KERNEL/fs/yukifs"

echo "==> YukiFS repo : $REPO"
echo "==> kernel tree : $KERNEL"
echo "==> target dir  : $DST"

mkdir -p "$DST"

for f in file.c file.h inode.c misc.c misc.h; do
    cp "$KO/$f" "$DST/$f"
done

for f in internal.h version.h file_table.h; do
    cp "$INC/$f" "$DST/$f"
done

# kbuild Makefile (tree-internal form; the src/ko one is for external M= builds)
cat > "$DST/Makefile" <<'EOF'
obj-$(CONFIG_YUKI_FS) += yukifs.o
yukifs-objs := misc.o file.o inode.o
EOF

cp "$REPO/fs.Kconfig" "$DST/Kconfig"

# Rewrite "../../include/X.h" to "X.h" (they now live in the same directory).
# Only the files that actually use the relative form are touched.
sed -i 's|#include "../../include/\([^"]*\)"|#include "\1"|' \
    "$DST/file.h" "$DST/inode.c" "$DST/misc.h"

# Hook into fs/Kconfig (idempotent, testkrnl.sh-compatible).
if ! grep -q "config YUKI_FS" "$KERNEL/fs/Kconfig"; then
    cp "$KERNEL/fs/Kconfig" "$KERNEL/fs/Kconfig.yukifs.bak"
    # drop trailing blank lines, then the closing endmenu
    sed -i -e :a -e '/^[[:space:]]*$/{$d;N;ba' -e '}' "$KERNEL/fs/Kconfig"
    sed -i '$d' "$KERNEL/fs/Kconfig"
    printf '\n' >> "$KERNEL/fs/Kconfig"
    cat "$REPO/fs.Kconfig" >> "$KERNEL/fs/Kconfig"
    echo "==> fs/Kconfig: appended YUKI_FS block (backup at fs/Kconfig.yukifs.bak)"
else
    echo "==> fs/Kconfig: already hooked, nothing to do"
fi

# Hook into fs/Makefile so kbuild descends into fs/yukifs/ when enabled.
# Without this the CONFIG_YUKI_FS symbol is never consumed and the module
# is silently not built (mount then fails with "No such device").
if ! grep -q 'obj-\$(CONFIG_YUKI_FS) += yukifs/' "$KERNEL/fs/Makefile"; then
    printf 'obj-$(CONFIG_YUKI_FS) += yukifs/\n' >> "$KERNEL/fs/Makefile"
    echo "==> fs/Makefile: appended 'obj-\$(CONFIG_YUKI_FS) += yukifs/'"
else
    echo "==> fs/Makefile: already hooked, nothing to do"
fi

echo "==> done. In $KERNEL run:"
echo "    make defconfig"
echo "    ./scripts/config --module YUKI_FS   # or --enable for built-in"
echo "    make olddefconfig && make -j\"\$(nproc)\""
