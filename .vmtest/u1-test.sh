#!/bin/bash
# YukiFS regression across image sizes: 360K / 720K / 1440K / 4M (floppy ladder)
# Run after `make all`. Needs root (sudo).
set -x
YUKIFS=/home/yuki/YukiFS
SIZES=(360 720 1440 4096)
cd /tmp || exit 1
rm -rf yukifs-test && mkdir -p yukifs-test && cd yukifs-test || exit 1

sudo rmmod yukifs 2>/dev/null
sleep 1
sudo insmod "$YUKIFS/src/ko/yukifs.ko" || { echo INSMOD-FAIL; sudo dmesg | tail -30; exit 1; }
echo "--- filesystems containing yuki ---"
grep yuki /proc/filesystems || echo NO-YUKI-IN-FILESYSTEMS

avail() { df --output=avail -B1 fs | tail -1; }

for K in "${SIZES[@]}"; do
  echo "################ SIZE=${K}KiB ################"
  dd if=/dev/zero of=test.img bs=1KiB count=$K status=none
  echo "img bytes: $(stat -c %s test.img)"
  sudo "$YUKIFS/src/mkfs/mkfs.yukifs" -y test.img || { echo MKFS-FAIL-$K; exit 1; }
  mkdir -p fs
  sudo mount -t yuki -o loop test.img fs || { echo MOUNT-FAIL-$K; sudo dmesg | tail -20; sudo rmmod yukifs 2>/dev/null; exit 1; }
  echo MOUNT-OK-$K

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
  # truncate on a NEW file: create takes 1 block, grow to 2000 takes 1 more (-2),
  # shrink to 5 frees the tail block but keeps 1 (-1 vs baseline)
  [ "$tg" -eq $((tb - 2048)) ] && [ "$ts" -eq $((tb - 1024)) ] && echo TC11-OK-freed || echo TC11-FAIL

  echo "--- TC12: mode/uid/gid/mtime persist across remount ---"
  echo hi > fs/meta.txt
  chmod 640 fs/meta.txt
  chown 12:34 fs/meta.txt
  touch -d '2026-09-09 12:00:00' fs/meta.txt
  before=$(stat -c '%a %u %g %Y' fs/meta.txt)
  echo "before-remount: $before"
  sudo umount fs
  sudo mount -t yuki -o loop test.img fs
  after=$(stat -c '%a %u %g %Y' fs/meta.txt)
  echo "after-remount:  $after"
  [ "$before" = "$after" ] && echo TC12-OK || echo TC12-FAIL

  echo "--- TC13: fill to near capacity, then ENOSPC on overflow ---"
  cap=$(avail)
  fillb=$(( cap / 1024 - 4 ))   # KiB to fill, keep ~4KiB free
  dd if=/dev/zero of=fs/fill.bin bs=${fillb}KiB count=1 status=none 2>/dev/null && echo "fill ${fillb}KiB ok"
  echo -n "avail after fill: "; avail
  freeb=$(avail)
  if head -c $(( (freeb / 1024 + 2) * 1024 )) /dev/zero >> fs/fill.bin 2>/dev/null; then
    echo TC13-FAIL-no-enospc
  else
    echo TC13-OK-enospc
  fi
  echo -n "final size: "; wc -c fs/fill.bin

  echo "--- TC14: rename (mv) ---"
  echo hello > fs/1.txt
  mv fs/1.txt fs/2.txt && echo TC14A-OK-mv || echo TC14A-FAIL
  [ "$(cat fs/2.txt)" = "hello" ] && echo TC14B-OK-content || echo TC14B-FAIL
  ls fs | grep -q '^1.txt$' && echo TC14C-FAIL-old-gone || echo TC14C-OK-old-gone
  # mv over an existing target replaces it (POSIX)
  echo new > fs/3.txt
  mv fs/2.txt fs/3.txt && echo TC14D-OK-replace || echo TC14D-FAIL
  [ "$(cat fs/3.txt)" = "hello" ] && echo TC14E-OK-replaced-content || echo TC14E-FAIL
  ls fs | grep -q '^2.txt$' && echo TC14F-FAIL || echo TC14F-OK
  # 12-char target rejected (via syscall so we test the kernel, not coreutils mv)
  python3 -c "import os
