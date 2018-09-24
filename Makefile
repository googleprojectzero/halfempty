CC          = gcc
CFLAGS      = -Wall -std=gnu99 -O2 -fPIC -Wno-format-zero-length -Wno-unused-parameter
LDFLAGS     = -pie
CPPFLAGS    = -UNDEBUG -UG_DISABLE_ASSERT `getconf LFS_CFLAGS` `pkg-config --cflags glib-2.0` -D_GNU_SOURCE
LDLIBS      = `pkg-config --libs glib-2.0`

.PHONY: clean check

all: check halfempty

check:
	@echo -n "Checking for glib-2.0..."
	@pkg-config glib-2.0 || { echo "not found (install libglib2.0-dev or glib2-devel)"; false; }
	@echo ok

halfempty: proc.o bisect.o util.o zero.o tree.o flags.o halfempty.o limits.o

util.o: monitor.h util.c

test: all
	make -C test

%.h: %.tpl
	hexdump -ve '"" 1/1 "%#02x" ","' $< > $@

clean:
	rm -f *.o *.dot *.out halfempty monitor.h
	make -C test clean
