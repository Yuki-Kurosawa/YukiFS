#! /bin/bash
clear
dd if=/dev/zero of=test.img bs=1KiB count=10
make -f Makefile.mkfs
./mkfs.yukifs -v
./mkfs.yukifs -h
# ./mkfs.yukifs /dev/memory/20480
./mkfs.yukifs -y test.img
make clean
#rm -rf test.img