try:
    os.rename('fs/3.txt','fs/abcdefghijkl'); print('TC14G-FAIL-name12')
except OSError as e:
    print('TC14G-OK-name12-rejected' if e.errno==36 else 'TC14G-WRONG-ERRNO-%d'%e.errno)"
  # mv to same name: no-op (rename(2) on identical path)
  python3 -c "import os
os.rename('fs/3.txt','fs/3.txt'); print('TC14H-OK-same')"
  [ "$(cat fs/3.txt)" = "hello" ] && echo TC14I-OK-still-there || echo TC14I-FAIL
  # rename persists across remount
  sudo umount fs
  sudo mount -t yuki -o loop test.img fs
  [ "$(cat fs/3.txt)" = "hello" ] && echo TC14J-OK-persist || echo TC14J-FAIL
  ls fs | grep -q '^2.txt$' && echo TC14K-FAIL || echo TC14K-OK

  echo "--- TC15: fresh image, append past a neighbor (relocation) ---"
  sudo umount fs
  rm -f test.img
  dd if=/dev/zero of=test.img bs=1KiB count=$K status=none
  sudo "$YUKIFS/src/mkfs/mkfs.yukifs" -y test.img >/dev/null 2>&1 || { echo TC15-MKFS-FAIL; exit 1; }
  sudo mount -t yuki -o loop test.img fs || { echo TC15-MOUNT-FAIL; exit 1; }
  sudo dmesg -c > /dev/null 2>&1   # isolate this size's relocation trace
  python3 << 'PYEOF'
import os, hashlib
def md5b(p): return hashlib.md5(open(p,'rb').read()).digest()
def append(p, n):
    try:
        with open(p,'ab') as f: f.write(b'x'*n)
        return 'OK'
    except OSError as e: return 'ENOSPC' if e.errno==28 else 'ERR%d'%e.errno

def verify(tag, pa, exp_a, pb, exp_b):
    ok = (md5b(pa)==hashlib.md5(exp_a).digest() and os.path.getsize(pa)==len(exp_a)
          and md5b(pb)==hashlib.md5(exp_b).digest())
    print('TC15-'+tag+': '+('OK' if ok else 'FAIL'))

# S1: (A multi, B multi, free blocks after) -> append succeeds via relocation
open('fs/A1.bin','wb').write(b'a'*2000)
open('fs/B1.bin','wb').write(b'b'*2000)
r = append('fs/A1.bin', 100)
print('TC15-S1-A-multi-B-multi-free: append='+r)
verify('S1', 'fs/A1.bin', b'a'*2000+b'x'*100, 'fs/B1.bin', b'b'*2000)

# S2: (A single, 1 free block, B multi, enough free after) -> succeeds
#      layout: A blk1, temp at blk2 removed -> hole, B takes blk3-4
open('fs/A2.bin','wb').write(b'a'*1000)
open('fs/tmp2.bin','wb').write(b't')
os.remove('fs/tmp2.bin')
open('fs/B2.bin','wb').write(b'b'*2000)
r = append('fs/A2.bin', 2000)          # need 3 blocks: blk2 free, blk3 taken -> relocate
print('TC15-S2-A-single-hole-B-multi: append='+r)
verify('S2', 'fs/A2.bin', b'a'*1000+b'x'*2000, 'fs/B2.bin', b'b'*2000)

# S3: (A multi, B single, free blocks after) -> succeeds
open('fs/A3.bin','wb').write(b'a'*2000)
open('fs/B3.bin','wb').write(b'b'*1000)
r = append('fs/A3.bin', 100)
print('TC15-S3-A-multi-B-single-free: append='+r)
verify('S3', 'fs/A3.bin', b'a'*2000+b'x'*100, 'fs/B3.bin', b'b'*1000)
PYEOF
  # check the relocation trace BEFORE the fillx flood can evict it from the
  # dmesg ring buffer. expected exactly 4: A1 (S1 append blocked by B1),
  # B2 (S2 initial write grows past B1), A2 (S2 append), A3 (S3 append)
  echo -n "relocations in dmesg (S1-S3): "; sudo dmesg | grep -c 'YukiFS: relocated'
  [ "$(sudo dmesg | grep -c 'YukiFS: relocated')" = "4" ] && echo TC15-relocations-OK || echo TC15-relocations-UNEXPECTED
  python3 << 'PYEOF'
