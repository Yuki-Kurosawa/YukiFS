.PHONY: all clean tool

all: tool ko mkfs infofs

ko: 
	$(MAKE) -C src/ko

tool: ko
	$(MAKE) -C tools

mkfs: tool ko
	$(MAKE) -C src/mkfs

infofs:
	$(MAKE) -C src/infofs

clean:
	$(MAKE) -C src/ko clean
	$(MAKE) -C tools clean
	$(MAKE) -C src/mkfs clean
	$(MAKE) -C src/infofs clean

install:
	$(MAKE)	-C src/mkfs install
	$(MAKE)	-C src/infofs install

remove:
	$(MAKE)	-C src/mkfs remove
	$(MAKE)	-C src/infofs remove