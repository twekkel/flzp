CC      = gcc
ARCH   ?= -march=native
CFLAGS ?= -O3 $(ARCH) -Wall -Wextra -std=c99
TARGET  = flzp

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

.PHONY: all clean install test

all: $(TARGET)

$(TARGET): flzp.c
	$(CC) $(CFLAGS) -o $@ $<

# Round trip: compress, decompress, compare with the original
test: $(TARGET)
	@[ "$$(echo test | ./$(TARGET) -c | ./$(TARGET) -d)" = test ] && echo OK || { echo FAILED; exit 1; }

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	rm -f $(TARGET)
