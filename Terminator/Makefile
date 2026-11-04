# Terminator - a custom terminal emulator and shell (CS69201 term project)
#
#   make            build ./terminator
#   make test       run the automated tests (headless, no X server needed)
#   make clean      remove build products

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -pthread

# X11 compiler and linker flags (pkg-config when available, plain -lX11 otherwise)
X11_CFLAGS := $(shell pkg-config --cflags x11 2>/dev/null)
X11_LIBS   := $(shell pkg-config --libs x11 2>/dev/null || echo -lX11)

LDLIBS  += $(X11_LIBS) -pthread
ifeq ($(shell uname -s),Linux)
LDLIBS  += -lrt
endif

SRCS := $(wildcard src/*.c)
OBJS := $(SRCS:src/%.c=build/%.o)
HDRS := $(wildcard src/*.h)

terminator: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

build/%.o: src/%.c $(HDRS) | build
	$(CC) $(CFLAGS) $(X11_CFLAGS) -c -o $@ $<

build:
	mkdir -p build

test: terminator
	./tests/run_tests.sh

clean:
	rm -rf build terminator

.PHONY: test clean
