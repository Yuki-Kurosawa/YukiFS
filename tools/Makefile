.PHONY: all clean

all: built-in.h

built-in.h: built-in.c linker.ld
	./gen-built-in.sh

clean:
	rm *.o built-in.h built-in built-in-data.c built-in.ph linker.ld
