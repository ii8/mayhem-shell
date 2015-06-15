
include ../config.mk

CFLAGS+=-I/usr/include/cairo -I../protocol $$(python3-config --cflags)
LDFLAGS=-lwayland-client -lcairo -lrt -lpython3.4m
SOURCES=\
	src/client.c\
	src/menu.c\
	src/pool.c\
	../protocol/xdg-shell.c\
	../protocol/mayhem.c

OBJECTS=$(SOURCES:.c=.o)
TARGET=mayhem-menu

.PHONY: all clean install

all: $(TARGET)

clean:
	rm -f $(OBJECTS) $(TARGET)

install:
	mkdir -p $(LIBEXECDIR)
	install $(TARGET) $(LIBEXECDIR)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) $(OBJECTS) -o $@

../protocol/%.c: ../protocol/%.xml
	$(MAKE) -C .. gen
