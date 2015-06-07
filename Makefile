
include config.mk

override CFLAGS+=-fpic -I/usr/include/pixman-1 -Iprotocol
LDFLAGS=-shared
TARGET=mayhem-shell.so
GENFILES=\
	protocol/mayhem.c\
	protocol/mayhem-client.h\
	protocol/mayhem-server.h\
	protocol/xdg-shell.c\
	protocol/xdg-shell-client.h\
	protocol/xdg-shell-server.h

SOURCES=\
	src/shell.c\
	protocol/mayhem.c\
	protocol/xdg-shell.c

OBJECTS=$(SOURCES:.c=.o)


.PHONY: all shell client clean install gen

all: gen client $(TARGET)

shell: gen $(TARGET)

client:
	$(MAKE) -C client

clean:
	rm -f $(GENFILES) $(OBJECTS) $(TARGET)
	$(MAKE) -C client clean

install:
	mkdir -p $(LIBDIR)/weston
	install $(TARGET) $(LIBDIR)/weston
	install mayhem-shell.sh $(BINDIR)/mayhem-shell
	cp -np mayhem-shell.ini $(CONFIGDIR)/mayhem-shell.ini
	$(MAKE) -C client install

gen: $(GENFILES)


$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) $(OBJECTS) -o $@

protocol/%.c: protocol/%.xml
	wayland-scanner code < $< > $@

protocol/%-server.h: protocol/%.xml
	wayland-scanner server-header < $< > $@

protocol/%-client.h: protocol/%.xml
	wayland-scanner client-header < $< > $@

