#! /bin/bash

set -e

KERNEL_VERSION=$(uname -r | tr -d '\n')
KERNEL_MAJOR=$(echo $KERNEL_VERSION | cut -d '.' -f 1)
KERNEL_MINOR=$(echo $KERNEL_VERSION | cut -d '.' -f 2)

KERNEL_VERSION=${KERNEL_MAJOR}.${KERNEL_MINOR}
echo "Kernel version: $KERNEL_VERSION"
tar acvf ../fs.tar.gz .

# get the kernel sources

if ! [ -f ../kernel.tar.gz ]; then
    curl https://www.kernel.org/pub/linux/kernel/v${KERNEL_MAJOR}.x/linux-${KERNEL_VERSION}.tar.gz -o ../kernel.tar.gz
fi

# extract the kernel sources
tar -xvf ../kernel.tar.gz

# put this to the current kernel

cd linux-${KERNEL_VERSION}/fs
mkdir yukifs
cd yukifs
tar xvf ../../../../fs.tar.gz
cd ../../../
rm ../fs.tar.gz

cd linux-${KERNEL_VERSION}

# patch fs Kconfig

# remove the last 2 lines from  fs/Kconfig
sed -i '$d' ./fs/Kconfig
sed -i '$d' ./fs/Kconfig


cat ../fs.Kconfig >> ./fs/Kconfig


# compile the kernel
make defconfig
make menuconfig
#make -j$(nproc)

#cd ..


make modules_install

cd ..
rm -rvf linux-${KERNEL_VERSION}
rm -rvf ../fs.tar.gz