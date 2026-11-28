# SPDX-License-Identifier: GPL-3.0-or-later
CC ?= cc
CFLAGS ?= -O2
BASE = -std=c11 -Wall -Wextra -Werror -I Libraries/include
SRC = native/filesystems/gfs2/gfs2.c
HDR = native/filesystems/gfs2/reader.h Libraries/include/infiltratr/filesystem_session_v2.h
.PHONY: all check clean
all: build/GFS2.so build/freestanding/GFS2.so
build/GFS2.so: $(SRC) $(HDR) Makefile
	mkdir -p build
	$(CC) $(BASE) $(CFLAGS) -fPIC -fvisibility=hidden -shared -Wl,-z,defs -o $@ $(SRC)
build/freestanding/GFS2.so: $(SRC) $(HDR) Makefile
	mkdir -p build/freestanding
	$(CC) $(BASE) -O2 -march=x86-64 -DIFS_GFS2_FREESTANDING -ffreestanding -fno-builtin -fno-stack-protector -fno-tree-loop-distribute-patterns -fPIC -fvisibility=hidden -shared -nostdlib -Wl,-z,defs,-Bsymbolic -o $@ $(SRC)
build/test_gfs2: tests/test_gfs2.c Makefile
	mkdir -p build
	$(CC) $(BASE) $(CFLAGS) -o $@ $< -ldl -pthread
check: all build/test_gfs2
	test "$$(nm -D --defined-only build/GFS2.so | awk '{print $$3}')" = infiltrator_filesystem_api_v2
	test "$$(nm -D --defined-only build/freestanding/GFS2.so | awk '{print $$3}')" = infiltrator_filesystem_api_v2
	test -z "$$(nm -D --undefined-only build/freestanding/GFS2.so)"
	! readelf -d build/freestanding/GFS2.so | grep -q NEEDED
	build/test_gfs2 ./build/GFS2.so
	build/test_gfs2 ./build/freestanding/GFS2.so
clean:
	rm -rf build
