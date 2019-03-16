CC          = gcc
CFLAGS      = -Wall -std=gnu99 -O2 -ggdb3 -march=native -fPIC -Wno-format-zero-length -Wno-unused-parameter
LDFLAGS     =
CPPFLAGS    = -UNDEBUG -UG_DISABLE_ASSERT `getconf LFS_CFLAGS` `pkg-config --cflags glib-2.0` -D_GNU_SOURCE
LDLIBS      = `pkg-config --libs glib-2.0`
EXTRA       =

.PHONY: clean check

all: check halfempty

check:
	@echo -n "Checking for glib-2.0..."
	@pkg-config glib-2.0 || { echo "not found (install libglib2.0-dev or glib2-devel)"; false; }
	@echo ok

ifeq ($(shell uname),Darwin)
    EXTRA = sendfile_generic.o splice_generic.o
endif

# splice() does not appear to work reliably on Windows
ifeq ($(findstring Microsoft,$(shell uname -r)), Microsoft)
    EXTRA = splice_generic.o sendfile_generic.o
endif

halfempty: proc.o bisect.o util.o zero.o tree.o flags.o halfempty.o limits.o $(EXTRA)

util.o: monitor.h util.c

test: all
	make -C test

%.h: %.tpl
	hexdump -ve '"" 1/1 "%#02x" ","' $< > $@

clean:
	rm -f *.o *.dot *.out halfempty monitor.h
	make -C test clean
