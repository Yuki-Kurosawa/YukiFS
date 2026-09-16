.PHONY: all clean tool fsck

all: tool ko mkfs infofs fsck

ko: 
	$(MAKE) -C src/ko

tool: ko
	$(MAKE) -C tools

mkfs: tool ko
	$(MAKE) -C src/mkfs

infofs:
	$(MAKE) -C src/infofs

fsck:
	$(MAKE) -C src/fsck

clean:
	$(MAKE) -C src/ko clean
	$(MAKE) -C tools clean
	$(MAKE) -C src/mkfs clean
	$(MAKE) -C src/infofs clean
	$(MAKE) -C src/fsck clean

install:
	$(MAKE)	-C src/mkfs install
	$(MAKE)	-C src/infofs install
	$(MAKE)	-C src/fsck install

remove:
	$(MAKE)	-C src/mkfs remove
	$(MAKE)	-C src/infofs remove
	$(MAKE)	-C src/fsck remove