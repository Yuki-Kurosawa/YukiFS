#! /bin/bash
unset LANG
unset LANGUAGE

clear
make all install > /dev/null 2>&1
modinfo src/ko/yukifs.ko
insmod src/ko/yukifs.ko
mkdir fs
cat /proc/filesystems | grep yuki

dd if=/dev/zero of=test.img bs=1KiB count=100
mkfs.yukifs -y test.img

mount -t yuki -o loop test.img fs
mount | grep yuki

ls -alc . | grep fs
ls -alc fs
touch fs/test.txt
cat fs/version.txt
cd fs/test_folder
cd ../../
file fs/version.txt

df -kh fs
umount fs
rm -rvf fs
rmmod yukifs
make remove clean > /dev/null 2>&1

rm -rvf test.img