import os, hashlib
def md5b(p): return hashlib.md5(open(p,'rb').read()).digest()
def append(p, n):
    try:
        with open(p,'ab') as f: f.write(b'x'*n)
        return 'OK'
    except OSError as e: return 'ENOSPC' if e.errno==28 else 'ERR%d'%e.errno

# ENOSPC variant: A + B, then fill ALL remaining free blocks -> no run left
open('fs/Ax.bin','wb').write(b'a'*2000)
open('fs/Bx.bin','wb').write(b'b'*2000)
f = open('fs/fillx.bin','wb')
try:
    while True:
        f.write(b'f'*1024)      # 1KiB chunks: exhaust the disk block by block
except OSError as e:
    f.close()
    print('TC15-fillx stopped errno=%d' % e.errno)
r = append('fs/Ax.bin', 100)
ok = (r=='ENOSPC' and md5b('fs/Ax.bin')==hashlib.md5(b'a'*2000).digest()
      and os.path.getsize('fs/Ax.bin')==2000 and md5b('fs/Bx.bin')==hashlib.md5(b'b'*2000).digest())
print('TC15-ENOSPC-full-disk: append='+r+' -> '+('OK' if ok else 'FAIL'))
PYEOF
  # everything must survive a remount byte-for-byte
  sudo umount fs
  sudo mount -t yuki -o loop test.img fs
  python3 -c "
import hashlib
exp = {'fs/A1.bin': b'a'*2000+b'x'*100, 'fs/B1.bin': b'b'*2000,
       'fs/A2.bin': b'a'*1000+b'x'*2000, 'fs/B2.bin': b'b'*2000,
       'fs/A3.bin': b'a'*2000+b'x'*100, 'fs/B3.bin': b'b'*1000,
       'fs/Ax.bin': b'a'*2000, 'fs/Bx.bin': b'b'*2000}
ok = all(hashlib.md5(open(p,'rb').read()).digest()==hashlib.md5(c).digest() for p,c in exp.items())
print('TC15-remount-persist:', 'OK' if ok else 'FAIL')
"

  sudo umount fs || { echo UMOUNT-FAIL-$K; exit 1; }
  rm -rf fs
done

echo '################ TC16: symlink + hardlink ################'
cd /tmp || exit 1
rm -rf yukifs-test && mkdir yukifs-test && cd yukifs-test || exit 1
dd if=/dev/zero of=test.img bs=1KiB count=360 status=none
sudo "$YUKIFS/src/mkfs/mkfs.yukifs" -y test.img >/dev/null 2>&1 || { echo TC16-MKFS-FAIL; exit 1; }
mkdir fs
sudo mount -t yuki -o loop test.img fs || { echo TC16-MOUNT-FAIL; exit 1; }

echo '--- TC16A: symlink ---'
echo payload > fs/target.txt
ln -s target.txt fs/link.txt && echo TC16A-OK-symlink
[ "$(cat fs/link.txt)" = "payload" ] && echo TC16B-OK-follow
ls -l fs/link.txt | grep -q '^l' && echo TC16C-OK-mode-l
[ "$(readlink fs/link.txt)" = "target.txt" ] && echo TC16D-OK-readlink
ln -s missing.txt fs/dangle
[ ! -e fs/dangle ] && echo TC16E-OK-dangling

echo '--- TC16F: hard link ---'
ln fs/target.txt fs/hard.txt && echo TC16F-OK-hardlink
[ "$(stat -c %h fs/target.txt)" = "2" ] && echo TC16G-OK-nlink2
[ "$(stat -c %i fs/target.txt)" = "$(stat -c %i fs/hard.txt)" ] && echo TC16H-OK-same-inode
echo more >> fs/hard.txt
[ "$(cat fs/target.txt)" = "$(printf 'payload\nmore')" ] && echo TC16I-OK-shared-data
ln fs/target.txt fs/keep.txt

