#! /bin/bash
unset LANG
unset LANGUAGE

clear
make all install > /dev/null 2>&1
modinfo src/ko/yukifs.ko > /dev/null 2>&1
insmod src/ko/yukifs.ko > /dev/null 2>&1
mkdir fs > /dev/null 2>&1
cat /proc/filesystems | grep yuki > /dev/null 2>&1

dd if=/dev/zero of=test.img bs=1KiB count=45 > /dev/null 2>&1
mkfs.yukifs -y test.img > /dev/null 2>&1

infofs.yukifs test.img > /dev/null 2>&1
infofs.yukifs -s test.img > /dev/null 2>&1

#mount -t yuki -o loop test.img fs
mount test.img fs > /dev/null 2>&1
mount | grep yuki > /dev/null 2>&1


file fs  > /dev/null 2>&1
ls -alci . | grep fs > /dev/null 2>&1
ls -alci fs > /dev/null 2>&1
touch fs/test.txt
infofs.yukifs -s test.img
echo 123 >> fs/test.txt
echo 456 >> fs/test.txt
echo "O TESTED"
cat fs/test.txt
echo "I TESTED"
ls -alci fs > /dev/null 2>&1


df -kh fs > /dev/null 2>&1
rm -rvf fs/test.txt > /dev/null 2>&1
umount fs > /dev/null 2>&1
rm -rvf fs > /dev/null 2>&1
rmmod yukifs > /dev/null 2>&1
make remove clean > /dev/null 2>&1

cp test.img disk.img > /dev/null 2>&1
rm -rvf test.img > /dev/null 2>&1
#dmesg