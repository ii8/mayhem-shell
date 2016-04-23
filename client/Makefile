
include ../config.mk

CFLAGS+=-I../protocol `pkg-config --cflags cairo freetype2`
LDFLAGS=-lrt `pkg-config --libs wayland-client wayland-cursor cairo freetype2 fontconfig`
SOURCES=\
	src/client.c\
	src/menu.c\
	src/pool.c\
	src/font.c\
	../protocol/xdg-shell.c\
	../protocol/mayhem.c

ifeq ($(USE_PYTHON), true)
CFLAGS+=$$(python3-config --cflags) -DAPI_PYTHON
LDFLAGS+=-lpython3.4m
SOURCES+=src/python-api.c
endif

ifeq ($(USE_LUA), true)
CFLAGS+=-DAPI_LUA
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
