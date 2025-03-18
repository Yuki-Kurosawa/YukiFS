.PHONY: all clean
default: all

mkfs.yukifs: mkfs.c
	@gcc -o mkfs.yukifs mkfs.c

all: mkfs.yukifs

clean:
	@rm -f mkfs.yukifs