echo '--- TC16J: remount persistence ---'
sudo umount fs
sudo mount -t yuki -o loop test.img fs
[ "$(readlink fs/link.txt)" = "target.txt" ] && echo TC16J-OK-symlink-persist
[ "$(cat fs/link.txt)" = "payload" ] && echo TC16K-OK-follow-persist
[ "$(cat fs/keep.txt)" = "$(printf 'payload\nmore')" ] && echo TC16L-OK-hardlink-persist
[ "$(stat -c %h fs/keep.txt)" = "2" ] && echo TC16M-OK-nlink-persist
[ "$(stat -c %i fs/target.txt)" = "$(stat -c %i fs/keep.txt)" ] && echo TC16N-OK-ino-persist

echo '--- TC16O: unlink one link ---'
rm fs/hard.txt
[ "$(stat -c %h fs/target.txt)" = "2" ] && echo TC16O-OK-nlink-back
[ "$(cat fs/target.txt)" = "$(printf 'payload\nmore')" ] && echo TC16P-OK-data-kept

echo '--- TC16Q: unlink real name while link remains ---'
rm fs/target.txt
ls fs | grep -q '^target.txt$' && echo TC16Q-FAIL-target-still-listed || echo TC16Q-OK-target-gone
[ "$(cat fs/keep.txt)" = "$(printf 'payload\nmore')" ] && echo TC16R-OK-record-live
[ "$(stat -c %h fs/keep.txt)" = "1" ] && echo TC16S-OK-record-nlink1

echo '--- TC16T: record survives second remount ---'
sudo umount fs
sudo mount -t yuki -o loop test.img fs
[ "$(cat fs/keep.txt)" = "$(printf 'payload\nmore')" ] && echo TC16T-OK-record-persist
[ "$(stat -c %h fs/keep.txt)" = "1" ] && echo TC16U-OK-record-nlink-persist

echo '--- TC16V: hard link of a symlink + rename a record ---'
ln -s target.txt fs/slink    # dangling now (target.txt is gone) but linkable
ln fs/slink fs/slink2 && echo TC16V-OK-link-symlink
[ "$(readlink fs/slink2)" = "target.txt" ] && echo TC16W-OK-symlink-target
mv fs/slink2 fs/slink3 && echo TC16X-OK-rename-record
[ "$(readlink fs/slink3)" = "target.txt" ] && echo TC16Y-OK-record-renamed
[ ! -e fs/slink2 ] && echo TC16Z-OK-old-name-gone

sudo umount fs
rm -rf fs

sudo rmmod yukifs

echo '################ TC17: subdirectories ################'
cd /tmp || exit 1
rm -rf yukifs-test && mkdir yukifs-test && cd yukifs-test || exit 1
dd if=/dev/zero of=test.img bs=1KiB count=360 status=none
sudo insmod "$YUKIFS/src/ko/yukifs.ko" || { echo TC17-INSMOD-FAIL; exit 1; }
sudo "$YUKIFS/src/mkfs/mkfs.yukifs" -y test.img >/dev/null 2>&1 || { echo TC17-MKFS-FAIL; exit 1; }
mkdir fs
sudo mount -t yuki -o loop test.img fs || { echo TC17-MOUNT-FAIL; exit 1; }

echo '--- TC17A: mkdir ---'
mkdir fs/sub && echo TC17A-OK-mkdir
ls -ld fs/sub | grep -q '^d' && echo TC17B-OK-mode-d

echo '--- TC17C: read/write inside subdir ---'
echo hello > fs/sub/a.txt
[ "$(cat fs/sub/a.txt)" = "hello" ] && echo TC17C-OK-read-write

echo '--- TC17D: root/sub isolation ---'
ls fs | grep -q '^sub$' && echo TC17D-OK-root-list
ls fs/sub | grep -q '^a.txt$' && echo TC17D-OK-sub-list
[ -e fs/a.txt ] && echo TC17D-FAIL-leak || echo TC17D-OK-no-leak

echo '--- TC17E: nested mkdir/path ---'
mkdir -p fs/sub/deep/dir
echo deep > fs/sub/deep/dir/f.txt
[ "$(cat fs/sub/deep/dir/f.txt)" = "deep" ] && echo TC17E-OK-nested-path

