#! /bin/bash
unset LANG
unset LANGUAGE

clear
make all install > /dev/null 2>&1
modinfo src/ko/yukifs.ko
insmod src/ko/yukifs.ko
mkdir fs
cat /proc/filesystems | grep yuki

dd if=/dev/zero of=test.img bs=1KiB count=45
mkfs.yukifs -y test.img
infofs.yukifs test.img
infofs.yukifs -s test.img #> /dev/null 2>&1

#mount -t yuki -o loop test.img fs
mount test.img fs
mount | grep yuki


file fs
ls -alci . | grep fs
ls -alci fs
touch fs/test.txt
infofs.yukifs -s test.img
echo 123 >> fs/test.txt
echo 456 >> fs/test.txt
echo "O TESTED"
cat fs/test.txt
echo "I TESTED"
ls -alci fs
#cat fs/version.txt
#cd fs/test_folder
#cd ../../
#file fs/version.txt

df -kh fs
rm -rvf fs/test.txt
umount fs
rm -rvf fs
rmmod yukifs
make remove clean > /dev/null 2>&1

cp test.img disk.img
rm -rvf test.img
#dmesg