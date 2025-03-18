.PHONY: all clean

all: ko mkfs

ko:
	$(MAKE) -C src/ko

mkfs:
	$(MAKE) -C src/mkfs

clean:
	$(MAKE) -C src/ko clean
	$(MAKE) -C src/mkfs clean