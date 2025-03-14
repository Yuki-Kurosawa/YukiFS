#! /bin/bash
clear
make 
modinfo yukifs.ko
insmod yukifs.ko
mkdir fs
cat /proc/filesystems | grep yuki
mount -t yuki yuki fs
mount | grep yuki

ls -alc fs
touch fs/test.txt
cat fs/version.txt
cd fs/test_folder
cd ../..
file fs/version.txt

df -kh fs
umount fs
rm -rvf fs
rmmod yukifs.ko
make clean