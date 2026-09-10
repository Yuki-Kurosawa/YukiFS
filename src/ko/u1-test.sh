#!/bin/bash
# YukiFS regression test on u1 (real 6.12 kernel). Run after `make all`.
# Covers: direct-create bug, append, overwrite-preserve, multi-block IO,
#         long-name rejection, unlink + real statfs counters, truncate.
set -x
YUKIFS=/home/yuki/YukiFS
cd /tmp || exit 1
rm -rf yukifs-test && mkdir -p yukifs-test && cd yukifs-test || exit 1

sudo rmmod yukifs 2>/dev/null
sleep 1
sudo insmod "$YUKIFS/src/ko/yukifs.ko" || { echo INSMOD-FAIL; sudo dmesg | tail -30; exit 1; }
echo "--- filesystems containing yuki ---"
grep yuki /proc/filesystems || echo NO-YUKI-IN-FILESYSTEMS

dd if=/dev/zero of=test.img bs=1KiB count=128 status=none
echo "img bytes: $(stat -c %s test.img)"
sudo "$YUKIFS/src/mkfs/mkfs.yukifs" -y test.img
echo "mkfs rc=$?"
mkdir -p fs

sudo mount -t yuki -o loop test.img fs || { echo MOUNT-FAIL; sudo dmesg | tail -40; sudo rmmod yukifs 2>/dev/null; exit 1; }
echo MOUNT-OK

avail() { df --output=avail -B1 fs | tail -1; }
echo "--- baseline df avail (root dir only) ---"; avail

echo "--- TC1: direct '>' to NEW file (the reported bug) ---"
echo 123 > fs/test.txt && echo TC1-OK
echo -n "cat: "; cat fs/test.txt
echo "--- TC2: append '>>' ---"
echo 456 >> fs/test.txt
echo -n "cat: "; cat fs/test.txt
echo "--- TC3: overwrite '>' ---"
echo abc > fs/test.txt
echo -n "cat: "; cat fs/test.txt
echo "--- TC4: touch then write/append ---"
touch fs/foo
echo hi > fs/foo
echo 77 >> fs/foo
echo -n "cat: "; cat fs/foo
echo "--- TC5: ~100B write ---"
head -c 100 /dev/zero | tr '\0' x > fs/big.txt
echo -n "size: "; wc -c fs/big.txt
echo "--- TC6: ls / df ---"
ls -alci fs
df -h fs

echo "--- TC7: multi-block write/read (3000B, previously single-block) ---"
head -c 3000 /dev/zero | tr '\0' x > fs/multi.txt
echo -n "size: "; wc -c fs/multi.txt
echo -n "md5-ok: "
if [ "$(dd if=fs/multi.txt bs=1 count=3000 2>/dev/null | md5sum | cut -d' ' -f1)" = "$(head -c 3000 /dev/zero | tr '\0' x | md5sum | cut -d' ' -f1)" ]; then echo YES; else echo NO; fi

echo "--- TC8: partial overwrite must preserve surrounding bytes ---"
echo -n abcdefgh > fs/ow.txt
printf XY | dd of=fs/ow.txt bs=1 seek=1 conv=notrunc 2>/dev/null
echo -n "content: "; cat fs/ow.txt; echo
[ "$(cat fs/ow.txt)" = "aXYdefgh" ] && echo TC8-OK || echo TC8-FAIL

echo "--- TC9: 12-char name rejected (ENAMETOOLONG), 11-char ok ---"
echo 1 > fs/abcdefghijkl 2>/dev/null && echo TC9A-FAIL-name12-accepted || echo TC9A-OK-name12-rejected
echo 1 > fs/abcdefghijk 2>/dev/null && echo TC9B-OK-name11-works || echo TC9B-FAIL-name11

echo "--- TC10: unlink frees blocks (real statfs) ---"
a0=$(avail)
echo 12345678 > fs/del.txt
a1=$(avail)
rm fs/del.txt
a2=$(avail)
echo "avail: before=$a0 with-file=$a1 after-rm=$a2"
[ "$a1" -lt "$a0" ] && [ "$a2" = "$a0" ] && echo TC10-OK || echo TC10-FAIL
ls fs | grep -q del.txt && echo TC10-FAIL-still-listed || echo TC10-OK-unlinked

echo "--- TC11: truncate shrink/grow ---"
tb=$(avail)
truncate -s 2000 fs/t.txt && echo -n "trunc-2000 size: "; wc -c fs/t.txt
tg=$(avail)
truncate -s 5 fs/t.txt && echo -n "trunc-5 size: "; wc -c fs/t.txt
ts=$(avail)
echo "avail: before=$tb grow-2000=$tg shrink-5=$ts"
[ "$tg" -eq $((tb - 1024)) ] && [ "$ts" = "$tb" ] && echo TC11-OK-freed || echo TC11-FAIL

sudo umount fs
sudo rmmod yukifs
echo "--- dmesg (yuki) ---"
sudo dmesg | grep -i yuki | tail -60
echo TEST-DONE
