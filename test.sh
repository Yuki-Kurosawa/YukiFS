#! /bin/bash
clear
make 
insmod inode.ko
mkdir fs
cat /proc/filesystems | grep yuki
mount -t yuki yuki fs
mount | grep yuki

ls -alc fs
touch fs/test.txt

df -kh fs
umount fs
rm -rvf fs
rmmod inode.ko
make clean