# smsc95xx.class - Poseidon class for SMSC/Microchip LAN95xx USB Ethernet.
#
# Toolchain: bebbo's amiga-gcc (m68k-amigaos-gcc). Override CC/PREFIX if it
# lives elsewhere:   make PREFIX=/opt/amiga
#
#   make              build build/smsc95xx.class
#   make fpucheck     prove the binary contains no FPU instructions
#   make clean

PREFIX  ?= /opt/amiga
CC      := $(PREFIX)/bin/m68k-amigaos-gcc
OBJDUMP := $(PREFIX)/bin/m68k-amigaos-objdump
BUILD   := build

# devices/sana2.h comes with the SANA-II / Roadshow part of the official NDK
# ("SANA+RoadshowTCP-IP/include"), not with every toolchain. Point SANA_INC at
# the directory that contains devices/sana2.h:
#     make SANA_INC=/path/to/NDK/SANA+RoadshowTCP-IP/include
SANA_INC ?= $(PREFIX)/m68k-amigaos/ndk-include

VERSION  := 0
REVISION := 17
DATE     := $(shell date +%d.%m.%Y)

# -m68040 / -msoft-float: Emu68 is the only target, and its FPU is partial, so
# the binary must not contain FPU code. -nostdlib: hand-rolled ROMTag.
CFLAGS := -nostdlib -nostartfiles -O2 -Wall -Wextra -Werror \
	-Wno-pointer-sign -fomit-frame-pointer -m68040 -msoft-float -Iinclude -I"$(SANA_INC)" \
	-DCLASS_VERSION=$(VERSION) -DCLASS_REVISION=$(REVISION) \
	-DCLASS_DATE=\"$(DATE)\"

# src/smsc95xx.class.c MUST come first: _start() has to land at text offset 0.
SRCS := src/smsc95xx.class.c src/lan95.c src/platform.c src/data.c src/sana.c
BIN  := $(BUILD)/smsc95xx.class
HDRS := $(wildcard include/*.h)

.PHONY: all clean fpucheck
all: $(BIN)

$(BUILD):
	mkdir -p $(BUILD)

$(BIN): $(SRCS) $(HDRS) | $(BUILD)
	@test -f "$(SANA_INC)/devices/sana2.h" || { \
	  echo "devices/sana2.h not found in SANA_INC=$(SANA_INC)"; \
	  echo "Set SANA_INC to the NDK directory SANA+RoadshowTCP-IP/include"; exit 1; }
	$(CC) $(CFLAGS) -o $@ $(SRCS)
	@ls -la $@

fpucheck: $(BIN)
	@n=$$($(OBJDUMP) -d $(BIN) 2>/dev/null | awk -F'\t' 'NF>=3 {split($$3,a," "); print a[1]}' | grep -ciE '^f'); \
	  if [ "$$n" -eq 0 ]; then echo "no FPU instructions"; else echo "FPU instructions: $$n"; exit 1; fi

clean:
	rm -rf $(BUILD)