echo '--- TC17F: ls -a shows . and .. ---'
ls -a fs/sub | grep -qx '\.' && ls -a fs/sub | grep -qx '\.\.' && echo TC17F-OK-dotdot

echo '--- TC17G: dir nlink = 2 + subdir count ---'
[ "$(stat -c %h fs/sub)" = "3" ] && echo TC17G-OK-nlink3
[ "$(stat -c %h fs)" = "3" ] && echo TC17G-OK-root-nlink3

echo '--- TC17H: rmdir non-empty fails ---'
rmdir fs/sub && echo TC17H-FAIL || echo TC17H-OK-enotempty

echo '--- TC17I: rmdir empty chain ---'
rm fs/sub/deep/dir/f.txt
rmdir fs/sub/deep/dir && echo TC17I-OK-rmdir
rmdir fs/sub/deep && echo TC17I-OK-rmdir2
[ ! -e fs/sub/deep ] && echo TC17I-OK-gone

echo '--- TC17J: cross-dir rename file ---'
mkdir fs/sub2
echo x > fs/sub2/x.txt
mv fs/sub/a.txt fs/sub2/a.txt
[ "$(cat fs/sub2/a.txt)" = "hello" ] && echo TC17J-OK-content
[ ! -e fs/sub/a.txt ] && echo TC17J-OK-old-gone

echo '--- TC17K: cross-dir rename dir ---'
mkdir fs/dirA
mv fs/dirA fs/sub2/dirB && echo TC17K-OK-mvdir
[ -d fs/sub2/dirB ] && echo TC17K-OK-exists

echo '--- TC17L: mv dir into itself rejected ---'
python3 -c "
import os
try:
    os.rename('fs/sub2', 'fs/sub2/self')
    print('TC17L-FAIL')
except OSError as e:
    print('TC17L-OK-einval errno=%d' % e.errno)
"
[ -d fs/sub2/dirB ] && echo TC17L-OK-intact

echo '--- TC17M: rename replaces empty dir ---'
mkdir fs/repA; mkdir fs/repB
mv fs/repA fs/repB && echo TC17M-OK-replace
[ -d fs/repB ] && echo TC17M-OK-dir
[ ! -e fs/repA ] && echo TC17M-OK-old-gone

echo '--- TC17N: symlink + hardlink in subdir ---'
echo t > fs/sub2/t.txt
ln -s t.txt fs/sub2/l.txt
[ "$(cat fs/sub2/l.txt)" = "t" ] && echo TC17N-OK-symlink
ln fs/sub2/t.txt fs/sub2/h.txt
[ "$(stat -c %h fs/sub2/t.txt)" = "2" ] && echo TC17N-OK-hardlink

echo '--- TC17O: remount persistence ---'
echo P > fs/sub2/p.txt
sudo umount fs
sudo mount -t yuki -o loop test.img fs
[ -d fs/sub2/dirB ] && echo TC17O-OK-dir-persist
[ "$(cat fs/sub2/a.txt)" = "hello" ] && echo TC17O-OK-file-persist
[ "$(cat fs/sub2/p.txt)" = "P" ] && echo TC17O-OK-new-persist
[ "$(cat fs/sub2/l.txt)" = "t" ] && echo TC17O-OK-symlink-persist
[ "$(stat -c %h fs/sub2/t.txt)" = "2" ] && echo TC17O-OK-nlink-persist

echo '--- TC17P: unlink in subdir frees blocks ---'
avail() { df --output=avail -B1 fs | tail -1; }
echo 12345678 > fs/sub2/free.txt
a0=$(avail)
rm fs/sub2/free.txt
a1=$(avail)
[ "$a1" -gt "$a0" ] && echo TC17P-OK-freed

echo '--- TC17Q: rmdir root fails ---'
rmdir fs 2>/dev/null && echo TC17Q-FAIL || echo TC17Q-OK-busy

echo '--- TC17R: rm -r empty dir ---'
rm -r fs/sub2/dirB && [ ! -e fs/sub2/dirB ] && echo TC17R-OK

sudo umount fs
rm -rf fs
sudo rmmod yukifs
echo "--- dmesg (yuki) ---"
sudo dmesg | grep -i yuki | tail -80
echo ALL-SIZES-DONE
