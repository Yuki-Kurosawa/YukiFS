#!/bin/bash
# yk-syntax.sh — syntax-only diagnostic for YukiFS ko sources (no artifacts produced)
cd /home/yuki/YukiFS/src/ko || exit 1
K=/usr/src/linux-headers-6.12.107+deb13-amd64
C=/usr/src/linux-headers-6.12.107+deb13-common
FLAGS="-fsyntax-only -nostdinc -isystem $(gcc -print-file-name=include)"
FLAGS="$FLAGS -I$K/arch/x86/include -I$K/arch/x86/include/generated"
FLAGS="$FLAGS -I$C/include -I$K/include"
FLAGS="$FLAGS -I$C/arch/x86/include -I$C/arch/x86/include/uapi"
FLAGS="$FLAGS -I$K/arch/x86/include/uapi -I$K/arch/x86/include/generated/uapi"
FLAGS="$FLAGS -I$C/include/uapi -I$K/include/generated -I$K/include/generated/uapi"
FLAGS="$FLAGS -I. -include $C/include/linux/kconfig.h -D__KERNEL__ -DMODULE"
for f in misc.c file.c inode.c; do
  echo "===== $f ====="
  gcc $FLAGS "$f" 2>&1 | grep -Ev "ibt.h|nocf_check|attribute ignored" | head -40
done
echo SYNTAX-DIAG-DONE
