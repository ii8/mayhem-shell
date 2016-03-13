
include ../config.mk

CFLAGS+=-I/usr/include/cairo -I../protocol
LDFLAGS=-lwayland-client -lwayland-cursor -lcairo -lrt
SOURCES=\
	src/client.c\
	src/menu.c\
	src/pool.c\
	../protocol/xdg-shell.c\
	../protocol/mayhem.c

ifeq ($(USE_PYTHON), true)
CFLAGS+=$$(python3-config --cflags)
LDFLAGS+=-lpython3.4m
SOURCES+=src/python-api.c
endif

ifeq ($(USE_LUA), true)
LDFLAGS+=-llua
SOURCES+=src/lua-api.c
endif

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
