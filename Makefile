# Groovebox for the Shanling Q2. `make` builds the device binary into dist/.groovebox/ (needs the Q2
# sysroot, see README), `make host` an SDL2 window, `make check` the asserts.
SYSROOT ?= $(HOME)/.cache/q2/sysroot
CC_Q2 = clang -D_GNU_SOURCE --target=mipsel-linux-gnu --sysroot=$(SYSROOT) -march=mips32r2 -mabi=32 -mfp64 -mnan=2008 -mabs=2008
CFLAGS = -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -fno-strict-aliasing
LDFLAGS_Q2 = -m elf32ltsmip -e __start --dynamic-linker /lib/ld-linux-mipsn8.so.1 --image-base=0x400000 -z noexecstack \
             --hash-style=sysv --build-id=none -s
SRC = src/main.c src/ui.c src/engine.c src/seq.c src/wav.c src/project.c src/load.c src/fx.c src/source.c src/synth.c src/x0x.c src/drop.c
X0X_SRC = src/x0x/dsp/drum909.c src/x0x/dsp/drum808.c src/x0x/dsp/bass303.c src/x0x/dsp/breaks.c src/x0x/dsp/fxbus.c src/x0x/seq/tb3po.c
X0X_FLAGS = -std=gnu11 -O2 -ffp-contract=off -w
Q2_SRC = $(SRC) src/platform_q2.c src/audio.c src/crt-q2.c
HOST_SRC = $(SRC) src/platform_sdl.c
DIST = dist/.groovebox

KITS = dist/kits.stamp

all: $(DIST)/q2-groovebox $(DIST)/q2app $(KITS) dist/groovebox.zip

build/q2/fx.o: src/punch.c src/sloop_buses.c
build/q2/%.o: src/%.c src/*.h
	@mkdir -p build/q2
	$(CC_Q2) $(CFLAGS) -c $< -o $@
build/q2/synth.o: src/felucca/*
build/q2/x0x/%.o: src/x0x/%.c src/x0x/dsp/*.h src/x0x/seq/*.h
	@mkdir -p $(dir $@)
	$(CC_Q2) $(X0X_FLAGS) -c $< -o $@
$(DIST)/q2-groovebox: $(Q2_SRC:src/%.c=build/q2/%.o) $(X0X_SRC:src/x0x/%.c=build/q2/x0x/%.o)
	@mkdir -p $(DIST)
	ld.lld $(LDFLAGS_Q2) -o $@ $^ $(SYSROOT)/lib/libc.so.6 $(SYSROOT)/lib/libm.so.6 $(SYSROOT)/lib/libpthread.so.0 \
	    $(SYSROOT)/lib/libasound.so.2 $(SYSROOT)/lib/libgcc_s.so.1 $(SYSROOT)/lib/ld-linux-mipsn8.so.1
$(DIST)/q2app: q2app
	@mkdir -p $(DIST)
	cp $< $@
$(KITS): kits/* kits/*/*.wav
	@mkdir -p $(DIST)
	rm -rf $(DIST)/kits
	cp -r kits $(DIST)/kits
	@touch $@
dist/groovebox.zip: $(DIST)/q2-groovebox $(DIST)/q2app $(KITS)
	cd dist && rm -f groovebox.zip && zip -qr groovebox.zip .groovebox
	@ls -l $@

host: build/host/q2-groovebox
	@ln -sfn ../../kits build/host/kits
build/host/libx0x.a: $(X0X_SRC) src/x0x/dsp/*.h src/x0x/seq/*.h
	@mkdir -p build/host/x0x
	cd build/host/x0x && cc $(X0X_FLAGS) -fno-sanitize=all -c $(X0X_SRC:%=../../../%)
	ar rcs $@ build/host/x0x/*.o
build/host/q2-groovebox: $(HOST_SRC) src/*.h src/felucca/* build/host/libx0x.a
	@mkdir -p build/host
	cc $(CFLAGS) -DHOST $(shell pkg-config --cflags sdl2) -o $@ $(HOST_SRC) build/host/libx0x.a $(shell pkg-config --libs sdl2) -lm -lpthread

check: build/host/check
	./build/host/check
build/host/check: src/check.c src/*.c src/*.h src/felucca/* build/host/libx0x.a
	@mkdir -p build/host
	cc -std=gnu11 -O1 -g -Wall -fsanitize=address,undefined -fno-sanitize=shift-base -c -o build/host/fx.o src/fx.c
	cc -std=gnu11 -O1 -g -Wall -fsanitize=address,undefined -fno-sanitize=shift-base,signed-integer-overflow -c -o build/host/synth.o src/synth.c
	cc -std=gnu11 -O1 -g -Wall -fsanitize=address,undefined -c -o build/host/source.o src/source.c
	cc -std=gnu11 -O1 -g -Wall -fsanitize=address,undefined -c -o build/host/x0x.o src/x0x.c
	cc -std=gnu11 -O1 -g -Wall -Wno-unused-function -Wno-unused-parameter $(shell pkg-config --cflags sdl2) -fsanitize=address,undefined \
	    -o $@ src/check.c build/host/fx.o build/host/synth.o build/host/source.o build/host/x0x.o build/host/libx0x.a $(shell pkg-config --libs sdl2) -lm -lpthread

clean:
	rm -rf build dist
.PHONY: all host check clean
