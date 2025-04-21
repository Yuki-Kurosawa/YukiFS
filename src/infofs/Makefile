.PHONY: all clean
default: all

infofs.yukifs: infofs.c
	@gcc -o infofs.yukifs infofs.c

viewfs.yukifs: viewfs.c
	@gcc -o viewfs.yukifs viewfs.c

all: infofs.yukifs viewfs.yukifs

clean:
	@rm -f infofs.yukifs
	@rm -f viewfs.yukifs

install:
	@cp infofs.yukifs /usr/bin/infofs.yukifs
	@cp viewfs.yukifs /usr/bin/viewfs.yukifs

remove:
	@rm -f /usr/bin/infofs.yukifs
	@rm -f /usr/bin/viewfs.yukifs