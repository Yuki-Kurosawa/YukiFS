#! /bin/bash
gcc -c -o built-in.o -Wno-builtin-declaration-mismatch -nostdlib -nostartfiles -s built-in.c
ld -o built-in -T linker.ld --static -build-id=none built-in.o
./built-in
strip --strip-all -R .comment -R .eh_frame -R .tbss -R .note.gnu.property -s built-in
ldd built-in
file built-in
ls -alc built-in
