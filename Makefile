# DisketteRecover - CRC-guided flux-level floppy repair.
# SPDX-License-Identifier: GPL-2.0-or-later

HXC      := third_party/HxCFloppyEmulator
HXCBUILD := $(HXC)/build
LIBHXCFE := $(HXCBUILD)/libhxcfe.so

CC       ?= cc
CFLAGS   ?= -O2 -g
CFLAGS   += -Wall -Wextra -Wno-unused-parameter -std=gnu99 -D_GNU_SOURCE
INCLUDES := -Isrc \
            -I$(HXC)/libhxcfe/sources \
            -I$(HXC)/libhxcadaptor/sources \
            -I$(HXCBUILD)
LDLIBS   := -lhxcfe -lm
LDFLAGS  += -L$(HXCBUILD) -Wl,-rpath,$(abspath $(HXCBUILD))

SRCS := src/main.c src/dr_core.c src/dr_view.c src/dr_flux.c \
        src/dr_repair.c src/dr_rebin.c src/dr_pattern.c src/dr_crc.c src/dr_json.c src/dr_http.c \
        src/dr_web.c src/dr_web_plot.c
OBJS := $(SRCS:.c=.o)

BIN  := disketterecover

.PHONY: all clean distclean hxc test

all: $(BIN)

$(BIN): $(LIBHXCFE) $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(OBJS): src/dr.h src/dr_internal.h | $(LIBHXCFE)

# The viewer page is compiled into the binary so `serve` has no runtime
# asset dependency.
src/dr_web.c: web/index.html tools/embed.sh
	sh tools/embed.sh dr_web_index $< > $@

src/dr_web_plot.c: web/plot.html tools/embed.sh
	sh tools/embed.sh dr_web_plot $< > $@

hxc: $(LIBHXCFE)

$(LIBHXCFE):
	@if [ ! -f $(HXC)/readme.md ]; then \
		echo "third_party/HxCFloppyEmulator is empty - run:"; \
		echo "    git submodule update --init --recursive"; \
		exit 1; \
	fi
	$(MAKE) -C $(HXC)/libhxcadaptor/build
	$(MAKE) -C $(HXC)/libhxcfe/build

test: $(BIN)
	sh tests/run.sh

clean:
	rm -f $(OBJS) $(BIN) src/dr_web.c src/dr_web_plot.c

distclean: clean
	-$(MAKE) -C $(HXC)/libhxcfe/build clean
	-$(MAKE) -C $(HXC)/libhxcadaptor/build clean
