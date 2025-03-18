#! /bin/bash
clear
cd src/mkfs
unset LANG
unset LANGUAGE
dd if=/dev/zero of=test.img bs=1KiB count=10
make
./mkfs.yukifs -v
./mkfs.yukifs -h
# ./mkfs.yukifs /dev/memory/20480
./mkfs.yukifs -y test.img
chmod a+x test.img
file test.img
./test.img
#objdump -x test.img
#objdump -d test.img
#readelf -a test.img
make clean
rm -rf test.img
cd ../../