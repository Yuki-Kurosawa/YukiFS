#! /bin/bash

set -e

# Kernel version comes from uname (dynamic).  To target a release that
# differs from the running host, put the fake uname (fakebin/uname) first
# on PATH — e.g. YUKI_FAKE_KERNEL=6.18.0 for the 6.18 integration test.
KERNEL_VERSION=$(uname -r | cut -d. -f1-2)
echo "Kernel version: $KERNEL_VERSION"
tar acvf ../fs.tar.gz .

# get the kernel sources

if ! [ -f ../kernel.tar.gz ]; then
    curl https://www.kernel.org/pub/linux/kernel/v6.x/linux-${KERNEL_VERSION}.tar.gz -o ../kernel.tar.gz
fi

# extract the kernel sources
tar -xvf ../kernel.tar.gz

# put this to the current kernel

SRC_DIR=$(pwd)
cd linux-${KERNEL_VERSION}/fs
mkdir -p yukifs
cd yukifs
tar xvf ../../../../fs.tar.gz
# adapt project-internal headers for the in-tree build
# (module sources reference them as ../../include/xxx.h, which would resolve
#  into the kernel tree; copy them here and rewrite the includes instead)
cp "${SRC_DIR}/include/internal.h" "${SRC_DIR}/include/version.h" "${SRC_DIR}/include/file_table.h" ./
sed -i 's#\.\./\.\./include/##' misc.h file.h inode.c
cd ../../../
rm ../fs.tar.gz

cd linux-${KERNEL_VERSION}

# register the yukifs subdir in fs/Makefile
echo 'obj-$(CONFIG_YUKI_FS) += yukifs/' >> ./fs/Makefile

# patch fs Kconfig

# remove the last 2 lines from  fs/Kconfig
sed -i '$d' ./fs/Kconfig
sed -i '$d' ./fs/Kconfig


cat ../fs.Kconfig >> ./fs/Kconfig


# compile the kernel
make defconfig
# non-interactive: enable yukifs, built-in (=y) for the QEMU integration test
./scripts/config --enable YUKI_FS
make olddefconfig
grep CONFIG_YUKI_FS .config
make -j$(nproc) bzImage

#cd ..


# built-in (=y) integration test: modules_install not needed on this host
# make modules_install

# keep bzImage for the QEMU integration test
cp arch/x86/boot/bzImage ../bzImage-${KERNEL_VERSION}-yuki

cd ..
rm -rvf linux-${KERNEL_VERSION}
rm -rvf ../fs.tar.gz