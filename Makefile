# CitrusPilot — build the citrusvid player (+ diagnostic tools, host unit tests).
#
# Native build (on the board, which has the libdrm + libav headers):
#   make
#
# Cross build (x86 dev host, against a buildroot staging sysroot):
#   make SYSROOT=/path/to/output/<defconfig>/staging \
#        CC=/path/to/host/bin/aarch64-none-linux-gnu-gcc
#
# Host unit tests (pure modules — any C compiler, no DRM/libav/board):
#   make test
#
# Install citrusvid (default PREFIX=/usr/local):
#   make install            (or: make install PREFIX=/usr DESTDIR=/staging)

CC      ?= cc
PREFIX  ?= /usr/local
DESTDIR ?=

SRC := src
BIN := citrusvid

# Player = the top-level src/*.c modules (tools/ and tests/ build separately).
PLAYER_SRCS := $(SRC)/citrusvid.c $(SRC)/osd.c $(SRC)/osd_render.c \
               $(SRC)/stats.c $(SRC)/rtp_h265.c

CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS  := -lavcodec -lavutil -ldrm

# Cross-compile: aim every include/lib path at the staging sysroot. Native: the
# only non-default path is libdrm's header dir.
ifdef SYSROOT
  SYSFLAGS := --sysroot=$(SYSROOT) -I$(SYSROOT)/usr/include -I$(SYSROOT)/usr/include/libdrm
  LDFLAGS  += --sysroot=$(SYSROOT) -L$(SYSROOT)/usr/lib
else
  SYSFLAGS := -I/usr/include/libdrm
endif
ALLCFLAGS := $(CFLAGS) $(SYSFLAGS)

.PHONY: all clean test tools install
all: $(BIN)

# Single-shot compile+link — matches the proven build line; no .o intermediates.
$(BIN): $(PLAYER_SRCS)
	$(CC) $(ALLCFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

# Board-side diagnostics: plane-* need libdrm, v4l2-formats needs only headers.
TOOLS := plane-formats plane-probe v4l2-formats
tools: $(addprefix $(SRC)/tools/,$(TOOLS))
$(SRC)/tools/plane-formats: $(SRC)/tools/plane-formats.c
	$(CC) $(ALLCFLAGS) $< -o $@ $(LDFLAGS) -ldrm
$(SRC)/tools/plane-probe: $(SRC)/tools/plane-probe.c
	$(CC) $(ALLCFLAGS) $< -o $@ $(LDFLAGS) -ldrm
$(SRC)/tools/v4l2-formats: $(SRC)/tools/v4l2-formats.c
	$(CC) $(ALLCFLAGS) $< -o $@ $(LDFLAGS)

# Pure-module unit tests on the host (run-tests.sh picks up $CC, default cc).
test:
	$(SRC)/tests/run-tests.sh

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)

clean:
	rm -f $(BIN) $(addprefix $(SRC)/tools/,$(TOOLS))
