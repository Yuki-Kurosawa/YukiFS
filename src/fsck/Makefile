.PHONY: all clean install remove

default: all

fsck.yukifs: fsck.c
	@gcc -Wall -Wextra -O2 -o fsck.yukifs fsck.c

all: fsck.yukifs

clean:
	@rm -f fsck.yukifs

install:
	@cp fsck.yukifs /usr/bin/fsck.yukifs

remove:
	@rm -f /usr/bin/fsck.yukifs
