.PHONY: all clean
default: all

infofs.yukifs: infofs.c
	@gcc -o infofs.yukifs infofs.c

all: infofs.yukifs

clean:
	@rm -f infofs.yukifs

install:
	@cp infofs.yukifs /usr/bin/infofs.yukifs

remove:
	@rm -f /usr/bin/infofs.yukifs