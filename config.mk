
PREFIX=/usr/local
BINDIR=$(PREFIX)/bin
LIBDIR=$(PREFIX)/lib
LIBEXECDIR=$(PREFIX)/libexec
CONFIGDIR=$$(getent passwd $(SUDO_USER) | cut -d: -f6)/.config

DEBUG?=true
CC=gcc
CFLAGS=-v
WARNS=	-Wall -Wextra -Wpedantic -ansi -Wshadow -Wpointer-arith -Wcast-qual\
	-Wstrict-prototypes -Wmissing-prototypes -Wredundant-decls\
	-Wcast-align -Wfloat-equal -Wmissing-include-dirs -Wlogical-op\
	-Waggregate-return -Wold-style-definition -Wpadded\
	-Wunsuffixed-float-constants -Winit-self -Wmisleading-indentation\
	-Woverlength-strings

ifeq ($(DEBUG), true)
CFLAGS+=-DDEBUG -g -Og $(WARNS)
else
CFLAGS+=-DNDEBUG -O2 -Wno-missing-field-initializers -Wno-unused-parameter
endif

USE_PYTHON?=false
USE_LUA?=true
