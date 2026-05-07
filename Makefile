CC = gcc
CFLAGS = -Wall -Wextra -g -O2 -Iinclude
LDFLAGS = -lfuse3
TARGET = plus3fuse
SRCS = src/main.c src/plus3dos.c
OBJS = $(SRCS:src/%.c=build/%.o)

all: build/$(TARGET)

build/$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build:
	mkdir -p build

clean:
	rm -rf build

#test: build/$(TARGET)
#	@echo "Test requires a CP/M disk image. Place in tests/ and run: ./build/plus3fuse tests/image.img /mnt/cpm"

.PHONY: all clean test
