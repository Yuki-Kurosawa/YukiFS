.PHONY: all clean tool

all: tool ko mkfs

ko: 
	$(MAKE) -C src/ko

tool: ko
	$(MAKE) -C tools

mkfs: tool ko
	$(MAKE) -C src/mkfs

clean:
	$(MAKE) -C src/ko clean
	$(MAKE) -C tools clean
	$(MAKE) -C src/mkfs clean