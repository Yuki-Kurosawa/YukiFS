#! /bin/bash
clear

unset LANG
unset LANGUAGE
dd if=/dev/zero of=test.img bs=1KiB count=100
make > /dev/null 2>&1
make install > /dev/null 2>&1
mkfs.yukifs -v
mkfs.yukifs -h
# mkfs.yukifs /dev/memory/20480
mkfs.yukifs -y test.img
chmod a+x test.img
file test.img
./test.img

if [ -f test.log ]; then
    rm test.log
fi
touch test.log

echo "Test 1: 1024 block size" >> test.log
mkfs.yukifs -y -b 1024 test.img
infofs.yukifs test.img >> test.log

echo "Test 2: 2048 block size" >> test.log
mkfs.yukifs -y -b 2048 test.img
infofs.yukifs test.img >> test.log

echo "Test 3: 4096 block size" >> test.log
mkfs.yukifs -y -b 4096 test.img
infofs.yukifs test.img >> test.log

echo "Test 4: 8192 block size" >> test.log
mkfs.yukifs -y -b 8192 test.img
infofs.yukifs test.img >> test.log

make remove clean > /dev/null 2>&1
rm -rf test.img
cd ../../