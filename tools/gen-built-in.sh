#! /bin/bash

unset LANG

# read built-in-info.txt and convert to C string
echo -n "Converting built-in-info to C-Style bytes ... "
xxd -i -c 1024 < built-in-info.txt > built-in.ph
LENGTH=$(wc -c < built-in-info.txt)
sed -e 's/, 0x/\\\\x/g' -e 's/0x/"\\\\x/g' -e 's/ //g' -re 's/$/"/g' -re "s/$/,$LENGTH/g" built-in.ph > built-in.pph
rm built-in.ph
mv built-in.pph built-in.ph

# built C string into C file
sed -e "s@BUILT_IN_DATA@$(cat built-in.ph)@g" built-in.c > built-in-data.c
echo "OK"

echo -n "Compiling built-in first stage ... "
# get built-in.o
gcc -c -o built-in.o -Wno-builtin-declaration-mismatch -nostdlib -nostartfiles -s built-in-data.c
echo "OK"

echo -n "Generating C ELF linker script ... "
# get .text and .rodata size
TEXT_SIZE=$(size -Ax built-in.o | grep -E '\.text' | awk '{print $2}')
RODATA_SIZE=$(size -Ax built-in.o | grep -E '\.rodata' | awk '{print $2}')

# do some math
export RODATA_OFFSET=0x4000b0
TEXT_OFFSET=$((RODATA_OFFSET + RODATA_SIZE))
TEXT_OFFSET=$(printf "0x%x" $TEXT_OFFSET)

# sed into linker.ld
sed -e "s/RODATA_OFFSET/$RODATA_OFFSET/g" -e "s/TEXT_OFFSET/$TEXT_OFFSET/g" linker.ld.example>linker.ld
echo "OK"

echo -n "Compiling built-in second stage ... "
# link built-in.o
ld -o built-in -T linker.ld --static -build-id=none built-in.o
echo "OK"

echo -n "Trying to execute built-in ... "
./built-in > /dev/null

if ! [ $? -eq 0 ];then
    echo "FAILED"
    echo "ERROR: built-in ELF didn't run properly"
    exit 1
fi
echo "SUCCESS"

echo "============ built-in info ============"

# get size to BUILT_IN_SIZE
BUILT_IN_SIZE=$(stat -c%s built-in)
echo "built-in ELF size before strip: $BUILT_IN_SIZE bytes"

strip --strip-all -R .comment -R .eh_frame -R .tbss -R .note.gnu.property -s built-in

# get size to BUILT_IN_SIZE again
BUILT_IN_SIZE=$(stat -c%s built-in)
echo "built-in ELF size after strip: $BUILT_IN_SIZE bytes"

# get some SIZE from header ../include/file_table.h
FS_PADDING_SIZE=$(grep -E '^#define FS_PADDING_SIZE' ../include/file_table.h | awk '{print $3}')
echo "FS_PADDING_SIZE from header: $FS_PADDING_SIZE bytes"
MINIMAL_BLOCK_SIZE=$(grep -E '^#define MINIMAL_BLOCK_SIZE' ../include/file_table.h | awk '{print $3}')
echo "MINIMAL_BLOCK_SIZE from header: $MINIMAL_BLOCK_SIZE bytes"
MAXIMUM_BLOCK_SIZE=$(grep -E '^#define MAXIMUM_BLOCK_SIZE' ../include/file_table.h | awk '{print $3}')
echo "MAXIMUM_BLOCK_SIZE from header: $MAXIMUM_BLOCK_SIZE bytes"

echo "======================================="

if [ $BUILT_IN_SIZE -gt $FS_PADDING_SIZE ]; then
    echo "There is not enough space in FS_PADDING_HEADER to embed built-in"
    echo "Please increase FS_PADDING_SIZE in ../../include/file_table.h"
    exit 1
fi

if [ $MINIMAL_BLOCK_SIZE -lt $FS_PADDING_SIZE ];then
    echo "MINIMAL_BLOCK_SIZE must equal or larger then FS_PADDING_SIZE"
    echo "Please increase MINIMAL_BLOCK_SIZE in ../../include/file_table.h"
    exit 1
fi

if [ $MAXIMUM_BLOCK_SIZE -lt $MINIMAL_BLOCK_SIZE ];then
    echo "MAXIMUM_BLOCK_SIZE must equal or larger then MINIMAL_BLOCK_SIZE"
    echo "Please increase MAXIMUM_BLOCK_SIZE in ../../include/file_table.h"
    exit 1
fi

# add more checks for $FS_PADDING_SIZE , $MINIMAL_BLOCK_SIZE ,$MAXIMUM_BLOCK_SIZE are all based on 2^N and N >0
is_power_of_two() {
    local num=$1
    if [ $num -le 0 ]; then
        return 1
    fi
    while [ $num -gt 1 ]; do
        if [ $((num % 2)) -ne 0 ]; then
            return 1
        fi
        num=$((num / 2))
    done
    return 0
}

if ! is_power_of_two $FS_PADDING_SIZE; then
    echo "FS_PADDING_SIZE must be 2-power-based"
    echo "Please modify FS_PADDING_SIZE in ../../include/file_table.h"
    exit 1
fi

if ! is_power_of_two $MINIMAL_BLOCK_SIZE; then
    echo "MINIMAL_BLOCK_SIZE must be 2-power-based"
    echo "Please modify MINIMAL_BLOCK_SIZE in ../../include/file_table.h"
    exit 1
fi

if ! is_power_of_two $MAXIMUM_BLOCK_SIZE; then
    echo "MAXIMUM_BLOCK_SIZE must be 2-power-based"
    echo "Please modify MAXIMUM_BLOCK_SIZE in ../../include/file_table.h"
    exit 1
fi
