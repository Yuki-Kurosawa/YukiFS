#! /bin/bash

unset LANG

# read built-in-info.txt and convert to C string
xxd -i -c 1024 < built-in-info.txt > built-in.ph
LENGTH=$(wc -c < built-in-info.txt)
sed -e 's/, 0x/\\\\x/g' -e 's/0x/"\\\\x/g' -e 's/ //g' -re 's/$/"/g' -re "s/$/,$LENGTH/g" built-in.ph > built-in.pph
rm built-in.ph
mv built-in.pph built-in.ph

# built C string into C file
sed -e "s@BUILT_IN_DATA@$(cat built-in.ph)@g" built-in.c > built-in-data.c

# get built-in.o
gcc -c -o built-in.o -Wno-builtin-declaration-mismatch -nostdlib -nostartfiles -s built-in-data.c

# size built-in.o
size -A built-in.o

ld -o built-in -T linker.ld --static -build-id=none built-in.o
./built-in
strip --strip-all -R .comment -R .eh_frame -R .tbss -R .note.gnu.property -s built-in
ldd built-in
file built-in
ls -alc built-in
