.PHONY: all clean tool

all: tool ko mkfs

tool:
	$(MAKE) -C tools

ko:
	$(MAKE) -C src/ko

mkfs: tool
	$(MAKE) -C src/mkfs

clean:
	$(MAKE) -C src/ko clean
	$(MAKE) -C tools clean
	$(MAKE) -C src/mkfs clean