# Groovebox for the Shanling Q2. `make` builds the device binary into dist/.groovebox/ (needs the Q2
# sysroot, see README), `make host` an SDL2 window, `make check` the asserts.
SYSROOT ?= $(HOME)/.cache/q2/sysroot
CC_Q2 = clang --target=mipsel-linux-gnu --sysroot=$(SYSROOT) -march=mips32r2 -mabi=32 -mfp64 -mnan=2008 -mabs=2008
CFLAGS = -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -fno-strict-aliasing
LDFLAGS_Q2 = -m elf32ltsmip -e __start --dynamic-linker /lib/ld-linux-mipsn8.so.1 --image-base=0x400000 -z noexecstack \
             --hash-style=sysv --build-id=none -s
SRC = src/main.c src/ui.c src/engine.c src/seq.c src/wav.c src/project.c
Q2_SRC = $(SRC) src/platform_q2.c src/audio.c src/crt-q2.c
HOST_SRC = $(SRC) src/platform_sdl.c
DIST = dist/.groovebox

all: $(DIST)/q2-groovebox $(DIST)/q2app $(DIST)/kits/default dist/groovebox.zip

build/q2/%.o: src/%.c src/*.h
	@mkdir -p build/q2
	$(CC_Q2) $(CFLAGS) -c $< -o $@
$(DIST)/q2-groovebox: $(Q2_SRC:src/%.c=build/q2/%.o)
	@mkdir -p $(DIST)
	ld.lld $(LDFLAGS_Q2) -o $@ $^ $(SYSROOT)/lib/libc.so.6 $(SYSROOT)/lib/libm.so.6 $(SYSROOT)/lib/libpthread.so.0 \
	    $(SYSROOT)/lib/libasound.so.2 $(SYSROOT)/lib/libgcc_s.so.1 $(SYSROOT)/lib/ld-linux-mipsn8.so.1
$(DIST)/q2app: q2app
	@mkdir -p $(DIST)
	cp $< $@
$(DIST)/kits/default: kits/default/*.wav
	@mkdir -p $@
	cp kits/default/*.wav $@/
	@touch $@
dist/groovebox.zip: $(DIST)/q2-groovebox $(DIST)/q2app $(DIST)/kits/default
	cd dist && rm -f groovebox.zip && zip -qr groovebox.zip .groovebox
	@ls -l $@

host: build/host/q2-groovebox
	@ln -sfn ../../kits build/host/kits
build/host/q2-groovebox: $(HOST_SRC) src/*.h
	@mkdir -p build/host
	cc $(CFLAGS) -DHOST $(shell pkg-config --cflags sdl2) -o $@ $(HOST_SRC) $(shell pkg-config --libs sdl2) -lm -lpthread

check: build/host/check
	./build/host/check
build/host/check: src/check.c src/*.c src/*.h
	@mkdir -p build/host
	cc -std=gnu11 -O1 -g -Wall -Wno-unused-function -Wno-unused-parameter $(shell pkg-config --cflags sdl2) -fsanitize=address,undefined \
	    -o $@ src/check.c $(shell pkg-config --libs sdl2) -lm -lpthread

clean:
	rm -rf build dist
.PHONY: all host check clean
