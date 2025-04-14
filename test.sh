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

mount -t yuki -o loop test.img fs > /dev/null 2>&1
#mount test.img fs > /dev/null 2>&1
mount | grep yuki > /dev/null 2>&1

file fs  > /dev/null 2>&1
ls -alci . | grep fs
ls -alci fs 
umount fs 

echo "----- Test Case 1 Begin -----"
echo "Operation: echo 123 > test.txt (test.txt does not exist)"
mount -t yuki -o loop test.img fs 
echo 123 > fs/test.txt
echo "Expected: 31 32 33 0A 00 00 00 00"
echo -n "Actual: "
viewfs.yukifs --if=test.img --block-num=1 --count=8 --format=hex
umount fs > /dev/null 2>&1
echo "----- Test Case 1 End -----"

# echo "----- Test Case 2 Begin -----"
# echo "Operation: echo 456 >> test.txt (test.txt exists)"
# mount test.img fs > /dev/null 2>&1
# echo 456 >> fs/test.txt
# echo "Expected: 31 32 33 0A 34 35 36 0A 00 00 00 00"
# echo -n "Actual: "
# viewfs.yukifs --if=test.img --block-num=1 --count=12 --format=hex
# umount fs > /dev/null 2>&1
# echo "----- Test Case 2 End -----"

# echo "----- Test Case 3 Begin -----"
# echo "Operation: echo 123 > test.txt (test.txt exists)"
# mount test.img fs > /dev/null 2>&1
# echo 123 > fs/test.txt
# echo "Expected: 31 32 33 0A 00 00 00 00 00 00 00 00"
# echo -n "Actual: "
# viewfs.yukifs --if=test.img --block-num=1 --count=12 --format=hex
# umount fs > /dev/null 2>&1
# echo "----- Test Case 3 End -----"

# echo "----- Test Case 4 Begin -----"
# echo "Operation: echo 456 >> test.txt (test.txt does not exists)"
# mount test.img fs > /dev/null 2>&1
# echo 456 >> fs/test.txt
# echo "Expected: 34 35 36 0A 00 00 00 00"
# echo -n "Actual: "
# viewfs.yukifs --if=test.img --block-num=1 --count=8 --format=hex
# umount fs > /dev/null 2>&1
# echo "----- Test Case 4 End -----"

ls -alci fs > /dev/null 2>&1

df -kh fs > /dev/null 2>&1
rm -rvf fs > /dev/null 2>&1
rmmod yukifs > /dev/null 2>&1
make remove clean > /dev/null 2>&1

cp test.img disk.img > /dev/null 2>&1
rm -rvf test.img > /dev/null 2>&1
#dmesg