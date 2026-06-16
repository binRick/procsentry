# procsentry — interactive process picker + live exec() tracer.
# termpaint is vendored under termpaint/, so a plain `make` builds everything
# with no downloads. Binaries land in build/.
TERMPAINT := termpaint
BUILD := build

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -I$(TERMPAINT)
LDLIBS += -lm

# defines meson normally supplies; -include stdarg.h papers over a missing
# include in termpaintx_ttyrescue.c that only bites on macOS
LIB_CFLAGS := -DTERMPAINT_RESCUE_EMBEDDED -DTERMPAINT_RESCUE_PATH='"/usr/libexec"' \
              -include stdarg.h -Wno-unused-parameter -Wno-unused-but-set-variable

LIB_SRCS := termpaint.c termpaint_event.c termpaint_input.c \
            termpaintx.c termpaintx_ttyrescue.c ttyrescue.c
LIB_OBJS := $(LIB_SRCS:%.c=$(BUILD)/termpaint/%.o)

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DOCDIR ?= $(PREFIX)/share/doc/procsentry

all: $(BUILD)/procsentry $(BUILD)/procsentry-gfx

# procsentry.c is the whole program (cell rendering); procsentry-gfx.c is a
# one-line shim that #defines TUI_BUILD_GFX and #includes procsentry.c to turn
# on the kitty-graphics backdrop. Both fall back to cells without graphics.
$(BUILD)/procsentry: $(BUILD)/procsentry.o $(BUILD)/tui.o $(BUILD)/kitty_gfx.o $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/procsentry-gfx: $(BUILD)/procsentry-gfx.o $(BUILD)/tui.o $(BUILD)/kitty_gfx.o $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/procsentry.o: procsentry.c tui.h kitty_gfx.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<
$(BUILD)/procsentry-gfx.o: procsentry-gfx.c procsentry.c tui.h kitty_gfx.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<
$(BUILD)/tui.o: tui.c tui.h kitty_gfx.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<
$(BUILD)/kitty_gfx.o: kitty_gfx.c kitty_gfx.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/termpaint/%.o: $(TERMPAINT)/%.c | $(BUILD)/termpaint
	$(CC) $(CFLAGS) $(LIB_CFLAGS) -c -o $@ $<

$(BUILD) $(BUILD)/termpaint:
	mkdir -p $@

install: all
	install -Dm755 $(BUILD)/procsentry     $(DESTDIR)$(BINDIR)/procsentry
	install -Dm755 $(BUILD)/procsentry-gfx $(DESTDIR)$(BINDIR)/procsentry-gfx
	install -Dm644 README.md               $(DESTDIR)$(DOCDIR)/README.md
	install -Dm644 LICENSE                  $(DESTDIR)$(DOCDIR)/LICENSE

run: $(BUILD)/procsentry-gfx
	./$(BUILD)/procsentry-gfx

clean:
	rm -rf $(BUILD)

.PHONY: all install run clean
