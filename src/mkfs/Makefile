.PHONY: all clean
default: all

mkfs.yukifs: mkfs.c
	@gcc -o mkfs.yukifs mkfs.c

all: mkfs.yukifs

clean:
	@rm -f mkfs.yukifs

install:
	@cp mkfs.yukifs /usr/bin/mkfs.yukifs

remove:
	@rm -f /usr/bin/mkfs.yukifs